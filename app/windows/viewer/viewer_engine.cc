#include "viewer_engine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "chast/audio.h"
#include "chast/decoder.h"
#include "chast/identity.h"
#include "chast/protocol.h"
#include "chast/quic.h"

namespace chast_viewer {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kReceiveTimeout = std::chrono::milliseconds(250);
// Input and audio are drained once per receive poll, so an idle desktop (where
// the receive timeout is the only thing pacing the loop) still carries pointer
// motion and audio frames within this bound.
constexpr auto kMediaPoll = std::chrono::milliseconds(10);
constexpr auto kFrameStall = std::chrono::seconds(20);
constexpr auto kKeyframeRetry = std::chrono::seconds(1);
constexpr auto kProbeInterval = std::chrono::seconds(1);
constexpr size_t kMaxPendingInput = 256;
constexpr size_t kMaxPendingClipboard = 16;
constexpr size_t kMaxReceivedClipboard = 16;

std::string codec_name(chast::VideoCodec codec) {
    return codec == chast::VideoCodec::H264 ? "H.264" : "HEVC";
}

int64_t steady_us(std::chrono::steady_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
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
    {
        std::lock_guard<std::mutex> lock(clipboard_mutex_);
        pending_clipboard_.clear();
        received_clipboard_.clear();
        clipboard_enabled_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(cursor_mutex_);
        pending_cursor_.reset();
        cursor_updates_ = 0;
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

bool ViewerEngine::SendClipboard(const std::string& text) {
    if (!running_.load(std::memory_order_acquire) || !chast::valid_clipboard_text(text)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(clipboard_mutex_);
    if (!clipboard_enabled_) {
        return false;
    }
    if (pending_clipboard_.size() >= kMaxPendingClipboard) {
        pending_clipboard_.pop_front();
    }
    pending_clipboard_.push_back(text);
    return true;
}

bool ViewerEngine::TakeClipboard(std::string& text) {
    std::lock_guard<std::mutex> lock(clipboard_mutex_);
    if (received_clipboard_.empty()) {
        return false;
    }
    text = std::move(received_clipboard_.front());
    received_clipboard_.pop_front();
    return true;
}

bool ViewerEngine::TakeCursor(chast::CursorUpdate& update) {
    std::lock_guard<std::mutex> lock(cursor_mutex_);
    if (!pending_cursor_) {
        return false;
    }
    update = std::move(*pending_cursor_);
    pending_cursor_.reset();
    std::lock_guard<std::mutex> status_lock(mutex_);
    status_.cursor_pending = false;
    return true;
}

bool ViewerEngine::SetAudioMuted(bool mute) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status_.audio_enabled) {
            return false;
        }
    }
    audio_muted_.store(mute, std::memory_order_relaxed);
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
    auto last_probe = started - kProbeInterval;
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
    bool clipboard_enabled = false;
    uint64_t clipboard_sent = 0;
    uint64_t clipboard_received = 0;
    uint64_t clipboard_failed = 0;
    uint64_t cursor_updates = 0;
    bool audio_announced = false;
    bool audio_finished = false;
    bool audio_broken = false;
    bool audio_enabled = false;
    bool audio_muted_applied = false;
    uint32_t audio_sample_rate = 0;
    uint32_t audio_channels = 0;
    uint32_t audio_bitrate = 0;
    uint64_t audio_received = 0;
    uint64_t audio_decoded = 0;
    uint64_t audio_errors = 0;
    uint64_t audio_underruns = 0;
    uint64_t audio_recoveries = 0;
    bool audio_recovering = false;
    uint32_t audio_buffer_ms = 0;
    std::string audio_error;
    double rtt_ms = 0.0;
    double latency_ms = 0.0;
    bool clock_synced = false;
    int64_t clock_offset_us = 0;
    uint64_t digest = chast::kFrameDigestSeed;
    uint64_t window_frames = 0;
    uint64_t window_bytes = 0;
    uint32_t display_width = 0;
    uint32_t display_height = 0;
    // Pointer and input space: the captured desktop size, unchanged by the
    // host's resolution ladder. Display sizes above follow the decoded
    // picture.
    uint32_t display_source_width = 0;
    uint32_t display_source_height = 0;
    bool video_finished = false;
    bool handshaken = false;
    bool logged_frame = false;
    bool logged_drop = false;
    uint64_t init_errors = 0;
    std::string failure;
    std::unique_ptr<chast::IAudioDecoder> audio_decoder;
    std::unique_ptr<chast::IAudioPlayer> audio_player;

    // Folds the worker's counters into the status the UI polls. Called from
    // this thread only; the window counters are reset once per second.
    auto sync_status = [&](bool final) {
        const auto now = Clock::now();
        const double window_seconds =
            std::chrono::duration<double>(now - window_start).count();
        double fps = 0.0;
        double kbps = 0.0;
        if (audio_player) {
            const chast::AudioPlayerStats audio_stats = audio_player->stats();
            audio_buffer_ms = audio_stats.buffer_ms;
            audio_underruns = audio_stats.underruns;
            audio_recoveries = audio_stats.recoveries;
            audio_recovering = audio_stats.recovering;
        }
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
            status_.rtt_ms = rtt_ms;
            status_.latency_ms = latency_ms;
            status_.width = display_width;
            status_.height = display_height;
            status_.source_width = display_source_width;
            status_.source_height = display_source_height;
            status_.input_sent = input_sent;
            status_.input_failed = input_failed;
            status_.clipboard_enabled = clipboard_enabled;
            status_.clipboard_sent = clipboard_sent;
            status_.clipboard_received = clipboard_received;
            status_.clipboard_failed = clipboard_failed;
            status_.cursor_updates = cursor_updates;
            status_.audio_announced = audio_announced;
            status_.audio_enabled = audio_enabled;
            status_.audio_muted = audio_muted_.load(std::memory_order_relaxed);
            status_.audio_sample_rate = audio_sample_rate;
            status_.audio_channels = audio_channels;
            status_.audio_bitrate = audio_bitrate;
            status_.audio_received = audio_received;
            status_.audio_decoded = audio_decoded;
            status_.audio_errors = audio_errors;
            status_.audio_underruns = audio_underruns;
            status_.audio_recoveries = audio_recoveries;
            status_.audio_recovering = audio_recovering;
            status_.audio_buffer_ms = audio_buffer_ms;
            status_.audio_error = audio_error;
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
            display_source_width = info.source_width;
            display_source_height = info.source_height;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.codec = info.codec;
                status_.width = info.width;
                status_.height = info.height;
                status_.source_width = info.source_width;
                status_.source_height = info.source_height;
                status_.framerate = info.framerate;
                status_.server_bitrate = info.bitrate;
                status_.server_encoder = info.encoder;
                status_.input_enabled = info.input_enabled;
                status_.clipboard_enabled = info.clipboard_enabled;
                status_.origin_x = info.origin_x;
                status_.origin_y = info.origin_y;
                status_.displays = info.displays;
            }
            {
                std::lock_guard<std::mutex> lock(input_mutex_);
                input_enabled_ = info.input_enabled;
            }
            clipboard_enabled = info.clipboard_enabled;
            {
                std::lock_guard<std::mutex> lock(clipboard_mutex_);
                clipboard_enabled_ = clipboard_enabled;
                if (!clipboard_enabled) {
                    pending_clipboard_.clear();
                }
            }

            // Audio is optional end to end: a missing render endpoint or a
            // decoder that cannot start leaves video untouched and reports
            // why through audio_error.
            auto stop_audio_pipeline = [&] {
                if (audio_player) {
                    const chast::AudioPlayerStats stats = audio_player->stats();
                    audio_buffer_ms = stats.buffer_ms;
                    audio_underruns = stats.underruns;
                    audio_recoveries = stats.recoveries;
                    audio_recovering = stats.recovering;
                    audio_player->stop();
                    audio_player.reset();
                }
                audio_decoder.reset();
                audio_enabled = false;
            };
            auto start_audio = [&](const chast::AudioInfo& audio) {
                audio_announced = audio.enabled;
                audio_finished = false;
                audio_broken = false;
                audio_sample_rate = audio.sample_rate;
                audio_channels = audio.channels;
                audio_bitrate = audio.bitrate;
                if (!audio.enabled) {
                    return;
                }
                std::string audio_setup_error;
                audio_decoder = chast::create_aac_decoder(audio, &audio_setup_error);
                if (!audio_decoder) {
                    audio_error = "decoder: " + audio_setup_error;
                    std::fprintf(stderr, "viewer: audio %s\n", audio_error.c_str());
                    return;
                }
                chast::AudioPlayerConfig player_config;
                audio_player =
                    chast::create_audio_player(player_config, &audio_setup_error);
                if (!audio_player) {
                    audio_decoder.reset();
                    audio_error = "playback: " + audio_setup_error;
                    std::fprintf(stderr, "viewer: audio %s\n", audio_error.c_str());
                    return;
                }
                const bool muted = audio_muted_.load(std::memory_order_relaxed);
                audio_player->set_mute(muted);
                audio_muted_applied = muted;
                chast::Error audio_started = audio_player->start();
                if (!audio_started.ok()) {
                    audio_error = "playback: " + audio_started.to_string();
                    std::fprintf(stderr, "viewer: audio %s\n", audio_error.c_str());
                    audio_player.reset();
                    audio_decoder.reset();
                    return;
                }
                audio_enabled = true;
                audio_error.clear();
                std::fprintf(stderr,
                             "viewer: audio AAC-LC %u Hz %u ch %u kbps, decoder and "
                             "renderer running\n",
                             audio.sample_rate, audio.channels, audio.bitrate / 1000u);
            };
            start_audio(info.audio);

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
                uint64_t clipboard_seq = 1;
                std::vector<chast::DecodedFrame> frames;

                // Sends the texts the UI queued since the last poll and moves
                // every received host text into the UI queue. Runs on the
                // session thread, like drain_input().
                auto drain_clipboard = [&] {
                    std::deque<std::string> texts;
                    {
                        std::lock_guard<std::mutex> lock(clipboard_mutex_);
                        if (!clipboard_enabled) {
                            pending_clipboard_.clear();
                            return;
                        }
                        texts.swap(pending_clipboard_);
                    }
                    for (const std::string& text : texts) {
                        chast::Error sent = session->send_clipboard(clipboard_seq, text);
                        if (sent.ok()) {
                            ++clipboard_seq;
                            ++clipboard_sent;
                            continue;
                        }
                        ++clipboard_failed;
                        if (clipboard_failed <= 3) {
                            std::fprintf(stderr, "viewer: send_clipboard: %s\n",
                                         sent.to_string().c_str());
                        }
                    }
                };

                auto receive_clipboard = [&] {
                    chast::ClipboardUpdate update;
                    while (clipboard_enabled && session->take_clipboard_update(update)) {
                        {
                            std::lock_guard<std::mutex> lock(clipboard_mutex_);
                            if (received_clipboard_.size() >= kMaxReceivedClipboard) {
                                received_clipboard_.pop_front();
                            }
                            received_clipboard_.push_back(std::move(update.text));
                        }
                        ++clipboard_received;
                    }
                };

                // Moves the newest pointer state the host sent into the slot
                // the UI takes from. A newer update replaces an older one that
                // was never taken, so the UI always sees the current position.
                auto receive_cursor = [&] {
                    chast::CursorUpdate update;
                    bool any = false;
                    while (session->take_cursor_update(update)) {
                        any = true;
                        ++cursor_updates;
                        std::lock_guard<std::mutex> lock(cursor_mutex_);
                        pending_cursor_ = std::move(update);
                        ++cursor_updates_;
                    }
                    if (any) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.cursor_updates = cursor_updates;
                        status_.cursor_pending = true;
                    }
                };

                // Drains every complete audio packet that arrived since the
                // last poll. Audio never fails the session: a malformed packet
                // or a dead render endpoint stops local playback and video
                // keeps running.
                auto drain_audio = [&] {
                    if (!audio_announced || audio_finished || audio_broken) {
                        return;
                    }
                    chast::AudioHeader audio_header;
                    std::vector<uint8_t> audio_payload;
                    for (;;) {
                        chast::Error next = session->receive_audio(
                            audio_header, audio_payload, std::chrono::milliseconds(0));
                        if (next.status() == chast::Status::WaitTimeout) {
                            break;
                        }
                        if (next.status() == chast::Status::Unavailable) {
                            audio_finished = true;
                            audio_announced = false;
                            break;
                        }
                        if (!next.ok()) {
                            // The parser cannot resynchronise after a bad
                            // header, so stop consuming rather than spin on
                            // the same error.
                            ++audio_errors;
                            audio_broken = true;
                            audio_error = "protocol: " + next.to_string();
                            std::fprintf(stderr, "viewer: audio %s\n", audio_error.c_str());
                            break;
                        }
                        ++audio_received;
                        if (!audio_player || !audio_decoder) {
                            // Local playback is unavailable; keep consuming so
                            // the transport queue cannot grow.
                            continue;
                        }
                        if (audio_header.payload_size != audio_payload.size() ||
                            chast::crc32(audio_payload.data(), audio_payload.size()) !=
                                audio_header.payload_crc32) {
                            ++audio_errors;
                            continue;
                        }
                        std::vector<chast::DecodedAudio> audio_blocks;
                        chast::Error audio_decode = audio_decoder->decode(
                            audio_header, audio_payload.data(), audio_payload.size(),
                            audio_blocks);
                        if (!audio_decode.ok()) {
                            ++audio_errors;
                            if (audio_errors <= 3) {
                                std::fprintf(stderr, "viewer: audio decode: %s\n",
                                             audio_decode.to_string().c_str());
                            }
                            continue;
                        }
                        ++audio_decoded;
                        for (const chast::DecodedAudio& block : audio_blocks) {
                            chast::Error pushed = audio_player->push(block);
                            if (!pushed.ok()) {
                                ++audio_errors;
                                if (audio_errors <= 3) {
                                    std::fprintf(stderr, "viewer: audio push: %s\n",
                                                 pushed.to_string().c_str());
                                }
                                break;
                            }
                        }
                        if (!audio_player->error().empty()) {
                            break;
                        }
                    }
                    if (audio_player) {
                        const bool muted = audio_muted_.load(std::memory_order_relaxed);
                        if (muted != audio_muted_applied) {
                            audio_player->set_mute(muted);
                            audio_muted_applied = muted;
                        }
                        const std::string player_error = audio_player->error();
                        if (!player_error.empty()) {
                            audio_error = "playback: " + player_error;
                            std::fprintf(stderr, "viewer: audio stopped: %s\n",
                                         player_error.c_str());
                            stop_audio_pipeline();
                        }
                    }
                    if ((audio_finished || audio_broken) && (audio_player || audio_decoder)) {
                        stop_audio_pipeline();
                    }
                };

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
                        display_source_width = info.source_width;
                        display_source_height = info.source_height;
                        if (!info.audio.enabled) {
                            if (audio_announced || audio_player || audio_decoder) {
                                audio_announced = false;
                                audio_finished = true;
                                stop_audio_pipeline();
                                std::fprintf(stderr, "viewer: the host stopped audio\n");
                            }
                        } else if (!audio_announced) {
                            start_audio(info.audio);
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.codec = info.codec;
                            status_.framerate = info.framerate;
                            status_.server_bitrate = info.bitrate;
                            status_.server_encoder = info.encoder;
                            status_.input_enabled = info.input_enabled;
                            status_.clipboard_enabled = info.clipboard_enabled;
                            status_.origin_x = info.origin_x;
                            status_.origin_y = info.origin_y;
                            status_.displays = info.displays;
                        }
                        {
                            std::lock_guard<std::mutex> lock(input_mutex_);
                            input_enabled_ = info.input_enabled;
                        }
                        clipboard_enabled = info.clipboard_enabled;
                        {
                            std::lock_guard<std::mutex> lock(clipboard_mutex_);
                            clipboard_enabled_ = clipboard_enabled;
                            if (!clipboard_enabled) {
                                pending_clipboard_.clear();
                            }
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
                    {
                        const auto probe_now = Clock::now();
                        if (probe_now - last_probe >= kProbeInterval) {
                            last_probe = probe_now;
                            session->send_latency_probe();
                            const chast::LatencySample sample = session->latency();
                            if (sample.valid) {
                                rtt_ms = static_cast<double>(sample.rtt_us) / 1000.0;
                                clock_offset_us = sample.offset_us;
                                clock_synced = true;
                                if (audio_player) {
                                    audio_player->set_clock_offset(clock_offset_us / 1000);
                                }
                            }
                        }
                    }
                    drain_input();
                    drain_clipboard();
                    receive_clipboard();
                    receive_cursor();
                    drain_audio();
                    chast::FrameHeader header;
                    std::vector<uint8_t> payload;
                    const bool fast_media_poll =
                        info.input_enabled || info.clipboard_enabled || audio_announced;
                    chast::Error received_frame = session->receive_frame(
                        header, payload, fast_media_poll ? kMediaPoll : kReceiveTimeout);
                    // Audio packets arrive on their own stream and never wake
                    // receive_frame(), so drain them as soon as the wait ends.
                    drain_audio();
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
                    if (clock_synced && header.capture_ms != 0) {
                        const int64_t host_now_us = steady_us(now) + clock_offset_us;
                        const int64_t age_us =
                            host_now_us - static_cast<int64_t>(header.capture_ms) * 1000;
                        // A wild value means the offset is stale (for example
                        // after a long receive stall); keep the last good one
                        // instead of showing a bogus age.
                        if (age_us > -100000 && age_us < 10000000) {
                            latency_ms = static_cast<double>(std::max<int64_t>(age_us, 0)) / 1000.0;
                        }
                    }
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
    {
        std::lock_guard<std::mutex> lock(clipboard_mutex_);
        pending_clipboard_.clear();
        received_clipboard_.clear();
        clipboard_enabled_ = false;
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
