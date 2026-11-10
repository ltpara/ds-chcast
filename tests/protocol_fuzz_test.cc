// Deterministic mutation coverage for the wire parsers: no GPU, no network.
//
// Every control-message parser and every fixed-width stream header decoder is
// driven from a valid seed through truncations, bit flips, range erasures,
// insertions and random tails. A parser may reject any of these, but must not
// crash, over-allocate or loop. The same table also receives the fixed
// adversarial sizes that exercise the length-prefix checks directly.
//
// libFuzzer is not available with the MSVC toolchain this project builds with,
// so the driver is a self-contained xorshift loop. The seeds are real messages
// produced by the makers, which keeps the mutations inside the deep paths
// rather than only the first length check.

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "chast/protocol.h"

namespace {

int failures = 0;
// The parse lambdas share one error sink; every call overwrites it.
std::string parse_error;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

std::vector<uint8_t> payload_of(const std::vector<uint8_t>& message) {
    if (message.size() < chast::kControlHeaderSize) {
        return {};
    }
    return std::vector<uint8_t>(message.begin() + chast::kControlHeaderSize, message.end());
}

struct Rng {
    explicit Rng(uint64_t seed) : state(seed ? seed : 0x9e3779b97f4a7c15ull) {}

    uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545f4914f6cdd1dull;
    }

    size_t below(size_t bound) {
        return bound == 0 ? 0 : static_cast<size_t>(next() % bound);
    }

    uint8_t byte() { return static_cast<uint8_t>(next() >> 56); }

    uint64_t state;
};

struct Target {
    const char* name;
    std::vector<uint8_t> seed;
    bool seed_ok = true;
    std::function<bool(const std::vector<uint8_t>&)> parse;
};

Target message_target(const char* name, const std::vector<uint8_t>& message,
                      std::function<bool(const std::vector<uint8_t>&)> parse) {
    return Target{name, payload_of(message), message.size() >= chast::kControlHeaderSize,
                  std::move(parse)};
}

Target raw_target(const char* name, std::vector<uint8_t> seed,
                  std::function<bool(const std::vector<uint8_t>&)> parse) {
    const bool seed_ok = !seed.empty();
    return Target{name, std::move(seed), seed_ok, std::move(parse)};
}

chast::ServerInfo desktop_server_info() {
    chast::ServerInfo info;
    info.width = 1920;
    info.height = 1080;
    info.source_width = 1920;
    info.source_height = 1080;
    info.framerate = 60;
    info.bitrate = 8'000'000;
    info.input_enabled = true;
    info.encoder = "oneVPL";
    info.displays.push_back(chast::DisplayDescription{"Display 0", 0, 0, 1920, 1080, 0, true});
    info.audio.enabled = true;
    info.audio.codec = chast::AudioCodec::Opus;
    info.audio.sample_rate = 48000;
    info.audio.channels = 1;
    info.audio.frame_samples = chast::kOpusFrameSamples;
    info.audio.extradata.assign(19, 0);
    std::memcpy(info.audio.extradata.data(), "OpusHead", 8);
    info.audio.extradata[8] = 1;
    info.audio.extradata[9] = 1;
    info.audio.extradata[10] = 0x38;
    info.audio.extradata[11] = 0x01;
    info.audio.extradata[14] = 0x80;
    info.audio.extradata[15] = 0xBB;
    return info;
}

// All parser entry points, with a seed each. The seed is verified before the
// mutations start, so a maker that starts rejecting a default-constructed
// message fails here instead of silently shrinking the fuzzed surface.
std::vector<Target> make_targets() {
    std::vector<Target> targets;
    chast::CaptureDisplaysRequest capture;
    capture.request_id = 1;
    capture.add = {0, 1}; capture.sub = {1}; capture.set = {0};
    targets.push_back(message_target("capture_displays", chast::make_capture_displays(capture),
        [](const std::vector<uint8_t>& payload) {
            chast::CaptureDisplaysRequest value;
            return chast::parse_capture_displays(payload, value, &parse_error);
        }));
    chast::DisplayStreamInfo display{7, 0, desktop_server_info()};
    targets.push_back(message_target("display_stream_info", chast::make_display_stream_info(display),
        [](const std::vector<uint8_t>& payload) {
            chast::DisplayStreamInfo value;
            return chast::parse_display_stream_info(payload, value, &parse_error);
        }));
    chast::CaptureDisplaysResult capture_result;
    capture_result.request_id = 1;
    capture_result.streams.push_back(display);
    targets.push_back(message_target("capture_displays_result", chast::make_capture_displays_result(capture_result),
        [](const std::vector<uint8_t>& payload) {
            chast::CaptureDisplaysResult value;
            return chast::parse_capture_displays_result(payload, value, &parse_error);
        }));
    targets.push_back(message_target("display_keyframe_request", chast::make_display_keyframe_request(7),
        [](const std::vector<uint8_t>& payload) {
            uint32_t id = 0;
            return chast::parse_display_keyframe_request(payload, id, &parse_error);
        }));

    targets.push_back(message_target(
        "hello", chast::make_hello(chast::ClientHello{}),
        [](const std::vector<uint8_t>& payload) {
            chast::ClientHello hello;
            return chast::parse_hello(payload, hello, &parse_error);
        }));
    targets.push_back(message_target(
        "hello_ack", chast::make_hello_ack(desktop_server_info()),
        [](const std::vector<uint8_t>& payload) {
            chast::ServerInfo info;
            return chast::parse_hello_ack(payload, info, &parse_error);
        }));
    targets.push_back(message_target(
        "stream_info", chast::make_stream_info(desktop_server_info()),
        [](const std::vector<uint8_t>& payload) {
            chast::ServerInfo info;
            return chast::parse_hello_ack(payload, info, &parse_error);
        }));
    targets.push_back(message_target(
        "access_denied", chast::make_access_denied(chast::AccessDenial::Declined, "no"),
        [](const std::vector<uint8_t>& payload) {
            chast::AccessDenial code = chast::AccessDenial::Declined;
            std::string reason;
            return chast::parse_access_denied(payload, code, reason, &parse_error);
        }));
    targets.push_back(message_target(
        "stats", chast::make_stats(chast::ClientStats{}),
        [](const std::vector<uint8_t>& payload) {
            chast::ClientStats stats;
            return chast::parse_stats(payload, stats, &parse_error);
        }));
    targets.push_back(message_target(
        "latency_probe", chast::make_latency_probe(123456),
        [](const std::vector<uint8_t>& payload) {
            uint64_t client_us = 0;
            return chast::parse_latency_probe(payload, client_us, &parse_error);
        }));
    targets.push_back(message_target(
        "video_receipt", chast::make_video_receipt(0xfedcba9876543210ull),
        [](const std::vector<uint8_t>& payload) {
            uint64_t sequence = 0;
            return chast::parse_video_receipt(payload, sequence, &parse_error);
        }));
    targets.push_back(message_target(
        "latency_reply", chast::make_latency_reply(1, 2, 3),
        [](const std::vector<uint8_t>& payload) {
            uint64_t client_us = 0;
            uint64_t host_recv_us = 0;
            uint64_t host_send_us = 0;
            return chast::parse_latency_reply(payload, client_us, host_recv_us,
                                              host_send_us, &parse_error);
        }));
    targets.push_back(message_target(
        "input_event", chast::make_input_event(chast::InputEvent{}),
        [](const std::vector<uint8_t>& payload) {
            chast::InputEvent event;
            return chast::parse_input_event(payload, event, &parse_error);
        }));
    targets.push_back(message_target(
        "clipboard_update", chast::make_clipboard_update(7, "clipboard"),
        [](const std::vector<uint8_t>& payload) {
            chast::ClipboardUpdate update;
            return chast::parse_clipboard_update(payload, update, &parse_error);
        }));
    targets.push_back(message_target(
        "clipboard_file_read", chast::make_clipboard_file_read({1, 7, 2, 9, 4096}),
        [](const std::vector<uint8_t>& payload) {
            chast::ClipboardFileRead request;
            return chast::parse_clipboard_file_read(payload, request, &parse_error);
        }));
    targets.push_back(message_target(
        "clipboard_file_data", chast::make_clipboard_file_data({1, 7, true, {0, 1, 255}}),
        [](const std::vector<uint8_t>& payload) {
            chast::ClipboardFileData response;
            return chast::parse_clipboard_file_data(payload, response, &parse_error);
        }));
    targets.push_back(message_target(
        "chat_message", chast::make_chat_message("hello"),
        [](const std::vector<uint8_t>& payload) {
            std::string text;
            return chast::parse_chat_message(payload, text, &parse_error);
        }));
    targets.push_back(message_target(
        "text_input", chast::make_text_input("typed"),
        [](const std::vector<uint8_t>& payload) {
            std::string text;
            return chast::parse_text_input(payload, text, &parse_error);
        }));

    chast::PortForwardRequest forward;
    forward.forward_id = 1;
    forward.host = "127.0.0.1";
    forward.port = 4455;
    targets.push_back(message_target(
        "port_forward_request", chast::make_port_forward_request(forward),
        [](const std::vector<uint8_t>& payload) {
            chast::PortForwardRequest request;
            return chast::parse_port_forward_request(payload, request, &parse_error);
        }));
    chast::PortForwardResult forward_result;
    forward_result.forward_id = 1;
    targets.push_back(message_target(
        "port_forward_result", chast::make_port_forward_result(forward_result),
        [](const std::vector<uint8_t>& payload) {
            chast::PortForwardResult result;
            return chast::parse_port_forward_result(payload, result, &parse_error);
        }));
    chast::PortForwardStop forward_stop;
    forward_stop.forward_id = 1;
    targets.push_back(message_target(
        "port_forward_stop", chast::make_port_forward_stop(forward_stop),
        [](const std::vector<uint8_t>& payload) {
            chast::PortForwardStop stop;
            return chast::parse_port_forward_stop(payload, stop, &parse_error);
        }));

    chast::TerminalRequest terminal;
    terminal.terminal_id = 1;
    terminal.columns = 80;
    terminal.rows = 24;
    targets.push_back(message_target(
        "terminal_request", chast::make_terminal_request(terminal),
        [](const std::vector<uint8_t>& payload) {
            chast::TerminalRequest request;
            return chast::parse_terminal_request(payload, request, &parse_error);
        }));
    chast::TerminalResult terminal_result;
    terminal_result.terminal_id = 1;
    targets.push_back(message_target(
        "terminal_result", chast::make_terminal_result(terminal_result),
        [](const std::vector<uint8_t>& payload) {
            chast::TerminalResult result;
            return chast::parse_terminal_result(payload, result, &parse_error);
        }));
    targets.push_back(message_target(
        "elevation_request", chast::make_elevation_request(42),
        [](const std::vector<uint8_t>& payload) {
            uint32_t id = 0;
            return chast::parse_elevation_request(payload, id, &parse_error);
        }));
    targets.push_back(message_target(
        "elevation_state", chast::make_elevation_state({42, chast::ElevationCode::Failed, "cancelled"}),
        [](const std::vector<uint8_t>& payload) {
            chast::ElevationState state;
            return chast::parse_elevation_state(payload, state, &parse_error);
        }));
    chast::TerminalStop terminal_stop;
    terminal_stop.terminal_id = 1;
    targets.push_back(message_target(
        "terminal_stop", chast::make_terminal_stop(terminal_stop),
        [](const std::vector<uint8_t>& payload) {
            chast::TerminalStop stop;
            return chast::parse_terminal_stop(payload, stop, &parse_error);
        }));
    targets.push_back(message_target(
        "terminal_resize", chast::make_terminal_resize(1, 100, 30),
        [](const std::vector<uint8_t>& payload) {
            chast::TerminalSize size;
            return chast::parse_terminal_resize(payload, size, &parse_error);
        }));

    targets.push_back(message_target(
        "camera_list_request", chast::make_camera_list_request(),
        [](const std::vector<uint8_t>& payload) {
            return chast::parse_camera_list_request(payload, &parse_error);
        }));
    targets.push_back(message_target(
        "camera_list", chast::make_camera_list(chast::CameraList{}),
        [](const std::vector<uint8_t>& payload) {
            chast::CameraList list;
            return chast::parse_camera_list(payload, list, &parse_error);
        }));
    targets.push_back(message_target(
        "camera_start", chast::make_camera_start(1),
        [](const std::vector<uint8_t>& payload) {
            uint16_t camera_id = 0;
            return chast::parse_camera_start(payload, camera_id, &parse_error);
        }));
    chast::CameraResult camera_result;
    camera_result.camera_id = 1;
    targets.push_back(message_target(
        "camera_result", chast::make_camera_result(camera_result),
        [](const std::vector<uint8_t>& payload) {
            chast::CameraResult result;
            return chast::parse_camera_result(payload, result, &parse_error);
        }));
    targets.push_back(message_target(
        "camera_stop", chast::make_camera_stop(1),
        [](const std::vector<uint8_t>& payload) {
            uint16_t camera_id = 0;
            return chast::parse_camera_stop(payload, camera_id, &parse_error);
        }));

    chast::VoiceCallRequest voice_request;
    voice_request.request_id = 1;
    targets.push_back(message_target(
        "voice_call_request", chast::make_voice_call_request(voice_request),
        [](const std::vector<uint8_t>& payload) {
            chast::VoiceCallRequest request;
            return chast::parse_voice_call_request(payload, request, &parse_error);
        }));
    chast::VoiceCallResponse voice_response;
    voice_response.request_id = 1;
    voice_response.code = chast::VoiceCallCode::Refused;
    targets.push_back(message_target(
        "voice_call_response", chast::make_voice_call_response(voice_response),
        [](const std::vector<uint8_t>& payload) {
            chast::VoiceCallResponse response;
            return chast::parse_voice_call_response(payload, response, &parse_error);
        }));
    {
        chast::AudioHeader header;
        header.sample_rate = 48000;
        header.frame_samples = chast::kOpusFrameSamples;
        header.channels = 1;
        header.codec = chast::AudioCodec::Opus;
        const std::vector<uint8_t> packet(60, 0xA5);
        targets.push_back(message_target(
            "voice_audio",
            chast::make_voice_audio(header, packet.data(), packet.size()),
            [](const std::vector<uint8_t>& payload) {
                chast::AudioHeader parsed;
                const uint8_t* audio = nullptr;
                size_t audio_size = 0;
                return chast::parse_voice_audio(payload, parsed, &audio, audio_size, &parse_error);
            }));
    }

    targets.push_back(message_target(
        "host_action_request", chast::make_host_action_request(chast::HostControlRequest{}),
        [](const std::vector<uint8_t>& payload) {
            chast::HostControlRequest request;
            return chast::parse_host_action_request(payload, request, &parse_error);
        }));
    targets.push_back(message_target(
        "host_action_result", chast::make_host_action_result(chast::HostControlResult{}),
        [](const std::vector<uint8_t>& payload) {
            chast::HostControlResult result;
            return chast::parse_host_action_result(payload, result, &parse_error);
        }));
    targets.push_back(message_target(
        "os_login_request", chast::make_os_login_request("secret"),
        [](const std::vector<uint8_t>& payload) {
            std::string password;
            return chast::parse_os_login_request(payload, password, &parse_error);
        }));
    targets.push_back(message_target(
        "os_login_result", chast::make_os_login_result(chast::OsLoginCode::Ok),
        [](const std::vector<uint8_t>& payload) {
            chast::OsLoginCode code = chast::OsLoginCode::Ok;
            std::string reason;
            return chast::parse_os_login_result(payload, code, reason, &parse_error);
        }));

    targets.push_back(message_target(
        "session_list_request", chast::make_session_list_request(),
        [](const std::vector<uint8_t>& payload) {
            return chast::parse_session_list_request(payload, &parse_error);
        }));
    targets.push_back(message_target(
        "session_list_result", chast::make_session_list_result(chast::SessionListResult{}),
        [](const std::vector<uint8_t>& payload) {
            chast::SessionListResult result;
            return chast::parse_session_list_result(payload, result, &parse_error);
        }));
    targets.push_back(message_target(
        "session_select_request", chast::make_session_select_request(3),
        [](const std::vector<uint8_t>& payload) {
            uint32_t session_id = 0;
            return chast::parse_session_select_request(payload, session_id, &parse_error);
        }));
    chast::SessionSelectResult select_result;
    select_result.session_id = 3;
    targets.push_back(message_target(
        "session_select_result", chast::make_session_select_result(select_result),
        [](const std::vector<uint8_t>& payload) {
            chast::SessionSelectResult result;
            return chast::parse_session_select_result(payload, result, &parse_error);
        }));

    chast::CursorUpdate cursor;
    cursor.visible = true;
    cursor.x = 10;
    cursor.y = 20;
    cursor.shape_id = 17;
    cursor.width = cursor.height = 2;
    cursor.sprite.assign(16, 0x80);
    targets.push_back(message_target(
        "display_cursor_update", chast::make_display_cursor_update({0xffffffffu, cursor}),
        [](const std::vector<uint8_t>& payload) {
            chast::DisplayCursorUpdate update;
            return chast::parse_display_cursor_update(payload, update, &parse_error);
        }));
    targets.push_back(message_target(
        "cursor_update", chast::make_cursor_update(cursor),
        [](const std::vector<uint8_t>& payload) {
            chast::CursorUpdate update;
            return chast::parse_cursor_update(payload, update, &parse_error);
        }));
    targets.push_back(message_target(
        "permission_update", chast::make_permission_update(1),
        [](const std::vector<uint8_t>& payload) {
            chast::PermissionMask permissions = 0;
            return chast::parse_permission_update(payload, permissions, &parse_error);
        }));
    targets.push_back(message_target(
        "set_privacy_mode", chast::make_set_privacy_mode(true),
        [](const std::vector<uint8_t>& payload) {
            bool enabled = false;
            return chast::parse_set_privacy_mode(payload, enabled, &parse_error);
        }));
    targets.push_back(message_target(
        "privacy_mode_state",
        chast::make_privacy_mode_state(true, chast::PrivacyModeCode::Ok, {}),
        [](const std::vector<uint8_t>& payload) {
            bool enabled = false;
            chast::PrivacyModeCode code = chast::PrivacyModeCode::Ok;
            std::string reason;
            return chast::parse_privacy_mode_state(payload, enabled, code, reason, &parse_error);
        }));
    targets.push_back(message_target(
        "desktop_state", chast::make_desktop_state(chast::DesktopKind::Default),
        [](const std::vector<uint8_t>& payload) {
            chast::DesktopKind kind = chast::DesktopKind::Default;
            return chast::parse_desktop_state(payload, kind, &parse_error);
        }));
    targets.push_back(message_target(
        "send_sas", chast::make_send_sas(),
        [](const std::vector<uint8_t>& payload) {
            return chast::parse_send_sas(payload, &parse_error);
        }));
    targets.push_back(message_target(
        "sas_result", chast::make_sas_result(chast::SasCode::Ok, {}),
        [](const std::vector<uint8_t>& payload) {
            chast::SasCode code = chast::SasCode::Ok;
            std::string reason;
            return chast::parse_sas_result(payload, code, reason, &parse_error);
        }));

    targets.push_back(message_target(
        "set_stream_options", chast::make_set_stream_options(chast::StreamOptions{}),
        [](const std::vector<uint8_t>& payload) {
            chast::StreamOptions options;
            return chast::parse_set_stream_options(payload, options, &parse_error);
        }));
    targets.push_back(message_target(
        "stream_options_result", chast::make_stream_options_result(chast::StreamOptionsResult{}),
        [](const std::vector<uint8_t>& payload) {
            chast::StreamOptionsResult result;
            return chast::parse_stream_options_result(payload, result, &parse_error);
        }));
    targets.push_back(message_target(
        "switch_display", chast::make_switch_display(0),
        [](const std::vector<uint8_t>& payload) {
            uint16_t display_index = 0;
            return chast::parse_switch_display(payload, display_index, &parse_error);
        }));
    targets.push_back(message_target(
        "display_switch_result",
        chast::make_display_switch_result(0, chast::DisplaySwitchCode::Ok, {}),
        [](const std::vector<uint8_t>& payload) {
            uint16_t display_index = 0;
            chast::DisplaySwitchCode code = chast::DisplaySwitchCode::Ok;
            std::string reason;
            return chast::parse_display_switch_result(payload, display_index, code, reason,
                                                      &parse_error);
        }));
    targets.push_back(message_target(
        "set_display_follow", chast::make_set_display_follow(0),
        [](const std::vector<uint8_t>& payload) {
            uint8_t flags = 0;
            return chast::parse_set_display_follow(payload, flags, &parse_error);
        }));
    targets.push_back(message_target(
        "display_follow_state",
        chast::make_display_follow_state(chast::DisplayFollowState{}),
        [](const std::vector<uint8_t>& payload) {
            chast::DisplayFollowState state;
            return chast::parse_display_follow_state(payload, state, &parse_error);
        }));
    targets.push_back(message_target(
        "remote_config_request",
        chast::make_remote_config_request(chast::RemoteConfigRequest{}),
        [](const std::vector<uint8_t>& payload) {
            chast::RemoteConfigRequest request;
            return chast::parse_remote_config_request(payload, request, &parse_error);
        }));
    targets.push_back(message_target(
        "remote_config_state", chast::make_remote_config_state(chast::RemoteConfigState{}),
        [](const std::vector<uint8_t>& payload) {
            chast::RemoteConfigState state;
            return chast::parse_remote_config_state(payload, state, &parse_error);
        }));
    targets.push_back(message_target(
        "list_display_modes", chast::make_list_display_modes(0),
        [](const std::vector<uint8_t>& payload) {
            uint16_t display_index = 0;
            return chast::parse_list_display_modes(payload, display_index, &parse_error);
        }));
    {
        chast::DisplayModeList modes;
        modes.modes.push_back(chast::DisplayModeInfo{1920, 1080, 60, true});
        targets.push_back(message_target(
            "display_modes", chast::make_display_modes(modes),
            [](const std::vector<uint8_t>& payload) {
                chast::DisplayModeList list;
                return chast::parse_display_modes(payload, list, &parse_error);
            }));
    }
    targets.push_back(message_target(
        "set_display_mode", chast::make_set_display_mode(0, 1920, 1080, 60),
        [](const std::vector<uint8_t>& payload) {
            uint16_t display_index = 0;
            uint16_t width = 0;
            uint16_t height = 0;
            uint16_t refresh_hz = 0;
            return chast::parse_set_display_mode(payload, display_index, width, height,
                                                 refresh_hz, &parse_error);
        }));
    targets.push_back(message_target(
        "display_mode_result",
        chast::make_display_mode_result(0, 1920, 1080, 60, chast::DisplayModeCode::Ok, {}),
        [](const std::vector<uint8_t>& payload) {
            uint16_t display_index = 0;
            uint16_t width = 0;
            uint16_t height = 0;
            uint16_t refresh_hz = 0;
            chast::DisplayModeCode code = chast::DisplayModeCode::Ok;
            std::string reason;
            return chast::parse_display_mode_result(payload, display_index, width, height,
                                                    refresh_hz, code, reason, &parse_error);
        }));

    chast::FileOffer offer;
    offer.transfer_id = 1;
    offer.size = 12;
    offer.name = "a.txt";
    targets.push_back(message_target(
        "file_offer", chast::make_file_offer(offer),
        [](const std::vector<uint8_t>& payload) {
            chast::FileOffer parsed;
            return chast::parse_file_offer(payload, parsed, &parse_error);
        }));
    chast::FileAnswer answer;
    answer.transfer_id = 1;
    targets.push_back(message_target(
        "file_answer", chast::make_file_answer(answer),
        [](const std::vector<uint8_t>& payload) {
            chast::FileAnswer parsed;
            return chast::parse_file_answer(payload, parsed, &parse_error);
        }));
    chast::FileResult file_result;
    file_result.transfer_id = 1;
    targets.push_back(message_target(
        "file_result", chast::make_file_result(file_result),
        [](const std::vector<uint8_t>& payload) {
            chast::FileResult parsed;
            return chast::parse_file_result(payload, parsed, &parse_error);
        }));
    chast::FileCancel cancel;
    cancel.transfer_id = 1;
    targets.push_back(message_target(
        "file_cancel", chast::make_file_cancel(cancel),
        [](const std::vector<uint8_t>& payload) {
            chast::FileCancel parsed;
            return chast::parse_file_cancel(payload, parsed, &parse_error);
        }));
    targets.push_back(message_target(
        "file_list_request", chast::make_file_list_request(1, ""),
        [](const std::vector<uint8_t>& payload) {
            uint32_t request_id = 0;
            std::string directory;
            return chast::parse_file_list_request(payload, request_id, directory, &parse_error);
        }));
    {
        chast::FileListResult list;
        list.request_id = 1;
        list.entries.push_back(chast::FileListEntry{"a.txt", 1, 1, false});
        targets.push_back(message_target(
            "file_list_result", chast::make_file_list_result(list),
            [](const std::vector<uint8_t>& payload) {
                chast::FileListResult parsed;
                return chast::parse_file_list_result(payload, parsed, &parse_error);
            }));
    }
    targets.push_back(message_target(
        "file_download_request", chast::make_file_download_request(1, "a.txt"),
        [](const std::vector<uint8_t>& payload) {
            uint32_t request_id = 0;
            std::string path;
            return chast::parse_file_download_request(payload, request_id, path, &parse_error);
        }));
    chast::FileDownloadResult download;
    download.request_id = 1;
    download.transfer_id = 1;
    targets.push_back(message_target(
        "file_download_result", chast::make_file_download_result(download),
        [](const std::vector<uint8_t>& payload) {
            chast::FileDownloadResult parsed;
            return chast::parse_file_download_result(payload, parsed, &parse_error);
        }));
    targets.push_back(message_target(
        "file_op_request",
        chast::make_file_op_request(1, chast::FileOperation::MakeDirectory, "dir", {}),
        [](const std::vector<uint8_t>& payload) {
            uint32_t request_id = 0;
            chast::FileOperation op = chast::FileOperation::MakeDirectory;
            std::string path;
            std::string new_name;
            return chast::parse_file_op_request(payload, request_id, op, path, new_name,
                                                &parse_error);
        }));
    chast::FileOpResult op_result;
    op_result.request_id = 1;
    targets.push_back(message_target(
        "file_op_result", chast::make_file_op_result(op_result),
        [](const std::vector<uint8_t>& payload) {
            chast::FileOpResult parsed;
            return chast::parse_file_op_result(payload, parsed, &parse_error);
        }));

    {
        chast::FileStreamHeader header;
        header.transfer_id = 1;
        header.offset = 8;
        uint8_t wire[chast::kFileStreamHeaderSize]{};
        chast::encode_file_stream_header(header, wire);
        targets.push_back(raw_target(
            "file_stream_header",
            std::vector<uint8_t>(wire, wire + sizeof(wire)),
            [](const std::vector<uint8_t>& payload) {
                chast::FileStreamHeader parsed;
                return chast::decode_file_stream_header(payload.data(), payload.size(), parsed,
                                                        &parse_error);
            }));
    }
    {
        chast::FileBlockHeader block;
        block.stored_size = 700;
        block.raw_size = 1024;
        block.compressed = true;
        uint8_t wire[chast::kFileBlockHeaderSize]{};
        chast::encode_file_block_header(block, wire);
        targets.push_back(raw_target(
            "file_block_header", std::vector<uint8_t>(wire, wire + sizeof(wire)),
            [](const std::vector<uint8_t>& payload) {
                chast::FileBlockHeader parsed;
                return chast::decode_file_block_header(payload.data(), payload.size(), parsed,
                                                       &parse_error);
            }));
    }
    targets.push_back(raw_target(
        "port_forward_stream_header", chast::make_port_forward_stream_header(1, 2),
        [](const std::vector<uint8_t>& payload) {
            chast::PortForwardStreamHeader header;
            return chast::decode_port_forward_stream_header(payload.data(), payload.size(),
                                                            header, &parse_error);
        }));
    targets.push_back(raw_target(
        "terminal_stream_header", chast::make_terminal_stream_header(1),
        [](const std::vector<uint8_t>& payload) {
            chast::TerminalStreamHeader header;
            return chast::decode_terminal_stream_header(payload.data(), payload.size(),
                                                        header, &parse_error);
        }));
    targets.push_back(raw_target(
        "camera_stream_header", chast::make_camera_stream_header(1),
        [](const std::vector<uint8_t>& payload) {
            chast::CameraStreamHeader header;
            return chast::decode_camera_stream_header(payload.data(), payload.size(),
                                                      header, &parse_error);
        }));
    {
        chast::FrameHeader header;
        header.payload_size = 1;
        header.width = 64;
        header.height = 64;
        uint8_t wire[chast::kFrameHeaderSize]{};
        chast::encode_frame_header(header, wire);
        targets.push_back(raw_target(
            "frame_header", std::vector<uint8_t>(wire, wire + sizeof(wire)),
            [](const std::vector<uint8_t>& payload) {
                chast::FrameHeader parsed;
                return chast::decode_frame_header(payload.data(), payload.size(), parsed,
                                                  &parse_error);
            }));
    }
    {
        chast::AudioHeader header;
        header.payload_size = 1;
        header.sample_rate = 48000;
        header.frame_samples = chast::kOpusFrameSamples;
        header.channels = 1;
        uint8_t wire[chast::kAudioHeaderSize]{};
        chast::encode_audio_header(header, wire);
        targets.push_back(raw_target(
            "audio_header", std::vector<uint8_t>(wire, wire + sizeof(wire)),
            [](const std::vector<uint8_t>& payload) {
                chast::AudioHeader parsed;
                return chast::decode_audio_header(payload.data(), payload.size(), parsed,
                                                  &parse_error);
            }));
    }

    return targets;
}

std::vector<uint8_t> random_bytes(Rng& rng, size_t size) {
    std::vector<uint8_t> data(size);
    for (uint8_t& byte : data) {
        byte = rng.byte();
    }
    return data;
}

std::vector<uint8_t> mutate(const std::vector<uint8_t>& seed, Rng& rng) {
    constexpr size_t kMaxMutationBytes = 64u * 1024u;
    std::vector<uint8_t> data = seed;
    const int operations = 1 + static_cast<int>(rng.below(8));
    for (int i = 0; i < operations; ++i) {
        switch (rng.below(7)) {
            case 0: {  // truncate anywhere, including to zero
                data.resize(rng.below(data.size() + 1));
                break;
            }
            case 1: {  // flip one bit
                if (!data.empty()) {
                    data[rng.below(data.size())] ^= static_cast<uint8_t>(1u << rng.below(8));
                }
                break;
            }
            case 2: {  // replace one byte
                if (!data.empty()) {
                    data[rng.below(data.size())] = rng.byte();
                }
                break;
            }
            case 3: {  // erase a short range
                if (!data.empty()) {
                    const size_t at = rng.below(data.size());
                    const size_t room = std::min<size_t>(32, data.size() - at);
                    const size_t length = rng.below(room + 1);
                    data.erase(data.begin() + static_cast<ptrdiff_t>(at),
                               data.begin() + static_cast<ptrdiff_t>(at + length));
                }
                break;
            }
            case 4: {  // insert random bytes
                const size_t at = rng.below(data.size() + 1);
                const size_t length = 1 + rng.below(32);
                std::vector<uint8_t> inserted = random_bytes(rng, length);
                data.insert(data.begin() + static_cast<ptrdiff_t>(at), inserted.begin(),
                            inserted.end());
                break;
            }
            case 5: {  // duplicate a range
                if (!data.empty()) {
                    const size_t at = rng.below(data.size());
                    const size_t room = std::min<size_t>(32, data.size() - at);
                    const size_t length = 1 + rng.below(room);
                    std::vector<uint8_t> copy(data.begin() + static_cast<ptrdiff_t>(at),
                                              data.begin() +
                                                  static_cast<ptrdiff_t>(at + length));
                    data.insert(data.begin() + static_cast<ptrdiff_t>(at), copy.begin(),
                                copy.end());
                }
                break;
            }
            default: {  // append a random tail
                const size_t length = 1 + rng.below(256);
                std::vector<uint8_t> tail = random_bytes(rng, length);
                data.insert(data.end(), tail.begin(), tail.end());
                break;
            }
        }
        if (data.size() > kMaxMutationBytes) {
            data.resize(kMaxMutationBytes);
        }
    }
    return data;
}

void fuzz_stateless(const Target& target, Rng& rng) {
    if (!target.seed_ok) {
        std::printf("FAIL %s has no seed\n", target.name);
        ++failures;
        return;
    }
    if (!target.parse(target.seed)) {
        std::printf("FAIL %s seed is rejected\n", target.name);
        ++failures;
        return;
    }

    constexpr int kIterations = 4000;
    for (int i = 0; i < kIterations; ++i) {
        target.parse(mutate(target.seed, rng));
    }

    // Fixed sizes that land on header boundaries and the 1 MiB control cap.
    constexpr size_t kFixedSizes[] = {0,   1,    2,    3,    4,    7,    8,
                                      11,  12,   16,   19,   20,   32,   47,
                                      48,  49,   64,   255,  256,  1024, 4096,
                                      65535, 65536, 262144, 1024 * 1024};
    for (size_t size : kFixedSizes) {
        target.parse(random_bytes(rng, size));
    }
    std::printf("ok   %-26s %d mutations\n", target.name, kIterations);
}

void fuzz_framing(Rng& rng) {
    chast::ClientHello hello;
    hello.password = "pw";
    const std::vector<uint8_t> message =
        chast::make_control(chast::ControlType::Hello, payload_of(chast::make_hello(hello)));
    chast::FrameHeader frame_header;
    frame_header.payload_size = 32;
    frame_header.width = 64;
    frame_header.height = 64;
    uint8_t frame_wire[chast::kFrameHeaderSize + 32]{};
    chast::encode_frame_header(frame_header, frame_wire);
    chast::AudioHeader audio_header;
    audio_header.payload_size = 16;
    audio_header.sample_rate = 48000;
    audio_header.frame_samples = chast::kOpusFrameSamples;
    audio_header.channels = 1;
    uint8_t audio_wire[chast::kAudioHeaderSize + 16]{};
    chast::encode_audio_header(audio_header, audio_wire);

    constexpr int kIterations = 2000;
    for (int i = 0; i < kIterations; ++i) {
        {
            chast::ControlParser parser;
            std::vector<std::pair<chast::ControlType, std::vector<uint8_t>>> messages;
            std::string error;
            const std::vector<uint8_t> mutated = mutate(message, rng);
            parser.push(mutated.data(), mutated.size(), messages, &parse_error);
        }
        {
            chast::FrameParser parser;
            std::string error;
            const std::vector<uint8_t> mutated =
                mutate(std::vector<uint8_t>(frame_wire, frame_wire + sizeof(frame_wire)), rng);
            if (parser.push(mutated.data(), mutated.size(), &parse_error)) {
                chast::FrameHeader header;
                const uint8_t* payload = nullptr;
                while (parser.next(header, &payload, &parse_error) ==
                       chast::FrameParser::Status::Frame) {
                    parser.consume();
                }
            }
        }
        {
            chast::AudioParser parser;
            std::string error;
            const std::vector<uint8_t> mutated =
                mutate(std::vector<uint8_t>(audio_wire, audio_wire + sizeof(audio_wire)), rng);
            if (parser.push(mutated.data(), mutated.size(), &parse_error)) {
                chast::AudioHeader header;
                const uint8_t* payload = nullptr;
                while (parser.next(header, &payload, &parse_error) ==
                       chast::AudioParser::Status::Packet) {
                    parser.consume();
                }
            }
        }
    }
    std::printf("ok   %-26s %d mutations\n", "stream framing", kIterations);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Rng rng(0x5eed1234);
    std::vector<Target> targets = make_targets();
    for (const Target& target : targets) {
        fuzz_stateless(target, rng);
    }
    fuzz_framing(rng);
    if (failures != 0) {
        std::printf("protocol fuzz checks failed: %d\n", failures);
        return 1;
    }
    std::printf("protocol fuzz checks passed (%zu parser entry points)\n", targets.size());
    return 0;
}
