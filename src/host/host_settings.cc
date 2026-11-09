#include "chast/host_settings.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

#include "chast/security.h"
#include "chast/access_ticket.h"

namespace chast {

namespace {

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::wstring to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) || raw == nullptr) {
        if (raw != nullptr) {
            CoTaskMemFree(raw);
        }
        return {};
    }
    std::string path = to_utf8(raw);
    CoTaskMemFree(raw);
    return path;
}

std::filesystem::path native_path(const std::string& utf8) {
    return std::filesystem::path(to_wide(utf8));
}

std::string trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (begin < end && space(text[begin])) {
        ++begin;
    }
    while (end > begin && space(text[end - 1])) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

void warn(std::string& warnings, const std::string& message) {
    if (!warnings.empty()) {
        warnings += "; ";
    }
    warnings += message;
}

bool parse_bool(const std::string& value, bool& out) {
    const std::string text = lower(value);
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parse_u32(const std::string& value, uint32_t& out) {
    try {
        size_t used = 0;
        const unsigned long parsed = std::stoul(value, &used, 10);
        if (used != value.size() || parsed > 0xFFFFFFFFul) {
            return false;
        }
        out = static_cast<uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_float(const std::string& value, float& out) {
    try {
        size_t used = 0;
        const float parsed = std::stof(value, &used);
        if (used != value.size()) {
            return false;
        }
        out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool valid_token(const std::string& token) {
    if (token.size() > kMaxAccessTokenLength) {
        return false;
    }
    return token.find_first_of("\r\n") == std::string::npos;
}

bool printable_secret(const std::string& value, size_t minimum, size_t maximum) {
    if (value.size() < minimum || value.size() > maximum) {
        return false;
    }
    for (unsigned char c : value) {
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') {
            return false;
        }
    }
    return true;
}

bool id_character(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

bool alphanumeric(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool parse_i64(const std::string& value, int64_t& out) {
    if (value.empty()) {
        return false;
    }
    int64_t parsed = 0;
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size()) {
        return false;
    }
    out = parsed;
    return true;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    size_t begin = 0;
    for (;;) {
        const size_t at = text.find(separator, begin);
        if (at == std::string::npos) {
            parts.push_back(text.substr(begin));
            return parts;
        }
        parts.push_back(text.substr(begin, at - begin));
        begin = at + 1;
    }
}

std::string encode_field(const std::string& text) {
    return base64_encode(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

constexpr char kTotpEntropy[] = "chast-totp-v1";

}  // namespace

bool parse_server_url(const std::string& input, std::string& normalized) {
    std::string text = trim(input);
    if (text.empty()) {
        normalized.clear();
        return true;
    }
    if (text.size() > 512) {
        return false;
    }
    if (text.find("://") == std::string::npos) {
        text = "http://" + text;
    }
    for (unsigned char c : text) {
        if (c < 0x21 || c > 0x7E || c == '"' || c == '\\' || c == '<' || c == '>') {
            return false;
        }
    }
    if (text.find('?') != std::string::npos || text.find('#') != std::string::npos) {
        return false;
    }
    const size_t separator = text.find("://");
    if (separator == std::string::npos || separator == 0) {
        return false;
    }
    const std::string scheme = lower(text.substr(0, separator));
    if (scheme != "http" && scheme != "https") {
        return false;
    }
    std::string rest = text.substr(separator + 3);
    while (!rest.empty() && rest.back() == '/') {
        rest.pop_back();
    }
    if (rest.empty() || rest.front() == '/') {
        return false;
    }
    const size_t slash = rest.find('/');
    if ((slash == std::string::npos ? rest : rest.substr(0, slash)).empty()) {
        return false;
    }
    normalized = scheme + "://" + rest;
    return true;
}

bool parse_proxy_url(const std::string& input, std::string& normalized) {
    std::string text = trim(input);
    if (text.empty()) {
        normalized.clear();
        return true;
    }
    if (text.size() > 512) {
        return false;
    }
    for (unsigned char c : text) {
        if (c < 0x21 || c > 0x7E || c == '"' || c == '\\' || c == '<' || c == '>') {
            return false;
        }
    }
    if (text.find('?') != std::string::npos || text.find('#') != std::string::npos) {
        return false;
    }
    const size_t separator = text.find("://");
    if (separator == std::string::npos || separator == 0) {
        return false;
    }
    const std::string scheme = lower(text.substr(0, separator));
    if (scheme != "http" && scheme != "https" && scheme != "socks5" && scheme != "socks5h") {
        return false;
    }
    std::string rest = text.substr(separator + 3);
    while (!rest.empty() && rest.back() == '/') {
        rest.pop_back();
    }
    if (rest.empty() || rest.find('/') != std::string::npos) {
        return false;
    }
    // Strip optional user:password@ before validating host:port.
    const size_t at = rest.rfind('@');
    if (at != std::string::npos) {
        if (at == 0) {
            return false;
        }
        rest = rest.substr(at + 1);
    }
    const size_t colon = rest.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= rest.size()) {
        return false;
    }
    const std::string host = rest.substr(0, colon);
    const std::string port_text = rest.substr(colon + 1);
    if (host.empty() || host.size() > 255) {
        return false;
    }
    if (host.front() == '[') {
        if (host.size() < 3 || host.back() != ']') {
            return false;
        }
    } else if (host.find('[') != std::string::npos || host.find(']') != std::string::npos) {
        return false;
    }
    if (port_text.empty() || port_text.size() > 5) {
        return false;
    }
    uint32_t port = 0;
    for (char c : port_text) {
        if (c < '0' || c > '9') {
            return false;
        }
        port = port * 10 + static_cast<uint32_t>(c - '0');
    }
    if (port < 1 || port > 65535) {
        return false;
    }
    normalized = scheme + "://" + text.substr(separator + 3);
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    return true;
}

bool valid_server_key(const std::string& key) {
    return key.empty() || printable_secret(key, 0, 256);
}

bool parse_tunnel_id(const std::string& input, std::string& normalized) {
    const std::string text = trim(input);
    if (text.empty() || text.size() > 64 || !alphanumeric(text.front()) ||
        !alphanumeric(text.back())) {
        return false;
    }
    for (unsigned char c : text) {
        if (!id_character(c)) {
            return false;
        }
    }
    normalized = text;
    return true;
}

bool valid_tunnel_key(const std::string& key) {
    return printable_secret(key, 8, 128);
}

bool parse_tunnel_listen(const std::string& input, std::string& normalized) {
    const std::string text = trim(input);
    if (text.empty() || text.size() > 128) {
        return false;
    }
    for (unsigned char c : text) {
        if (c < 0x21 || c > 0x7E || c == '"' || c == '\\') {
            return false;
        }
    }
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }
    const std::string host = text.substr(0, colon);
    const std::string port_text = text.substr(colon + 1);
    if (host.empty()) {
        return false;
    }
    if (host.front() == '[') {
        if (host.size() < 3 || host.back() != ']') {
            return false;
        }
    } else {
        for (unsigned char c : host) {
            if (!alphanumeric(c) && c != '.' && c != '-') {
                return false;
            }
        }
    }
    uint32_t port = 0;
    if (!parse_u32(port_text, port) || port == 0 || port > 65535) {
        return false;
    }
    normalized = host + ":" + std::to_string(port);
    return true;
}

bool tunnel_listen_port(const std::string& listen, uint16_t& port) {
    std::string normalized;
    if (!parse_tunnel_listen(listen, normalized)) {
        return false;
    }
    const size_t colon = normalized.rfind(':');
    uint32_t value = 0;
    if (colon == std::string::npos || !parse_u32(normalized.substr(colon + 1), value) ||
        value == 0 || value > 65535) {
        return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
}

std::string audit_spool_path_for(const std::string& settings_path) {
    if (settings_path.empty()) {
        return {};
    }
    std::filesystem::path path = native_path(settings_path);
    std::filesystem::path name = path.stem();
    name += L".audit.jsonl";
    path.replace_filename(name);
    return to_utf8(path.wstring());
}

std::string policy_state_path_for(const std::string& settings_path) {
    if (settings_path.empty()) {
        return {};
    }
    std::filesystem::path path = native_path(settings_path);
    std::filesystem::path name = path.stem();
    name += L".policy.json";
    path.replace_filename(name);
    return to_utf8(path.wstring());
}

std::string policy_ack_path_for(const std::string& settings_path) {
    if (settings_path.empty()) {
        return {};
    }
    std::filesystem::path path = native_path(settings_path);
    std::filesystem::path name = path.stem();
    name += L".policy-ack.json";
    path.replace_filename(name);
    return to_utf8(path.wstring());
}

namespace {

// The device policy state is a machine file written by chast-tunnel, so these
// readers only cover the JSON subset json.MarshalIndent emits. A key is looked
// up as an exact quoted token; values the agent never writes (arrays, nested
// objects, unicode escapes) make the state unreadable instead of being
// guessed at.
bool json_object_complete(const std::string& body) {
    size_t cursor = 0;
    while (cursor < body.size() && (body[cursor] == ' ' || body[cursor] == '\t' ||
                                    body[cursor] == '\r' || body[cursor] == '\n')) {
        ++cursor;
    }
    if (cursor >= body.size() || body[cursor] != '{') {
        return false;
    }
    int depth = 0;
    bool in_string = false;
    for (; cursor < body.size(); ++cursor) {
        const char c = body[cursor];
        if (in_string) {
            if (c == '\\') {
                ++cursor;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            for (++cursor; cursor < body.size(); ++cursor) {
                const char tail = body[cursor];
                if (tail != ' ' && tail != '\t' && tail != '\r' && tail != '\n') {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

bool json_key_cursor(const std::string& body, const std::string& name, size_t& cursor) {
    const std::string key = "\"" + name + "\"";
    const size_t at = body.find(key);
    if (at == std::string::npos) {
        return false;
    }
    cursor = at + key.size();
    const auto skip_space = [&body, &cursor] {
        while (cursor < body.size() && (body[cursor] == ' ' || body[cursor] == '\t' ||
                                        body[cursor] == '\r' || body[cursor] == '\n')) {
            ++cursor;
        }
    };
    skip_space();
    if (cursor >= body.size() || body[cursor] != ':') {
        return false;
    }
    ++cursor;
    skip_space();
    return true;
}

bool json_bool_at(const std::string& body, size_t cursor, bool& value) {    if (body.compare(cursor, 4, "true") == 0) {
        value = true;
        return true;
    }
    if (body.compare(cursor, 5, "false") == 0) {
        value = false;
        return true;
    }
    return false;
}

bool json_string_at(const std::string& body, size_t cursor, std::string& value) {
    if (cursor >= body.size() || body[cursor] != '"') {
        return false;
    }
    ++cursor;
    std::string out;
    while (cursor < body.size()) {
        const char c = body[cursor++];
        if (c == '"') {
            value = std::move(out);
            return true;
        }
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (cursor >= body.size()) {
            return false;
        }
        switch (body[cursor++]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: return false;
        }
    }
    return false;
}

bool json_int_at(const std::string& body, size_t cursor, int64_t& value) {
    bool negative = false;
    if (cursor < body.size() && body[cursor] == '-') {
        negative = true;
        ++cursor;
    }
    const size_t digits = cursor;
    int64_t parsed = 0;
    while (cursor < body.size() && body[cursor] >= '0' && body[cursor] <= '9') {
        if (parsed > (INT64_MAX - 9) / 10) {
            return false;
        }
        parsed = parsed * 10 + (body[cursor] - '0');
        ++cursor;
    }
    if (cursor == digits) {
        return false;
    }
    value = negative ? -parsed : parsed;
    return true;
}

std::string json_quote(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[7]{};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out += '"';
    return out;
}

}  // namespace

ManagedPolicyState read_managed_policy_state(const std::string& path) {
    ManagedPolicyState state;
    if (path.empty()) {
        return state;
    }
    std::ifstream in(native_path(path), std::ios::binary);
    if (!in) {
        std::error_code ec;
        const bool exists = std::filesystem::exists(native_path(path), ec);
        if (ec || exists) {
            state.readable = false;
        }
        return state;
    }
    std::ostringstream text;
    text << in.rdbuf();
    const std::string body = text.str();
    if (!json_object_complete(body)) {
        state.readable = false;
        return state;
    }
    size_t cursor = 0;
    bool managed = false;
    if (!json_key_cursor(body, "managed", cursor) || !json_bool_at(body, cursor, managed)) {
        state.readable = false;
        return state;
    }
    if (!managed) {
        return state;
    }
    std::string mode;
    if (!json_key_cursor(body, "mode", cursor) || !json_string_at(body, cursor, mode) ||
        mode.empty()) {
        state.readable = false;
        return state;
    }
    int64_t version = 0;
    if (!json_key_cursor(body, "policy_version", cursor) || !json_int_at(body, cursor, version) ||
        version <= 0 || version > 0xFFFFFFFFLL) {
        state.readable = false;
        return state;
    }
    state.managed = true;
    state.mode = std::move(mode);
    state.policy_version = static_cast<uint32_t>(version);
    int64_t ceiling = 0;
    if (json_key_cursor(body, "permission_ceiling", cursor) &&
        json_int_at(body, cursor, ceiling) && ceiling >= 0 && ceiling <= 0xFFFFFFFFLL) {
        state.permission_ceiling = static_cast<uint32_t>(ceiling);
    }
    int64_t updated_at_ms = 0;
    if (json_key_cursor(body, "updated_at_ms", cursor) &&
        json_int_at(body, cursor, updated_at_ms) && updated_at_ms >= 0) {
        state.updated_at_ms = updated_at_ms;
    }
    return state;
}

bool write_managed_policy_ack(const std::string& ack_path, const std::string& state_path,
                              const std::string& mode, uint32_t version,
                              uint32_t permission_ceiling, int64_t updated_at_ms,
                              std::string* error) {
    const auto fail = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };
    if (ack_path.empty() || state_path.empty()) {
        return fail("the managed policy acknowledgement path is not configured");
    }
    const ManagedPolicyState marker = read_managed_policy_state(state_path);
    if (!marker.readable) {
        return fail("the managed policy state cannot be read");
    }
    if (!marker.managed) {
        return fail("the managed policy state is not managed");
    }
    if (marker.mode != mode || marker.policy_version != version) {
        return fail("the managed policy state does not name version " +
                    std::to_string(version));
    }
    if (marker.permission_ceiling != permission_ceiling) {
        return fail("the managed policy state grants a different permission ceiling");
    }
    if (updated_at_ms != 0 && marker.updated_at_ms != updated_at_ms) {
        return fail("the managed policy state has a different update time");
    }
    const int64_t recorded_updated = updated_at_ms != 0 ? updated_at_ms : marker.updated_at_ms;
    const int64_t applied_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
    const std::string detail =
        (mode == "open" || mode == "restricted")
            ? "managed fail-closed: access tickets are not implemented (v62)"
            : "unknown managed mode enforced fail-closed: access tickets are not implemented (v62)";
    std::string body = "{\n  \"policy_version\": " + std::to_string(version) +
                       ",\n  \"mode\": " + json_quote(mode) +
                       ",\n  \"permission_ceiling\": " + std::to_string(permission_ceiling) +
                       ",\n  \"updated_at_ms\": " + std::to_string(recorded_updated) +
                       ",\n  \"applied\": true,\n  \"applied_at_ms\": " +
                       std::to_string(applied_at_ms) + ",\n  \"detail\": " + json_quote(detail) +
                       "\n}\n";
    const std::filesystem::path target = native_path(ack_path);
    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    std::filesystem::path temp = target;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail("cannot write " + ack_path);
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.flush();
        if (!out) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return fail("cannot write " + ack_path);
        }
    }
    if (!MoveFileExW(temp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return fail("cannot replace " + ack_path);
    }
    return true;
}

std::string generate_tunnel_id() {
    wchar_t buffer[256]{};
    DWORD length = static_cast<DWORD>(std::size(buffer));
    std::string name;
    if (GetComputerNameW(buffer, &length)) {
        name = to_utf8(std::wstring(buffer, length));
    }
    std::string id;
    for (unsigned char c : name) {
        if (alphanumeric(c)) {
            id.push_back(static_cast<char>(std::tolower(c)));
        } else if (!id.empty() && id.back() != '-') {
            id.push_back('-');
        }
    }
    while (!id.empty() && (id.back() == '-' || id.back() == '.')) {
        id.pop_back();
    }
    if (id.size() > 40) {
        id.resize(40);
        while (!id.empty() && (id.back() == '-' || id.back() == '.')) {
            id.pop_back();
        }
    }
    if (id.empty()) {
        id = "chast-host";
    }
    return id;
}

HostSettingsLoad load_host_settings(const std::string& path) {
    std::ifstream in(native_path(path), std::ios::binary);
    if (!in) {
        HostSettingsLoad missing;
        missing.settings.audit_spool = audit_spool_path_for(path);
        missing.settings.policy_state = policy_state_path_for(path);
        missing.settings.policy_ack = policy_ack_path_for(path);
        return missing;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parse_host_settings(text.str(), path);
}

HostSettingsLoad parse_host_settings(const std::string& contents, const std::string& path) {
    HostSettingsLoad result;
    result.settings.audit_spool = audit_spool_path_for(path);
    result.settings.policy_state = policy_state_path_for(path);
    result.settings.policy_ack = policy_ack_path_for(path);
    result.existed = true;
    std::istringstream in(contents);
    bool auth_mode_seen = false;
    bool token_auth_seen = false;
    bool permissions_seen = false;
    PermissionMask permissions_mask = kPermissionAll;
    std::string line;
    while (std::getline(in, line)) {
        const std::string text = trim(line);
        if (text.empty() || text[0] == '#' || text[0] == ';') {
            continue;
        }
        const size_t equals = text.find('=');
        if (equals == std::string::npos) {
            warn(result.warning, "ignored a line without '=': " + text);
            continue;
        }
        const std::string key = lower(trim(text.substr(0, equals)));
        const std::string value = trim(text.substr(equals + 1));
        HostSettings& settings = result.settings;
        if (key == "bind") {
            if (value.empty()) {
                warn(result.warning, "bind is empty; using " + settings.bind_address);
            } else {
                settings.bind_address = value;
            }
        } else if (key == "port") {
            uint32_t port = 0;
            if (!parse_u32(value, port) || port == 0 || port > 65535) {
                warn(result.warning, "port '" + value + "' is not 1..65535; using 4433");
            } else {
                settings.port = static_cast<uint16_t>(port);
            }
        } else if (key == "carrier") {
            if (!parse_bool(value, settings.carrier)) {
                warn(result.warning, "carrier '" + value + "' is not a boolean");
            }
        } else if (key == "max_viewers") {
            uint32_t viewers = 0;
            if (!parse_u32(value, viewers) || viewers == 0 || viewers > 8) {
                warn(result.warning,
                     "max_viewers '" + value + "' is not 1..8; using " +
                         std::to_string(settings.max_viewers));
            } else {
                settings.max_viewers = viewers;
            }
        } else if (key == "input") {
            if (!parse_bool(value, settings.allow_input)) {
                warn(result.warning, "input '" + value + "' is not a boolean");
            }
        } else if (key == "clipboard") {
            if (!parse_bool(value, settings.allow_clipboard)) {
                warn(result.warning, "clipboard '" + value + "' is not a boolean");
            }
        } else if (key == "clipboard_direction") {
            if (value == "both") settings.clipboard_direction = ClipboardDirection::Both;
            else if (value == "from_host") settings.clipboard_direction = ClipboardDirection::HostToViewer;
            else if (value == "to_host") settings.clipboard_direction = ClipboardDirection::ViewerToHost;
            else if (value == "disabled") settings.clipboard_direction = ClipboardDirection::Disabled;
            else {
                settings.clipboard_direction = ClipboardDirection::Disabled;
                warn(result.warning, "clipboard_direction '" + value + "' is invalid; clipboard disabled");
            }
        } else if (key == "file_transfer") {
            if (!parse_bool(value, settings.allow_file_transfer)) {
                warn(result.warning, "file_transfer '" + value + "' is not a boolean");
            }
        } else if (key == "port_forward") {
            if (!parse_bool(value, settings.allow_port_forward)) {
                warn(result.warning, "port_forward '" + value + "' is not a boolean");
            }
        } else if (key == "terminal") {
            if (!parse_bool(value, settings.allow_terminal)) {
                warn(result.warning, "terminal '" + value + "' is not a boolean");
            }
        } else if (key == "terminal_persist") {
            if (!parse_bool(value, settings.allow_terminal_persist)) {
                warn(result.warning, "terminal_persist '" + value + "' is not a boolean");
            }
        } else if (key == "camera") {
            if (!parse_bool(value, settings.allow_camera)) {
                warn(result.warning, "camera '" + value + "' is not a boolean");
            }
        } else if (key == "record") {
            if (!parse_bool(value, settings.allow_record)) {
                warn(result.warning, "record '" + value + "' is not a boolean");
            }
        } else if (key == "file_receive_dir") {
            settings.file_receive_directory = value;
        } else if (key == "record_incoming") {
            if (!parse_bool(value, settings.record_incoming)) {
                settings.record_incoming = false;
                warn(result.warning, "record_incoming must be a boolean");
            }
        } else if (key == "record_directory") {
            settings.record_directory = value;
        } else if (key == "audio") {
            if (!parse_bool(value, settings.allow_audio)) {
                warn(result.warning, "audio '" + value + "' is not a boolean");
            }
        } else if (key == "microphone") {
            if (!parse_bool(value, settings.allow_microphone)) {
                warn(result.warning, "microphone '" + value + "' is not a boolean");
            }
        } else if (key == "privacy_mode") {
            if (!parse_bool(value, settings.allow_privacy_mode)) {
                warn(result.warning, "privacy_mode '" + value + "' is not a boolean");
            }
        } else if (key == "privacy_mode_impl") {
            const std::string impl = lower(value);
            if (impl == "overlay" || impl == "virtual_display") {
                settings.privacy_mode_impl = impl;
            } else {
                // A typo must not silently select a different privacy policy.
                settings.privacy_mode_impl = "invalid";
                warn(result.warning, "privacy_mode_impl must be overlay or virtual_display");
            }
        } else if (key == "restart") {
            if (!parse_bool(value, settings.allow_restart)) {
                warn(result.warning, "restart '" + value + "' is not a boolean");
            }
        } else if (key == "block_input") {
            if (!parse_bool(value, settings.allow_block_input)) {
                warn(result.warning, "block_input '" + value + "' is not a boolean");
            }
        } else if (key == "os_login") {
            if (!parse_bool(value, settings.allow_os_login)) {
                warn(result.warning, "os_login '" + value + "' is not a boolean");
            }
        } else if (key == "session_select") {
            if (!parse_bool(value, settings.allow_session_select)) {
                warn(result.warning, "session_select '" + value + "' is not a boolean");
            }
        } else if (key == "allow_remote_config") {
            if (!parse_bool(value, settings.allow_remote_config)) {
                warn(result.warning,
                     "allow_remote_config '" + value + "' is not a boolean");
            }
        } else if (key == "lock_after_session_end") {
            if (!parse_bool(value, settings.lock_after_session_end)) {
                warn(result.warning,
                     "lock_after_session_end '" + value + "' is not a boolean");
            }
        } else if (key == "keep_awake") {
            if (!parse_bool(value, settings.keep_awake)) {
                warn(result.warning, "keep_awake '" + value + "' is not a boolean");
            }
        } else if (key == "auto_disconnect_minutes") {
            uint32_t minutes = 0;
            if (!parse_u32(value, minutes) || minutes > 10080) {
                warn(result.warning, "auto_disconnect_minutes '" + value +
                                         "' is not 0..10080; leaving it off");
            } else {
                settings.auto_disconnect_minutes = minutes;
            }
        } else if (key == "trusted_device_days") {
            uint32_t days = 0;
            // Ten years is the practical ceiling; 0 keeps the list forever.
            if (!parse_u32(value, days) || days > 3650) {
                warn(result.warning, "trusted_device_days '" + value +
                                         "' is not 0..3650; keeping 90 days");
            } else {
                settings.trusted_device_days = days;
            }
        } else if (key == "audio_device") {
            if (value.size() > 1024) {
                warn(result.warning, "audio_device is too long; using the default output");
            } else {
                // An id that is missing right now is valid: the operator may
                // have chosen a device that is currently unplugged, and the
                // session reports audio unavailable rather than picking one.
                settings.audio_device = value;
            }
        } else if (key == "audio_aec") {
            if (!parse_bool(value, settings.audio_aec)) {
                warn(result.warning, "audio_aec '" + value + "' is not a boolean");
            }
        } else if (key == "audio_silence_gate_ms") {
            uint32_t ms = 0;
            if (!parse_u32(value, ms) || ms > 60000) {
                warn(result.warning, "audio_silence_gate_ms '" + value +
                                         "' is not 0..60000; using 2000");
            } else {
                settings.audio_silence_gate_ms = ms;
            }
        } else if (key == "auth_mode") {
            HostAuthMode mode = HostAuthMode::Both;
            if (!parse_auth_mode(value, mode)) {
                warn(result.warning,
                     "auth_mode '" + value + "' is not password, click or both");
            } else {
                settings.auth_mode = mode;
                auth_mode_seen = true;
            }
        } else if (key == "password_hash") {
            if (value.empty()) {
                settings.password_hash.clear();
            } else if (value.size() > 512 ||
                       value.find_first_of(" \t\r\n") != std::string::npos ||
                       value.rfind("pbkdf2-sha256$", 0) != 0) {
                warn(result.warning,
                     "password_hash is not a pbkdf2-sha256 hash; clearing it");
                settings.password_hash.clear();
            } else {
                settings.password_hash = value;
            }
        } else if (key == "token_auth") {
            if (!parse_bool(value, settings.token_auth)) {
                warn(result.warning, "token_auth '" + value + "' is not a boolean");
            } else {
                token_auth_seen = true;
            }
        } else if (key == "permissions") {
            uint32_t mask = 0;
            // Bits outside the known set are refused, not silently dropped: a
            // newer build may have added a permission this one cannot honour.
            if (!parse_u32(value, mask) ||
                (mask & ~static_cast<uint32_t>(kPermissionAll)) != 0) {
                warn(result.warning, "permissions '" + value + "' is not a mask below " +
                                         std::to_string(kPermissionAll + 1));
            } else {
                permissions_seen = true;
                permissions_mask = mask;
            }
        } else if (key == "allow_permission_change") {
            if (!parse_bool(value, settings.allow_permission_change)) {
                warn(result.warning,
                     "allow_permission_change '" + value + "' is not a boolean");
            }
        } else if (key == "totp_secret") {
            if (value.empty()) {
                settings.totp_secret.clear();
            } else if (value.size() > 2048 || value.find_first_of(" \t\r\n") != std::string::npos) {
                warn(result.warning, "totp_secret is not a single base64 token; clearing it");
                settings.totp_secret.clear();
            } else {
                settings.totp_secret = value;
            }
        } else if (key == "trusted_devices") {
            std::string device_error;
            if (!decode_trusted_devices(value, settings.trusted_devices, &device_error)) {
                warn(result.warning, "trusted_devices: " + device_error + "; ignoring the list");
                settings.trusted_devices.clear();
            }
        } else if (key == "ip_whitelist") {
            settings.ip_whitelist = trim(value);
            for (const std::string& entry : split(settings.ip_whitelist, ',')) {
                const std::string item = trim(entry);
                IpRange range;
                if (!item.empty() && !parse_ip_cidr(item, range)) {
                    warn(result.warning, "ip_whitelist entry '" + item +
                                             "' is not an IP address or CIDR; the list will "
                                             "refuse every address");
                }
            }
        } else if (key == "approve") {
            if (!parse_bool(value, settings.require_approval)) {
                warn(result.warning, "approve '" + value + "' is not a boolean");
            }
        } else if (key == "token_mode") {
            const std::string mode = lower(value);
            if (mode == "one_time" || mode == "one-time") {
                settings.token_mode = HostTokenMode::OneTime;
            } else if (mode == "reusable") {
                settings.token_mode = HostTokenMode::Reusable;
            } else {
                warn(result.warning, "token_mode '" + value + "' is not one_time or reusable");
            }
        } else if (key == "token") {
            if (!valid_token(value)) {
                warn(result.warning, "token is longer than 128 bytes or spans lines; a new one "
                                     "will be generated");
            } else {
                settings.access_token = value;
            }
        } else if (key == "codec") {
            if (!parse_codec_setting(value, settings.codec)) {
                warn(result.warning, "codec '" + value + "' is not h264 or hevc");
            }
        } else if (key == "bitrate_kbps") {
            uint32_t rate = 0;
            if (!parse_u32(value, rate) || rate > 100000) {
                warn(result.warning, "bitrate_kbps '" + value + "' is not 0..100000");
            } else {
                settings.bitrate_kbps = rate;
            }
        } else if (key == "framerate") {
            uint32_t framerate = 0;
            if (!parse_u32(value, framerate) || framerate == 0 || framerate > 240) {
                warn(result.warning, "framerate '" + value + "' is not 1..240");
            } else {
                settings.framerate = static_cast<uint16_t>(framerate);
            }
        } else if (key == "quality") {
            float quality = 0.0f;
            if (!parse_float(value, quality) || quality < 0.1f || quality > 1.0f) {
                warn(result.warning, "quality '" + value + "' is not 0.1..1.0");
            } else {
                settings.quality = quality;
            }
        } else if (key == "autostart") {
            if (!parse_bool(value, settings.autostart)) {
                warn(result.warning, "autostart '" + value + "' is not a boolean");
            }
        } else if (key == "display") {
            if (lower(value) == "auto") {
                settings.display = kDisplayAuto;
            } else {
                uint32_t display = 0;
                if (!parse_u32(value, display) || display >= kMaxDisplayCount) {
                    warn(result.warning, "display '" + value + "' is not auto or an index below " +
                                             std::to_string(kMaxDisplayCount));
                } else {
                    settings.display = static_cast<uint16_t>(display);
                }
            }
        } else if (key == "virtual_display") {
            if (!parse_virtual_display_policy(value, settings.virtual_display)) {
                warn(result.warning,
                     "virtual_display '" + value + "' is not off, auto or on");
            }
        } else if (key == "virtual_display_monitors") {
            uint32_t monitors = 0;
            if (!parse_u32(value, monitors) || monitors == 0) {
                warn(result.warning, "virtual_display_monitors '" + value + "' is not 1.." +
                                         std::to_string(kMaxVirtualDisplayMonitors));
            } else if (monitors > kMaxVirtualDisplayMonitors) {
                warn(result.warning, "virtual_display_monitors '" + value + "' exceeds " +
                                         std::to_string(kMaxVirtualDisplayMonitors) +
                                         "; clamping");
                settings.virtual_display_monitors = kMaxVirtualDisplayMonitors;
            } else {
                settings.virtual_display_monitors = monitors;
            }
        } else if (key == "virtual_display_resolution") {
            std::string normalized;
            if (!parse_virtual_display_resolution(value, normalized)) {
                warn(result.warning, "virtual_display_resolution '" + value +
                                         "' is not WIDTHxHEIGHT; using " +
                                         settings.virtual_display_resolution);
            } else {
                settings.virtual_display_resolution = normalized;
            }
        } else if (key == "virtual_display_provider") {
            std::string normalized;
            if (!parse_virtual_display_provider(value, normalized)) {
                warn(result.warning, "virtual_display_provider '" + value +
                                         "' is not auto, usbmmidd or vdd");
            } else {
                settings.virtual_display_provider = normalized;
            }
        } else if (key == "tunnel") {
            if (!parse_bool(value, settings.tunnel)) {
                warn(result.warning, "tunnel '" + value + "' is not a boolean");
            }
        } else if (key == "server_url") {
            std::string normalized;
            if (!parse_server_url(value, normalized)) {
                warn(result.warning, "server_url '" + value + "' is not an http(s) URL");
            } else {
                settings.server_url = normalized;
            }
        } else if (key == "server_key") {
            if (!valid_server_key(value)) {
                warn(result.warning, "server_key is not 256 bytes of printable text; clearing it");
            } else {
                settings.server_key = value;
            }
        } else if (key == "host_key") {
            if (!valid_server_key(value)) {
                warn(result.warning, "host_key is not 256 bytes of printable text; clearing it");
            } else {
                settings.host_key = value;
            }
        } else if (key == "proxy") {
            std::string normalized;
            if (!parse_proxy_url(value, normalized)) {
                // Do not echo the value: proxy URLs may embed credentials.
                warn(result.warning,
                     "proxy is not an http(s) or socks5 URL with an explicit port; ignoring it");
            } else {
                settings.proxy = normalized;
            }
        } else if (key == "tunnel_id") {
            if (value.empty()) {
                settings.tunnel_id.clear();
            } else {
                std::string normalized;
                if (!parse_tunnel_id(value, normalized)) {
                    warn(result.warning,
                         "tunnel_id '" + value + "' is not 1..64 letters, digits, '.', '_' or '-'");
                } else {
                    settings.tunnel_id = normalized;
                }
            }
        } else if (key == "tunnel_old_id") {
            if (value.empty()) {
                settings.tunnel_old_id.clear();
            } else {
                std::string normalized;
                if (!parse_tunnel_id(value, normalized)) {
                    warn(result.warning,
                         "tunnel_old_id '" + value + "' is not 1..64 letters, digits, '.', '_' or '-'");
                } else {
                    settings.tunnel_old_id = normalized;
                }
            }
        } else if (key == "tunnel_key") {
            if (value.empty()) {
                settings.tunnel_key.clear();
            } else if (!valid_tunnel_key(value)) {
                warn(result.warning,
                     "tunnel_key must be 8..128 printable characters without quotes");
            } else {
                settings.tunnel_key = value;
            }
        } else if (key == "tunnel_listen") {
            std::string normalized;
            if (!parse_tunnel_listen(value, normalized)) {
                warn(result.warning, "tunnel_listen '" + value + "' is not host:port");
            } else {
                settings.tunnel_listen = normalized;
            }
        } else if (key == "access_issuer") {
            if (value.empty()) {
                settings.access_issuer.clear();
            } else if (value.size() > 128 ||
                       value.find_first_of("\r\n") != std::string::npos ||
                       value.find('\0') != std::string::npos) {
                warn(result.warning, "access_issuer must be at most 128 characters");
            } else {
                settings.access_issuer = value;
            }
        } else if (key == "access_kid") {
            if (value.empty()) {
                settings.access_kid.clear();
            } else if (value.size() > 64 ||
                       value.find_first_of("\r\n") != std::string::npos ||
                       value.find('\0') != std::string::npos) {
                warn(result.warning, "access_kid must be at most 64 characters");
            } else {
                settings.access_kid = value;
            }
        } else if (key == "access_public_key") {
            if (value.empty()) {
                settings.access_public_key.clear();
            } else {
                std::vector<uint8_t> decoded;
                if (!decode_ticket_base64url(value, &decoded) || decoded.size() != 32) {
                    warn(result.warning,
                         "access_public_key is not a base64url Ed25519 public key");
                } else {
                    settings.access_public_key = value;
                }
            }
        } else if (key == "access_key") {
            // <kid>:<base64url public key>. Repeating the line trusts several
            // issuer kids at once so a rotation can overlap.
            const size_t colon = value.find(':');
            std::vector<uint8_t> decoded;
            const std::string kid =
                colon == std::string::npos ? std::string() : value.substr(0, colon);
            if (colon == std::string::npos || colon == 0 ||
                colon + 1 >= value.size() || kid.size() > 64 ||
                kid.find_first_of("\r\n") != std::string::npos ||
                kid.find('\0') != std::string::npos ||
                !decode_ticket_base64url(value.substr(colon + 1), &decoded) ||
                decoded.size() != 32) {
                warn(result.warning,
                     "access_key is not <kid>:<base64url Ed25519 public key>");
            } else if (settings.access_keys.size() >= 8) {
                warn(result.warning, "at most 8 additional access keys are kept");
            } else {
                settings.access_keys.push_back(
                    {kid, value.substr(colon + 1)});
            }
        } else if (key == "audit_spool") {
            // Empty keeps the file next to host.ini; there is no separate
            // switch, because a host that forwards nothing writes nothing.
            if (!value.empty()) {
                settings.audit_spool = value;
            }
        }
        // Unknown keys are kept quiet: a newer build may write settings this
        // one does not know, and that is not an error the operator can fix.
    }

    HostSettings& settings = result.settings;
    if (settings.record_incoming && (settings.record_directory.empty() ||
        settings.record_directory.find('\0') != std::string::npos ||
        !native_path(settings.record_directory).is_absolute())) {
        settings.record_incoming = false;
        warn(result.warning, "record_incoming disabled: choose an absolute record_directory");
    }
    if (auth_mode_seen) {
        settings.require_approval = settings.auth_mode != HostAuthMode::Password;
    } else {
        // Files written before M7 only had approve=. Migrating keeps the old
        // behaviour and, because those hosts relied on the token, leaves token
        // authentication on until the operator sets a password or turns it off.
        settings.auth_mode =
            settings.require_approval ? HostAuthMode::Both : HostAuthMode::Password;
        if (!token_auth_seen && !settings.access_token.empty()) {
            settings.token_auth = true;
        }
    }
    if (permissions_seen) {
        settings.allow_input = (permissions_mask & kPermissionInput) != 0;
        settings.allow_clipboard = (permissions_mask & kPermissionClipboard) != 0;
        settings.allow_audio = (permissions_mask & kPermissionAudio) != 0;
        settings.allow_microphone = (permissions_mask & kPermissionMicrophone) != 0;
        settings.allow_file_transfer = (permissions_mask & kPermissionFileTransfer) != 0;
        settings.allow_port_forward = (permissions_mask & kPermissionPortForward) != 0;
        settings.allow_terminal = (permissions_mask & kPermissionTerminal) != 0;
        settings.allow_privacy_mode = (permissions_mask & kPermissionPrivacyMode) != 0;
        // The mask is authoritative for every gate it predates, but not for
        // record, camera, restart, block input, OS login or session select:
        // older builds never wrote those bits, and the explicit lines (which
        // save always emits alongside the mask) decide them.
    }
    return result;
}

bool serialize_host_settings(const std::string& path, const HostSettings& settings,
                             std::string& output, std::string* error) {
    if (settings.record_directory.find_first_of("\r\n") != std::string::npos ||
        settings.record_directory.find('\0') != std::string::npos ||
        (settings.record_incoming && !native_path(settings.record_directory).is_absolute())) {
        if (error) *error = "Host recording requires an absolute directory without line breaks.";
        return false;
    }
    const PermissionMask permissions =
        (settings.allow_input ? kPermissionInput : 0) |
        (settings.allow_clipboard ? kPermissionClipboard : 0) |
        (settings.allow_audio ? kPermissionAudio : 0) |
        (settings.allow_microphone ? kPermissionMicrophone : 0) |
        (settings.allow_file_transfer ? kPermissionFileTransfer : 0) |
        (settings.allow_port_forward ? kPermissionPortForward : 0) |
        (settings.allow_terminal ? kPermissionTerminal : 0) |
        (settings.allow_camera ? kPermissionCamera : 0) |
        (settings.allow_privacy_mode ? kPermissionPrivacyMode : 0) |
        (settings.allow_record ? kPermissionRecord : 0) |
        (settings.allow_restart ? kPermissionRestart : 0) |
        (settings.allow_block_input ? kPermissionBlockInput : 0) |
        (settings.allow_os_login ? kPermissionOsLogin : 0) |
        (settings.allow_session_select ? kPermissionSessionSelect : 0);
    std::ostringstream text;
    text << "# chast host settings\n"
         << "bind=" << settings.bind_address << "\n"
         << "port=" << settings.port << "\n"
         << "carrier=" << (settings.carrier ? 1 : 0) << "\n"
         << "max_viewers=" << settings.max_viewers << "\n"
         << "# security\n"
         << "auth_mode=" << auth_mode_name(settings.auth_mode) << "\n"
         << "password_hash=" << settings.password_hash << "\n"
         << "token_auth=" << (settings.token_auth ? 1 : 0) << "\n"
         << "permissions=" << permissions << "\n"
         << "allow_permission_change=" << (settings.allow_permission_change ? 1 : 0) << "\n"
         << "totp_secret=" << settings.totp_secret << "\n"
         << "trusted_devices=" << encode_trusted_devices(settings.trusted_devices) << "\n"
         << "ip_whitelist=" << settings.ip_whitelist << "\n"
         << "# the legacy switches stay for older builds\n"
         << "input=" << (settings.allow_input ? 1 : 0) << "\n"
         << "clipboard=" << (settings.allow_clipboard ? 1 : 0) << "\n"
         << "clipboard_direction=" << (settings.clipboard_direction == ClipboardDirection::Both ? "both" :
                settings.clipboard_direction == ClipboardDirection::HostToViewer ? "from_host" :
                settings.clipboard_direction == ClipboardDirection::ViewerToHost ? "to_host" : "disabled") << "\n"
         << "file_transfer=" << (settings.allow_file_transfer ? 1 : 0) << "\n"
         << "port_forward=" << (settings.allow_port_forward ? 1 : 0) << "\n"
         << "terminal=" << (settings.allow_terminal ? 1 : 0) << "\n"
         << "terminal_persist=" << (settings.allow_terminal_persist ? 1 : 0) << "\n"
         << "camera=" << (settings.allow_camera ? 1 : 0) << "\n"
         << "record=" << (settings.allow_record ? 1 : 0) << "\n"
         << "record_incoming=" << (settings.record_incoming ? 1 : 0) << "\n"
         << "record_directory=" << settings.record_directory << "\n"
         << "file_receive_dir=" << settings.file_receive_directory << "\n"
         << "audio=" << (settings.allow_audio ? 1 : 0) << "\n"
         << "audio_device=" << settings.audio_device << "\n"
         << "audio_aec=" << (settings.audio_aec ? 1 : 0) << "\n"
         << "audio_silence_gate_ms=" << settings.audio_silence_gate_ms << "\n"
         << "microphone=" << (settings.allow_microphone ? 1 : 0) << "\n"
         << "privacy_mode=" << (settings.allow_privacy_mode ? 1 : 0) << "\n"
         << "privacy_mode_impl=" << settings.privacy_mode_impl << "\n"
         << "restart=" << (settings.allow_restart ? 1 : 0) << "\n"
         << "block_input=" << (settings.allow_block_input ? 1 : 0) << "\n"
         << "os_login=" << (settings.allow_os_login ? 1 : 0) << "\n"
         << "session_select=" << (settings.allow_session_select ? 1 : 0) << "\n"
         << "allow_remote_config=" << (settings.allow_remote_config ? 1 : 0) << "\n"
         << "lock_after_session_end=" << (settings.lock_after_session_end ? 1 : 0)
         << "\n"
         << "keep_awake=" << (settings.keep_awake ? 1 : 0) << "\n"
         << "auto_disconnect_minutes=" << settings.auto_disconnect_minutes << "\n"
         << "trusted_device_days=" << settings.trusted_device_days << "\n"
         << "approve=" << (settings.auth_mode == HostAuthMode::Password ? 0 : 1) << "\n"
         << "token_mode="
         << (settings.token_mode == HostTokenMode::Reusable ? "reusable" : "one_time") << "\n"
         << "token=" << settings.access_token << "\n"
         << "codec=" << codec_setting_name(settings.codec) << "\n"
         << "bitrate_kbps=" << settings.bitrate_kbps << "\n"
         << "framerate=" << settings.framerate << "\n"
         << "quality=" << std::fixed << std::setprecision(2) << settings.quality << "\n"
         << "autostart=" << (settings.autostart ? 1 : 0) << "\n"
         << "display="
         << (settings.display == kDisplayAuto ? "auto" : std::to_string(settings.display))
         << "\n"
         << "virtual_display=" << virtual_display_policy_name(settings.virtual_display) << "\n"
         << "virtual_display_monitors=" << settings.virtual_display_monitors << "\n"
         << "virtual_display_resolution=" << settings.virtual_display_resolution << "\n"
         << "virtual_display_provider=" << settings.virtual_display_provider << "\n"
         << "# rendezvous service\n"
         << "tunnel=" << (settings.tunnel ? 1 : 0) << "\n"
         << "server_url=" << settings.server_url << "\n"
         << "server_key=" << settings.server_key << "\n"
         << "host_key=" << settings.host_key << "\n"
         << "proxy=" << settings.proxy << "\n"
         << "tunnel_id=" << settings.tunnel_id << "\n"
         << "tunnel_old_id=" << settings.tunnel_old_id << "\n"
         << "tunnel_key=" << settings.tunnel_key << "\n"
         << "tunnel_listen=" << settings.tunnel_listen << "\n"
         << "access_issuer=" << settings.access_issuer << "\n"
         << "access_kid=" << settings.access_kid << "\n"
         << "access_public_key=" << settings.access_public_key << "\n";
    for (const HostSettings::AccessKeyPin& pin : settings.access_keys) {
        text << "access_key=" << pin.kid << ":" << pin.public_key << "\n";
    }
    // Only a custom spool is written; the derived default would otherwise
    // pin a machine-specific path into a file that may be copied elsewhere.
    const std::string default_spool = audit_spool_path_for(path);
    if (!settings.audit_spool.empty() && settings.audit_spool != default_spool) {
        text << "audit_spool=" << settings.audit_spool << "\n";
    }

    output = text.str();
    return true;
}

bool save_host_settings(const std::string& path, const HostSettings& settings,
                        std::string* error) {
    std::string data;
    if (!serialize_host_settings(path, settings, data, error)) return false;
    const std::filesystem::path target = native_path(path);
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            *error = "could not create " + target.parent_path().string() + ": " + ec.message();
            return false;
        }
    }
    std::filesystem::path temporary = target;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            *error = "could not write " + path;
            return false;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            *error = "could not write " + path;
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ec);
        *error = "could not replace " + path + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

std::string host_settings_directory() {
    static const std::string base = known_folder(FOLDERID_RoamingAppData);
    return base.empty() ? std::string() : base + "\\chast";
}

std::string host_settings_path() {
    const std::string dir = host_settings_directory();
    return dir.empty() ? std::string() : dir + "\\host.ini";
}

std::string machine_settings_directory() {
    static const std::string base = known_folder(FOLDERID_ProgramData);
    return base.empty() ? std::string() : base + "\\chast";
}

std::string machine_settings_path() {
    const std::string dir = machine_settings_directory();
    return dir.empty() ? std::string() : dir + "\\host.ini";
}

std::string portable_settings_path(const std::string& executable_path) {
    if (executable_path.empty()) {
        return {};
    }
    const std::filesystem::path exe = native_path(executable_path);
    if (!exe.has_parent_path()) {
        return {};
    }
    return (exe.parent_path() / L"chast-portable" / L"host.ini").string();
}

std::string generate_access_token(size_t length) {
    static constexpr char kAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    constexpr uint32_t kAlphabetSize = sizeof(kAlphabet) - 1;
    constexpr uint32_t kLimit = 256 - (256 % kAlphabetSize);
    std::string token;
    token.reserve(length);
    uint8_t byte = 0;
    while (token.size() < length) {
        if (!secure_random(&byte, 1)) {
            return {};
        }
        if (byte >= kLimit) {
            continue;
        }
        token.push_back(kAlphabet[byte % kAlphabetSize]);
    }
    return token;
}

const char* auth_mode_name(HostAuthMode mode) {
    switch (mode) {
        case HostAuthMode::Password:
            return "password";
        case HostAuthMode::Click:
            return "click";
        case HostAuthMode::Both:
            return "both";
    }
    return "both";
}

bool parse_auth_mode(const std::string& text, HostAuthMode& mode) {
    const std::string value = lower(trim(text));
    if (value == "password") {
        mode = HostAuthMode::Password;
        return true;
    }
    if (value == "click" || value == "approval") {
        mode = HostAuthMode::Click;
        return true;
    }
    if (value == "both") {
        mode = HostAuthMode::Both;
        return true;
    }
    return false;
}

std::string encode_trusted_devices(const std::vector<TrustedDevice>& devices) {
    std::string out;
    for (const TrustedDevice& device : devices) {
        if (!out.empty()) {
            out.push_back(',');
        }
        const uint8_t platform = device.platform;
        out += base64_encode(device.public_key);
        out.push_back(':');
        out += encode_field(device.name);
        out.push_back(':');
        out += base64_encode(&platform, 1);
        out.push_back(':');
        out += std::to_string(device.added_unix_seconds);
        out.push_back(':');
        out += std::to_string(device.last_seen_unix_seconds);
    }
    return out;
}

bool decode_trusted_devices(const std::string& text, std::vector<TrustedDevice>& devices,
                            std::string* error) {
    devices.clear();
    const std::string value = trim(text);
    if (value.empty()) {
        return true;
    }
    const std::vector<std::string> entries = split(value, ',');
    if (entries.size() > kMaxTrustedDevices) {
        if (error != nullptr) {
            *error = "more than " + std::to_string(kMaxTrustedDevices) + " trusted devices";
        }
        return false;
    }
    for (const std::string& entry : entries) {
        const std::vector<std::string> fields = split(entry, ':');
        if (fields.size() != 5) {
            if (error != nullptr) {
                *error = "a trusted device needs five colon-separated fields";
            }
            return false;
        }
        TrustedDevice device;
        if (!base64_decode(fields[0], device.public_key) || device.public_key.empty() ||
            device.public_key.size() > kMaxDeviceKeyLength) {
            if (error != nullptr) {
                *error = "a trusted device key is not valid base64";
            }
            return false;
        }
        std::vector<uint8_t> name;
        if (!base64_decode(fields[1], name) || name.size() > kMaxDeviceNameLength) {
            if (error != nullptr) {
                *error = "a trusted device name is not valid";
            }
            return false;
        }
        device.name.assign(name.begin(), name.end());
        std::vector<uint8_t> platform;
        if (!base64_decode(fields[2], platform) || platform.size() != 1) {
            if (error != nullptr) {
                *error = "a trusted device platform is not one byte";
            }
            return false;
        }
        device.platform = platform[0];
        if (!parse_i64(fields[3], device.added_unix_seconds) ||
            !parse_i64(fields[4], device.last_seen_unix_seconds) ||
            device.added_unix_seconds < 0 || device.last_seen_unix_seconds < 0) {
            if (error != nullptr) {
                *error = "a trusted device timestamp is not a non-negative integer";
            }
            return false;
        }
        devices.push_back(std::move(device));
    }
    return true;
}

bool protect_totp_secret(const std::string& plaintext, SecretScope scope,
                         std::string& stored_base64, std::string* error) {
    if (plaintext.empty()) {
        stored_base64.clear();
        return true;
    }
    return protect_secret(plaintext, scope, kTotpEntropy, stored_base64, error);
}

bool unprotect_totp_secret(const std::string& stored_base64, SecretScope scope,
                           std::string& plaintext, std::string* error) {
    if (stored_base64.empty()) {
        plaintext.clear();
        return true;
    }
    return unprotect_secret(stored_base64, scope, kTotpEntropy, plaintext, error);
}

const char* codec_setting_name(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::H264:
            return "h264";
        case VideoCodec::Hevc:
            return "hevc";
        case VideoCodec::Vp9:
            return "vp9";
        case VideoCodec::Av1:
            return "av1";
    }
    return "h264";
}

bool parse_codec_setting(const std::string& text, VideoCodec& codec) {
    const std::string value = lower(trim(text));
    if (value == "h264" || value == "avc") {
        codec = VideoCodec::H264;
        return true;
    }
    if (value == "hevc" || value == "h265") {
        codec = VideoCodec::Hevc;
        return true;
    }
    if (value == "vp9") { codec = VideoCodec::Vp9; return true; }
    if (value == "av1") {
        codec = VideoCodec::Av1;
        return true;
    }
    return false;
}

const char* virtual_display_policy_name(VirtualDisplayPolicy policy) {
    switch (policy) {
        case VirtualDisplayPolicy::Off:
            return "off";
        case VirtualDisplayPolicy::Auto:
            return "auto";
        case VirtualDisplayPolicy::On:
            return "on";
    }
    return "off";
}

bool parse_virtual_display_policy(const std::string& text, VirtualDisplayPolicy& policy) {
    const std::string value = lower(trim(text));
    if (value == "off") {
        policy = VirtualDisplayPolicy::Off;
        return true;
    }
    if (value == "auto") {
        policy = VirtualDisplayPolicy::Auto;
        return true;
    }
    if (value == "on") {
        policy = VirtualDisplayPolicy::On;
        return true;
    }
    return false;
}

bool parse_virtual_display_resolution(const std::string& input, std::string& normalized) {
    const std::string value = lower(trim(input));
    const size_t separator = value.find('x');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size() ||
        value.find('x', separator + 1) != std::string::npos) {
        return false;
    }
    uint32_t width = 0;
    uint32_t height = 0;
    if (!parse_u32(value.substr(0, separator), width) ||
        !parse_u32(value.substr(separator + 1), height)) {
        return false;
    }
    if (width < 640 || width > 7680 || height < 480 || height > 4320) {
        return false;
    }
    normalized = std::to_string(width) + "x" + std::to_string(height);
    return true;
}

bool parse_virtual_display_provider(const std::string& input, std::string& normalized) {
    const std::string value = lower(trim(input));
    if (value == "auto" || value == "usbmmidd" || value == "vdd") {
        normalized = value;
        return true;
    }
    return false;
}

}  // namespace chast
