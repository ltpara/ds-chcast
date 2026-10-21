#include "viewer_engine.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "chast/decoder.h"
#include "chast/identity.h"
#include "chast/protocol.h"
#include "chast/quic.h"

namespace chast_viewer {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kReceiveTimeout = std::chrono::milliseconds(250);
// Input is drained once per receive poll, so an idle desktop (where the
// receive timeout is the only thing pacing the loop) still carries pointer
// motion within this bound.
constexpr auto kInputPoll = std::chrono::milliseconds(10);
constexpr auto kFrameStall = std::chrono::seconds(20);
constexpr auto kKeyframeRetry = std::chrono::seconds(1);
constexpr size_t kMaxPendingInput = 256;

std::string codec_name(chast::VideoCodec codec) {
    return codec == chast::VideoCodec::H264 ? "H.264" : "HEVC";
}

}  // namespace

ViewerEngine::ViewerEngine(FrameSink* sink) : sink_(sink) {}

ViewerEngine::~ViewerEngine() {
    Stop();
}

bool ViewerEngine::Start(const std::string& host, uint16_t port, uint16_t display,
                         std::string pinned_fingerprint, bool allow_unverified,
                         std::string access_token) {
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
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        pending_input_.clear();
        input_enabled_ = false;
    }
    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ViewerEngine::Run, this, host, port, display,
                          std::move(pinned_fingerprint), allow_unverified,
                          std::move(access_token));
    return true;
}

void ViewerEngine::Stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

bool ViewerEngine::SendInput(const chast::InputEvent& event) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (!input_enabled_ || pending_input_.size() >= kMaxPendingInput) {
        return false;
    }
    PendingInput pending;
    pending.event = event;
    pending_input_.push_back(pending);
    return true;
}

bool ViewerEngine::ReleaseInput() {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (!input_enabled_ || pending_input_.size() >= kMaxPendingInput) {
        return false;
    }
    PendingInput pending;
    pending.release = true;
    pending_input_.push_back(pending);
    return true;
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

void ViewerEngine::Run(std::string host, uint16_t port, uint16_t display,
                       std::string pinned_fingerprint, bool allow_unverified,
                       std::string access_token) {
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
    uint64_t input_sent = 0;
    uint64_t input_failed = 0;
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
            status_.input_sent = input_sent;
            status_.input_failed = input_failed;
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
    client_config.pinned_fingerprint = std::move(pinned_fingerprint);
    client_config.allow_any_certificate = allow_unverified;
    client_config.access_token = std::move(access_token);
    std::string error;
    std::unique_ptr<chast::QuicSession> session =
        chast::QuicSession::create_client(client_config, &error);
    if (!session) {
        fail("QUIC client: " + error);
        sync_status(true);
        std::fprintf(stderr, "viewer: %s\n", failure.c_str());
        SetState(ViewerState::kFailed, failure);
        running_.store(false, std::memory_order_release);
        return;
    }

    chast::Error connected = session->connect(std::chrono::seconds(15));
    // The certificate callback records the presented fingerprint before it
    // decides, so even a rejected identity is visible to the UI.
    const std::string server_fingerprint = session->peer_fingerprint();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.server_fingerprint = server_fingerprint;
    }
    if (!server_fingerprint.empty()) {
        std::fprintf(stderr, "viewer: server identity SHA256 %s\n",
                     chast::format_fingerprint(server_fingerprint).c_str());
    }
    if (!connected.ok()) {
        fail("connect: " + connected.to_string());
    } else if (server_fingerprint.empty()) {
        failure = "the server presented no certificate";
    } else {
        SetState(ViewerState::kHandshaking);
        chast::ServerInfo info;
        chast::Error handshake =
            session->handshake(info, std::chrono::seconds(10), display);
        if (!handshake.ok()) {
            if (handshake.status() == chast::Status::PermissionDenied) {
                // The server named its refusal; the UI picks the wording from
                // the code and shows `error` as the server's reason.
                const std::optional<chast::AccessDenial> denial = session->denial();
                std::lock_guard<std::mutex> lock(mutex_);
                status_.denial =
                    denial.has_value() ? static_cast<int>(*denial) : 0;
            }
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
                status_.input_enabled = info.input_enabled;
                status_.origin_x = info.origin_x;
                status_.origin_y = info.origin_y;
                status_.displays = info.displays;
            }
            {
                std::lock_guard<std::mutex> lock(input_mutex_);
                input_enabled_ = info.input_enabled;
            }
            std::fprintf(stderr,
                         "viewer: display request %s; %zu announced, stream origin %d,%d\n",
                         display == chast::kDisplayAuto ? "auto"
                                                       : std::to_string(display).c_str(),
                         info.displays.size(), info.origin_x, info.origin_y);

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

                // Sends everything the UI queued since the last poll, in
                // order. Runs on this thread, which owns the session, so
                // send_input() never races receive_frame().
                auto drain_input = [&] {
                    if (!info.input_enabled) {
                        return;
                    }
                    std::deque<PendingInput> events;
                    {
                        std::lock_guard<std::mutex> lock(input_mutex_);
                        events.swap(pending_input_);
                    }
                    for (const PendingInput& pending : events) {
                        chast::Error sent = pending.release
                                                ? session->release_input()
                                                : session->send_input(pending.event);
                        if (sent.ok()) {
                            ++input_sent;
                            continue;
                        }
                        ++input_failed;
                        if (input_failed <= 3) {
                            std::fprintf(stderr, "viewer: %s: %s\n",
                                         pending.release ? "release_input" : "send_input",
                                         sent.to_string().c_str());
                        }
                    }
                };

                uint64_t expected_seq = 0;
                std::vector<chast::DecodedFrame> frames;

                // Applies StreamInfo messages: the captured output changed
                // size, rotation or origin after the server rebuilt its
                // duplication. Frames keep flowing on the same stream, so only
                // the description and (when the codec or size changed) the
                // decoder are replaced. Returns false with `failure` set when
                // the replacement decoder cannot be created.
                auto apply_stream_updates = [&]() -> bool {
                    chast::ServerInfo update;
                    bool applied = false;
                    while (session->take_stream_update(update)) {
                        applied = true;
                        const bool needs_decoder = update.codec != info.codec ||
                                                   update.width != info.width ||
                                                   update.height != info.height;
                        info = update;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.codec = info.codec;
                            status_.framerate = info.framerate;
                            status_.server_bitrate = info.bitrate;
                            status_.server_encoder = info.encoder;
                            status_.input_enabled = info.input_enabled;
                            status_.origin_x = info.origin_x;
                            status_.origin_y = info.origin_y;
                            status_.displays = info.displays;
                        }
                        {
                            std::lock_guard<std::mutex> lock(input_mutex_);
                            input_enabled_ = info.input_enabled;
                        }
                        std::fprintf(stderr,
                                     "viewer: stream is now %s %ux%u at %d,%d\n",
                                     codec_name(info.codec).c_str(), info.width, info.height,
                                     info.origin_x, info.origin_y);
                        if (!needs_decoder) {
                            continue;
                        }
                        decoder.reset();
                        decoder_config.codec = info.codec;
                        decoder = chast::create_decoder(decoder_config, &error);
                        if (!decoder) {
                            fail("decoder (" + codec_name(info.codec) + "): " + error);
                            return false;
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.decoder = decoder->implementation();
                        }
                        std::fprintf(stderr, "viewer: decoder %s\n",
                                     decoder->implementation().c_str());
                        // The server forces an IDR when it re-announces, but a
                        // request makes the wait explicit if that one was lost
                        // with the old stream.
                        request_keyframe();
                    }
                    if (applied) {
                        sync_status(false);
                    }
                    return true;
                };

                while (!stop_requested_.load(std::memory_order_acquire)) {
                    if (!apply_stream_updates()) {
                        break;
                    }
                    drain_input();
                    chast::FrameHeader header;
                    std::vector<uint8_t> payload;
                    chast::Error received_frame = session->receive_frame(
                        header, payload,
                        info.input_enabled ? kInputPoll : kReceiveTimeout);
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
                        // The description and the first picture of a rebuilt
                        // stream travel on different QUIC streams, so the
                        // picture can win the race. Look for the update before
                        // treating the header as a violation.
                        if (!apply_stream_updates()) {
                            break;
                        }
                    }
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

    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        input_enabled_ = false;
        pending_input_.clear();
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
        std::fprintf(stderr, "viewer: %s\n", failure.c_str());
        SetState(ViewerState::kFailed, failure);
    } else {
        SetState(ViewerState::kEnded);
    }
    running_.store(false, std::memory_order_release);
}

}  // namespace chast_viewer
