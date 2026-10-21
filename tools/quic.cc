// QUIC transport harness: capture -> encode -> MsQuic -> decode.
//
//   chast-quic serve   --port 4433 --codec h264 --seconds 10 [--save sent.h264]
//   chast-quic connect --host 127.0.0.1 --port 4433 [--save received.h264]
//   chast-quic verify  --port 45433 --seconds 5
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

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "annexb.h"
#include "chast/capture.h"
#include "chast/decoder.h"
#include "chast/encoder.h"
#include "chast/protocol.h"
#include "chast/quic.h"
#include "crash_handler.h"

namespace {

struct Options {
    std::string mode;
    std::string bind = "127.0.0.1";
    std::string host = "127.0.0.1";
    uint16_t port = 4433;
    size_t display = 0;
    chast::VideoCodec codec = chast::VideoCodec::H264;
    UINT bitrate_kbps = 0;
    UINT framerate = 30;
    float quality = 0.67f;
    UINT keyframe_ms = 0;
    // Receiver-side: ask for an IDR this often, exercising the control stream
    // and the sender's key-frame-on-request path.
    UINT keyframe_request_ms = 0;
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
                options.display = static_cast<size_t>(std::stoul(argv[++i]));
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
    return true;
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
    chast::VideoCodec codec = chast::VideoCodec::H264;
    std::string encoder;
    std::string save_path;
    std::string error;
    bool client_connected = false;
    bool peer_left = false;
    bool stats_received = false;
    chast::ClientStats peer;
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
    chast::VideoCodec codec = chast::VideoCodec::H264;
    std::string decoder;
    std::string error;
    bool ok = false;
};

// Captures until a frame with an NV12 texture is available. The reference
// returned by capturer.frame() stays valid until the next capture() call.
bool wait_for_usable_frame(chast::IDxgiCapturer& capturer, bool quiet, std::string* error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        const chast::CaptureResult result = capturer.capture(std::chrono::milliseconds(100));
        switch (result) {
            case chast::CaptureResult::NoNewFrame:
                continue;
            case chast::CaptureResult::ReinitNeeded:
                if (!quiet) {
                    std::printf("reinitializing: %s\n",
                                capturer.last_error().to_string().c_str());
                }
                if (chast::Error err = capturer.reinitialize(); !err.ok()) {
                    *error = "reinit failed: " + err.to_string();
                    return false;
                }
                continue;
            case chast::CaptureResult::FatalError:
                *error = "capture failed: " + capturer.last_error().to_string();
                return false;
            case chast::CaptureResult::Frame:
                break;
        }
        const chast::FrameInfo& frame = capturer.frame();
        if (!frame.nv12_texture) {
            continue;
        }
        if (frame.rotation_degrees != 0) {
            *error = "the encoder cannot carry " + std::to_string(frame.rotation_degrees) +
                     "-degree output and capture could not rotate the desktop";
            return false;
        }
        return true;
    }
    *error = "no frame with an NV12 texture arrived in 30 s";
    return false;
}

SenderStats run_sender(const Options& options, const std::string& save_path,
                       std::promise<bool>* listening) {
    SenderStats stats;
    stats.save_path = save_path;
    bool ready_published = false;
    auto publish_ready = [&](bool value) {
        if (listening && !ready_published) {
            ready_published = true;
            listening->set_value(value);
        }
    };
    auto fail = [&](std::string message) {
        stats.error = std::move(message);
        stats.ok = false;
        publish_ready(false);
        return stats;
    };

    std::string error;
    std::unique_ptr<chast::IDxgiCapturer> capturer =
        chast::create_capturer(options.display, options.config, &error);
    if (!capturer) {
        return fail("capturer creation failed: " + error);
    }

    chast::QuicServerConfig server_config;
    server_config.bind_address = options.bind;
    server_config.port = options.port;
    std::unique_ptr<chast::QuicSession> server =
        chast::QuicSession::create_server(server_config, &error);
    if (!server) {
        return fail("QUIC server: " + error);
    }
    publish_ready(true);
    if (!options.quiet) {
        std::printf("quic: listening on %s:%u\n", options.bind.c_str(), options.port);
    }

    chast::Error waited =
        server->wait_for_client(std::chrono::milliseconds(
            static_cast<long long>(options.wait_seconds * 1000.0)));
    if (!waited.ok()) {
        return fail("waiting for a client: " + waited.to_string());
    }
    stats.client_connected = true;
    if (!options.quiet) {
        std::printf("quic: client connected; starting the capture\n");
    }

    if (!wait_for_usable_frame(*capturer, options.quiet, &error)) {
        return fail(error);
    }
    const chast::FrameInfo& first = capturer->frame();

    chast::EncoderConfig encoder_config;
    encoder_config.codec = options.codec;
    encoder_config.bitrate = options.bitrate_kbps * 1000u;
    encoder_config.framerate = options.framerate;
    encoder_config.quality_ratio = options.quality;
    encoder_config.gop_size = 0;
    encoder_config.width = first.width;
    encoder_config.height = first.height;
    const chast::AdapterDevice adapter = capturer->adapter_device();
    std::unique_ptr<chast::IVideoEncoder> encoder =
        chast::create_encoder(adapter.device, encoder_config, &error);
    if (!encoder) {
        return fail("encoder creation failed: " + error);
    }
    stats.codec = encoder->codec();
    stats.width = encoder->width();
    stats.height = encoder->height();
    stats.encoder = encoder->implementation();

    if (!options.quiet) {
        std::printf("encoder: %s via %s, %ux%u, target %u kbps\n",
                    chast_tools::codec_name(stats.codec),
                    stats.encoder.empty() ? "oneVPL" : stats.encoder.c_str(), stats.width,
                    stats.height, encoder->bitrate() / 1000u);
        if (stats.codec != options.codec) {
            std::printf("encoder fell back: requested %s, producing %s\n",
                        chast_tools::codec_name(options.codec),
                        chast_tools::codec_name(stats.codec));
        }
    }

    chast::ServerInfo info;
    info.codec = encoder->codec();
    info.width = static_cast<uint16_t>(encoder->width());
    info.height = static_cast<uint16_t>(encoder->height());
    info.framerate = static_cast<uint16_t>(options.framerate);
    info.bitrate = encoder->bitrate();
    info.encoder = encoder->implementation().empty() ? "oneVPL" : encoder->implementation();
    chast::Error announced = server->announce(info);
    if (!announced.ok()) {
        return fail("announce: " + announced.to_string());
    }

    FILE* sink = nullptr;
    if (!save_path.empty() &&
        (fopen_s(&sink, save_path.c_str(), "wb") != 0 || !sink)) {
        return fail("could not open " + save_path);
    }

    const auto start = std::chrono::steady_clock::now();
    // DXGI delivers frames at the desktop refresh rate. Pace the capture so
    // the stream matches the framerate the encoder and HelloAck declare,
    // instead of sending 60 fps into a 30 fps rate-control configuration.
    const std::chrono::steady_clock::duration frame_interval =
        options.framerate > 0
            ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(1.0 / static_cast<double>(options.framerate)))
            : std::chrono::steady_clock::duration::zero();
    auto next_frame_due = start + frame_interval;
    auto last_keyframe = start;
    bool force_keyframe = true;  // the first access unit is always an IDR
    bool have_input = false;
    bool keep_running = true;
    std::vector<chast::EncodedPacket> packets;

    auto process_frame = [&](uint64_t pts_ms) -> bool {
        const chast::FrameInfo& frame = capturer->frame();
        if (!frame.nv12_texture) {
            return true;
        }
        if (frame.rotation_degrees != 0) {
            stats.error = "capture lost rotation while streaming";
            return false;
        }
        packets.clear();
        chast::Error err = chast::Error::success();
        if (!frame.nv12_updated && have_input) {
            err = encoder->encode_repeat(pts_ms, force_keyframe, packets);
            if (err.ok()) {
                ++stats.repeats;
            } else if (err.status() == chast::Status::DeviceRemoved ||
                       err.status() == chast::Status::DeviceReset) {
                stats.error = "encode_repeat: " + err.to_string();
                return false;
            } else {
                packets.clear();
                err = encoder->encode(frame.nv12_texture, pts_ms, force_keyframe, packets);
            }
        } else {
            err = encoder->encode(frame.nv12_texture, pts_ms, force_keyframe, packets);
        }
        if (!err.ok()) {
            stats.error = "encode: " + err.to_string();
            return false;
        }
        have_input = true;

        for (const chast::EncodedPacket& packet : packets) {
            if (packet.data.empty()) {
                continue;
            }
            chast::FrameHeader header;
            header.pts_ms = packet.pts_ms;
            header.width = encoder->width();
            header.height = encoder->height();
            header.codec = encoder->codec();
            header.keyframe = packet.keyframe;
            header.idr = packet.idr;
            chast::Error sent =
                server->send_frame(header, packet.data.data(), packet.data.size());
            if (!sent.ok()) {
                if (sent.status() == chast::Status::Unavailable ||
                    sent.status() == chast::Status::WaitTimeout) {
                    stats.peer_left = true;
                } else {
                    ++stats.send_errors;
                    stats.error = "send_frame: " + sent.to_string();
                }
                return false;
            }
            if (sink) {
                fwrite(packet.data.data(), 1, packet.data.size(), sink);
            }
            ++stats.encoded;
            stats.bytes += packet.data.size();
            if (packet.keyframe) {
                ++stats.keyframes;
                last_keyframe = std::chrono::steady_clock::now();
                force_keyframe = false;
            }
        }
        return true;
    };

    // The frame captured before the handshake is the first one on the wire.
    ++stats.frames;
    if (!first.desktop_updated) {
        ++stats.pointer_only;
    }
    keep_running = process_frame(0);

    uint64_t timeouts = 0;
    while (keep_running) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        if (options.seconds > 0.0 && elapsed >= options.seconds) {
            break;
        }
        if (server->take_keyframe_request()) {
            force_keyframe = true;
        }
        if (options.keyframe_ms > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_keyframe).count() >=
                static_cast<long long>(options.keyframe_ms)) {
            force_keyframe = true;
        }

        if (frame_interval != std::chrono::steady_clock::duration::zero()) {
            const auto wait_now = std::chrono::steady_clock::now();
            if (next_frame_due > wait_now) {
                std::this_thread::sleep_for(next_frame_due - wait_now);
            }
            // Advance on an absolute timeline; capture() itself waits for the
            // next present, so a relative sleep would halve the frame rate.
            next_frame_due += frame_interval;
            const auto late = std::chrono::steady_clock::now() - next_frame_due;
            if (late > std::chrono::seconds(1)) {
                next_frame_due = std::chrono::steady_clock::now();
            }
        }

        const chast::CaptureResult result = capturer->capture(std::chrono::milliseconds(100));
        switch (result) {
            case chast::CaptureResult::NoNewFrame:
                ++timeouts;
                continue;
            case chast::CaptureResult::ReinitNeeded: {
                if (!options.quiet) {
                    std::printf("reinitializing: %s\n",
                                capturer->last_error().to_string().c_str());
                }
                chast::Error err = capturer->reinitialize();
                if (!err.ok()) {
                    stats.error = "reinit failed: " + err.to_string();
                    keep_running = false;
                    break;
                }
                encoder.reset();
                have_input = false;
                force_keyframe = true;
                continue;
            }
            case chast::CaptureResult::FatalError:
                stats.error = "capture failed: " + capturer->last_error().to_string();
                keep_running = false;
                break;
            case chast::CaptureResult::Frame:
                break;
        }
        if (!keep_running) {
            break;
        }

        ++stats.frames;
        const chast::FrameInfo& frame = capturer->frame();
        if (!frame.desktop_updated) {
            ++stats.pointer_only;
        }
        if (!encoder) {
            // The reinitialized duplication may be a different size; the
            // announced stream description would be a lie, so stop instead.
            if (frame.width != stats.width || frame.height != stats.height) {
                stats.error = "the desktop changed size mid-stream";
                break;
            }
            encoder_config.width = frame.width;
            encoder_config.height = frame.height;
            encoder = chast::create_encoder(adapter.device, encoder_config, &error);
            if (!encoder) {
                stats.error = "encoder recreation failed: " + error;
                break;
            }
            if (encoder->codec() != stats.codec) {
                stats.error = "the encoder changed codec mid-stream; restart the session";
                break;
            }
            force_keyframe = true;
        }
        const uint64_t pts_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count());
        if (!process_frame(pts_ms)) {
            break;
        }
    }

    if (sink) {
        fclose(sink);
    }

    chast::ClientStats peer;
    bool received = false;
    chast::Error finished = server->finish_server(std::chrono::milliseconds(15000), peer,
                                                  received);
    stats.stats_received = received;
    stats.peer = peer;
    if (!finished.ok() && stats.error.empty()) {
        // A client that vanished is normal for serve; anything else is not.
        if (!stats.peer_left) {
            stats.error = "finish: " + finished.to_string();
        }
    }
    if (!options.quiet || !stats.error.empty()) {
        std::printf("sender: frames=%llu encoded=%llu keyframes=%llu pointer-only=%llu "
                    "repeats=%llu bytes=%llu send-errors=%llu\n",
                    static_cast<unsigned long long>(stats.frames),
                    static_cast<unsigned long long>(stats.encoded),
                    static_cast<unsigned long long>(stats.keyframes),
                    static_cast<unsigned long long>(stats.pointer_only),
                    static_cast<unsigned long long>(stats.repeats),
                    static_cast<unsigned long long>(stats.bytes),
                    static_cast<unsigned long long>(stats.send_errors));
        if (received) {
            std::printf("sender: peer received=%llu decoded=%llu digest=%016llX\n",
                        static_cast<unsigned long long>(peer.received),
                        static_cast<unsigned long long>(peer.decoded),
                        static_cast<unsigned long long>(peer.digest));
        }
    }
    stats.ok = stats.error.empty();
    return stats;
}

ReceiverStats run_receiver(const Options& options, std::shared_future<bool> server_ready) {
    ReceiverStats stats;
    auto fail = [&](std::string message) {
        stats.error = std::move(message);
        stats.ok = false;
        return stats;
    };

    if (server_ready.valid()) {
        if (server_ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready ||
            !server_ready.get()) {
            return fail("the local server did not start");
        }
    }

    chast::QuicClientConfig client_config;
    client_config.host = options.host;
    client_config.port = options.port;
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

    chast::ServerInfo info;
    chast::Error handshaken = session->handshake(info, std::chrono::seconds(15));
    if (!handshaken.ok()) {
        return fail("handshake: " + handshaken.to_string());
    }
    stats.codec = info.codec;
    stats.width = info.width;
    stats.height = info.height;
    if (!options.quiet) {
        std::printf("quic: connected, server streams %s %ux%u at %u kbps (%s)\n",
                    chast_tools::codec_name(info.codec), info.width, info.height,
                    info.bitrate / 1000u, info.encoder.c_str());
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

    while (std::chrono::steady_clock::now() < deadline) {
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
                    "protocol-errors=%llu bytes=%llu digest=%016llX elapsed=%llu ms%s\n",
                    stats.decoder.c_str(), chast_tools::codec_name(stats.codec), stats.width,
                    stats.height, static_cast<unsigned long long>(stats.received),
                    static_cast<unsigned long long>(stats.decoded),
                    static_cast<unsigned long long>(stats.keyframes),
                    static_cast<unsigned long long>(stats.crc_errors),
                    static_cast<unsigned long long>(stats.decode_errors),
                    static_cast<unsigned long long>(stats.gaps),
                    static_cast<unsigned long long>(stats.protocol_errors),
                    static_cast<unsigned long long>(stats.bytes),
                    static_cast<unsigned long long>(stats.digest),
                    static_cast<unsigned long long>(elapsed_ms),
                    saw_video_end ? "" : " (stream did not end cleanly)");
    }
    stats.ok = stats.error.empty() && stats.received > 0 && stats.decoded > 0 &&
               stats.crc_errors == 0 && stats.decode_errors == 0 && stats.gaps == 0 &&
               stats.protocol_errors == 0 && saw_video_end;
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

int run_verify(const Options& options) {
    Options local = options;
    if (local.seconds <= 0.0) {
        local.seconds = 5.0;
    }
    const bool keep_file = !options.save.empty();
    const std::string path = keep_file ? options.save : ".chast-quic-verify.bin";

    // Both ends must not open the same file: the sender's copy is the one the
    // offline replay checks.
    Options receiver_options = local;
    receiver_options.save.clear();

    std::promise<bool> listening;
    std::shared_future<bool> server_ready = listening.get_future().share();
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
    check(sent.stats_received, "the client's closing statistics reached the server");
    if (sent.stats_received) {
        check(sent.peer.received == received.received, "server-side receive count agrees");
        check(sent.peer.decoded == received.decoded, "server-side decode count agrees");
        check(sent.peer.digest == received.digest, "server-side content digest agrees");
    }
    check(received.width == sent.width && received.height == sent.height,
          "decoded picture size matches the encoded size");

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
