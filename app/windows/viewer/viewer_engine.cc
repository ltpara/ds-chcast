#include "viewer_engine.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "chast/decoder.h"
#include "chast/protocol.h"
#include "chast/quic.h"

namespace chast_viewer {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kReceiveTimeout = std::chrono::milliseconds(250);
constexpr auto kFrameStall = std::chrono::seconds(20);
constexpr auto kKeyframeRetry = std::chrono::seconds(1);

std::string codec_name(chast::VideoCodec codec) {
    return codec == chast::VideoCodec::H264 ? "H.264" : "HEVC";
}

}  // namespace

ViewerEngine::ViewerEngine(FrameSink* sink) : sink_(sink) {}

ViewerEngine::~ViewerEngine() {
    Stop();
}

bool ViewerEngine::Start(const std::string& host, uint16_t port) {
    if (running_.load(std::memory_order_acquire)) {
        return false;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = ViewerStatus{};
        status_.host = host;
        status_.port = port;
        status_.state = ViewerState::kConnecting;
        started_at_ = Clock::now();
    }
    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ViewerEngine::Run, this, host, port);
    return true;
}

void ViewerEngine::Stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

ViewerStatus ViewerEngine::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ViewerStatus status = status_;
    if (status.state == ViewerState::kConnecting || status.state == ViewerState::kHandshaking ||
        status.state == ViewerState::kStreaming) {
        status.elapsed_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at_)
                .count());
    }
    return status;
}

void ViewerEngine::SetState(ViewerState state, std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = state;
    status_.error = std::move(error);
}

void ViewerEngine::Run(std::string host, uint16_t port) {
    const auto started = Clock::now();
    auto last_frame = started;
    auto last_keyframe_request = started - kKeyframeRetry;
    auto window_start = started;

    uint64_t received = 0;
    uint64_t decoded = 0;
    uint64_t keyframes = 0;
    uint64_t published = 0;
    uint64_t crc_errors = 0;
    uint64_t decode_errors = 0;
    uint64_t protocol_errors = 0;
    uint64_t gaps = 0;
    uint64_t bytes = 0;
    uint64_t digest = chast::kFrameDigestSeed;
    uint64_t window_frames = 0;
    uint64_t window_bytes = 0;
    uint32_t display_width = 0;
    uint32_t display_height = 0;
    bool video_finished = false;
    bool handshaken = false;
    bool logged_frame = false;
    bool logged_drop = false;
    uint64_t init_errors = 0;
    std::string failure;

    // Folds the worker's counters into the status the UI polls. Called from
    // this thread only; the window counters are reset once per second.
    auto sync_status = [&](bool final) {
        const auto now = Clock::now();
        const double window_seconds =
            std::chrono::duration<double>(now - window_start).count();
        double fps = 0.0;
        double kbps = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (window_seconds >= 1.0) {
                fps = static_cast<double>(window_frames) / window_seconds;
                kbps = static_cast<double>(window_bytes) * 8.0 / 1000.0 / window_seconds;
                status_.fps = fps;
                status_.kbps = kbps;
            }
            status_.received = received;
            status_.decoded = decoded;
            status_.keyframes = keyframes;
            status_.published = published;
            status_.crc_errors = crc_errors;
            status_.decode_errors = decode_errors;
            status_.protocol_errors = protocol_errors;
            status_.gaps = gaps;
            status_.bytes = bytes;
            status_.width = display_width;
            status_.height = display_height;
            status_.elapsed_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count());
            if (final) {
                status_.video_finished = video_finished;
            }
        }
        if (window_seconds >= 1.0) {
            window_frames = 0;
            window_bytes = 0;
            window_start = now;
        }
    };

    auto fail = [&](std::string message) {
        failure = std::move(message);
    };

    chast::QuicClientConfig client_config;
    client_config.host = host;
    client_config.port = port;
    std::string error;
    std::unique_ptr<chast::QuicSession> session =
        chast::QuicSession::create_client(client_config, &error);
    if (!session) {
        fail("QUIC client: " + error);
        sync_status(true);
        SetState(ViewerState::kFailed, failure);
        running_.store(false, std::memory_order_release);
        return;
    }

    chast::Error connected = session->connect(std::chrono::seconds(15));
    if (!connected.ok()) {
        fail("connect: " + connected.to_string());
    } else {
        SetState(ViewerState::kHandshaking);
        chast::ServerInfo info;
        chast::Error handshake = session->handshake(info, std::chrono::seconds(10));
        if (!handshake.ok()) {
            fail("handshake: " + handshake.to_string());
        } else {
            handshaken = true;
            display_width = info.width;
            display_height = info.height;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.codec = info.codec;
                status_.width = info.width;
                status_.height = info.height;
                status_.framerate = info.framerate;
                status_.server_bitrate = info.bitrate;
                status_.server_encoder = info.encoder;
            }

            chast::DecoderConfig decoder_config;
            decoder_config.codec = info.codec;
            if (sink_ == nullptr || sink_->device() == nullptr) {
                fail("display: " +
                     (sink_ != nullptr ? sink_->error()
                                       : std::string("the frame sink is not available")));
            }
            // Decode straight into a D3D11 texture and hand each picture to
            // the sink while the decoder still owns the surface: the image
            // never reaches system memory.
            decoder_config.d3d11_device = sink_ != nullptr ? sink_->device() : nullptr;
            decoder_config.gpu_handler =
                [&](const chast::GpuFrame& frame) -> chast::Error {
                ++decoded;
                display_width = frame.width;
                display_height = frame.height;
                if (sink_ != nullptr &&
                    sink_->OnGpuFrame(frame.texture, frame.subresource, frame.width,
                                      frame.height, frame.crop_x, frame.crop_y,
                                      frame.pts_ms)) {
                    ++published;
                    ++window_frames;
                    if (!logged_frame) {
                        logged_frame = true;
                        std::fprintf(stderr, "viewer: first GPU frame %ux%u\n", frame.width,
                                     frame.height);
                    }
                    return chast::Error::success();
                }
                if (sink_ == nullptr) {
                    return chast::Error(chast::Status::Unsupported,
                                        "display: the frame sink is not available");
                }
                if (!sink_->error().empty()) {
                    return chast::Error(chast::Status::CaptureFailed,
                                        "display: " + sink_->error());
                }
                if (!logged_drop) {
                    logged_drop = true;
                    std::fprintf(stderr,
                                 "viewer: the display has no GPU output; dropping frames\n");
                }
                // The plugin texture is gone (for example while the UI is
                // tearing the page down); dropping keeps the decoder running.
                return chast::Error::success();
            };
            std::unique_ptr<chast::IVideoDecoder> decoder =
                decoder_config.d3d11_device != nullptr
                    ? chast::create_decoder(decoder_config, &error)
                    : nullptr;
            if (!decoder && failure.empty()) {
                fail("decoder (" + codec_name(info.codec) + "): " + error);
            } else if (decoder) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.decoder = decoder->implementation();
                }
                std::fprintf(stderr, "viewer: decoder %s\n", decoder->implementation().c_str());
                SetState(ViewerState::kStreaming);

                auto request_keyframe = [&] {
                    const auto now = Clock::now();
                    if (now - last_keyframe_request >= kKeyframeRetry) {
                        last_keyframe_request = now;
                        session->request_keyframe();
                    }
                };

                uint64_t expected_seq = 0;
                std::vector<chast::DecodedFrame> frames;

                while (!stop_requested_.load(std::memory_order_acquire)) {
                    chast::FrameHeader header;
                    std::vector<uint8_t> payload;
                    chast::Error received_frame = session->receive_frame(
                        header, payload, kReceiveTimeout);
                    const auto now = Clock::now();

                    if (received_frame.status() == chast::Status::WaitTimeout) {
                        const chast::Error async = session->async_error();
                        if (!async.ok()) {
                            fail("transport: " + async.to_string());
                            break;
                        }
                        if (session->video_finished()) {
                            video_finished = true;
                            break;
                        }
                        if (now - last_frame >= kFrameStall) {
                            fail("no frames arrived for 20 s");
                            break;
                        }
                        if (sink_ != nullptr && !sink_->error().empty()) {
                            fail("display: " + sink_->error());
                            break;
                        }
                        sync_status(false);
                        continue;
                    }
                    if (received_frame.status() == chast::Status::Unavailable) {
                        video_finished = session->video_finished();
                        if (!video_finished) {
                            const chast::Error async = session->async_error();
                            fail(async.ok() ? "the connection closed before the video stream ended"
                                            : async.to_string());
                        }
                        break;
                    }
                    if (!received_frame.ok()) {
                        fail("receive: " + received_frame.to_string());
                        break;
                    }

                    ++received;
                    last_frame = now;
                    bytes += payload.size();
                    window_bytes += payload.size();
                    if (header.keyframe) {
                        ++keyframes;
                    }
                    if (header.seq != expected_seq) {
                        ++gaps;
                        request_keyframe();
                    }
                    expected_seq = header.seq + 1;

                    if (header.codec != info.codec || header.width != info.width ||
                        header.height != info.height) {
                        ++protocol_errors;
                        sync_status(false);
                        continue;
                    }
                    if (chast::crc32(payload.data(), payload.size()) != header.payload_crc32) {
                        ++crc_errors;
                        sync_status(false);
                        continue;
                    }

                    frames.clear();
                    chast::Error decode_error = decoder->decode(
                        payload.data(), payload.size(), header.pts_ms, header.keyframe, frames);
                    if (!decode_error.ok()) {
                        if (sink_ != nullptr && !sink_->error().empty()) {
                            fail("display: " + sink_->error());
                            break;
                        }
                        if (!decoder->initialized()) {
                            // No sequence header yet; ask for an IDR instead
                            // of counting delta frames as damage.
                            if (init_errors++ < 3) {
                                std::fprintf(stderr, "viewer: init: %s\n",
                                             decode_error.to_string().c_str());
                            }
                            request_keyframe();
                            sync_status(false);
                            continue;
                        }
                        if (decode_errors < 3) {
                            std::fprintf(stderr, "viewer: decode: %s\n",
                                         decode_error.to_string().c_str());
                        }
                        ++decode_errors;
                        if (decode_errors >= 30) {
                            fail("too many decode failures");
                            break;
                        }
                        sync_status(false);
                        continue;
                    }

                    // Pictures arrive through gpu_handler while decode() runs,
                    // so `frames` stays empty on this path.
                    sync_status(false);
                }
            }
        }
    }

    if (!stop_requested_.load(std::memory_order_acquire) && failure.empty() &&
        !video_finished && handshaken) {
        // Leaving the loop without a failure or an explicit stop means the
        // decoder path itself ended (it should not); keep the state honest.
        fail("the video session ended unexpectedly");
    }

    if (handshaken) {
        chast::ClientStats closing;
        closing.received = received;
        closing.decoded = decoded;
        closing.crc_errors = crc_errors;
        closing.decode_errors = decode_errors;
        closing.digest = digest;
        closing.elapsed_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started)
                .count());
        session->finish_client(closing, std::chrono::milliseconds(1500));
    }

    sync_status(true);
    if (!failure.empty()) {
        SetState(ViewerState::kFailed, failure);
    } else {
        SetState(ViewerState::kEnded);
    }
    running_.store(false, std::memory_order_release);
}

}  // namespace chast_viewer
