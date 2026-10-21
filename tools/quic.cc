// QUIC transport harness: capture -> encode -> MsQuic -> decode.
//
//   chast-quic serve   --port 4433 --codec h264 --seconds 10 [--display N] [--save sent.h264] [--input] [--input-dry-run]
//   chast-quic connect --host 127.0.0.1 --port 4433 [--display N] [--save received.h264] [--input]
//   chast-quic verify  --port 45433 --seconds 5 [--display N] [--input-real]
//
// serve runs the sender: one client connects, the actual encoder codec and
// picture size are announced, and every access unit is written to a
// server-initiated unidirectional stream. connect runs the receiver: it
// reassembles the frames, checks the per-frame CRC32, decodes each access unit
// on the hardware VPL decoder, and reports counts plus a digest of the decoded
// picture content.
//
// verify runs both ends in one process and closes the loop: it replays the
// stream the server actually sent through chast-decode's access-unit splitter
// and a fresh decoder, and requires the receiver's frame count and content
// digest to match. That is the repeatable proof that the bytes crossing QUIC
// decoded into the same pictures the sender encoded. No display is involved.
//
// With --input the serve end accepts InputEvents and injects them, connect
// replays a scripted pointer/key sequence (for real: the pointer moves and a
// Shift+A is typed), and verify also closes that loop: the events the server
// received must equal the ones the client sent, in order, and every one must
// have been injected. verify injects in dry-run mode unless --input-real is
// given, so a normal test run never touches the real pointer; keys are left
// out of the script in that mode, because a test must not type into whatever
// window happens to be focused.
//
// Hello carries a display index: connect --display N asks serve to capture the
// Nth output of the announced list, and serve creates its capturer only after
// Hello arrives so the choice can be honoured. Without --display the client
// leaves the choice to the server, whose own --display is the fallback
// (primary when unset). verify always asks for display 0 and checks that the
// announced origin and picture size are that display's rect.
//
// Access control: serve --token T requires Hello to carry T and spends it on
// the first accepted viewer unless --token-reuse is given; --token auto
// generates and prints one. --approve asks on stdin before letting a viewer
// in. connect --token sends the token, and a refusal prints the server's
// reason. verify exercises all three refusal paths on throwaway servers.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "annexb.h"
#include "chast/capture.h"
#include "chast/decoder.h"
#include "chast/encoder.h"
#include "chast/host_server.h"
#include "chast/identity.h"
#include "chast/protocol.h"
#include "chast/quic.h"
#include "crash_handler.h"

namespace {

struct Options {
    std::string mode;
    std::string bind = "127.0.0.1";
    std::string host = "127.0.0.1";
    uint16_t port = 4433;
    // serve: fallback output when the client does not ask for one.
    // connect: the output to ask the server for.
    size_t display = 0;
    bool display_set = false;
    chast::VideoCodec codec = chast::VideoCodec::H264;
    UINT bitrate_kbps = 0;
    UINT framerate = 30;
    float quality = 0.67f;
    UINT keyframe_ms = 0;
    // Receiver-side: ask for an IDR this often, exercising the control stream
    // and the sender's key-frame-on-request path.
    UINT keyframe_request_ms = 0;
    // serve: accept and inject InputEvents. connect: send the scripted input
    // sequence once the handshake reports input enabled. verify: the same,
    // and check that the server observed exactly the events the client sent.
    bool input = false;
    // serve only: accept input but stop at the OS boundary. Lets the Flutter
    // viewer exercise its full send path against a server on the same machine
    // without the injected events looping back into the viewer's own window.
    bool input_dry_run = false;
    // serve: print every InputEvent the server observes, for diagnosing
    // the viewer's mapping.
    bool dump_input = false;
    // serve and verify: send a StreamInfo with the current description once,
    // two seconds in. serve uses it to exercise a viewer's update path without
    // changing the host's display mode; verify turns it on to cover the wire
    // and session layers.
    bool reannounce = false;
    // verify only: really call SendInput instead of stopping at the OS
    // boundary. Keys are left out of the script in that mode, because a test
    // run must not type into whatever window happens to be focused; the key
    // path is covered by the dry run and the injector's own checks.
    bool input_real = false;
    // connect/verify: SHA-256 fingerprint to pin. verify fills it from the
    // server it starts when the option is absent.
    std::string fingerprint;
    // connect: accept any certificate. Development only, and never combined
    // with a pin.
    bool insecure = false;
    // serve: Hello must carry this token. connect: the token to send. verify:
    // shared by the local server and the receiver.
    std::string token;
    // serve --token auto: generate a token and print it. Cleared once the
    // token is resolved, so later code only sees the value.
    bool token_auto = false;
    // serve: let the same token serve later sessions instead of spending it
    // on the first accepted viewer.
    bool token_reuse = false;
    // serve: ask on stdin before letting a viewer in.
    bool approve = false;
    double seconds = 0.0;
    bool seconds_set = false;
    double wait_seconds = 30.0;
    std::string save;
    chast::CaptureConfig config;
    bool quiet = false;
};

bool parse_args(int argc, char** argv, Options& options) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: chast-quic serve|connect|verify [options]\n");
        return false;
    }
    options.mode = argv[1];
    if (options.mode != "serve" && options.mode != "connect" && options.mode != "verify") {
        std::fprintf(stderr, "unknown mode: %s (want serve, connect or verify)\n",
                     options.mode.c_str());
        return false;
    }
    try {
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--port" && i + 1 < argc) {
                options.port = static_cast<uint16_t>(std::stoul(argv[++i]));
            } else if (arg == "--bind" && i + 1 < argc) {
                options.bind = argv[++i];
            } else if (arg == "--host" && i + 1 < argc) {
                options.host = argv[++i];
            } else if (arg == "--display" && i + 1 < argc) {
                const unsigned long value = std::stoul(argv[++i]);
                if (value >= chast::kMaxDisplayCount) {
                    std::fprintf(stderr, "display index %lu is past the %u-display limit\n",
                                 value, static_cast<unsigned>(chast::kMaxDisplayCount));
                    return false;
                }
                options.display = static_cast<size_t>(value);
                options.display_set = true;
            } else if (arg == "--codec" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value == "h264" || value == "avc") {
                    options.codec = chast::VideoCodec::H264;
                } else if (value == "hevc" || value == "h265") {
                    options.codec = chast::VideoCodec::Hevc;
                } else {
                    std::fprintf(stderr, "unknown codec: %s\n", value.c_str());
                    return false;
                }
            } else if (arg == "--bitrate" && i + 1 < argc) {
                options.bitrate_kbps = static_cast<UINT>(std::stoul(argv[++i]));
            } else if (arg == "--framerate" && i + 1 < argc) {
                options.framerate = static_cast<UINT>(std::stoul(argv[++i]));
            } else if (arg == "--quality" && i + 1 < argc) {
                options.quality = std::stof(argv[++i]);
            } else if (arg == "--keyframe-interval" && i + 1 < argc) {
                options.keyframe_ms = static_cast<UINT>(std::stoul(argv[++i]));
            } else if (arg == "--request-keyframe-ms" && i + 1 < argc) {
                options.keyframe_request_ms = static_cast<UINT>(std::stoul(argv[++i]));
            } else if (arg == "--seconds" && i + 1 < argc) {
                options.seconds = std::stod(argv[++i]);
                options.seconds_set = true;
            } else if (arg == "--wait" && i + 1 < argc) {
                options.wait_seconds = std::stod(argv[++i]);
            } else if (arg == "--save" && i + 1 < argc) {
                options.save = argv[++i];
            } else if (arg == "--input") {
                options.input = true;
            } else if (arg == "--input-dry-run") {
                options.input = true;
                options.input_dry_run = true;
            } else if (arg == "--dump-input") {
                options.dump_input = true;
            } else if (arg == "--reannounce") {
                options.reannounce = true;
            } else if (arg == "--input-real") {
                options.input = true;
                options.input_real = true;
            } else if (arg == "--fingerprint" && i + 1 < argc) {
                options.fingerprint = argv[++i];
            } else if (arg == "--insecure") {
                options.insecure = true;
            } else if (arg == "--token" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value == "auto") {
                    options.token_auto = true;
                } else {
                    options.token = value;
                }
            } else if (arg == "--token-reuse") {
                options.token_reuse = true;
            } else if (arg == "--approve") {
                options.approve = true;
            } else if (arg == "--no-hdr") {
                options.config.want_hdr = false;
            } else if (arg == "--no-cursor") {
                options.config.capture_cursor = false;
            } else if (arg == "--quiet") {
                options.quiet = true;
            } else {
                std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
                return false;
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "invalid argument value: %s\n", e.what());
        return false;
    }
    // The hardware encoder consumes NV12 and nothing else.
    options.config.nv12.enabled = true;
    options.config.nv12.colorimetry = chast::Nv12Colorimetry::Bt601Limited;
    if (!options.seconds_set) {
        // serve and verify need an end; connect follows the server's FIN.
        options.seconds = options.mode == "connect" ? 0.0 : 5.0;
    }
    if (options.token.size() > chast::kMaxAccessTokenLength) {
        std::fprintf(stderr, "--token is longer than %u bytes\n",
                     static_cast<unsigned>(chast::kMaxAccessTokenLength));
        return false;
    }
    if ((options.token_reuse || options.approve) && options.mode != "serve") {
        std::fprintf(stderr, "--token-reuse and --approve only apply to serve\n");
        return false;
    }
    return true;
}

// Eight characters from a 32-symbol alphabet: about 40 bits, which is far
// beyond online guessing for a single one-time token, and short enough to read
// aloud or type on a phone.
std::string generate_token() {
    static constexpr char kAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    std::random_device random;
    std::string token;
    token.reserve(8);
    for (int i = 0; i < 8; ++i) {
        token.push_back(kAlphabet[random() % (sizeof(kAlphabet) - 1)]);
    }
    return token;
}

struct ServerReady {
    bool ok = false;
    std::string fingerprint;
};

int64_t steady_clock_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct SenderStats {
    uint64_t frames = 0;
    uint64_t encoded = 0;
    uint64_t keyframes = 0;
    uint64_t bytes = 0;
    uint64_t pointer_only = 0;
    uint64_t repeats = 0;
    uint64_t send_errors = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    chast::VideoCodec codec = chast::VideoCodec::H264;
    std::string encoder;
    std::string save_path;
    std::string error;
    bool client_connected = false;
    bool peer_left = false;
    bool stats_received = false;
    chast::ClientStats peer;
    // Input events the server observed, in order, plus the injector's
    // counters. Empty unless --input was given.
    std::vector<chast::InputEvent> input_events;
    chast::InputStats input_stats;
    uint64_t input_releases = 0;
    bool input_enabled = false;
    std::vector<chast::DisplayDescription> displays;
    // What Hello asked for (kDisplayAuto when the client left it to the
    // server) and the index the capturer actually used.
    uint16_t requested_display = chast::kDisplayAuto;
    size_t display_index = 0;
    // StreamInfo messages this server sent.
    uint64_t stream_updates_sent = 0;
    // Adaptive bitrate telemetry: the encoder's current target (the setting is
    // the ceiling), how often the controller changed it, and the transport RTT
    // the controller sampled.
    uint32_t target_bitrate = 0;
    uint64_t abr_changes = 0;
    double rtt_ms = 0.0;
    double send_kbps = 0.0;
    // SHA-256 identity the server presents (uppercase hex).
    std::string fingerprint;
    bool ok = false;
};

struct ReceiverStats {
    uint64_t received = 0;
    uint64_t decoded = 0;
    uint64_t keyframes = 0;
    uint64_t bytes = 0;
    uint64_t crc_errors = 0;
    uint64_t decode_errors = 0;
    uint64_t gaps = 0;
    uint64_t protocol_errors = 0;
    uint64_t digest = chast::kFrameDigestSeed;
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    chast::VideoCodec codec = chast::VideoCodec::H264;
    std::string decoder;
    std::string error;
    bool ok = false;
    // Events this receiver sent, and whether the HelloAck allowed them.
    std::vector<chast::InputEvent> input_sent;
    bool input_enabled = false;
    uint64_t input_errors = 0;
    std::vector<chast::DisplayDescription> displays;
    // StreamInfo messages received, and the description from the last one.
    uint64_t stream_updates = 0;
    chast::ServerInfo last_update;
    // Latency probes: the last clock-sync sample plus counters, and the frame
    // age (host capture -> local receipt) of the newest and worst frame seen.
    chast::LatencySample latency;
    uint64_t probes_sent = 0;
    uint64_t probe_errors = 0;
    uint64_t frames_without_capture = 0;
    double last_frame_age_ms = -1.0;
    double max_frame_age_ms = -1.0;
    // Bitrate the last StreamInfo announced (the encoder's target after ABR).
    uint32_t target_bitrate = 0;
    // Fingerprint of the certificate the server presented.
    std::string peer_fingerprint;
};

// The input script connect and verify replay. It exercises every action the
// protocol carries: an absolute move, a click, both wheel axes, a relative
// move, and a chord that leaves a modifier down across other events.
std::vector<chast::InputEvent> input_script(int32_t origin_x, int32_t origin_y, bool keys) {
    std::vector<chast::InputEvent> events;
    auto mouse = [&](chast::MouseAction action, uint16_t buttons, int32_t x, int32_t y) {
        chast::InputEvent event;
        event.kind = chast::InputKind::Mouse;
        event.mouse.action = action;
        event.mouse.buttons = buttons;
        event.mouse.x = x;
        event.mouse.y = y;
        events.push_back(event);
    };
    auto key = [&](uint16_t usage, chast::KeyAction action, uint16_t modifiers) {
        chast::InputEvent event;
        event.kind = chast::InputKind::Key;
        event.key.hid_usage = usage;
        event.key.action = action;
        event.modifiers = modifiers;
        events.push_back(event);
    };

    mouse(chast::MouseAction::Move, 0, origin_x + 10, origin_y + 10);
    mouse(chast::MouseAction::ButtonDown, chast::kMouseLeft, 0, 0);
    mouse(chast::MouseAction::ButtonUp, chast::kMouseLeft, 0, 0);
    mouse(chast::MouseAction::Wheel, 0, 0, -WHEEL_DELTA);
    mouse(chast::MouseAction::WheelHorizontal, 0, WHEEL_DELTA, 0);
    mouse(chast::MouseAction::MoveRelative, 0, 5, -3);
    if (keys) {
        key(0xE1, chast::KeyAction::Down, chast::kModifierLeftShift);  // Left Shift
        key(0x04, chast::KeyAction::Down, chast::kModifierLeftShift);  // A
        key(0x04, chast::KeyAction::Up, chast::kModifierLeftShift);
        key(0xE1, chast::KeyAction::Up, 0);
    }
    return events;
}

bool same_event(const chast::InputEvent& a, const chast::InputEvent& b) {
    if (a.kind != b.kind || a.modifiers != b.modifiers) {
        return false;
    }
    if (a.kind == chast::InputKind::Mouse) {
        return a.mouse.action == b.mouse.action && a.mouse.buttons == b.mouse.buttons &&
               a.mouse.x == b.mouse.x && a.mouse.y == b.mouse.y;
    }
    return a.key.action == b.key.action && a.key.hid_usage == b.key.hid_usage;
}

bool same_display(const chast::DisplayDescription& a, const chast::DisplayDescription& b) {
    return a.name == b.name && a.left == b.left && a.top == b.top && a.right == b.right &&
           a.bottom == b.bottom && a.rotation_degrees == b.rotation_degrees &&
           a.primary == b.primary;
}

// One host run over HostServer: the tool supplies the policy (display
// fallback, save sink, approval prompt, run length) and keeps its own stats
// view for the checks.
SenderStats run_sender(const Options& options, const std::string& save_path,
                       std::promise<ServerReady>* listening) {
    SenderStats stats;
    stats.save_path = save_path;
    bool ready_published = false;
    auto publish_ready = [&](bool value) {
        if (listening && !ready_published) {
            ready_published = true;
            ServerReady ready;
            ready.ok = value;
            ready.fingerprint = value ? stats.fingerprint : std::string();
            listening->set_value(std::move(ready));
        }
    };
    auto fail = [&](std::string message) {
        stats.error = std::move(message);
        stats.ok = false;
        publish_ready(false);
        return stats;
    };

    struct FileCloser {
        void operator()(FILE* file) const {
            if (file) {
                std::fclose(file);
            }
        }
    };
    std::unique_ptr<FILE, FileCloser> sink;
    if (!save_path.empty()) {
        FILE* raw = nullptr;
        if (fopen_s(&raw, save_path.c_str(), "wb") != 0 || !raw) {
            return fail("could not open " + save_path);
        }
        sink.reset(raw);
    }

    chast::HostServerConfig config;
    config.settings.bind_address = options.bind;
    config.settings.port = options.port;
    config.settings.allow_input = options.input;
    config.settings.token_mode = options.token_reuse ? chast::HostTokenMode::Reusable
                                                     : chast::HostTokenMode::OneTime;
    config.settings.access_token = options.token;
    config.settings.codec = options.codec;
    config.settings.bitrate_kbps = options.bitrate_kbps;
    config.settings.framerate = static_cast<uint16_t>(options.framerate);
    config.settings.quality = options.quality;
    if (options.display_set) {
        config.settings.display = static_cast<uint16_t>(options.display);
    }
    config.dry_run_input =
        options.input_dry_run || (options.input && options.mode == "verify" && !options.input_real);
    config.want_hdr = options.config.want_hdr;
    config.capture_cursor = options.config.capture_cursor;
    config.serve_once = true;
    config.run_seconds = options.seconds;
    config.reannounce = options.reannounce;
    config.keyframe_interval_ms = options.keyframe_ms;
    config.first_client_timeout_ms = static_cast<uint32_t>(options.wait_seconds * 1000.0);
    if (!options.quiet) {
        config.log = [](const std::string& message) {
            std::printf("quic: %s\n", message.c_str());
        };
    }
    if (options.approve) {
        // Runs on the host thread while wait_for_client() is blocked, so
        // reading stdin here cannot stall the transport's own workers.
        config.approve_client = [](const chast::ClientRequest& request) {
            const std::string display =
                request.display == chast::kDisplayAuto ? "auto"
                                                       : std::to_string(request.display);
            std::printf("quic: %s asks to connect (display %s); allow? [y/N] ",
                        request.peer_address.empty() ? "a client" : request.peer_address.c_str(),
                        display.c_str());
            std::fflush(stdout);
            char line[16]{};
            if (!std::fgets(line, sizeof(line), stdin)) {
                std::printf("\n");
                return false;
            }
            return line[0] == 'y' || line[0] == 'Y';
        };
    }
    if (sink) {
        config.packet = [&sink](const chast::FrameHeader&, const uint8_t* data, size_t size) {
            std::fwrite(data, 1, size, sink.get());
        };
    }
    config.input_event = [&stats, &options](const chast::InputEvent& event) {
        stats.input_events.push_back(event);
        if (!options.dump_input) {
            return;
        }
        if (event.kind == chast::InputKind::Mouse) {
            std::printf("input: mouse action=%u buttons=%u x=%d y=%d state=0x%03X\n",
                        static_cast<unsigned>(event.mouse.action), event.mouse.buttons,
                        event.mouse.x, event.mouse.y, event.modifiers);
        } else {
            std::printf("input: key action=%u usage=0x%02X state=0x%03X\n",
                        static_cast<unsigned>(event.key.action), event.key.hid_usage,
                        event.modifiers);
        }
    };

    std::string error;
    std::unique_ptr<chast::HostServer> server =
        chast::HostServer::create(std::move(config), &error);
    if (!server) {
        return fail("QUIC server: " + error);
    }
    const chast::HostStatus initial = server->status();
    stats.fingerprint = initial.fingerprint;
    publish_ready(true);
    if (!options.quiet) {
        std::printf("quic: identity SHA256 %s\n",
                    chast::format_fingerprint(stats.fingerprint).c_str());
        if (!initial.token.empty()) {
            std::printf("quic: access token %s (%s)\n", initial.token.c_str(),
                        options.token_reuse ? "reusable" : "one-time");
        }
        if (options.approve) {
            std::printf("quic: every viewer will need operator approval\n");
        }
    }

    server->start();
    server->wait();
    const chast::HostStatus status = server->status();

    stats.client_connected = status.stats.client_connected;
    stats.requested_display = status.requested_display;
    stats.display_index = status.display_index;
    stats.codec = status.stream.codec;
    stats.width = status.stream.width;
    stats.height = status.stream.height;
    stats.origin_x = status.stream.origin_x;
    stats.origin_y = status.stream.origin_y;
    stats.encoder = status.stream.encoder;
    stats.displays = status.stream.displays;
    stats.input_enabled = options.input;
    stats.input_stats = status.stats.input_stats;
    stats.input_releases = status.stats.input_releases;
    stats.stats_received = status.stats.stats_received;
    stats.peer = status.stats.peer;
    stats.peer_left = status.stats.peer_left;
    stats.stream_updates_sent = status.stats.stream_updates;
    stats.frames = status.stats.frames;
    stats.encoded = status.stats.encoded;
    stats.keyframes = status.stats.keyframes;
    stats.bytes = status.stats.bytes;
    stats.pointer_only = status.stats.pointer_only;
    stats.repeats = status.stats.repeats;
    stats.send_errors = status.stats.send_errors;
    stats.target_bitrate = status.stats.target_bitrate;
    stats.abr_changes = status.stats.abr_changes;
    stats.rtt_ms = status.stats.rtt_ms;
    stats.send_kbps = status.stats.send_kbps;
    stats.error = status.error;

    if (!options.quiet || !stats.error.empty()) {
        std::printf("sender: frames=%llu encoded=%llu keyframes=%llu pointer-only=%llu "
                    "repeats=%llu bytes=%llu send-errors=%llu stream-updates=%llu\n",
                    static_cast<unsigned long long>(stats.frames),
                    static_cast<unsigned long long>(stats.encoded),
                    static_cast<unsigned long long>(stats.keyframes),
                    static_cast<unsigned long long>(stats.pointer_only),
                    static_cast<unsigned long long>(stats.repeats),
                    static_cast<unsigned long long>(stats.bytes),
                    static_cast<unsigned long long>(stats.send_errors),
                    static_cast<unsigned long long>(stats.stream_updates_sent));
        std::printf("sender: target-bitrate=%u kbps abr-changes=%llu rtt=%.1f ms "
                    "send-rate=%.1f kbps\n",
                    stats.target_bitrate / 1000u,
                    static_cast<unsigned long long>(stats.abr_changes), stats.rtt_ms,
                    stats.send_kbps);
        if (stats.stats_received) {
            std::printf("sender: peer received=%llu decoded=%llu digest=%016llX\n",
                        static_cast<unsigned long long>(stats.peer.received),
                        static_cast<unsigned long long>(stats.peer.decoded),
                        static_cast<unsigned long long>(stats.peer.digest));
        }
        if (options.input) {
            std::printf("sender: input received=%llu applied=%llu rejected=%llu "
                        "failed=%llu dropped=%llu releases=%llu\n",
                        static_cast<unsigned long long>(stats.input_events.size()),
                        static_cast<unsigned long long>(stats.input_stats.applied),
                        static_cast<unsigned long long>(stats.input_stats.rejected),
                        static_cast<unsigned long long>(stats.input_stats.failed),
                        static_cast<unsigned long long>(stats.input_stats.dropped),
                        static_cast<unsigned long long>(stats.input_releases));
        }
    }
    stats.ok = stats.error.empty();
    return stats;
}

ReceiverStats run_receiver(const Options& options, std::shared_future<ServerReady> server_ready) {
    ReceiverStats stats;
    auto fail = [&](std::string message) {
        stats.error = std::move(message);
        stats.ok = false;
        return stats;
    };

    ServerReady ready;
    if (server_ready.valid()) {
        if (server_ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            return fail("the local server did not start");
        }
        ready = server_ready.get();
        if (!ready.ok) {
            return fail("the local server did not start");
        }
    }

    std::string pinned = chast::normalize_fingerprint(options.fingerprint);
    if (!options.fingerprint.empty() && pinned.empty()) {
        return fail("--fingerprint is not a SHA-256 fingerprint");
    }
    if (pinned.empty()) {
        // verify starts the server itself, so it can take the identity from
        // there instead of asking the caller to copy it.
        pinned = ready.fingerprint;
    }
    if (pinned.empty() && !options.insecure) {
        return fail("no pinned server identity: pass --fingerprint <SHA256> "
                    "(printed by serve), or --insecure for local development");
    }

    chast::QuicClientConfig client_config;
    client_config.host = options.host;
    client_config.port = options.port;
    client_config.pinned_fingerprint = pinned;
    client_config.allow_any_certificate = pinned.empty() && options.insecure;
    client_config.access_token = options.token;
    std::string error;
    std::unique_ptr<chast::QuicSession> session =
        chast::QuicSession::create_client(client_config, &error);
    if (!session) {
        return fail("QUIC client: " + error);
    }
    chast::Error connected = session->connect(std::chrono::seconds(30));
    if (!connected.ok()) {
        return fail("connect: " + connected.to_string());
    }
    stats.peer_fingerprint = session->peer_fingerprint();
    if (!options.quiet) {
        std::printf("quic: server identity SHA256 %s%s\n",
                    chast::format_fingerprint(stats.peer_fingerprint).c_str(),
                    pinned.empty() ? " (unverified)" : " (pinned)");
    }

    chast::ServerInfo info;
    const uint16_t requested_display =
        options.display_set ? static_cast<uint16_t>(options.display) : chast::kDisplayAuto;
    chast::Error handshaken =
        session->handshake(info, std::chrono::seconds(15), requested_display);
    if (!handshaken.ok()) {
        return fail("handshake: " + handshaken.to_string());
    }
    stats.codec = info.codec;
    stats.width = info.width;
    stats.height = info.height;
    stats.origin_x = info.origin_x;
    stats.origin_y = info.origin_y;
    stats.input_enabled = info.input_enabled;
    stats.displays = info.displays;
    stats.target_bitrate = info.bitrate;
    if (!options.quiet) {
        std::printf("quic: connected, server streams %s %ux%u at %u kbps (%s)\n",
                    chast_tools::codec_name(info.codec), info.width, info.height,
                    info.bitrate / 1000u, info.encoder.c_str());
        if (!options.token.empty()) {
            std::printf("quic: access token accepted\n");
        }
        std::printf("quic: server has %zu display%s\n", info.displays.size(),
                    info.displays.size() == 1 ? "" : "s");
        for (size_t i = 0; i < info.displays.size(); ++i) {
            const chast::DisplayDescription& display = info.displays[i];
            std::printf("quic:   [%zu] %s %dx%d at %d,%d rotation %u%s\n", i,
                        display.name.empty() ? "(unnamed)" : display.name.c_str(),
                        display.right - display.left, display.bottom - display.top,
                        display.left, display.top,
                        static_cast<unsigned>(display.rotation_degrees),
                        display.primary ? " primary" : "");
        }
        if (options.display_set) {
            std::printf("quic: asked the server for display %zu\n", options.display);
        } else {
            std::printf("quic: let the server choose the display\n");
        }
        if (options.input) {
            std::printf("quic: server input %s (origin %d,%d)\n",
                        info.input_enabled ? "enabled" : "disabled", info.origin_x,
                        info.origin_y);
        }
    }
    if (options.input) {
        if (!info.input_enabled) {
            return fail("the server refused input (it was not started with --input)");
        }
        // Sent before the frame loop: the server injects on its own thread, so
        // these land while the stream is already running and the check does
        // not depend on frame timing.
        stats.input_sent = input_script(info.origin_x, info.origin_y, !options.input_real);
        for (const chast::InputEvent& event : stats.input_sent) {
            if (chast::Error sent = session->send_input(event); !sent.ok()) {
                ++stats.input_errors;
                if (stats.input_errors == 1) {
                    std::fprintf(stderr, "send_input: %s\n", sent.to_string().c_str());
                }
            }
        }
        // The viewer does this whenever it gives up control; here it also
        // proves the release message travels and reaches the injector.
        if (chast::Error released = session->release_input(); !released.ok()) {
            ++stats.input_errors;
            std::fprintf(stderr, "release_input: %s\n", released.to_string().c_str());
        }
    }

    chast::DecoderConfig decoder_config;
    decoder_config.codec = info.codec;
    std::unique_ptr<chast::IVideoDecoder> decoder =
        chast::create_decoder(decoder_config, &error);
    if (!decoder) {
        return fail("decoder creation failed: " + error);
    }
    stats.decoder = decoder->implementation();

    FILE* sink = nullptr;
    if (!options.save.empty() &&
        (fopen_s(&sink, options.save.c_str(), "wb") != 0 || !sink)) {
        return fail("could not open " + options.save);
    }

    const auto start = std::chrono::steady_clock::now();
    // A finite window bounds a stalled peer. The extra 20 s covers the
    // sender's own duration and the closing Stats/Bye exchange, so the normal
    // end is always the server's FIN, not this deadline.
    const double window = options.seconds > 0.0 ? options.seconds + 20.0 : 0.0;
    const auto deadline =
        window > 0.0
            ? start + std::chrono::milliseconds(static_cast<long long>(window * 1000.0))
            : (std::chrono::steady_clock::time_point::max)();
    auto last_frame = start;
    auto last_keyframe_request = start - std::chrono::seconds(10);
    auto next_keyframe_request =
        start + std::chrono::milliseconds(options.keyframe_request_ms);
    uint64_t expected_seq = 0;
    std::vector<chast::DecodedFrame> frames;
    bool saw_video_end = false;
    auto last_probe = start - std::chrono::seconds(1);
    bool clock_synced = false;
    int64_t clock_offset_us = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        {
            const auto probe_now = std::chrono::steady_clock::now();
            if (probe_now - last_probe >= std::chrono::seconds(1)) {
                last_probe = probe_now;
                ++stats.probes_sent;
                if (chast::Error probed = session->send_latency_probe(); !probed.ok()) {
                    ++stats.probe_errors;
                    if (stats.probe_errors == 1) {
                        std::fprintf(stderr, "latency probe: %s\n",
                                     probed.to_string().c_str());
                    }
                } else {
                    stats.latency = session->latency();
                    if (stats.latency.valid) {
                        clock_synced = true;
                        clock_offset_us = stats.latency.offset_us;
                    }
                }
            }
        }
        chast::FrameHeader header;
        std::vector<uint8_t> payload;
        chast::Error received =
            session->receive_frame(header, payload, std::chrono::milliseconds(2000));
        if (received.status() == chast::Status::WaitTimeout) {
            if (session->async_error().ok() && !session->video_finished() &&
                std::chrono::steady_clock::now() - last_frame < std::chrono::seconds(30)) {
                continue;
            }
            saw_video_end = session->video_finished();
            if (!saw_video_end) {
                const chast::Error async = session->async_error();
                stats.error = async.ok() ? "no frames arrived for 30 s" : async.to_string();
            }
            break;
        }
        if (received.status() == chast::Status::Unavailable) {
            saw_video_end = session->video_finished();
            if (!saw_video_end) {
                const chast::Error async = session->async_error();
                stats.error = async.ok() ? "the connection closed before the video stream "
                                           "ended"
                                         : async.to_string();
            }
            break;
        }
        if (!received.ok()) {
            return fail("receive: " + received.to_string());
        }

        ++stats.received;
        last_frame = std::chrono::steady_clock::now();

        if (header.capture_ms == 0) {
            ++stats.frames_without_capture;
        } else if (clock_synced) {
            const int64_t age_us = steady_clock_us() + clock_offset_us -
                                   static_cast<int64_t>(header.capture_ms) * 1000;
            // A wild value means the offset does not belong to this clock pair
            // (or the frame is a replay); do not fold it into the reported age.
            if (age_us > -100000 && age_us < 10000000) {
                stats.last_frame_age_ms =
                    static_cast<double>(std::max<int64_t>(age_us, 0)) / 1000.0;
                stats.max_frame_age_ms =
                    (std::max)(stats.max_frame_age_ms, stats.last_frame_age_ms);
            }
        }

        chast::ServerInfo update;
        while (session->take_stream_update(update)) {
            ++stats.stream_updates;
            const bool reconfigure = update.codec != info.codec ||
                                     update.width != info.width ||
                                     update.height != info.height;
            info = update;
            stats.last_update = update;
            stats.codec = info.codec;
            stats.width = info.width;
            stats.height = info.height;
            stats.origin_x = info.origin_x;
            stats.origin_y = info.origin_y;
            stats.displays = info.displays;
            stats.input_enabled = info.input_enabled;
            stats.target_bitrate = info.bitrate;
            if (reconfigure) {
                // The next access unit may belong to a new stream (new SPS,
                // possibly a new codec). Start over and let the decoder wait
                // for that stream's first key frame.
                decoder.reset();
                chast::DecoderConfig updated_decoder;
                updated_decoder.codec = info.codec;
                decoder = chast::create_decoder(updated_decoder, &error);
                if (!decoder) {
                    return fail("decoder recreation failed: " + error);
                }
                stats.decoder = decoder->implementation();
                last_keyframe_request = start - std::chrono::seconds(10);
            }
            if (!options.quiet) {
                std::printf("quic: stream description updated: %s %ux%u at %d,%d\n",
                            chast_tools::codec_name(info.codec), info.width, info.height,
                            info.origin_x, info.origin_y);
            }
        }

        if (options.keyframe_request_ms > 0 &&
            last_frame >= next_keyframe_request) {
            session->request_keyframe();
            next_keyframe_request = last_frame +
                std::chrono::milliseconds(options.keyframe_request_ms);
        }
        if (header.seq != expected_seq) {
            ++stats.gaps;
            if (stats.gaps == 1 && !options.quiet) {
                std::fprintf(stderr, "sequence gap: expected %llu, got %llu\n",
                             static_cast<unsigned long long>(expected_seq),
                             static_cast<unsigned long long>(header.seq));
            }
        }
        expected_seq = header.seq + 1;

        if (header.codec != info.codec || header.width != info.width ||
            header.height != info.height) {
            ++stats.protocol_errors;
            if (stats.protocol_errors == 1) {
                std::fprintf(stderr,
                             "frame %llu disagrees with HelloAck: %s %ux%u\n",
                             static_cast<unsigned long long>(header.seq),
                             chast_tools::codec_name(header.codec), header.width,
                             header.height);
            }
            continue;
        }
        if (chast::crc32(payload.data(), payload.size()) != header.payload_crc32) {
            ++stats.crc_errors;
            if (stats.crc_errors == 1) {
                std::fprintf(stderr, "CRC mismatch on frame %llu\n",
                             static_cast<unsigned long long>(header.seq));
            }
            continue;
        }
        if (sink) {
            fwrite(payload.data(), 1, payload.size(), sink);
        }
        stats.bytes += payload.size();
        if (header.keyframe) {
            ++stats.keyframes;
        }

        frames.clear();
        chast::Error decoded =
            decoder->decode(payload.data(), payload.size(), header.pts_ms, header.keyframe,
                            frames);
        if (!decoded.ok()) {
            if (!decoder->initialized()) {
                // The stream has not reached its first sequence header. Ask
                // for an IDR instead of treating the delta frames as damage.
                if (std::chrono::steady_clock::now() - last_keyframe_request >=
                    std::chrono::seconds(1)) {
                    last_keyframe_request = std::chrono::steady_clock::now();
                    session->request_keyframe();
                }
                continue;
            }
            ++stats.decode_errors;
            if (stats.decode_errors == 1) {
                std::fprintf(stderr, "decode failed on frame %llu: %s\n",
                             static_cast<unsigned long long>(header.seq),
                             decoded.to_string().c_str());
            }
            if (stats.decode_errors >= 30) {
                return fail("too many decode failures");
            }
            continue;
        }
        for (const chast::DecodedFrame& frame : frames) {
            ++stats.decoded;
            stats.digest = chast::mix_frame_hash(stats.digest, frame.hash);
            stats.width = frame.width;
            stats.height = frame.height;
        }
    }

    std::vector<chast::DecodedFrame> tail;
    chast::Error flushed = decoder->flush(tail);
    if (flushed.ok()) {
        for (const chast::DecodedFrame& frame : tail) {
            ++stats.decoded;
            stats.digest = chast::mix_frame_hash(stats.digest, frame.hash);
        }
    } else if (decoder->initialized()) {
        ++stats.decode_errors;
    }
    if (sink) {
        fclose(sink);
    }

    const uint64_t elapsed_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
    // Drain the last reply that may have arrived with the final frames.
    stats.latency = session->latency();
    chast::ClientStats closing;
    closing.received = stats.received;
    closing.decoded = stats.decoded;
    closing.crc_errors = stats.crc_errors;
    closing.decode_errors = stats.decode_errors;
    closing.digest = stats.digest;
    closing.elapsed_ms = elapsed_ms;
    chast::Error finished = session->finish_client(closing, std::chrono::seconds(10));
    if (!finished.ok() && stats.error.empty()) {
        stats.error = "finish: " + finished.to_string();
    }

    if (!options.quiet || !stats.error.empty()) {
        std::printf("receiver: decoder=%s codec=%s size=%ux%u received=%llu decoded=%llu "
                    "keyframes=%llu crc-errors=%llu decode-errors=%llu gaps=%llu "
                    "protocol-errors=%llu stream-updates=%llu bytes=%llu digest=%016llX "
                    "elapsed=%llu ms%s\n",
                    stats.decoder.c_str(), chast_tools::codec_name(stats.codec), stats.width,
                    stats.height, static_cast<unsigned long long>(stats.received),
                    static_cast<unsigned long long>(stats.decoded),
                    static_cast<unsigned long long>(stats.keyframes),
                    static_cast<unsigned long long>(stats.crc_errors),
                    static_cast<unsigned long long>(stats.decode_errors),
                    static_cast<unsigned long long>(stats.gaps),
                    static_cast<unsigned long long>(stats.protocol_errors),
                    static_cast<unsigned long long>(stats.stream_updates),
                    static_cast<unsigned long long>(stats.bytes),
                    static_cast<unsigned long long>(stats.digest),
                    static_cast<unsigned long long>(elapsed_ms),
                    saw_video_end ? "" : " (stream did not end cleanly)");
        if (!options.quiet) {
            std::printf("receiver: rtt=%.1f ms offset=%.1f ms probes=%llu replies=%llu "
                        "frame-age=%.1f ms max-frame-age=%.1f ms target-bitrate=%u kbps "
                        "uncaptured=%llu\n",
                        stats.latency.valid ? static_cast<double>(stats.latency.rtt_us) / 1000.0
                                            : 0.0,
                        stats.latency.valid
                            ? static_cast<double>(stats.latency.offset_us) / 1000.0
                            : 0.0,
                        static_cast<unsigned long long>(stats.probes_sent),
                        static_cast<unsigned long long>(stats.latency.replies),
                        stats.last_frame_age_ms, stats.max_frame_age_ms,
                        stats.target_bitrate / 1000u,
                        static_cast<unsigned long long>(stats.frames_without_capture));
        }
    }
    stats.ok = stats.error.empty() && stats.received > 0 && stats.decoded > 0 &&
               stats.crc_errors == 0 && stats.decode_errors == 0 && stats.gaps == 0 &&
               stats.protocol_errors == 0 && stats.input_errors == 0 && saw_video_end;
    return stats;
}

struct StreamDigest {
    uint64_t units = 0;
    uint64_t decoded = 0;
    uint64_t errors = 0;
    uint64_t digest = chast::kFrameDigestSeed;
    UINT width = 0;
    UINT height = 0;
    std::string error;
};

// Replays a saved elementary stream the way chast-decode does: split at AUDs,
// decode each access unit, fold the picture hashes in order.
StreamDigest digest_saved_stream(const std::string& path, chast::VideoCodec codec) {
    StreamDigest result;
    FILE* input = nullptr;
    if (fopen_s(&input, path.c_str(), "rb") != 0 || !input) {
        result.error = "cannot open " + path;
        return result;
    }
    std::vector<uint8_t> data;
    uint8_t chunk[64 * 1024];
    for (;;) {
        const size_t got = fread(chunk, 1, sizeof(chunk), input);
        data.insert(data.end(), chunk, chunk + got);
        if (got != sizeof(chunk)) {
            break;
        }
    }
    fclose(input);

    const std::vector<chast_tools::AccessUnit> units =
        chast_tools::split_access_units(data, codec);
    result.units = units.size();
    if (units.empty()) {
        result.error = "the saved stream has no access units";
        return result;
    }

    chast::DecoderConfig config;
    config.codec = codec;
    std::string error;
    std::unique_ptr<chast::IVideoDecoder> decoder = chast::create_decoder(config, &error);
    if (!decoder) {
        result.error = "decoder creation failed: " + error;
        return result;
    }

    std::vector<chast::DecodedFrame> frames;
    for (size_t i = 0; i < units.size(); ++i) {
        frames.clear();
        chast::Error decoded = decoder->decode(units[i].bytes.data(), units[i].bytes.size(),
                                               static_cast<uint64_t>(i) * 33,
                                               units[i].keyframe, frames);
        if (!decoded.ok()) {
            ++result.errors;
            if (!decoder->initialized()) {
                continue;
            }
        }
        for (const chast::DecodedFrame& frame : frames) {
            ++result.decoded;
            result.digest = chast::mix_frame_hash(result.digest, frame.hash);
            result.width = frame.width;
            result.height = frame.height;
        }
    }
    std::vector<chast::DecodedFrame> tail;
    if (decoder->flush(tail).ok()) {
        for (const chast::DecodedFrame& frame : tail) {
            ++result.decoded;
            result.digest = chast::mix_frame_hash(result.digest, frame.hash);
        }
    }
    return result;
}

// Starts a second server and connects to it with a deliberately wrong pin.
// The TLS callback must refuse the certificate before any Hello is exchanged.
bool pinned_client_rejects_other_identity(uint16_t port, std::string* detail) {
    chast::QuicServerConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.port = port;
    server_config.idle_timeout_ms = 3000;
    std::string error;
    std::unique_ptr<chast::QuicSession> server =
        chast::QuicSession::create_server(server_config, &error);
    if (!server) {
        *detail = "server: " + error;
        return false;
    }
    std::thread waiting([&] { server->wait_for_client(std::chrono::seconds(3)); });

    chast::QuicClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = port;
    client_config.idle_timeout_ms = 3000;
    client_config.pinned_fingerprint.assign(64, '0');
    std::unique_ptr<chast::QuicSession> client =
        chast::QuicSession::create_client(client_config, &error);
    bool rejected = false;
    if (!client) {
        *detail = "client: " + error;
    } else {
        const chast::Error connected = client->connect(std::chrono::seconds(5));
        rejected = !connected.ok();
        *detail = connected.to_string();
    }
    waiting.join();
    return rejected;
}

struct ProbeClient {
    bool ok = false;
    chast::Status status = chast::Status::Ok;
    std::string error;
};

// Runs one full connect + Hello against `port`.
ProbeClient probe_handshake(uint16_t port, const std::string& pin, const std::string& token,
                            std::chrono::milliseconds timeout) {
    ProbeClient result;
    chast::QuicClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = port;
    client_config.idle_timeout_ms = 3000;
    client_config.pinned_fingerprint = pin;
    client_config.access_token = token;
    std::string error;
    std::unique_ptr<chast::QuicSession> client =
        chast::QuicSession::create_client(client_config, &error);
    if (!client) {
        result.error = "client: " + error;
        return result;
    }
    const chast::Error connected = client->connect(std::chrono::seconds(5));
    if (!connected.ok()) {
        result.status = connected.status();
        result.error = connected.to_string();
        return result;
    }
    chast::ServerInfo info;
    const chast::Error handshaken = client->handshake(info, timeout, chast::kDisplayAuto);
    result.ok = handshaken.ok();
    result.status = handshaken.status();
    result.error = handshaken.to_string();
    return result;
}

// Token and approval, on their own server so the main loopback stays a plain
// accepted session: a wrong token is refused with a reason, then an operator
// who declines the first viewer and accepts the second gets exactly that.
bool access_control_works(uint16_t port, std::string* detail) {
    {
        chast::QuicServerConfig server_config;
        server_config.bind_address = "127.0.0.1";
        server_config.port = port;
        server_config.idle_timeout_ms = 3000;
        server_config.access_token = "the-right-token";
        std::string error;
        std::unique_ptr<chast::QuicSession> server =
            chast::QuicSession::create_server(server_config, &error);
        if (!server) {
            *detail = "server: " + error;
            return false;
        }
        const std::string pin = server->server_fingerprint();
        std::thread waiting([&] { server->wait_for_client(std::chrono::seconds(2)); });
        const ProbeClient client =
            probe_handshake(port, pin, "the-wrong-token", std::chrono::seconds(3));
        waiting.join();
        if (client.status != chast::Status::PermissionDenied ||
            client.error.find("token") == std::string::npos) {
            *detail = "wrong token was not refused by name: " + client.error;
            return false;
        }
    }

    const uint16_t approval_port = static_cast<uint16_t>(port + 1);
    chast::QuicServerConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.port = approval_port;
    server_config.idle_timeout_ms = 3000;
    server_config.access_token = "approval-token";
    int decisions = 0;
    server_config.approve_client = [&decisions](const chast::ClientRequest&) {
        return ++decisions > 1;
    };
    std::string error;
    std::unique_ptr<chast::QuicSession> server =
        chast::QuicSession::create_server(server_config, &error);
    if (!server) {
        *detail = "approval server: " + error;
        return false;
    }
    const std::string pin = server->server_fingerprint();
    std::thread serving([&] {
        if (!server->wait_for_client(std::chrono::seconds(15)).ok()) {
            return;
        }
        chast::ServerInfo info;
        info.width = 64;
        info.height = 64;
        info.framerate = 30;
        info.bitrate = 100000;
        info.encoder = "approval probe";
        server->announce(info);
    });
    const ProbeClient declined =
        probe_handshake(approval_port, pin, "approval-token", std::chrono::seconds(3));
    ProbeClient approved;
    // The refusal closes the old connection and frees the slot on the
    // server's thread; retrying absorbs that hand-off.
    const auto approval_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!approved.ok && std::chrono::steady_clock::now() < approval_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        approved = probe_handshake(approval_port, pin, "approval-token",
                                   std::chrono::seconds(3));
    }
    serving.join();
    if (declined.status != chast::Status::PermissionDenied ||
        declined.error.find("declined") == std::string::npos) {
        *detail = "the declined viewer was not told why: " + declined.error;
        return false;
    }
    if (!approved.ok) {
        *detail = "the approved viewer did not connect: " + approved.error;
        return false;
    }
    if (decisions != 2) {
        *detail = "the approval callback ran " + std::to_string(decisions) + " times";
        return false;
    }

    // A one-time token is spent by the first accepted Hello: the next viewer
    // presenting it is refused by name rather than silently failing.
    const uint16_t spent_port = static_cast<uint16_t>(port + 2);
    {
        chast::QuicServerConfig spent_config;
        spent_config.bind_address = "127.0.0.1";
        spent_config.port = spent_port;
        spent_config.idle_timeout_ms = 3000;
        spent_config.access_token = "one-shot";
        std::string spent_error;
        std::unique_ptr<chast::QuicSession> spent =
            chast::QuicSession::create_server(spent_config, &spent_error);
        if (!spent) {
            *detail = "one-time server: " + spent_error;
            return false;
        }
        const std::string spent_pin = spent->server_fingerprint();
        std::thread spending([&] {
            if (!spent->wait_for_client(std::chrono::seconds(10)).ok()) {
                return;
            }
            chast::ServerInfo info;
            info.width = 64;
            info.height = 64;
            info.framerate = 30;
            info.bitrate = 100000;
            info.encoder = "one-time probe";
            spent->announce(info);
            // The first viewer leaves on its own; waiting again re-arms the
            // session so the second Hello reaches the token check.
            spent->wait_for_client(std::chrono::seconds(5));
        });
        const ProbeClient first =
            probe_handshake(spent_port, spent_pin, "one-shot", std::chrono::seconds(3));
        ProbeClient second;
        const auto spent_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (second.status != chast::Status::PermissionDenied &&
               std::chrono::steady_clock::now() < spent_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            second = probe_handshake(spent_port, spent_pin, "one-shot",
                                     std::chrono::seconds(3));
        }
        spending.join();
        if (!first.ok) {
            *detail = "the first one-time viewer did not connect: " + first.error;
            return false;
        }
        if (second.status != chast::Status::PermissionDenied ||
            second.error.find("already used") == std::string::npos) {
            *detail = "the spent token was not refused by name: " + second.error;
            return false;
        }
    }
    return true;
}

int run_verify(const Options& options) {
    Options local = options;
    if (local.seconds <= 0.0) {
        local.seconds = 5.0;
    }
    // A fingerprint the caller passed by hand is the one to check against; the
    // normal loopback run pins whatever identity the local server presents.
    local.insecure = false;
    // The loopback always covers the token path. A caller-supplied token is
    // reused so a manual `serve --token`/`connect --token` pair matches.
    if (local.token.empty()) {
        local.token = generate_token();
    }
    local.token_reuse = false;
    // The loopback check always covers the input channel: the server injects
    // in dry-run mode, so the events travel the whole path without touching
    // the real pointer.
    local.input = true;
    // Cover the mid-session description path: the sender re-announces the
    // current stream two seconds in.
    local.reannounce = true;
    if (local.seconds < 3.0) {
        local.seconds = 3.0;
    }
    const bool keep_file = !options.save.empty();
    const std::string path = keep_file ? options.save : ".chast-quic-verify.bin";

    // Both ends must not open the same file: the sender's copy is the one the
    // offline replay checks.
    Options receiver_options = local;
    receiver_options.save.clear();
    // Exercise the wire request instead of leaving the choice to the server.
    if (!receiver_options.display_set) {
        receiver_options.display_set = true;
        receiver_options.display = 0;
    }

    std::promise<ServerReady> listening;
    std::shared_future<ServerReady> server_ready = listening.get_future().share();
    ReceiverStats received;
    std::thread receiver([&] { received = run_receiver(receiver_options, server_ready); });
    const SenderStats sent = run_sender(local, path, &listening);
    receiver.join();

    std::printf("---- loopback check ----\n");
    bool failed = false;
    auto check = [&](bool condition, const char* what) {
        std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
        if (!condition) {
            failed = true;
        }
    };

    check(sent.error.empty(), "sender completed");
    if (!sent.error.empty()) {
        std::printf("     sender: %s\n", sent.error.c_str());
    }
    check(received.error.empty(), "receiver completed");
    if (!received.error.empty()) {
        std::printf("     receiver: %s\n", received.error.c_str());
    }
    check(!sent.fingerprint.empty() && received.peer_fingerprint == sent.fingerprint,
          "the viewer connected to the pinned server identity");
    check(!local.token.empty(), "the loopback ran with an access token");
    // Probes run on neighbouring ports so they cannot disturb the session
    // above.
    const uint16_t probe_base =
        options.port > 65530 ? static_cast<uint16_t>(options.port - 10) : options.port;
    {
        std::string detail;
        const bool rejected =
            pinned_client_rejects_other_identity(static_cast<uint16_t>(probe_base + 1), &detail);
        check(rejected, "a client pinned to another fingerprint rejects the server");
        if (!rejected) {
            std::printf("     mismatch probe: %s\n", detail.c_str());
        }
    }
    {
        std::string detail;
        const bool passed = access_control_works(static_cast<uint16_t>(probe_base + 2), &detail);
        check(passed, "wrong, spent and declined viewers are refused and told why");
        if (!passed) {
            std::printf("     access control probe: %s\n", detail.c_str());
        }
    }
    check(sent.encoded > 0, "the sender produced frames");
    check(sent.encoded == received.received, "every sent access unit was received");
    check(received.decoded == received.received,
          "every received access unit decoded to exactly one picture");
    check(received.keyframes > 0 && sent.keyframes == received.keyframes,
          "key-frame counts agree");
    if (options.keyframe_request_ms > 0) {
        check(sent.keyframes >= 2, "a key-frame request produced another IDR");
    }
    check(received.crc_errors == 0, "no payload CRC errors");
    check(received.decode_errors == 0, "no decode errors");
    check(received.gaps == 0, "no sequence gaps");
    check(received.protocol_errors == 0, "no protocol violations");
    check(received.latency.valid && received.latency.replies > 0,
          "the client received a latency reply");
    check(received.latency.valid && received.latency.rtt_us > 0 &&
              received.latency.rtt_us < 1000000,
          "the measured round trip is plausible");
    check(received.frames_without_capture == 0,
          "every received frame carried its capture timestamp");
    check(received.last_frame_age_ms >= -100.0 && received.last_frame_age_ms <= 1000.0,
          "the measured frame age is plausible");
    check(received.target_bitrate > 0, "the viewer saw the server's target bitrate");
    check(sent.target_bitrate > 0, "the server exposed adaptive-bitrate telemetry");
    check(sent.rtt_ms > 0.0, "the host sampled transport RTT");
    check(sent.abr_changes == 0, "the clean loopback stayed at the configured bitrate");
    check(sent.stats_received, "the client's closing statistics reached the server");
    if (sent.stats_received) {
        check(sent.peer.received == received.received, "server-side receive count agrees");
        check(sent.peer.decoded == received.decoded, "server-side decode count agrees");
        check(sent.peer.digest == received.digest, "server-side content digest agrees");
    }
    check(received.width == sent.width && received.height == sent.height,
          "decoded picture size matches the encoded size");
    check(sent.stream_updates_sent == 1, "the server re-announced the stream mid-session");
    check(received.stream_updates == sent.stream_updates_sent,
          "the viewer received every stream update");
    if (received.stream_updates > 0) {
        const chast::ServerInfo& update = received.last_update;
        check(update.codec == sent.codec && update.width == sent.width &&
                  update.height == sent.height && update.origin_x == sent.origin_x &&
                  update.origin_y == sent.origin_y && update.displays.size() == sent.displays.size(),
              "the stream update described the running stream");
    }

    if (local.input) {
        check(sent.input_enabled, "the server announced input as enabled");
        check(received.input_enabled, "the client saw input enabled in HelloAck");
        check(received.origin_x == sent.origin_x && received.origin_y == sent.origin_y,
              "the viewer saw the captured output's virtual-desktop origin");
        bool displays_same =
            !sent.displays.empty() && sent.displays.size() == received.displays.size();
        for (size_t i = 0; displays_same && i < sent.displays.size(); ++i) {
            displays_same = same_display(sent.displays[i], received.displays[i]);
        }
        check(displays_same, "the display list arrived intact and in order");
        size_t primary_count = 0;
        bool captured_listed = false;
        for (const chast::DisplayDescription& display : received.displays) {
            if (display.primary) {
                ++primary_count;
            }
            if (display.left == sent.origin_x && display.top == sent.origin_y &&
                static_cast<uint32_t>(display.right - display.left) == sent.width &&
                static_cast<uint32_t>(display.bottom - display.top) == sent.height) {
                captured_listed = true;
            }
        }
        check(primary_count == 1, "exactly one display is marked primary");
        check(captured_listed, "the captured output is listed among the displays");
        check(sent.requested_display == static_cast<uint16_t>(receiver_options.display),
              "the server saw the viewer's display choice");
        check(sent.display_index == receiver_options.display,
              "the server captured the display the viewer asked for");
        bool requested_matches = receiver_options.display < received.displays.size();
        if (requested_matches) {
            const chast::DisplayDescription& chosen =
                received.displays[receiver_options.display];
            requested_matches =
                chosen.left == sent.origin_x && chosen.top == sent.origin_y &&
                static_cast<uint32_t>(chosen.right - chosen.left) == sent.width &&
                static_cast<uint32_t>(chosen.bottom - chosen.top) == sent.height;
        }
        check(requested_matches,
              "the stream's origin and size are the requested display's rect");
        check(sent.input_events.size() == received.input_sent.size(),
              "every input event reached the server");
        bool same = sent.input_events.size() == received.input_sent.size();
        for (size_t i = 0; same && i < received.input_sent.size(); ++i) {
            same = same_event(sent.input_events[i], received.input_sent[i]);
        }
        check(same, "input events arrived intact and in order");
        check(sent.input_stats.applied == received.input_sent.size(),
              "every input event was injected");
        check(sent.input_stats.rejected == 0 && sent.input_stats.failed == 0 &&
                  sent.input_stats.dropped == 0,
              "no input event was rejected, failed or dropped");
        check(sent.input_releases == 1, "the viewer's release request reached the server");
    }

    const StreamDigest replay = digest_saved_stream(path, sent.codec);
    if (!replay.error.empty()) {
        std::printf("     replay: %s\n", replay.error.c_str());
    }
    check(replay.error.empty(), "the sent stream can be replayed offline");
    check(replay.units == sent.encoded, "the saved stream splits into the sent access units");
    check(replay.decoded == received.decoded, "offline decode produces the same frame count");
    check(replay.digest == received.digest, "offline decode produces the same content digest");

    if (!keep_file) {
        std::remove(path.c_str());
    } else {
        std::printf("saved the sent stream to %s\n", path.c_str());
    }

    if (failed) {
        std::printf("QUIC loopback FAILED\n");
        return 1;
    }
    std::printf("QUIC loopback PASSED: %llu access units, %llu pictures, digest %016llX\n",
                static_cast<unsigned long long>(sent.encoded),
                static_cast<unsigned long long>(received.decoded),
                static_cast<unsigned long long>(received.digest));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    chast_tools::install_crash_handler();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Options options;
    if (!parse_args(argc, argv, options)) {
        return 2;
    }
    if (options.token_auto) {
        options.token = generate_token();
        options.token_auto = false;
    }

    if (options.mode == "serve") {
        const SenderStats stats = run_sender(options, options.save, nullptr);
        if (!stats.error.empty()) {
            std::fprintf(stderr, "chast-quic serve: %s\n", stats.error.c_str());
        }
        return stats.ok || stats.peer_left ? 0 : 1;
    }
    if (options.mode == "connect") {
        const ReceiverStats stats = run_receiver(options, {});
        if (!stats.error.empty()) {
            std::fprintf(stderr, "chast-quic connect: %s\n", stats.error.c_str());
        }
        return stats.ok ? 0 : 1;
    }
    return run_verify(options);
}
