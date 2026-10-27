#include "chast/host_settings.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

#include "chast/security.h"

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
    HostSettingsLoad result;
    // The spool is per settings file, so a test config never mixes audit
    // lines into the operator's real one.
    result.settings.audit_spool = audit_spool_path_for(path);
    std::ifstream in(native_path(path), std::ios::binary);
    if (!in) {
        return result;
    }
    result.existed = true;
    bool auth_mode_seen = false;
    bool token_auth_seen = false;
    bool permissions_seen = false;
    uint16_t permissions_mask = kPermissionAll;
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
        } else if (key == "input") {
            if (!parse_bool(value, settings.allow_input)) {
                warn(result.warning, "input '" + value + "' is not a boolean");
            }
        } else if (key == "clipboard") {
            if (!parse_bool(value, settings.allow_clipboard)) {
                warn(result.warning, "clipboard '" + value + "' is not a boolean");
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
        } else if (key == "file_receive_dir") {
            settings.file_receive_directory = value;
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
                permissions_mask = static_cast<uint16_t>(mask);
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
                Ipv4Range range;
                if (!item.empty() && !parse_ipv4_cidr(item, range)) {
                    warn(result.warning, "ip_whitelist entry '" + item +
                                             "' is not an IPv4 address or CIDR; the list will "
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
    }
    return result;
}

bool save_host_settings(const std::string& path, const HostSettings& settings,
                        std::string* error) {
    const std::filesystem::path target = native_path(path);
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            *error = "could not create " + target.parent_path().string() + ": " + ec.message();
            return false;
        }
    }
    const uint16_t permissions =
        (settings.allow_input ? kPermissionInput : 0) |
        (settings.allow_clipboard ? kPermissionClipboard : 0) |
        (settings.allow_audio ? kPermissionAudio : 0) |
        (settings.allow_microphone ? kPermissionMicrophone : 0) |
        (settings.allow_file_transfer ? kPermissionFileTransfer : 0) |
        (settings.allow_port_forward ? kPermissionPortForward : 0) |
        (settings.allow_terminal ? kPermissionTerminal : 0) |
        (settings.allow_privacy_mode ? kPermissionPrivacyMode : 0);
    std::ostringstream text;
    text << "# chast host settings\n"
         << "bind=" << settings.bind_address << "\n"
         << "port=" << settings.port << "\n"
         << "carrier=" << (settings.carrier ? 1 : 0) << "\n"
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
         << "file_transfer=" << (settings.allow_file_transfer ? 1 : 0) << "\n"
         << "port_forward=" << (settings.allow_port_forward ? 1 : 0) << "\n"
         << "terminal=" << (settings.allow_terminal ? 1 : 0) << "\n"
         << "file_receive_dir=" << settings.file_receive_directory << "\n"
         << "audio=" << (settings.allow_audio ? 1 : 0) << "\n"
         << "audio_device=" << settings.audio_device << "\n"
         << "audio_aec=" << (settings.audio_aec ? 1 : 0) << "\n"
         << "microphone=" << (settings.allow_microphone ? 1 : 0) << "\n"
         << "privacy_mode=" << (settings.allow_privacy_mode ? 1 : 0) << "\n"
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
         << "# rendezvous service\n"
         << "tunnel=" << (settings.tunnel ? 1 : 0) << "\n"
         << "server_url=" << settings.server_url << "\n"
         << "server_key=" << settings.server_key << "\n"
         << "tunnel_id=" << settings.tunnel_id << "\n"
         << "tunnel_old_id=" << settings.tunnel_old_id << "\n"
         << "tunnel_key=" << settings.tunnel_key << "\n"
         << "tunnel_listen=" << settings.tunnel_listen << "\n";
    // Only a custom spool is written; the derived default would otherwise
    // pin a machine-specific path into a file that may be copied elsewhere.
    const std::string default_spool = audit_spool_path_for(path);
    if (!settings.audit_spool.empty() && settings.audit_spool != default_spool) {
        text << "audit_spool=" << settings.audit_spool << "\n";
    }

    std::filesystem::path temporary = target;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            *error = "could not write " + path;
            return false;
        }
        const std::string data = text.str();
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
    return codec == VideoCodec::Hevc ? "hevc" : "h264";
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
    return false;
}

}  // namespace chast
