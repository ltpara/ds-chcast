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

#include "chast/encoder.h"
#include "chast/protocol.h"

namespace chast {

// One-time: the first accepted viewer spends the token. Reusable: the same
// token keeps working until the operator rotates it.
enum class HostTokenMode {
    OneTime,
    Reusable,
};

struct HostSettings {
    // 0.0.0.0 serves the LAN; 127.0.0.1 keeps everything local.
    std::string bind_address = "0.0.0.0";
    uint16_t port = 4433;
    // The gate belongs to the host, and a remote-desktop host is not useful
    // without it; access control is the token and the approval, not this.
    bool allow_input = true;
    bool require_approval = true;
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
