// Pure-logic checks for the host settings file: no GPU, no network.

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "chast/host_settings.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

std::filesystem::path temp_dir() {
    wchar_t buffer[MAX_PATH]{};
    GetTempPathW(MAX_PATH, buffer);
    std::filesystem::path dir = std::filesystem::path(buffer) / L"chast-settings-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool same(const chast::HostSettings& a, const chast::HostSettings& b) {
    return a.bind_address == b.bind_address && a.port == b.port &&
           a.allow_input == b.allow_input && a.require_approval == b.require_approval &&
           a.token_mode == b.token_mode && a.access_token == b.access_token &&
           a.codec == b.codec && a.bitrate_kbps == b.bitrate_kbps &&
           a.framerate == b.framerate && a.quality == b.quality && a.autostart == b.autostart &&
           a.display == b.display && a.tunnel == b.tunnel && a.server_url == b.server_url &&
           a.server_key == b.server_key && a.tunnel_id == b.tunnel_id &&
           a.tunnel_key == b.tunnel_key && a.tunnel_listen == b.tunnel_listen;
}

void test_round_trip(const std::filesystem::path& dir) {
    const std::string path = (dir / L"round-trip" / L"host.ini").string();
    chast::HostSettings settings;
    settings.bind_address = "192.168.1.5";
    settings.port = 6100;
    settings.allow_input = false;
    settings.require_approval = false;
    settings.token_mode = chast::HostTokenMode::Reusable;
    settings.access_token = "ABC234XY";
    settings.codec = chast::VideoCodec::Hevc;
    settings.bitrate_kbps = 12000;
    settings.framerate = 60;
    settings.quality = 0.5f;
    settings.autostart = true;
    settings.display = 1;
    settings.tunnel = false;
    settings.server_url = "https://rendezvous.example.com/chast";
    settings.server_key = "server secret";
    settings.tunnel_id = "devbox.office";
    settings.tunnel_key = "host secret 123";
    settings.tunnel_listen = "127.0.0.1:46000";

    std::string error;
    check(chast::save_host_settings(path, settings, &error), "settings save");
    check(error.empty(), "save reports no error");

    const chast::HostSettingsLoad loaded = chast::load_host_settings(path);
    check(loaded.existed, "saved settings are found again");
    check(loaded.warning.empty(), "a saved file loads without warnings");
    check(same(loaded.settings, settings), "every setting round-trips");
}

void test_missing_file(const std::filesystem::path& dir) {
    const chast::HostSettingsLoad loaded =
        chast::load_host_settings((dir / L"absent.ini").string());
    check(!loaded.existed, "a missing file is not reported as existing");
    check(loaded.warning.empty(), "a missing file is not a warning");
    check(loaded.settings.port == 4433 && loaded.settings.bind_address == "0.0.0.0",
          "missing file yields the documented defaults");
    check(loaded.settings.allow_input && loaded.settings.require_approval,
          "a fresh host accepts input behind an approval");
    check(loaded.settings.token_mode == chast::HostTokenMode::OneTime,
          "a fresh host asks for a one-time token");
    check(loaded.settings.tunnel, "a fresh host enables the connection service");
    check(loaded.settings.server_url.empty(), "a fresh host has no service URL");
    check(loaded.settings.tunnel_id.empty() && loaded.settings.tunnel_key.empty(),
          "a fresh host has no service identity yet");
    check(loaded.settings.tunnel_listen == "0.0.0.0:14434",
          "the tunnel listens on the documented UDP port");
}

void test_invalid_values(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "invalid.ini";
    write_text(path,
               "# hand edited\n"
               "bind=\n"
               "port=70000\n"
               "input=maybe\n"
               "approve=off\n"
               "token_mode=forever\n"
               "token=" + std::string(200, 'x') + "\n"
               "codec=vp9\n"
               "bitrate_kbps=fast\n"
               "framerate=0\n"
               "quality=2\n"
               "display=99\n"
               "tunnel=perhaps\n"
               "server_url=ftp://example.com\n"
               "server_key=" + std::string(300, 's') + "\n"
               "tunnel_id=bad id\n"
               "tunnel_key=short\n"
               "tunnel_listen=127.0.0.1:0\n"
               "unknown_key=whatever\n"
               "this line has no equals\n");
    const chast::HostSettingsLoad loaded = chast::load_host_settings(path.string());
    const chast::HostSettings& settings = loaded.settings;
    check(loaded.existed, "an edited file is read");
    check(settings.bind_address == "0.0.0.0", "an empty bind falls back");
    check(settings.port == 4433, "an out-of-range port falls back");
    check(settings.allow_input, "an unparsable boolean keeps the default");
    check(!settings.require_approval, "a valid boolean is still applied");
    check(settings.token_mode == chast::HostTokenMode::OneTime, "an unknown token mode falls back");
    check(settings.access_token.empty(), "an oversized token is dropped");
    check(settings.codec == chast::VideoCodec::H264, "an unknown codec falls back");
    check(settings.bitrate_kbps == 0, "a non-numeric bitrate falls back");
    check(settings.framerate == 30, "a zero framerate falls back");
    check(settings.quality == 0.67f, "an out-of-range quality falls back");
    check(settings.display == chast::kDisplayAuto, "an out-of-range display falls back");
    check(settings.tunnel, "an unparsable tunnel toggle keeps the default");
    check(settings.server_url.empty(), "a non-http service URL is dropped");
    check(settings.server_key.empty(), "an oversized server key is dropped");
    check(settings.tunnel_id.empty(), "an invalid device id is dropped");
    check(settings.tunnel_key.empty(), "a short device key is dropped");
    check(settings.tunnel_listen == "0.0.0.0:14434", "an invalid tunnel listen falls back");

    for (const char* key : {"port", "input", "token_mode", "token", "codec", "bitrate_kbps",
                            "framerate", "quality", "display", "tunnel", "server_url",
                            "server_key", "tunnel_id", "tunnel_key", "tunnel_listen"}) {
        check(loaded.warning.find(key) != std::string::npos,
              (std::string("the warning names ") + key).c_str());
    }
    check(loaded.warning.find("unknown_key") == std::string::npos,
          "unknown keys are accepted silently");
    check(loaded.warning.find("without '='") != std::string::npos,
          "a line without '=' is reported");
}

void test_tokens() {
    const std::string token = chast::generate_access_token();
    check(token.size() == 8, "a generated token is eight characters");
    bool alphabet = true;
    for (char c : token) {
        alphabet = alphabet && (c == '2' || c == '3' || c == '4' || c == '5' || c == '6' ||
                                c == '7' || c == '8' || c == '9' || (c >= 'A' && c <= 'Z')) &&
                   c != 'I' && c != 'O';
    }
    check(alphabet, "a generated token avoids look-alike characters");
    bool differ = false;
    for (int i = 0; i < 8 && !differ; ++i) {
        differ = chast::generate_access_token() != token;
    }
    check(differ, "generated tokens are not constant");
    check(chast::generate_access_token(16).size() == 16, "the token length is honoured");
}

void test_codec_names() {
    chast::VideoCodec codec = chast::VideoCodec::H264;
    check(chast::parse_codec_setting("HEVC", codec) && codec == chast::VideoCodec::Hevc,
          "HEVC is accepted in any case");
    check(chast::parse_codec_setting("h265", codec) && codec == chast::VideoCodec::Hevc,
          "h265 is an accepted alias");
    check(chast::parse_codec_setting("avc", codec) && codec == chast::VideoCodec::H264,
          "avc is an accepted alias");
    check(!chast::parse_codec_setting("vp9", codec), "an unknown codec is rejected");
    check(std::string(chast::codec_setting_name(chast::VideoCodec::Hevc)) == "hevc",
          "HEVC is written as hevc");
}

void test_service_parsers() {
    std::string out;
    check(chast::parse_server_url("", out) && out.empty(), "an empty service URL is allowed");
    check(chast::parse_server_url(" 127.0.0.1:8443/ ", out) &&
              out == "http://127.0.0.1:8443",
          "a bare address gains http:// and loses its trailing slash");
    check(chast::parse_server_url("HTTPS://Example.COM/chast///", out) &&
              out == "https://Example.COM/chast",
          "the scheme is normalized and nested slashes survive");
    check(!chast::parse_server_url("ftp://example.com", out), "a non-http scheme is rejected");
    check(!chast::parse_server_url("http://example.com?x=1", out), "a query is rejected");
    check(!chast::parse_server_url("http://", out), "a URL without a host is rejected");
    check(!chast::parse_server_url("http://example.com/a b", out), "a space is rejected");

    check(chast::valid_server_key("") && chast::valid_server_key("dev secret"),
          "optional server keys accept printable text");
    check(!chast::valid_server_key(std::string(300, 's')), "an oversized server key is rejected");
    check(!chast::valid_server_key("bad\nkey"), "a server key cannot span lines");

    check(chast::parse_tunnel_id("devbox-1", out) && out == "devbox-1",
          "a device id accepts letters, digits and separators");
    check(!chast::parse_tunnel_id("bad id", out), "a device id rejects spaces");
    check(!chast::parse_tunnel_id("-leading", out), "a device id starts alphanumeric");
    check(!chast::parse_tunnel_id("dot.", out), "a device id ends alphanumeric");
    check(!chast::parse_tunnel_id(std::string(65, 'a'), out), "a device id is at most 64 bytes");

    check(chast::valid_tunnel_key("host-secret") && chast::valid_tunnel_key("two words"),
          "device keys accept printable text");
    check(!chast::valid_tunnel_key("short"), "a device key needs eight characters");
    check(!chast::valid_tunnel_key("bad\"key"), "a device key cannot contain a quote");

    check(chast::parse_tunnel_listen("0.0.0.0:14434", out) && out == "0.0.0.0:14434",
          "an IPv4 tunnel listener parses");
    check(chast::parse_tunnel_listen("localhost:014434", out) && out == "localhost:14434",
          "the port is normalized");
    check(chast::parse_tunnel_listen("[::]:14434", out) && out == "[::]:14434",
          "an IPv6 tunnel listener keeps its brackets");
    check(!chast::parse_tunnel_listen("127.0.0.1", out), "a listener needs a port");
    check(!chast::parse_tunnel_listen("127.0.0.1:0", out), "the port cannot be zero");
    check(!chast::parse_tunnel_listen("::1:14434", out),
          "an unbracketed IPv6 listener is rejected");

    uint16_t port = 0;
    check(chast::tunnel_listen_port("0.0.0.0:14434", port) && port == 14434,
          "the firewall port is read back from the listener");
    check(!chast::tunnel_listen_port("broken", port), "a broken listener has no port");
}

void test_generated_tunnel_id() {
    const std::string id = chast::generate_tunnel_id();
    std::string normalized;
    check(!id.empty() && id.size() <= 40, "a generated device id fits the limit");
    check(chast::parse_tunnel_id(id, normalized) && normalized == id,
          "a generated device id passes the parser");
    check(chast::generate_tunnel_id() == id, "the generated device id is stable");
}

void test_unicode_path(const std::filesystem::path& dir) {
    // Windows profiles are often non-ASCII; the config path goes through
    // UTF-8, so a Chinese folder name has to survive both directions.
    const std::filesystem::path folder = dir / L"主机设置";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::string path = (folder / L"host.ini").string();
    chast::HostSettings settings;
    settings.port = 5000;
    settings.access_token = "ABC234XY";
    std::string error;
    check(chast::save_host_settings(path, settings, &error), "a non-ASCII path saves");
    const chast::HostSettingsLoad loaded = chast::load_host_settings(path);
    check(loaded.existed && loaded.settings.port == 5000 && loaded.warning.empty(),
          "a non-ASCII path loads back");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::filesystem::path dir = temp_dir();
    test_round_trip(dir);
    test_missing_file(dir);
    test_invalid_values(dir);
    test_tokens();
    test_codec_names();
    test_service_parsers();
    test_generated_tunnel_id();
    test_unicode_path(dir);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (failures != 0) {
        std::printf("host settings checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("host settings checks passed\n");
    return 0;
}
