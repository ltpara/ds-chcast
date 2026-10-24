#pragma once

// Host-side settings for the tray application and the Windows service.
//
// One flat key=value file per host, read back with defaults for anything
// missing or invalid: a hand-edited config can never leave the server in an
// unusable state, and every correction is reported as a warning instead of
// silently changing a setting. The access token is stored next to the rest so
// a one-time token survives a restart until a viewer spends it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "chast/encoder.h"
#include "chast/protocol.h"
#include "chast/security.h"

namespace chast {

// One-time: the first accepted viewer spends the token. Reusable: the same
// token keeps working until the operator rotates it.
enum class HostTokenMode {
    OneTime,
    Reusable,
};

// How a viewer may get in. Password: a permanent or temporary password is
// required. Click: the operator approves every connection; passwords are not
// consulted. Both: a supplied password is checked and a wrong one is refused,
// while an empty one asks the operator (this is the default and the RustDesk
// "both" flow).
enum class HostAuthMode {
    Password,
    Click,
    Both,
};

// A viewer device that proved a 2FA login and asked to be remembered. The
// public key is not a secret; the private half stays on the viewer. Entries
// live for 90 days after their last use and can be revoked by the operator.
struct TrustedDevice {
    std::vector<uint8_t> public_key;
    std::string name;
    uint8_t platform = 0;
    int64_t added_unix_seconds = 0;
    int64_t last_seen_unix_seconds = 0;
};

inline constexpr size_t kMaxTrustedDevices = 32;

struct HostSettings {
    // 0.0.0.0 serves the LAN; 127.0.0.1 keeps everything local.
    std::string bind_address = "0.0.0.0";
    uint16_t port = 4433;
    // The gate belongs to the host, and a remote-desktop host is not useful
    // without it; access control is the token and the approval, not this.
    bool allow_input = true;
    // Sync clipboard changes (text, HTML, RTF and images) in both directions.
    // Off means the HelloAck reports clipboard unavailable and the viewer
    // stays silent.
    bool allow_clipboard = true;
    // Let the viewer offer files; the host answers and receives them into
    // file_receive_directory. Off means the HelloAck reports file transfer
    // unavailable and every FileOffer is ignored.
    bool allow_file_transfer = true;
    // UTF-8 directory for received files. Empty means Downloads\chast.
    std::string file_receive_directory;
    // Send the system sound to the viewer. Off means no audio stream is
    // opened and the HelloAck reports audio unavailable.
    bool allow_audio = true;
    // Accept the viewer's microphone into the host's speakers. Off by default:
    // the microphone is a privacy feature, so the HelloAck reports it
    // unavailable and CMIC streams are refused.
    bool allow_microphone = false;
    HostAuthMode auth_mode = HostAuthMode::Both;
    // Legacy mirror of auth_mode: true unless the mode is Password. The mode
    // is authoritative; this stays so older builds and callers keep working.
    bool require_approval = true;
    // PBKDF2-encoded permanent password. Empty means none; click mode and the
    // temporary password do not need one.
    std::string password_hash;
    // Accept the legacy access token from CLI tools. Off by default: the
    // interactive viewer authenticates with a password.
    bool token_auth = false;
    // The operator may narrow permissions in the approval window and toggle
    // them during a session.
    bool allow_permission_change = true;
    // DPAPI-protected TOTP secret (base64); empty means 2FA is off. Use
    // protect_totp_secret()/unprotect_totp_secret() for the clear text.
    std::string totp_secret;
    std::vector<TrustedDevice> trusted_devices;
    // Comma-separated IPv4 CIDR list; empty means no restriction. Entries that
    // do not parse make the list fail closed rather than disappear.
    std::string ip_whitelist;
    HostTokenMode token_mode = HostTokenMode::OneTime;
    // Empty means "generate one on first start".
    std::string access_token;
    VideoCodec codec = VideoCodec::H264;
    uint32_t bitrate_kbps = 0;
    uint16_t framerate = 30;
    float quality = 0.67f;
    bool autostart = false;
    // Fallback output when the viewer leaves the choice to the host.
    // kDisplayAuto means the primary display.
    uint16_t display = kDisplayAuto;

    // Rendezvous service. An empty server_url means standalone operation: the
    // host serves the LAN directly and does not register anywhere.
    bool tunnel = true;
    std::string server_url;
    std::string server_key;
    // Device identity registered with the service. The key is never uploaded;
    // the server stores only its SHA-256 digest.
    std::string tunnel_id;
    std::string tunnel_key;
    // Public-facing UDP socket of chast-tunnel host. It must differ from the
    // QUIC port above because both are UDP listeners.
    std::string tunnel_listen = "0.0.0.0:14434";
};

struct HostSettingsLoad {
    HostSettings settings;
    // Non-fatal problems found while reading, joined with "; ". The settings
    // still hold a usable value for every field.
    std::string warning;
    bool existed = false;
};

HostSettingsLoad load_host_settings(const std::string& path);
bool save_host_settings(const std::string& path, const HostSettings& settings, std::string* error);

// %APPDATA%\chast and %ProgramData%\chast. The tray uses the first (per-user),
// the service the second (machine-wide). Empty when the folder cannot be
// resolved.
std::string host_settings_directory();
std::string host_settings_path();
std::string machine_settings_directory();
std::string machine_settings_path();

// 8 characters from an alphabet without look-alikes (no 0/O/1/I), so a token
// read out loud or typed from a screen cannot be mistyped as another one.
std::string generate_access_token(size_t length = 8);

// "password"/"click"/"both", the spelling used in the settings file.
const char* auth_mode_name(HostAuthMode mode);
bool parse_auth_mode(const std::string& text, HostAuthMode& mode);

// trusted_devices= uses
// <base64(pubkey)>:<base64(name)>:<base64(platform)>:<added>:<last_seen>
// entries separated by commas. The decoder returns false on any malformed
// entry; callers drop the whole line so a corrupt list cannot silently trust
// the wrong key.
std::string encode_trusted_devices(const std::vector<TrustedDevice>& devices);
bool decode_trusted_devices(const std::string& text, std::vector<TrustedDevice>& devices,
                            std::string* error);

// The TOTP secret is DPAPI-protected with a fixed entropy string before it is
// written to host.ini. The tray uses CurrentUser, the service LocalMachine.
bool protect_totp_secret(const std::string& plaintext, SecretScope scope,
                         std::string& stored_base64, std::string* error);
bool unprotect_totp_secret(const std::string& stored_base64, SecretScope scope,
                           std::string& plaintext, std::string* error);

// "h264"/"hevc", the spelling used in the settings file.
const char* codec_setting_name(VideoCodec codec);
bool parse_codec_setting(const std::string& text, VideoCodec& codec);

// Service settings. All parsers trim their input and return false when the
// value is unusable; the caller decides whether to keep a default. An empty
// server URL is valid and means "not configured".
bool parse_server_url(const std::string& input, std::string& normalized);
bool valid_server_key(const std::string& key);
bool parse_tunnel_id(const std::string& input, std::string& normalized);
bool valid_tunnel_key(const std::string& key);
bool parse_tunnel_listen(const std::string& input, std::string& normalized);
bool tunnel_listen_port(const std::string& listen, uint16_t& port);
// Machine name normalized to the device-id alphabet, for a fresh host.
std::string generate_tunnel_id();

}  // namespace chast
