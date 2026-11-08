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
#include "chast/virtual_display.h"

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
    // Accept the TCP/WebSocket fallback on port + 1. QUIC needs UDP, which
    // some client networks block; the fallback carries the same QUIC
    // datagrams over a TCP stream without changing the session protocol.
    bool carrier = true;
    // Viewers served at the same time, 1..8. 1 keeps the sequential host: a
    // second connection waits until the current viewer leaves. Above 1 every
    // viewer gets its own handshake, permissions, streams and audit record;
    // only one of them holds the control lease for input and privacy state.
    uint32_t max_viewers = 2;
    // The gate belongs to the host, and a remote-desktop host is not useful
    // without it; access control is the token and the approval, not this.
    bool allow_input = true;
    // Sync clipboard changes (including copied files) within the direction
    // policy below. Direction can only be changed by the local operator.
    // Off means the HelloAck reports clipboard unavailable and the viewer
    // stays silent.
    bool allow_clipboard = true;
    ClipboardDirection clipboard_direction = ClipboardDirection::Both;
    // Let the viewer offer files; the host answers and receives them into
    // file_receive_directory. Off means the HelloAck reports file transfer
    // unavailable and every FileOffer is ignored.
    bool allow_file_transfer = true;
    // Let the viewer dial TCP endpoints through this machine (port
    // forwarding). Off by default: the viewer picks the targets, so this is a
    // firewall decision rather than a convenience. Off means the HelloAck
    // reports port forwarding unavailable and every PortForwardRequest is
    // refused.
    bool allow_port_forward = false;
    // Let the viewer open a shell on this machine (remote terminal). Off by
    // default: the viewer gets the operator's own account, so this is an
    // explicit decision in the same spirit as port forwarding. Off means the
    // HelloAck reports terminal unavailable and every TerminalRequest is
    // refused.
    bool allow_terminal = false;
    // Keep a terminal service alive across viewer disconnects so a later
    // session can attach to it by service id. Off by default: a detached shell
    // keeps running with the operator's account, so the host opts in
    // explicitly. Only meaningful while allow_terminal is on.
    bool allow_terminal_persist = false;
    // Let the viewer stream one of this machine's cameras. Off by default:
    // the camera looks into the room, so this is an explicit decision in the
    // same spirit as port forwarding. Off means the HelloAck reports cameras
    // unavailable and every ListCameras/StartCamera is refused.
    bool allow_camera = false;
    // Consent to the viewer recording the received stream locally, matching
    // RustDesk's "enable-record-session". On by default; advisory, because the
    // host cannot stop a recording once the stream has reached the viewer.
    bool allow_record = true;
    // Local-only incoming video archive, independent of viewer consent.
    // Off by default; enabling requires an explicit absolute UTF-8 folder.
    bool record_incoming = false;
    std::string record_directory;
    // UTF-8 directory for received files. Empty means Downloads\chast.
    std::string file_receive_directory;
    // Send the system sound to the viewer. Off means no audio stream is
    // opened and the HelloAck reports audio unavailable.
    bool allow_audio = true;
    // Accept the viewer's microphone into the host's speakers. Off by default:
    // the microphone is a privacy feature, so the HelloAck reports it
    // unavailable and CMIC streams are refused.
    bool allow_microphone = false;
    // Render endpoint used for system-sound capture and viewer-microphone
    // playback, as a WASAPI id. Empty follows the default output; an id that
    // is not present at session time makes audio unavailable instead of
    // silently moving to another endpoint.
    std::string audio_device;
    // Cancel the viewer's microphone out of the loopback capture. Needs a live
    // microphone playback reference; with no CMIC stream it stays bypassed.
    bool audio_aec = true;
    // Stop feeding the encoder after this much digital silence; 0 disables the
    // gate. The gate never moves the playout timeline, it only stops producing
    // packets while nothing is audible.
    uint32_t audio_silence_gate_ms = 2000;
    // Let the viewer black out this machine's physical displays while the
    // session runs (privacy mode). Off by default: the overlay covers every
    // monitor, so a person sitting at the machine cannot see the desktop until
    // the session ends or the operator revokes the permission.
    bool allow_privacy_mode = false;
    // How privacy mode hides the local screen: "overlay" draws a mask over
    // every physical output, "virtual_display" moves capture to a temporary
    // virtual output and disables the physical ones. Any other value is kept
    // as "invalid" so a typo cannot silently select a different policy.
    std::string privacy_mode_impl = "overlay";
    // Let the viewer reboot this machine. Off by default: a restart drops
    // every local session and unsaved work, so it is an explicit grant in the
    // same spirit as port forwarding and the camera.
    bool allow_restart = false;
    // Let the viewer block the local keyboard and pointer while it keeps
    // injecting its own input. Off by default; the action also needs input
    // control, and the host always releases the block when the session ends
    // or the permission is revoked.
    bool allow_block_input = false;
    // Let the viewer type the operating-system password at the logon or lock
    // screen, so it can sign this machine back in or unlock it. Off by
    // default: the password crosses the wire and is typed on the secure
    // desktop, and only a host running as SYSTEM in the user's session can do
    // it at all. Also needs input control, and the host still refuses it
    // unless a sign-in or lock screen is actually current.
    bool allow_os_login = false;
    // Let the viewer list this machine's Windows sessions and move the stream
    // to another one (share-RDP style). Off by default: the list names other
    // signed-in users, and a switch moves the stream into their session. Only a
    // host running in service mode can honour it, and every request is still
    // checked against this setting and the control lease.
    bool allow_session_select = false;
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
    // Let a viewer holding kPermissionRemoteConfig read and change this
    // host's exposed policy switches (clipboard, file transfer, audio,
    // microphone, privacy mode, restart, block input, OS login, session
    // select, terminal, port forwarding, camera, recording) and the
    // lock-after-session-end switch. Off by default: the change is stored in
    // host.ini and outlives the session, so it is an explicit grant.
    bool allow_remote_config = false;
    // Lock this machine's console when the last viewer of a session leaves.
    // Off by default: a remote viewer locking the operator's console is a
    // host-state change, like a restart. The lock runs after the session has
    // already ended, so it never interrupts the viewer.
    bool lock_after_session_end = false;
    // Hold a power request during authenticated connections. Desktop sessions
    // also keep the display awake; other session modes only keep the system
    // awake. Local policy, applied when the host next starts.
    bool keep_awake = true;
    // Close the session after this many minutes without viewer activity
    // (input, chat, clipboard and other viewer requests). 0 keeps it open until
    // one side leaves and is the default: a forgotten viewer should not hold
    // the screen forever, but long unattended transfers must not be cut.
    uint32_t auto_disconnect_minutes = 0;
    // DPAPI-protected TOTP secret (base64); empty means 2FA is off. Use
    // protect_totp_secret()/unprotect_totp_secret() for the clear text.
    std::string totp_secret;
    std::vector<TrustedDevice> trusted_devices;
    // How long a remembered device may skip two-factor authentication after
    // its last successful use. 0 means it never expires; the default is 90
    // days. Changing it applies to devices already on the list, because the
    // comparison is against each device's last_seen timestamp.
    uint32_t trusted_device_days = 90;
    // Comma-separated IPv4 and/or IPv6 CIDR list; empty means no restriction.
    // Entries that do not parse make the list fail closed rather than
    // disappear.
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

    // Virtual display. Off (the default) only reports status; the operator
    // enables one from the tray. Auto creates one before a session when the
    // machine has no active display; On always does. A display created this
    // way stays until the next start or an explicit disable, because removing
    // it mid-session reflows every window.
    VirtualDisplayPolicy virtual_display = VirtualDisplayPolicy::Off;
    // 1..kMaxVirtualDisplayMonitors; larger values are clamped with a warning.
    uint32_t virtual_display_monitors = 1;
    // Preferred mode as WIDTHxHEIGHT. The provider picks the closest mode it
    // supports when the exact one is missing.
    std::string virtual_display_resolution = "1920x1080";
    // auto detects an installed provider; "usbmmidd" pins one. "vdd" parses
    // for forward compatibility but is not implemented yet.
    std::string virtual_display_provider = "auto";

    // Rendezvous service. An empty server_url means standalone operation: the
    // host serves the LAN directly and does not register anywhere.
    bool tunnel = true;
    std::string server_url;
    std::string server_key;
    // Device identity registered with the service. The key is never uploaded;
    // the server stores only its SHA-256 digest.
    std::string tunnel_id;
    // Previous id waiting to be released on the server. Set when the operator
    // changes tunnel_id; the agent proves ownership and the tray clears it
    // after the registration reply confirms the move.
    std::string tunnel_old_id;
    std::string tunnel_key;
    // Outbound proxy for the rendezvous API and the relay's TCP carrier.
    // Accepted schemes: http, https, socks5 and socks5h with an explicit
    // port. UDP direct/relay paths are unaffected.
    std::string proxy;
    // Public-facing UDP socket of chast-tunnel host. It must differ from the
    // QUIC port above because both are UDP listeners.
    std::string tunnel_listen = "0.0.0.0:14434";
    // JSONL spool the host appends finished inbound sessions and uploads to;
    // chast-tunnel host signs each line and forwards it to the service. Empty
    // means the file next to host.ini (<stem>.audit.jsonl), which load fills
    // in automatically. The agent removes lines only after the service stored
    // them, so a short outage loses nothing.
    std::string audit_spool;
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
// Same representation as host.ini, for authenticated local process handoffs.
// source_path preserves path-derived defaults; these functions do no file I/O.
HostSettingsLoad parse_host_settings(const std::string& text, const std::string& source_path);
bool serialize_host_settings(const std::string& source_path, const HostSettings& settings,
                             std::string& text, std::string* error);

// %APPDATA%\chast and %ProgramData%\chast. The tray uses the first (per-user),
// the service the second (machine-wide). Empty when the folder cannot be
// resolved.
std::string host_settings_directory();
std::string host_settings_path();
std::string machine_settings_directory();
std::string machine_settings_path();

// Portable mode keeps settings beside the executable, so a copied folder
// carries its own identity and never inherits the installed machine's.
// `executable_path` is the module file name; empty when it cannot be resolved.
std::string portable_settings_path(const std::string& executable_path);

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

// "h264"/"hevc"/"av1", the spelling used in the settings file.
const char* codec_setting_name(VideoCodec codec);
bool parse_codec_setting(const std::string& text, VideoCodec& codec);

// "off"/"auto"/"on", the spelling used in the settings file.
const char* virtual_display_policy_name(VirtualDisplayPolicy policy);
bool parse_virtual_display_policy(const std::string& text, VirtualDisplayPolicy& policy);
// "auto"/"usbmmidd"/"vdd", normalized to lowercase.
bool parse_virtual_display_provider(const std::string& input, std::string& normalized);

// Service settings. All parsers trim their input and return false when the
// value is unusable; the caller decides whether to keep a default. An empty
// server URL is valid and means "not configured".
bool parse_server_url(const std::string& input, std::string& normalized);
bool parse_proxy_url(const std::string& input, std::string& normalized);
bool valid_server_key(const std::string& key);
bool parse_tunnel_id(const std::string& input, std::string& normalized);
bool valid_tunnel_key(const std::string& key);
bool parse_tunnel_listen(const std::string& input, std::string& normalized);
bool tunnel_listen_port(const std::string& listen, uint16_t& port);
// Spool next to a settings file: <dir>/<stem>.audit.jsonl. Empty for an empty
// settings path.
std::string audit_spool_path_for(const std::string& settings_path);
// Machine name normalized to the device-id alphabet, for a fresh host.
std::string generate_tunnel_id();

}  // namespace chast
