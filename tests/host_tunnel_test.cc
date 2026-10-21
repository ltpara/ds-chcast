// Checks for the tray's tunnel child process: JSON events, argument quoting,
// settings validation and the process lifecycle. The lifecycle test re-launches
// this executable as a fake agent, so no network or Go toolchain is needed.

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "chast/host_settings.h"
#include "tunnel.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

void check_text(const std::string& text, const std::string& wanted, const char* what) {
    check(text == wanted, what);
    if (text != wanted) {
        std::printf("     got  %s\n     want %s\n", text.c_str(), wanted.c_str());
    }
}

bool contains(const std::wstring& text, const std::wstring& part) {
    return text.find(part) != std::wstring::npos;
}

std::wstring own_executable() {
    wchar_t buffer[32768]{};
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::wstring(buffer, length);
}

void test_json_field() {
    const std::string json =
        R"({"event":"ready","listen":"0.0.0.0:14434","host":"devbox","punch":"1.2.3.4:9"})";
    check_text(chast::host::json_string_field(json, "event"), "ready", "a string field is read");
    check_text(chast::host::json_string_field(json, "listen"), "0.0.0.0:14434",
               "an address field is read");
    check_text(chast::host::json_string_field(json, "missing"), "", "a missing field is empty");
    check_text(chast::host::json_string_field("{\"event\":7}", "event"), "",
               "a non-string field is empty");
    check_text(chast::host::json_string_field("{}", "event"), "", "an empty object has no fields");
    check_text(chast::host::json_string_field(R"({"event":"a\"b\\c"})", "event"),
               "a\"b\\c", "quotes and backslashes are unescaped");
    check_text(chast::host::json_string_field(R"({"host":"\u4e2d\u6587"})", "host"),
               "\xE4\xB8\xAD\xE6\x96\x87", "unicode escapes become UTF-8");
}

void test_event_parse() {
    chast::host::TunnelEvent event;
    check(!chast::host::parse_tunnel_event("chast-tunnel: registering", event),
          "a log line is not an event");
    check(!chast::host::parse_tunnel_event("{\"event\":\"shutdown\"}", event),
          "an unknown event is ignored");
    check(!chast::host::parse_tunnel_event("{\"listen\":\"x\"}", event),
          "an object without an event is ignored");
    check(chast::host::parse_tunnel_event(
              " {\"event\":\"ready\",\"listen\":\"0.0.0.0:14434\",\"host\":\"devbox\","
              "\"punch\":\"1.2.3.4:9\",\"relay\":\"5.6.7.8:9\"}",
              event),
          "a ready event parses with leading whitespace");
    check_text(event.event, "ready", "the event name is kept");
    check_text(event.listen, "0.0.0.0:14434", "the ready listen address is kept");
    check_text(event.host, "devbox", "the ready device id is kept");
    check_text(event.punch, "1.2.3.4:9", "the ready punch address is kept");
    check_text(event.relay, "5.6.7.8:9", "the ready relay address is kept");
    check(chast::host::parse_tunnel_event(
              R"({"event":"viewer","path":"relay","remote":"5.6.7.8:9"})", event),
          "a viewer event parses");
    check_text(event.path, "relay", "the viewer path is kept");
    check_text(event.remote, "5.6.7.8:9", "the viewer remote address is kept");
    check(chast::host::parse_tunnel_event(R"({"event":"relay","relay":"9.9.9.9:9"})", event),
          "a relay event parses");
    check_text(event.relay, "9.9.9.9:9", "a refreshed relay address is kept");
}

void test_executable_paths() {
    check(chast::host::tunnel_executable_for(L"C:\\Program Files\\chast\\chast-host.exe") ==
              L"C:\\Program Files\\chast\\chast-tunnel.exe",
          "the agent sits next to the host executable");
    check(chast::host::tunnel_executable_for(L"chast-host.exe") == L"chast-tunnel.exe",
          "a relative host path yields a relative sibling");
    check(chast::host::tunnel_executable_for(L"C:/build/Release/chast-host.exe") ==
              L"C:/build/Release/chast-tunnel.exe",
          "forward slashes survive the sibling rule");

    SetEnvironmentVariableW(L"CHAST_TUNNEL", L"C:\\staging\\chast-tunnel.exe");
    check(chast::host::tunnel_executable() == L"C:\\staging\\chast-tunnel.exe",
          "CHAST_TUNNEL overrides the sibling path");
    SetEnvironmentVariableW(L"CHAST_TUNNEL", nullptr);
    const std::wstring fallback = chast::host::tunnel_executable();
    check(fallback.size() > 15 && fallback.find(L"chast-tunnel.exe") != std::wstring::npos,
          "without an override the sibling is used");
}

chast::HostSettings valid_settings() {
    chast::HostSettings settings;
    settings.port = 4433;
    settings.tunnel = true;
    settings.server_url = "https://rendezvous.example.com";
    settings.tunnel_id = "devbox.office";
    settings.tunnel_key = "host secret 123";
    settings.tunnel_listen = "0.0.0.0:14434";
    return settings;
}

void test_command_line() {
    chast::HostSettings settings = valid_settings();
    settings.server_key = "server secret";
    const std::wstring command = chast::host::tunnel_command_line(
        L"C:\\Program Files\\chast\\chast-tunnel.exe", settings, 4433, "AA:BB:CC");

    check(contains(command, L"\"C:\\Program Files\\chast\\chast-tunnel.exe\" host"),
          "the executable is quoted and followed by host mode");
    check(contains(command, L"--server https://rendezvous.example.com"),
          "the service URL is passed");
    check(contains(command, L"--id devbox.office"), "the device id is passed");
    check(contains(command, L"--key \"host secret 123\""),
          "a device key with spaces is quoted");
    check(contains(command, L"--local 127.0.0.1:4433"),
          "the local listen port is the host QUIC port");
    check(contains(command, L"--listen 0.0.0.0:14434"), "the tunnel listener is passed");
    check(contains(command, L"--server-key \"server secret\""),
          "a server key with spaces is quoted");
    check(contains(command, L"--fingerprint AA:BB:CC"), "the certificate fingerprint is passed");

    settings.server_key.clear();
    const std::wstring without_optional =
        chast::host::tunnel_command_line(L"chast-tunnel.exe", settings, 4433, "");
    check(!contains(without_optional, L"--server-key"),
          "an empty server key leaves out the flag");
    check(!contains(without_optional, L"--fingerprint"),
          "an empty fingerprint leaves out the flag");

    const std::wstring quote_test = chast::host::tunnel_command_line(
        L"chast-tunnel.exe", settings, 4433, "a\"b");
    check(contains(quote_test, L"--fingerprint \"a\\\"b\""),
          "an embedded quote is escaped inside the argument");
}

void test_validation() {
    std::string error;
    chast::HostSettings settings = valid_settings();
    settings.tunnel = false;
    settings.server_url.clear();
    settings.tunnel_id.clear();
    settings.tunnel_key.clear();
    settings.tunnel_listen.clear();
    check(chast::host::validate_tunnel_settings(settings, &error),
          "a disabled tunnel ignores the service fields");

    settings = valid_settings();
    settings.server_url.clear();
    check(!chast::host::validate_tunnel_settings(settings, &error) &&
              error.find("服务器地址") != std::string::npos,
          "an enabled tunnel needs a service URL");

    settings = valid_settings();
    settings.server_url = "ftp://example.com";
    check(!chast::host::validate_tunnel_settings(settings, &error),
          "a non-http service URL is rejected");

    settings = valid_settings();
    settings.tunnel_id = "bad id";
    error.clear();
    check(!chast::host::validate_tunnel_settings(settings, &error) &&
              error.find("设备 ID") != std::string::npos,
          "an invalid device id is rejected");

    settings = valid_settings();
    settings.tunnel_key = "short";
    error.clear();
    check(!chast::host::validate_tunnel_settings(settings, &error) &&
              error.find("设备密钥") != std::string::npos,
          "a short device key is rejected");

    settings = valid_settings();
    settings.server_key = std::string(300, 's');
    error.clear();
    check(!chast::host::validate_tunnel_settings(settings, &error) &&
              error.find("服务器密钥") != std::string::npos,
          "an oversized server key is rejected");

    settings = valid_settings();
    settings.tunnel_listen = "127.0.0.1:0";
    error.clear();
    check(!chast::host::validate_tunnel_settings(settings, &error),
          "an invalid tunnel listener is rejected");

    settings = valid_settings();
    settings.tunnel_listen = "0.0.0.0:4433";
    error.clear();
    check(!chast::host::validate_tunnel_settings(settings, &error) &&
              error.find("QUIC") != std::string::npos,
          "the tunnel cannot reuse the host QUIC port");

    settings = valid_settings();
    error.clear();
    check(chast::host::validate_tunnel_settings(settings, &error) && error.empty(),
          "a complete service configuration validates");
}

int fake_agent() {
    std::fputs(
        "{\"event\":\"ready\",\"listen\":\"0.0.0.0:14434\",\"host\":\"fake\","
        "\"punch\":\"127.0.0.1:5000\",\"relay\":\"127.0.0.1:5001\"}\n",
        stdout);
    std::fflush(stdout);
    std::fputs("{\"event\":\"viewer\",\"path\":\"relay\",\"remote\":\"127.0.0.1:5001\"}\n",
               stdout);
    std::fflush(stdout);
    wchar_t hold[8]{};
    if (GetEnvironmentVariableW(L"CHAST_TEST_TUNNEL_HOLD", hold,
                                static_cast<DWORD>(std::size(hold))) > 0) {
        Sleep(30000);
    }
    return 7;
}

struct Recorded {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<chast::host::TunnelEvent> events;
    std::vector<std::string> logs;
    bool exited = false;
    uint32_t exit_code = 0;
};

void test_process_lifecycle() {
    SetEnvironmentVariableW(L"CHAST_TUNNEL", own_executable().c_str());
    SetEnvironmentVariableW(L"CHAST_TEST_TUNNEL_HOLD", nullptr);

    {
        Recorded recorded;
        chast::host::TunnelProcess process;
        std::string error;
        const bool started = process.start(
            valid_settings(), 4433, "AA:BB",
            [&recorded](const chast::host::TunnelEvent& event) {
                std::lock_guard<std::mutex> lock(recorded.mutex);
                recorded.events.push_back(event);
                recorded.cv.notify_all();
            },
            [&recorded](const std::string& line) {
                std::lock_guard<std::mutex> lock(recorded.mutex);
                recorded.logs.push_back(line);
            },
            [&recorded](uint32_t code) {
                std::lock_guard<std::mutex> lock(recorded.mutex);
                recorded.exited = true;
                recorded.exit_code = code;
                recorded.cv.notify_all();
            },
            &error);
        check(started, "the tunnel child starts");
        if (!started) {
            std::printf("     start error: %s\n", error.c_str());
        }
        std::unique_lock<std::mutex> lock(recorded.mutex);
        const bool finished = recorded.cv.wait_for(lock, std::chrono::seconds(10),
                                                   [&recorded] { return recorded.exited; });
        lock.unlock();
        check(finished, "the child exit is reported");
        check(recorded.exit_code == 7, "the exit code is kept");
        check(!process.running(), "a finished child is not running");
        check(recorded.events.size() == 2, "both machine events are delivered");
        if (recorded.events.size() == 2) {
            check_text(recorded.events[0].event, "ready", "the first event is ready");
            check_text(recorded.events[0].host, "fake", "the ready device id is delivered");
            check_text(recorded.events[1].path, "relay", "the viewer path is delivered");
        }
        process.stop();
        check(!process.running(), "stopping an exited child is safe");
    }

    {
        SetEnvironmentVariableW(L"CHAST_TEST_TUNNEL_HOLD", L"1");
        Recorded recorded;
        chast::host::TunnelProcess process;
        std::string error;
        bool started = process.start(
            valid_settings(), 4433, "AA:BB",
            [&recorded](const chast::host::TunnelEvent& event) {
                std::lock_guard<std::mutex> lock(recorded.mutex);
                recorded.events.push_back(event);
                recorded.cv.notify_all();
            },
            nullptr, nullptr, &error);
        check(started, "a holding tunnel child starts");
        {
            std::unique_lock<std::mutex> lock(recorded.mutex);
            recorded.cv.wait_for(lock, std::chrono::seconds(10),
                                 [&recorded] { return !recorded.events.empty(); });
        }
        SetEnvironmentVariableW(L"CHAST_TEST_TUNNEL_HOLD", nullptr);
        const auto begin = std::chrono::steady_clock::now();
        process.stop();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - begin)
                                 .count();
        check(!process.running(), "stop ends a running child");
        check(elapsed < 5000, "stop returns instead of waiting for the agent");
    }
    SetEnvironmentVariableW(L"CHAST_TUNNEL", nullptr);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr && argc > 1 && std::wstring(argv[1]) == L"host") {
        LocalFree(argv);
        return fake_agent();
    }
    LocalFree(argv);

    test_json_field();
    test_event_parse();
    test_executable_paths();
    test_command_line();
    test_validation();
    test_process_lifecycle();
    if (failures != 0) {
        std::printf("host tunnel checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("host tunnel checks passed\n");
    return 0;
}
