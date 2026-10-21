#include "chast/host_settings.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string_view>

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
    std::ifstream in(native_path(path), std::ios::binary);
    if (!in) {
        return result;
    }
    result.existed = true;
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
        } else if (key == "input") {
            if (!parse_bool(value, settings.allow_input)) {
                warn(result.warning, "input '" + value + "' is not a boolean");
            }
        } else if (key == "clipboard") {
            if (!parse_bool(value, settings.allow_clipboard)) {
                warn(result.warning, "clipboard '" + value + "' is not a boolean");
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
        }
        // Unknown keys are kept quiet: a newer build may write settings this
        // one does not know, and that is not an error the operator can fix.
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
    std::ostringstream text;
    text << "# chast host settings\n"
         << "bind=" << settings.bind_address << "\n"
         << "port=" << settings.port << "\n"
         << "input=" << (settings.allow_input ? 1 : 0) << "\n"
         << "clipboard=" << (settings.allow_clipboard ? 1 : 0) << "\n"
         << "approve=" << (settings.require_approval ? 1 : 0) << "\n"
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
         << "tunnel_key=" << settings.tunnel_key << "\n"
         << "tunnel_listen=" << settings.tunnel_listen << "\n";

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
    static constexpr size_t kAlphabetSize = sizeof(kAlphabet) - 1;
    std::random_device device;
    std::mt19937 engine(device());
    std::uniform_int_distribution<size_t> pick(0, kAlphabetSize - 1);
    std::string token;
    token.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        token.push_back(kAlphabet[pick(engine)]);
    }
    return token;
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
