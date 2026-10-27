#include "tunnel.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <vector>

#include "platform.h"

namespace chast::host {

namespace {

std::wstring quote_argument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(c);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void append_utf8(std::string& out, unsigned int code) {
    if (code <= 0x7F) {
        out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

}  // namespace

std::string json_string_field(const std::string& json, const std::string& name) {
    const std::string needle = "\"" + name + "\"";
    size_t search = 0;
    while (search < json.size()) {
        const size_t pos = json.find(needle, search);
        if (pos == std::string::npos) {
            return {};
        }
        search = pos + needle.size();
        // Only match keys: a value may itself be the same word, most visibly
        // in {"event":"relay","relay":"..."}.
        size_t before = pos;
        while (before > 0 && (json[before - 1] == ' ' || json[before - 1] == '\t')) {
            --before;
        }
        if (before == 0 || (json[before - 1] != '{' && json[before - 1] != ',')) {
            continue;
        }
        size_t cursor = pos + needle.size();
        while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= json.size() || json[cursor] != ':') {
            continue;
        }
        ++cursor;
        while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= json.size() || json[cursor] != '"') {
            return {};
        }
        ++cursor;
        std::string value;
        while (cursor < json.size()) {
            const char c = json[cursor++];
            if (c == '"') {
                return value;
            }
            if (c != '\\') {
                value.push_back(c);
                continue;
            }
            if (cursor >= json.size()) {
                return {};
            }
            const char escaped = json[cursor++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    value.push_back(escaped);
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case 'u': {
                    if (cursor + 4 > json.size()) {
                        return {};
                    }
                    unsigned int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int digit = hex_value(json[cursor++]);
                        if (digit < 0) {
                            return {};
                        }
                        code = (code << 4) | static_cast<unsigned int>(digit);
                    }
                    append_utf8(value, code);
                    break;
                }
                default:
                    value.push_back(escaped);
                    break;
            }
        }
        return {};
    }
    return {};
}

bool parse_tunnel_event(const std::string& line, TunnelEvent& event) {
    size_t begin = 0;
    while (begin < line.size() && std::isspace(static_cast<unsigned char>(line[begin]))) {
        ++begin;
    }
    if (begin >= line.size() || line[begin] != '{') {
        return false;
    }
    event = TunnelEvent{};
    event.event = json_string_field(line, "event");
    if (event.event != "ready" && event.event != "viewer" && event.event != "relay") {
        return false;
    }
    event.listen = json_string_field(line, "listen");
    event.host = json_string_field(line, "host");
    event.path = json_string_field(line, "path");
    event.remote = json_string_field(line, "remote");
    event.punch = json_string_field(line, "punch");
    event.relay = json_string_field(line, "relay");
    event.migrated_from = json_string_field(line, "migrated_from");
    return true;
}

std::wstring tunnel_executable_for(const std::wstring& host_executable) {
    std::filesystem::path path(host_executable);
    path.replace_filename(L"chast-tunnel.exe");
    return path.wstring();
}

std::wstring tunnel_executable() {
    wchar_t override_path[32768]{};
    const DWORD length =
        GetEnvironmentVariableW(L"CHAST_TUNNEL", override_path,
                                static_cast<DWORD>(std::size(override_path)));
    if (length > 0 && length < static_cast<DWORD>(std::size(override_path))) {
        return std::wstring(override_path, length);
    }
    return tunnel_executable_for(executable_path());
}

std::wstring tunnel_command_line(const std::wstring& executable, const HostSettings& settings,
                                 uint16_t local_port, const std::string& fingerprint) {
    std::wstring command = quote_argument(executable);
    command += L" host";
    auto add = [&command](const wchar_t* flag, const std::wstring& value) {
        command += L" ";
        command += flag;
        command += L" ";
        command += quote_argument(value);
    };
    add(L"--server", widen(settings.server_url));
    add(L"--id", widen(settings.tunnel_id));
    if (!settings.tunnel_old_id.empty()) {
        add(L"--old-id", widen(settings.tunnel_old_id));
    }
    add(L"--key", widen(settings.tunnel_key));
    add(L"--local", L"127.0.0.1:" + std::to_wstring(local_port));
    add(L"--listen", widen(settings.tunnel_listen));
    if (settings.carrier && local_port != 0 && local_port < 65535) {
        // The carrier binds the same address as QUIC, but the agent always
        // reaches it over loopback when QUIC listens on a wildcard.
        const std::wstring carrier_host =
            (settings.bind_address.empty() || settings.bind_address == "0.0.0.0" ||
             settings.bind_address == "::")
                ? L"127.0.0.1"
                : widen(settings.bind_address);
        add(L"--carrier",
            carrier_host + L":" + std::to_wstring(static_cast<unsigned>(local_port) + 1));
    }
    if (!settings.server_key.empty()) {
        add(L"--server-key", widen(settings.server_key));
    }
    if (!settings.audit_spool.empty()) {
        // The agent signs and forwards the host's inbound audit records from
        // this spool; the path is shared with the host process, not a secret.
        add(L"--audit-spool", widen(settings.audit_spool));
    }
    if (!fingerprint.empty()) {
        add(L"--fingerprint", widen(fingerprint));
    }
    return command;
}

bool validate_tunnel_settings(const HostSettings& settings, std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (!settings.tunnel) {
        return true;
    }
    if (settings.server_url.empty()) {
        return fail("尚未填写服务器地址");
    }
    std::string normalized;
    if (!parse_server_url(settings.server_url, normalized)) {
        return fail("服务器地址无效，需要 http:// 或 https:// 地址");
    }
    if (!parse_tunnel_id(settings.tunnel_id, normalized)) {
        return fail("设备 ID 需要 1..64 个字母、数字、'.'、'_' 或 '-'，且以字母或数字开头结尾");
    }
    if (!valid_tunnel_key(settings.tunnel_key)) {
        return fail("设备密钥需要 8..128 个可打印字符，且不能包含引号或反斜杠");
    }
    if (!valid_server_key(settings.server_key)) {
        return fail("服务器密钥需要最多 256 个可打印字符，且不能包含引号或反斜杠");
    }
    if (!parse_tunnel_listen(settings.tunnel_listen, normalized)) {
        return fail("隧道监听地址无效，需要 host:port");
    }
    uint16_t tunnel_port = 0;
    if (!tunnel_listen_port(settings.tunnel_listen, tunnel_port)) {
        return fail("隧道监听端口无效");
    }
    if (tunnel_port == settings.port) {
        return fail("隧道监听端口不能与主机 QUIC 端口相同");
    }
    return true;
}

TunnelProcess::~TunnelProcess() {
    stop();
}

bool TunnelProcess::start(const HostSettings& settings, uint16_t local_port,
                          const std::string& fingerprint, EventCallback on_event,
                          LogCallback on_log, ExitCallback on_exit, std::string* error) {
    if (process_ != nullptr) {
        if (running_) {
            if (error != nullptr) {
                *error = "隧道代理已在运行";
            }
            return false;
        }
        stop();
    }
    std::string problem;
    if (!validate_tunnel_settings(settings, &problem)) {
        if (error != nullptr) {
            *error = problem;
        }
        return false;
    }
    const std::wstring executable = tunnel_executable();
    const std::wstring command =
        tunnel_command_line(executable, settings, local_port, fingerprint);

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &attributes, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &attributes, 0)) {
        if (stdout_read != nullptr) {
            CloseHandle(stdout_read);
        }
        if (stdout_write != nullptr) {
            CloseHandle(stdout_write);
        }
        if (stderr_read != nullptr) {
            CloseHandle(stderr_read);
        }
        if (stderr_write != nullptr) {
            CloseHandle(stderr_write);
        }
        if (error != nullptr) {
            *error = "无法创建隧道代理管道（Windows 错误 " + std::to_string(GetLastError()) + "）";
        }
        return false;
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    // Only the two pipe handles may cross the process boundary. Inheriting
    // every inheritable handle would also hand the agent the host's QUIC
    // socket, which then keeps port 4433 bound after the host process dies.
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<BYTE> attribute_storage(attribute_size);
    auto* attribute_list =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    HANDLE inherited[] = {stdout_write, stderr_write};
    const auto close_pipes = [&] {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        CloseHandle(stderr_read);
        CloseHandle(stderr_write);
    };
    if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_size)) {
        const DWORD code = GetLastError();
        close_pipes();
        if (error != nullptr) {
            *error = "无法准备隧道代理的启动属性（Windows 错误 " + std::to_string(code) + "）";
        }
        return false;
    }
    if (!UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof(inherited), nullptr, nullptr)) {
        const DWORD code = GetLastError();
        DeleteProcThreadAttributeList(attribute_list);
        close_pipes();
        if (error != nullptr) {
            *error = "无法准备隧道代理的启动属性（Windows 错误 " + std::to_string(code) + "）";
        }
        return false;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdOutput = stdout_write;
    startup.StartupInfo.hStdError = stderr_write;
    startup.StartupInfo.hStdInput = nullptr;
    startup.lpAttributeList = attribute_list;
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> command_buffer(command.begin(), command.end());
    command_buffer.push_back(L'\0');
    // Suspended until the agent is inside its own job, so a crash of this
    // process can never leave an unowned agent behind.
    const BOOL started = CreateProcessW(
        nullptr, command_buffer.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
        &startup.StartupInfo, &process);
    const DWORD start_error = GetLastError();
    DeleteProcThreadAttributeList(attribute_list);
    if (!started) {
        close_pipes();
        if (error != nullptr) {
            *error = "无法启动隧道代理（Windows 错误 " + std::to_string(start_error) + "）：" +
                     narrow(executable);
        }
        return false;
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    bool bound = false;
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        bound = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                        sizeof(limits)) != FALSE &&
                AssignProcessToJobObject(job, process.hProcess) != FALSE;
    }
    if (!bound) {
        const DWORD code = GetLastError();
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (job != nullptr) {
            CloseHandle(job);
        }
        close_pipes();
        if (error != nullptr) {
            *error = "无法约束隧道代理的生命周期（Windows 错误 " + std::to_string(code) + "）";
        }
        return false;
    }
    ResumeThread(process.hThread);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    CloseHandle(process.hThread);

    process_ = process.hProcess;
    job_ = job;
    stdout_read_ = stdout_read;
    stderr_read_ = stderr_read;
    on_event_ = std::move(on_event);
    on_log_ = std::move(on_log);
    on_exit_ = std::move(on_exit);
    running_ = true;
    finished_ = false;
    stopping_ = false;
    exit_code_ = 0;

    stdout_thread_ = CreateThread(nullptr, 0, stdout_entry, this, 0, nullptr);
    stderr_thread_ = CreateThread(nullptr, 0, stderr_entry, this, 0, nullptr);
    if (stdout_thread_ == nullptr || stderr_thread_ == nullptr) {
        if (error != nullptr) {
            *error = "无法创建隧道代理读取线程（Windows 错误 " +
                     std::to_string(GetLastError()) + "）";
        }
        stop();
        return false;
    }
    return true;
}

void TunnelProcess::stop() {
    if (process_ == nullptr) {
        running_ = false;
        return;
    }
    stopping_ = true;
    TerminateProcess(process_, 1);
    WaitForSingleObject(process_, 5000);
    if (stdout_thread_ != nullptr) {
        WaitForSingleObject(stdout_thread_, 5000);
        CloseHandle(stdout_thread_);
        stdout_thread_ = nullptr;
    }
    if (stderr_thread_ != nullptr) {
        WaitForSingleObject(stderr_thread_, 5000);
        CloseHandle(stderr_thread_);
        stderr_thread_ = nullptr;
    }
    if (stdout_read_ != nullptr) {
        CloseHandle(stdout_read_);
        stdout_read_ = nullptr;
    }
    if (stderr_read_ != nullptr) {
        CloseHandle(stderr_read_);
        stderr_read_ = nullptr;
    }
    CloseHandle(process_);
    process_ = nullptr;
    if (job_ != nullptr) {
        // Kills anything the agent itself may have left running.
        CloseHandle(job_);
        job_ = nullptr;
    }
    running_ = false;
}

bool TunnelProcess::running() const {
    return running_;
}

uint32_t TunnelProcess::exit_code() const {
    return exit_code_;
}

DWORD WINAPI TunnelProcess::stdout_entry(void* self) {
    static_cast<TunnelProcess*>(self)->read_stdout();
    return 0;
}

DWORD WINAPI TunnelProcess::stderr_entry(void* self) {
    static_cast<TunnelProcess*>(self)->read_stderr();
    return 0;
}

void TunnelProcess::read_stdout() {
    read_lines(stdout_read_, true);
}

void TunnelProcess::read_stderr() {
    read_lines(stderr_read_, false);
}

void TunnelProcess::read_lines(HANDLE pipe, bool events) {
    std::string pending;
    char chunk[4096];
    DWORD read = 0;
    while (ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
        pending.append(chunk, read);
        size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            publish_line(line, events);
        }
        if (pending.size() > 1024 * 1024) {
            pending.clear();
        }
    }
    if (!pending.empty()) {
        publish_line(pending, events);
    }
    if (events) {
        finish();
    }
}

void TunnelProcess::publish_line(const std::string& line, bool events) {
    if (line.empty()) {
        return;
    }
    if (!events) {
        if (on_log_) {
            on_log_(line);
        }
        return;
    }
    TunnelEvent event;
    if (parse_tunnel_event(line, event)) {
        if (on_event_) {
            on_event_(event);
        }
    } else if (on_log_) {
        on_log_("隧道代理输出：" + line);
    }
}

void TunnelProcess::finish() {
    if (stopping_ || finished_.exchange(true)) {
        return;
    }
    // stdout closes just before the process object is signalled; without this
    // wait GetExitCodeProcess would sometimes report STILL_ACTIVE.
    DWORD code = 1;
    if (process_ != nullptr) {
        WaitForSingleObject(process_, 2000);
        GetExitCodeProcess(process_, &code);
    }
    exit_code_ = code;
    running_ = false;
    if (!stopping_ && on_exit_) {
        on_exit_(code);
    }
}

}  // namespace chast::host
