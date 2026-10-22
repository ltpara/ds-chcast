// Pure-logic checks for the host settings file: no GPU, no network.

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>

#include "chast/host_server.h"
#include "chast/host_settings.h"
#include "chast/quic.h"

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

std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

bool same_devices(const std::vector<chast::TrustedDevice>& a,
                  const std::vector<chast::TrustedDevice>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].public_key != b[i].public_key || a[i].name != b[i].name ||
            a[i].platform != b[i].platform ||
            a[i].added_unix_seconds != b[i].added_unix_seconds ||
            a[i].last_seen_unix_seconds != b[i].last_seen_unix_seconds) {
            return false;
        }
    }
    return true;
}

bool same(const chast::HostSettings& a, const chast::HostSettings& b) {
    return a.bind_address == b.bind_address && a.port == b.port &&
           a.allow_input == b.allow_input && a.allow_clipboard == b.allow_clipboard &&
           a.allow_audio == b.allow_audio && a.auth_mode == b.auth_mode &&
           a.require_approval == b.require_approval && a.password_hash == b.password_hash &&
           a.token_auth == b.token_auth &&
           a.allow_permission_change == b.allow_permission_change &&
           a.totp_secret == b.totp_secret && same_devices(a.trusted_devices, b.trusted_devices) &&
           a.ip_whitelist == b.ip_whitelist && a.token_mode == b.token_mode &&
           a.access_token == b.access_token && a.codec == b.codec &&
           a.bitrate_kbps == b.bitrate_kbps && a.framerate == b.framerate &&
           a.quality == b.quality && a.autostart == b.autostart && a.display == b.display &&
           a.tunnel == b.tunnel && a.server_url == b.server_url &&
           a.server_key == b.server_key && a.tunnel_id == b.tunnel_id &&
           a.tunnel_key == b.tunnel_key && a.tunnel_listen == b.tunnel_listen;
}

void test_round_trip(const std::filesystem::path& dir) {
    const std::string path = (dir / L"round-trip" / L"host.ini").string();
    chast::HostSettings settings;
    settings.bind_address = "192.168.1.5";
    settings.port = 6100;
    settings.allow_input = false;
    settings.allow_clipboard = false;
    settings.allow_audio = false;
    settings.auth_mode = chast::HostAuthMode::Password;
    settings.require_approval = false;
    settings.password_hash =
        "pbkdf2-sha256$210000$00112233445566778899aabbccddeeff$"
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    settings.token_auth = true;
    settings.allow_permission_change = false;
    settings.totp_secret = "MFRGGZDFMZTWQ2LKNNWG23TP";
    chast::TrustedDevice phone;
    phone.public_key = {1, 2, 3, 4, 5};
    phone.name = "工作笔记本";
    phone.platform = 1;
    phone.added_unix_seconds = 1700000000;
    phone.last_seen_unix_seconds = 1700001000;
    chast::TrustedDevice tablet;
    tablet.public_key = {9, 8, 7};
    tablet.name = "tablet";
    tablet.platform = 0;
    tablet.added_unix_seconds = 1700002000;
    tablet.last_seen_unix_seconds = 1700003000;
    settings.trusted_devices = {phone, tablet};
    settings.ip_whitelist = "192.168.0.0/16, 10.0.0.0/8";
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
    check(loaded.settings.allow_clipboard, "a fresh host syncs the clipboard");
    check(loaded.settings.allow_audio, "a fresh host shares the system sound");
    check(loaded.settings.token_mode == chast::HostTokenMode::OneTime,
          "a fresh host asks for a one-time token");
    check(loaded.settings.auth_mode == chast::HostAuthMode::Both,
          "a fresh host accepts a password or asks for approval");
    check(!loaded.settings.token_auth, "a fresh host does not accept access tokens");
    check(loaded.settings.allow_permission_change,
          "a fresh host lets the operator adjust permissions");
    check(loaded.settings.password_hash.empty() && loaded.settings.totp_secret.empty() &&
              loaded.settings.trusted_devices.empty() && loaded.settings.ip_whitelist.empty(),
          "a fresh host has no secrets, trusted devices or allow list");
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
               "clipboard=perhaps\n"
               "audio=perhaps\n"
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
    check(settings.allow_clipboard, "an unparsable clipboard toggle keeps the default");
    check(settings.allow_audio, "an unparsable audio toggle keeps the default");
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

    for (const char* key : {"port", "input", "clipboard", "audio", "token_mode", "token", "codec",
                            "bitrate_kbps", "framerate", "quality", "display", "tunnel",
                            "server_url", "server_key", "tunnel_id", "tunnel_key",
                            "tunnel_listen"}) {
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

void test_security_migration(const std::filesystem::path& dir) {
    const std::filesystem::path legacy = dir / "legacy.ini";
    write_text(legacy,
               "bind=0.0.0.0\n"
               "port=4433\n"
               "approve=0\n"
               "token_mode=one_time\n"
               "token=ABC234XY\n");
    chast::HostSettingsLoad loaded = chast::load_host_settings(legacy.string());
    check(loaded.settings.auth_mode == chast::HostAuthMode::Password,
          "a legacy approve=0 file migrates to password mode");
    check(!loaded.settings.require_approval, "the legacy approval mirror stays false");
    check(loaded.settings.token_auth,
          "a legacy token stays enabled until a password is configured");
    check(loaded.warning.empty(), "a legacy file needs no warning");

    const std::filesystem::path legacy_approval = dir / "legacy-approval.ini";
    write_text(legacy_approval, "approve=1\ntoken=ABC234XY\n");
    loaded = chast::load_host_settings(legacy_approval.string());
    check(loaded.settings.auth_mode == chast::HostAuthMode::Both,
          "a legacy approve=1 file migrates to both");
    check(loaded.settings.token_auth, "the migrated token authentication survives");

    const std::filesystem::path modern = dir / "modern.ini";
    write_text(modern,
               "auth_mode=click\n"
               "approve=0\n"
               "token_auth=0\n"
               "permissions=5\n"
               "input=0\n"
               "clipboard=1\n"
               "audio=0\n");
    loaded = chast::load_host_settings(modern.string());
    check(loaded.settings.auth_mode == chast::HostAuthMode::Click,
          "auth_mode wins over the legacy approve line");
    check(loaded.settings.require_approval, "the approval mirror follows the mode");
    check(!loaded.settings.token_auth, "an explicit token_auth=0 is honoured");
    check(loaded.settings.allow_input && !loaded.settings.allow_clipboard &&
              loaded.settings.allow_audio,
          "the permissions mask wins over the legacy switches");

    const std::filesystem::path broken = dir / "broken-security.ini";
    write_text(broken,
               "auth_mode=magic\n"
               "approve=1\n"
               "permissions=99\n"
               "trusted_devices=not-base64:also:bad\n"
               "ip_whitelist=10.0.0.0/8,300.1.2.3/8\n"
               "password_hash=plain-text\n"
               "totp_secret=two words\n");
    loaded = chast::load_host_settings(broken.string());
    check(loaded.settings.auth_mode == chast::HostAuthMode::Both,
          "an invalid auth_mode falls back through approve");
    check(loaded.settings.trusted_devices.empty(), "a malformed trust list is dropped");
    check(loaded.settings.ip_whitelist == "10.0.0.0/8,300.1.2.3/8",
          "an invalid allow list is preserved so the policy can fail closed");
    check(loaded.settings.password_hash.empty(), "a plain-text password hash is dropped");
    check(loaded.settings.totp_secret.empty(), "a whitespace-bearing TOTP secret is dropped");
    for (const char* key : {"auth_mode", "permissions", "trusted_devices", "ip_whitelist",
                            "password_hash", "totp_secret"}) {
        check(loaded.warning.find(key) != std::string::npos,
              (std::string("the security warning names ") + key).c_str());
    }
}

void test_totp_storage() {
    std::string stored;
    std::string error;
    check(chast::protect_totp_secret("MFRGGZDFMZTWQ2LKNNWG23TP", chast::SecretScope::CurrentUser,
                                     stored, &error),
          "a TOTP secret is protected");
    check(!stored.empty() && stored.find("MFRG") == std::string::npos,
          "the protected secret does not contain the clear text");
    std::string plain;
    check(chast::unprotect_totp_secret(stored, chast::SecretScope::CurrentUser, plain, &error),
          "the TOTP secret is unprotected");
    check(plain == "MFRGGZDFMZTWQ2LKNNWG23TP", "the TOTP secret round-trips");
    std::string ignored;
    check(chast::protect_totp_secret("", chast::SecretScope::CurrentUser, ignored, &error),
          "an empty TOTP secret is allowed");
    check(ignored.empty(), "an empty TOTP secret stores nothing");
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

void test_machine_paths() {
    const std::string machine_dir = chast::machine_settings_directory();
    const std::string machine_path = chast::machine_settings_path();
    check(!machine_dir.empty(), "ProgramData resolves");
    check(machine_dir != chast::host_settings_directory(),
          "machine and per-user settings are separate");
    check(machine_dir.size() > 6 &&
              machine_dir.compare(machine_dir.size() - 6, 6, "\\chast") == 0,
          "machine settings live in a chast folder");
    check(machine_path == machine_dir + "\\host.ini", "the machine settings file is host.ini");

    wchar_t program_data[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        const std::wstring expected =
            (std::filesystem::path(std::wstring(program_data, length)) / L"chast").wstring();
        check(_wcsicmp(widen_utf8(machine_dir).c_str(), expected.c_str()) == 0,
              "machine settings follow %ProgramData%");
    } else {
        check(false, "ProgramData is available in the environment");
    }
}

void test_identity_defaults() {
    chast::QuicServerConfig quic;
    check(quic.identity_scope == chast::IdentityScope::CurrentUser,
          "QUIC keeps the per-user identity by default");
    chast::HostServerConfig host;
    check(host.identity_scope == chast::IdentityScope::CurrentUser,
          "the host keeps the per-user identity by default");
    check(host.max_consecutive_failures == 5,
          "the interactive host stops after five consecutive failures");
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
    test_security_migration(dir);
    test_totp_storage();
    test_codec_names();
    test_service_parsers();
    test_generated_tunnel_id();
    test_machine_paths();
    test_identity_defaults();
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
