// The capture and session headers pull in windows.h, whose min/max macros
// collide with std::min/std::max below.
#define NOMINMAX

#include "chast/host_server.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

#include "chast/audio.h"
#include "chast/bitrate_controller.h"
#include "chast/capture.h"
#include "chast/clipboard.h"

namespace chast {

namespace {

using Clock = std::chrono::steady_clock;

// FrameHeader::capture_ms is 48 bits: milliseconds since the steady clock's
// epoch, which is the same clock the latency probe reports.
uint64_t capture_clock_ms(Clock::time_point time) {
    return static_cast<uint64_t>(
               std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch())
                   .count()) &
           0xFFFFFFFFFFFFull;
}

uint32_t bitrate_floor(uint32_t ceiling) {
    return std::min(ceiling, std::max<uint32_t>(500000u, ceiling / 10u));
}

bool wait_for_usable_frame(IDxgiCapturer& capturer, bool quiet, const std::atomic<bool>& stopping,
                           std::string* error) {
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < deadline && !stopping.load()) {
        const CaptureResult result = capturer.capture(std::chrono::milliseconds(100));
        switch (result) {
            case CaptureResult::NoNewFrame:
                continue;
            case CaptureResult::ReinitNeeded:
                if (!quiet) {
                    std::printf("reinitializing: %s\n",
                                capturer.last_error().to_string().c_str());
                }
                if (Error err = capturer.reinitialize(); !err.ok()) {
                    *error = "reinit failed: " + err.to_string();
                    return false;
                }
                continue;
            case CaptureResult::FatalError:
                *error = "capture failed: " + capturer.last_error().to_string();
                return false;
            case CaptureResult::Frame:
                break;
        }
        const FrameInfo& frame = capturer.frame();
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

// The description HelloAck and StreamInfo carry. `display` is the output the
// capturer is reading, `attached` the full list from the moment of the call.
ServerInfo describe_stream(const std::vector<DisplayInfo>& attached,
                           const DisplayInfo& display, const IVideoEncoder& encoder,
                           uint16_t framerate, bool clipboard_enabled,
                           const AudioInfo& audio) {
    ServerInfo info;
    info.codec = encoder.codec();
    info.width = static_cast<uint16_t>(encoder.width());
    info.height = static_cast<uint16_t>(encoder.height());
    info.framerate = framerate;
    info.bitrate = encoder.bitrate();
    info.origin_x = display.rect.left;
    info.origin_y = display.rect.top;
    info.clipboard_enabled = clipboard_enabled;
    info.audio = audio;
    info.encoder = encoder.implementation().empty() ? "oneVPL" : encoder.implementation();
    for (const DisplayInfo& listed : attached) {
        DisplayDescription described;
        described.name = display_name_utf8(listed);
        described.left = listed.rect.left;
        described.top = listed.rect.top;
        described.right = listed.rect.right;
        described.bottom = listed.rect.bottom;
        described.rotation_degrees = static_cast<uint16_t>(listed.rotation_degrees);
        described.primary = listed.primary;
        info.displays.push_back(std::move(described));
    }
    return info;
}

size_t find_display(const std::vector<DisplayInfo>& attached, const DisplayInfo& display) {
    for (size_t i = 0; i < attached.size(); ++i) {
        if (attached[i].device_name == display.device_name) {
            return i;
        }
    }
    return 0;
}

const char* codec_name(VideoCodec codec) {
    return codec == VideoCodec::Hevc ? "hevc" : "h264";
}

}  // namespace

std::unique_ptr<HostServer> HostServer::create(HostServerConfig config, std::string* error) {
    if (!config.clipboard_read) {
        config.clipboard_read = [](std::string& text) { return read_clipboard_text(text); };
    }
    if (!config.clipboard_write) {
        config.clipboard_write = [](const std::string& text) { return write_clipboard_text(text); };
    }
    std::unique_ptr<HostServer> server(new HostServer(std::move(config)));
    if (server->settings_.access_token.empty()) {
        server->settings_.access_token = generate_access_token();
        if (server->config_.token_changed) {
            server->config_.token_changed(server->settings_.access_token);
        }
    }
    if (!server->open_session(error)) {
        return nullptr;
    }
    return server;
}

HostServer::~HostServer() {
    stop();
    session_.reset();
}

void HostServer::start() {
    if (started_) {
        return;
    }
    started_ = true;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void HostServer::stop() {
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void HostServer::wait() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool HostServer::running() const {
    return running_.load();
}

HostStatus HostServer::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

std::string HostServer::token() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_.token;
}

bool HostServer::open_session(std::string* error) {
    QuicServerConfig quic;
    quic.bind_address = settings_.bind_address;
    quic.port = settings_.port;
    quic.identity_scope = config_.identity_scope;
    quic.allow_input = settings_.allow_input;
    quic.allow_clipboard = settings_.allow_clipboard;
    quic.allow_audio = settings_.allow_audio;
    quic.dry_run_input = config_.dry_run_input;
    quic.access_token = settings_.access_token;
    quic.reuse_access_token = settings_.token_mode == HostTokenMode::Reusable;
    quic.approve_client = config_.approve_client;
    session_ = QuicSession::create_server(quic, error);
    if (!session_) {
        return false;
    }
    const std::string fingerprint = session_->server_fingerprint();
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.fingerprint = fingerprint;
    status_.token = settings_.access_token;
    return true;
}

void HostServer::run() {
    HostServerStats stats;
    uint32_t failures = 0;
    while (!stopping_.load()) {
        if (!session_) {
            std::string error;
            if (!open_session(&error)) {
                log("listener failed: " + error);
                publish_state(HostState::Failed, "QUIC server: " + error);
                if (config_.serve_once) {
                    break;
                }
                if (config_.max_consecutive_failures != 0 &&
                    ++failures >= config_.max_consecutive_failures) {
                    break;
                }
                sleep_unless_stopping(std::chrono::seconds(2));
                continue;
            }
        }
        publish_state(HostState::Listening);
        log("listening on " + settings_.bind_address + ":" + std::to_string(settings_.port));

        const bool first = stats.sessions == 0;
        const auto deadline =
            first && config_.first_client_timeout_ms > 0
                ? Clock::now() + std::chrono::milliseconds(config_.first_client_timeout_ms)
                : Clock::time_point::max();
        bool accepted = false;
        std::string wait_error;
        while (!stopping_.load()) {
            auto slice = std::chrono::milliseconds(500);
            if (deadline != Clock::time_point::max()) {
                const auto now = Clock::now();
                if (now >= deadline) {
                    wait_error = "no client connected in " +
                                 std::to_string(config_.first_client_timeout_ms / 1000) + " s";
                    break;
                }
                slice = std::min(slice, std::chrono::duration_cast<std::chrono::milliseconds>(
                                            deadline - now));
            }
            Error waited = session_->wait_for_client(slice);
            if (waited.ok()) {
                accepted = true;
                break;
            }
            if (waited.status() != Status::WaitTimeout) {
                wait_error = "waiting for a client: " + waited.to_string();
                break;
            }
        }
        if (!accepted) {
            if (stopping_.load()) {
                break;
            }
            log(wait_error);
            publish_state(HostState::Failed, wait_error);
            session_.reset();
            if (config_.serve_once) {
                break;
            }
            if (config_.max_consecutive_failures != 0 &&
                ++failures >= config_.max_consecutive_failures) {
                break;
            }
            sleep_unless_stopping(std::chrono::seconds(2));
            continue;
        }

        failures = 0;
        ++stats.sessions;
        stats.client_connected = true;
        const std::string peer = session_->client_address();
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.peer_address = peer;
            status_.requested_display = session_->requested_display();
            status_.display_index = 0;
            status_.display_name.clear();
            status_.stream = ServerInfo{};
        }
        publish_stats(stats);
        publish_state(HostState::Streaming);
        log(peer.empty() ? "a viewer connected" : "a viewer connected from " + peer);

        std::string error;
        bool peer_left = false;
        const bool ok = stream_session(stats, &error, &peer_left);

        ClientStats peer_stats;
        bool stats_received = false;
        const auto finish_timeout = std::chrono::milliseconds(
            ok ? config_.finish_timeout_ms : std::min<uint32_t>(config_.finish_timeout_ms, 2000));
        Error finished = session_->finish_server(finish_timeout, peer_stats, stats_received);
        stats.peer_left = peer_left;
        stats.stats_received = stats_received;
        if (stats_received) {
            stats.peer = peer_stats;
        }
        for (const InputEvent& event : session_->take_input_events()) {
            ++stats.input_events;
            if (config_.input_event) {
                config_.input_event(event);
            }
        }
        stats.input_stats = session_->input_stats();
        stats.input_releases = session_->input_release_requests();
        publish_stats(stats);

        if (ok && !stopping_.load()) {
            // A viewer that sends its closing Stats/Bye before the video send
            // fails left on purpose, even though the send then reports the
            // peer gone.
            log(peer_left && !stats_received ? "the viewer disappeared" : "the viewer left");
        }
        if (!finished.ok() && !peer_left && error.empty() && !stopping_.load()) {
            error = "finish: " + finished.to_string();
        }
        if (!ok || !error.empty()) {
            log(error.empty() ? "the session ended" : error);
            publish_state(HostState::Failed, error.empty() ? "the session failed" : error);
            session_.reset();
            if (config_.serve_once) {
                break;
            }
            if (config_.max_consecutive_failures != 0 &&
                ++failures >= config_.max_consecutive_failures) {
                break;
            }
            sleep_unless_stopping(std::chrono::seconds(2));
            continue;
        }

        // A spent one-time token must not guard the next connection, so it is
        // replaced while the listener is between viewers.
        if (settings_.token_mode == HostTokenMode::OneTime && !config_.serve_once) {
            settings_.access_token = generate_access_token();
            session_->set_access_token(settings_.access_token);
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.token = settings_.access_token;
            }
            if (config_.token_changed) {
                config_.token_changed(settings_.access_token);
            }
            log("the one-time token was spent; a new token is ready");
        }

        session_->reset_for_next_client();
        if (config_.serve_once) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (status_.state != HostState::Failed) {
            status_.state = HostState::Stopped;
        }
    }
    running_ = false;
}

bool HostServer::stream_session(HostServerStats& stats, std::string* error, bool* peer_left) {
    *peer_left = false;
    stats.audio_active = false;
    const std::vector<DisplayInfo> attached = enumerate_displays();
    if (attached.empty()) {
        *error = "no displays are attached";
        return false;
    }

    size_t display_index = attached.size();
    const uint16_t requested = session_->requested_display();
    if (requested != kDisplayAuto) {
        display_index = requested;
        if (display_index >= attached.size()) {
            *error = "the client asked for display " + std::to_string(display_index) +
                     ", but only " + std::to_string(attached.size()) + " are attached";
            return false;
        }
    } else if (settings_.display != kDisplayAuto) {
        display_index = settings_.display;
        if (display_index >= attached.size()) {
            *error = "display " + std::to_string(display_index) +
                     " from the settings is not attached (" + std::to_string(attached.size()) +
                     " present)";
            return false;
        }
    } else {
        display_index = 0;
        for (size_t i = 0; i < attached.size(); ++i) {
            if (attached[i].primary) {
                display_index = i;
                break;
            }
        }
    }

    CaptureConfig capture;
    capture.want_hdr = config_.want_hdr;
    capture.capture_cursor = config_.capture_cursor;
    capture.nv12.enabled = true;
    capture.nv12.colorimetry = Nv12Colorimetry::Bt601Limited;

    std::unique_ptr<IDxgiCapturer> capturer =
        create_capturer(display_index, capture, error);
    if (!capturer) {
        *error = "capturer creation failed: " + *error;
        return false;
    }
    if (!wait_for_usable_frame(*capturer, true, stopping_, error)) {
        return false;
    }
    const FrameInfo& first = capturer->frame();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.display_index = find_display(attached, capturer->display());
        status_.display_name = display_name_utf8(capturer->display());
    }
    log("capturing display " + std::to_string(find_display(attached, capturer->display())) + " (" +
        display_name_utf8(capturer->display()) + ")");

    EncoderConfig encoder_config;
    encoder_config.codec = settings_.codec;
    encoder_config.bitrate = settings_.bitrate_kbps * 1000u;
    encoder_config.framerate = settings_.framerate;
    encoder_config.quality_ratio = settings_.quality;
    encoder_config.gop_size = 0;
    encoder_config.width = first.width;
    encoder_config.height = first.height;
    const AdapterDevice adapter = capturer->adapter_device();
    std::unique_ptr<IVideoEncoder> encoder =
        create_encoder(adapter.device, encoder_config, error);
    if (!encoder) {
        *error = "encoder creation failed: " + *error;
        return false;
    }
    if (encoder->codec() != settings_.codec) {
        log(std::string("the encoder fell back: requested ") + codec_name(settings_.codec) +
            ", producing " + codec_name(encoder->codec()));
    }
    log("encoder: " + (encoder->implementation().empty() ? std::string("oneVPL")
                                                         : encoder->implementation()) +
        ", " + std::to_string(encoder->width()) + "x" + std::to_string(encoder->height()) +
        ", target " + std::to_string(encoder->bitrate() / 1000u) + " kbps");

    // The encoder's starting bitrate is the ceiling: the operator's configured
    // rate, or the hardware default when the settings leave it at zero. The
    // controller only ever lowers it and later climbs back to this value.
    BitrateController abr(encoder->bitrate(), bitrate_floor(encoder->bitrate()));
    stats.target_bitrate = encoder->bitrate();

    // System sound is optional: the endpoint and the AAC encoder are opened
    // before the announcement so AudioInfo describes what will actually be
    // encoded, and a machine without a usable endpoint simply streams video.
    std::unique_ptr<ILoopbackCapture> audio_capture;
    AudioInfo audio_info;
    if (settings_.allow_audio) {
        std::string audio_error;
        if (config_.audio_capture) {
            audio_capture = config_.audio_capture(&audio_error);
        } else {
            LoopbackCaptureConfig audio_config;
            audio_capture = create_loopback_capture(audio_config, &audio_error);
        }
        if (audio_capture) {
            audio_info = audio_capture->info();
            log("audio: AAC-LC " + std::to_string(audio_info.sample_rate / 1000u) + " kHz, " +
                std::to_string(audio_info.channels) + " ch, " +
                std::to_string(audio_info.bitrate / 1000u) + " kbps");
        } else {
            ++stats.audio_errors;
            log("audio unavailable: " + audio_error);
        }
    }
    stats.audio_active = audio_info.enabled;

    ServerInfo stream_info =
        describe_stream(attached, capturer->display(), *encoder, settings_.framerate,
                        settings_.allow_clipboard, audio_info);
    publish_stream(stream_info, find_display(attached, capturer->display()),
                   display_name_utf8(capturer->display()));
    if (config_.stream_announced) {
        config_.stream_announced(stream_info);
    }
    Error announced = session_->announce(stream_info);
    if (!announced.ok()) {
        *error = "announce: " + announced.to_string();
        return false;
    }
    if (audio_capture) {
        audio_capture->start();
    }
    if (!run_clock_started_) {
        run_clock_started_ = true;
        run_start_ = Clock::now();
    }

    const auto start = run_start_;
    const auto frame_interval =
        settings_.framerate > 0
            ? std::chrono::duration_cast<Clock::duration>(
                  std::chrono::duration<double>(1.0 / static_cast<double>(settings_.framerate)))
            : Clock::duration::zero();
    auto next_frame_due = Clock::now() + frame_interval;
    auto last_keyframe = Clock::now();
    const auto next_reannounce = start + std::chrono::seconds(2);
    bool reannounced = false;
    bool force_keyframe = true;
    bool have_input = false;
    std::vector<EncodedPacket> packets;

    // Clipboard baseline: remember what the host clipboard held when the
    // session started and upload only changes after that, never its history.
    const bool clipboard_enabled = settings_.allow_clipboard;
    std::string last_clipboard;
    if (clipboard_enabled) {
        std::string current;
        if (config_.clipboard_read(current)) {
            last_clipboard = std::move(current);
        }
    }
    uint64_t clipboard_seq = 1;
    auto next_clipboard_poll = Clock::now() + std::chrono::milliseconds(500);

    auto drain_peer = [&] {
        for (const InputEvent& event : session_->take_input_events()) {
            ++stats.input_events;
            if (config_.input_event) {
                config_.input_event(event);
            }
        }
        stats.input_stats = session_->input_stats();
        stats.input_releases = session_->input_release_requests();
        if (!clipboard_enabled) {
            return;
        }
        ClipboardUpdate update;
        while (session_->take_clipboard_update(update)) {
            if (update.text == last_clipboard) {
                continue;
            }
            if (config_.clipboard_write(update.text)) {
                // Remembering the applied text is what stops the next poll
                // from sending it straight back to the viewer.
                last_clipboard = std::move(update.text);
                ++stats.clipboard_received;
            }
        }
    };

    auto poll_clipboard = [&](Clock::time_point now) {
        if (!clipboard_enabled || now < next_clipboard_poll) {
            return;
        }
        next_clipboard_poll = now + std::chrono::milliseconds(500);
        std::string current;
        if (!config_.clipboard_read(current) || current == last_clipboard) {
            return;
        }
        // Remember text we cannot put on the wire (too large, embedded NUL)
        // so every later tick does not retry the same value.
        const bool sendable = valid_clipboard_text(current);
        last_clipboard = std::move(current);
        if (!sendable) {
            return;
        }
        Error sent = session_->send_clipboard(clipboard_seq, last_clipboard);
        if (!sent.ok()) {
            ++stats.send_errors;
            log("clipboard send failed: " + sent.to_string());
            return;
        }
        ++clipboard_seq;
        ++stats.clipboard_sent;
    };

    // Audio degrades on its own: a failed endpoint, encoder or send turns the
    // stream off with a StreamInfo and leaves video running.
    auto disable_audio = [&](const std::string& reason) {
        audio_capture->stop();
        audio_capture.reset();
        audio_info = AudioInfo{};
        stats.audio_active = false;
        stream_info.audio = audio_info;
        publish_stream(stream_info, find_display(attached, capturer->display()),
                       display_name_utf8(capturer->display()));
        Error refreshed = session_->refresh_stream(stream_info);
        if (!refreshed.ok()) {
            log("audio off: could not announce it: " + refreshed.to_string());
        }
        Error stopped = session_->stop_audio();
        if (!stopped.ok() && stopped.status() != Status::Unavailable &&
            stopped.status() != Status::InvalidCall) {
            log("audio off: " + stopped.to_string());
        }
        log("audio stopped: " + reason);
    };

    uint64_t audio_send_drops = 0;
    uint64_t audio_recoveries_logged = 0;
    bool audio_recovering_logged = false;
    const uint64_t audio_base_ms = capture_clock_ms(start);
    auto drain_audio = [&] {
        if (!audio_capture) {
            return;
        }
        EncodedAudio packet;
        while (audio_capture->take_packet(packet)) {
            const uint64_t host_ms = packet.capture_ms & 0xFFFFFFFFFFFFull;
            AudioHeader header;
            header.pts_ms = host_ms >= audio_base_ms ? host_ms - audio_base_ms : 0;
            header.capture_ms = host_ms;
            header.sample_rate = audio_info.sample_rate;
            header.frame_samples = audio_info.frame_samples;
            header.channels = audio_info.channels;
            header.codec = audio_info.codec;
            Error sent = session_->send_audio(header, packet.data.data(), packet.data.size());
            if (!sent.ok()) {
                if (sent.status() == Status::WaitTimeout) {
                    // The shared send queue is full; video owns the path and
                    // this packet is a short gap, not a reason to stop audio.
                    ++audio_send_drops;
                    continue;
                }
                ++stats.audio_errors;
                log("audio send failed: " + sent.to_string());
                disable_audio("send failed");
                return;
            }
            ++stats.audio_packets;
            stats.audio_bytes += packet.data.size();
        }
        const AudioCaptureStats captured = audio_capture->stats();
        stats.audio_dropped = audio_send_drops + captured.dropped;
        stats.audio_capture_recoveries = captured.recoveries;
        stats.audio_gate_transitions = captured.gate_transitions;
        stats.audio_recovering = captured.recovering;
        if (captured.recoveries != audio_recoveries_logged) {
            audio_recoveries_logged = captured.recoveries;
            log("audio capture switched to the default output endpoint");
        }
        if (captured.recovering != audio_recovering_logged) {
            audio_recovering_logged = captured.recovering;
            log(captured.recovering ? "audio capture endpoint lost; retrying"
                                    : "audio capture endpoint recovered");
        }
        if (audio_capture->failed()) {
            ++stats.audio_errors;
            const std::string reason = audio_capture->error();
            disable_audio(reason.empty() ? "capture failed" : reason);
        }
    };

    // Pointer state. Desktop Duplication keeps the pointer out of the desktop
    // image, so the viewer is told the sprite and where it is; the sprite
    // travels once per shape id and later updates carry only the position.
    // The cadence comes from capture(): DXGI produces a frame for every
    // pointer move, so no timer is needed and an idle pointer costs nothing.
    bool cursor_sent_once = false;
    CursorUpdate cursor_sent;
    auto publish_cursor = [&]() {
        if (!config_.capture_cursor) {
            return;
        }
        const CursorInfo& info = capturer->cursor();
        if (!info.position_valid) {
            return;
        }
        const bool shape_known = info.shape_valid && info.content_id != 0;
        CursorUpdate update;
        update.visible = info.visible;
        update.x = info.position.x;
        update.y = info.position.y;
        // A pointer whose shape the decoder has not seen yet (the first frames
        // of a session) still moves; reuse the geometry already sent so the
        // viewer can keep placing the sprite it holds.
        if (shape_known) {
            update.shape_id = info.content_id;
            update.width = static_cast<uint16_t>(info.width);
            update.height = static_cast<uint16_t>(info.height);
            update.hotspot_x = static_cast<uint16_t>(info.hotspot.x);
            update.hotspot_y = static_cast<uint16_t>(info.hotspot.y);
        } else if (cursor_sent_once) {
            update.shape_id = cursor_sent.shape_id;
            update.width = cursor_sent.width;
            update.height = cursor_sent.height;
            update.hotspot_x = cursor_sent.hotspot_x;
            update.hotspot_y = cursor_sent.hotspot_y;
        } else {
            return;
        }
        const bool new_shape = shape_known && update.shape_id != cursor_sent.shape_id;
        if (new_shape) {
            update.sprite = info.bitmap;
        }
        if (cursor_sent_once && !new_shape && update.visible == cursor_sent.visible &&
            update.x == cursor_sent.x && update.y == cursor_sent.y) {
            return;
        }
        Error sent = session_->send_cursor(update);
        if (!sent.ok()) {
            if (sent.status() == Status::WaitTimeout) {
                // The control queue is full; the next frame carries the same
                // state again, so nothing is lost by skipping this one.
                ++stats.cursor_dropped;
                return;
            }
            ++stats.cursor_errors;
            if (stats.cursor_errors <= 3) {
                log("cursor send failed: " + sent.to_string());
            }
            return;
        }
        ++stats.cursor_sent;
        if (new_shape) {
            ++stats.cursor_shapes;
            stats.cursor_bytes += update.sprite.size();
        }
        cursor_sent = std::move(update);
        cursor_sent_once = true;
    };

    auto process_frame = [&](uint64_t pts_ms, uint64_t capture_ms) -> bool {
        const FrameInfo& frame = capturer->frame();
        if (!frame.nv12_texture) {
            return true;
        }
        if (frame.rotation_degrees != 0) {
            *error = "capture lost rotation while streaming";
            return false;
        }
        packets.clear();
        Error err = Error::success();
        if (!frame.nv12_updated && have_input) {
            err = encoder->encode_repeat(pts_ms, force_keyframe, packets);
            if (err.ok()) {
                ++stats.repeats;
            } else if (err.status() == Status::DeviceRemoved ||
                       err.status() == Status::DeviceReset) {
                *error = "encode_repeat: " + err.to_string();
                return false;
            } else {
                packets.clear();
                err = encoder->encode(frame.nv12_texture, pts_ms, force_keyframe, packets);
            }
        } else {
            err = encoder->encode(frame.nv12_texture, pts_ms, force_keyframe, packets);
        }
        if (!err.ok()) {
            *error = "encode: " + err.to_string();
            return false;
        }
        have_input = true;

        for (const EncodedPacket& packet : packets) {
            if (packet.data.empty()) {
                continue;
            }
            FrameHeader header;
            header.pts_ms = packet.pts_ms;
            header.width = encoder->width();
            header.height = encoder->height();
            header.codec = encoder->codec();
            header.keyframe = packet.keyframe;
            header.idr = packet.idr;
            header.capture_ms = capture_ms;
            Error sent = session_->send_frame(header, packet.data.data(), packet.data.size());
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
                    *peer_left = true;
                } else {
                    ++stats.send_errors;
                    *error = "send_frame: " + sent.to_string();
                }
                return false;
            }
            if (config_.packet) {
                config_.packet(header, packet.data.data(), packet.data.size());
            }
            ++stats.encoded;
            stats.bytes += packet.data.size();
            if (packet.keyframe) {
                ++stats.keyframes;
                last_keyframe = Clock::now();
                force_keyframe = false;
            }
        }
        publish_stats(stats);
        return true;
    };

    // Once a second: link telemetry for the tray, and the bitrate decision
    // that follows from it. Runs on the host thread between frames so the
    // encoder reset set_bitrate performs cannot overlap an encode call.
    auto last_link_sample = Clock::now();
    uint64_t last_sample_bytes = stats.bytes;
    auto sample_link = [&](Clock::time_point now) -> bool {
        const uint32_t elapsed_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_link_sample)
                .count());
        if (elapsed_ms == 0) {
            return true;
        }
        const uint64_t byte_delta =
            stats.bytes >= last_sample_bytes ? stats.bytes - last_sample_bytes : 0;
        // bytes * 8 / milliseconds is exactly kilobits per second.
        stats.send_kbps =
            static_cast<double>(byte_delta) * 8.0 / static_cast<double>(elapsed_ms);
        last_sample_bytes = stats.bytes;
        last_link_sample = now;

        const TransportStats transport = session_->transport_stats();
        if (!transport.available) {
            return true;
        }
        stats.rtt_ms = static_cast<double>(transport.rtt_us) / 1000.0;
        stats.min_rtt_ms = static_cast<double>(transport.min_rtt_us) / 1000.0;
        if (!config_.adaptive_bitrate) {
            return true;
        }

        BitrateSample sample;
        sample.rtt_us = transport.rtt_us;
        sample.min_rtt_us = transport.min_rtt_us;
        sample.queued_bytes = transport.queued_bytes;
        sample.congestion_window = transport.congestion_window;
        sample.sent_bytes = transport.send_bytes;
        sample.lost_packets = transport.lost_packets;
        sample.sent_packets = transport.sent_packets;
        const BitrateDecision decision = abr.update(sample, elapsed_ms);
        if (!decision.changed) {
            return true;
        }

        Error applied = encoder->set_bitrate(decision.target_bps);
        if (!applied.ok()) {
            log("adaptive bitrate: cannot apply " +
                std::to_string(decision.target_bps / 1000u) + " kbps after " + decision.reason +
                ": " + applied.to_string());
            return true;
        }
        stats.target_bitrate = decision.target_bps;
        ++stats.abr_changes;
        // The encoder reset invalidates its reference state.
        force_keyframe = true;
        stream_info.bitrate = decision.target_bps;
        publish_stream(stream_info, find_display(attached, capturer->display()),
                       display_name_utf8(capturer->display()));
        Error refreshed = session_->refresh_stream(stream_info);
        if (!refreshed.ok()) {
            if (refreshed.status() == Status::Unavailable ||
                refreshed.status() == Status::WaitTimeout) {
                *peer_left = true;
            } else {
                ++stats.send_errors;
                *error = "refresh_stream: " + refreshed.to_string();
            }
            return false;
        }
        ++stats.stream_updates;
        log("adaptive bitrate: " + std::string(decision.reason) + "; target " +
            std::to_string(decision.target_bps / 1000u) + " kbps, RTT " +
            std::to_string(static_cast<int>(stats.rtt_ms + 0.5)) + " ms");
        publish_stats(stats);
        return true;
    };

    // The frame captured before the handshake is the first one on the wire.
    ++stats.frames;
    if (!first.desktop_updated) {
        ++stats.pointer_only;
    }
    if (!process_frame(0, capture_clock_ms(Clock::now()))) {
        // A vanished viewer is a normal end, not a failure.
        const bool clean = error->empty();
        drain_peer();
        return clean;
    }
    publish_cursor();

    while (!stopping_.load()) {
        const auto now = Clock::now();
        if (config_.run_seconds > 0.0 &&
            std::chrono::duration<double>(now - start).count() >= config_.run_seconds) {
            break;
        }
        drain_audio();
        drain_peer();
        poll_clipboard(now);
        if (now - last_link_sample >= std::chrono::seconds(1)) {
            if (!sample_link(now)) {
                const bool clean = error->empty();
                drain_peer();
                return clean;
            }
        }
        if (session_->take_keyframe_request()) {
            force_keyframe = true;
        }
        if (config_.keyframe_interval_ms > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_keyframe).count() >=
                static_cast<long long>(config_.keyframe_interval_ms)) {
            force_keyframe = true;
        }
        if (config_.reannounce && !reannounced && now >= next_reannounce) {
            reannounced = true;
            Error refreshed = session_->refresh_stream(stream_info);
            if (!refreshed.ok()) {
                if (refreshed.status() == Status::Unavailable ||
                    refreshed.status() == Status::WaitTimeout) {
                    *peer_left = true;
                } else {
                    ++stats.send_errors;
                    *error = "refresh_stream: " + refreshed.to_string();
                }
                const bool clean = error->empty();
                drain_peer();
                return clean;
            }
            ++stats.stream_updates;
            log("re-announced the stream description");
        }

        if (frame_interval != Clock::duration::zero()) {
            const auto wait_now = Clock::now();
            if (next_frame_due > wait_now) {
                if (!sleep_unless_stopping(
                        std::chrono::duration_cast<std::chrono::milliseconds>(next_frame_due -
                                                                            wait_now))) {
                    break;
                }
            }
            // Advance on an absolute timeline; capture() itself waits for the
            // next present, so a relative sleep would halve the frame rate.
            next_frame_due += frame_interval;
            if (Clock::now() - next_frame_due > std::chrono::seconds(1)) {
                next_frame_due = Clock::now();
            }
        }

        const CaptureResult result = capturer->capture(std::chrono::milliseconds(100));
        const auto captured_at = Clock::now();
        // The endpoint keeps producing while capture waits for the next
        // present; drain here so audio does not wait behind a slow frame.
        drain_audio();
        switch (result) {
            case CaptureResult::NoNewFrame:
                continue;
            case CaptureResult::ReinitNeeded: {
                log("reinitializing: " + capturer->last_error().to_string());
                Error err = capturer->reinitialize();
                if (!err.ok()) {
                    *error = "reinit failed: " + err.to_string();
                    drain_peer();
                    return false;
                }
                encoder.reset();
                have_input = false;
                force_keyframe = true;
                continue;
            }
            case CaptureResult::FatalError:
                *error = "capture failed: " + capturer->last_error().to_string();
                drain_peer();
                return false;
            case CaptureResult::Frame:
                break;
        }

        ++stats.frames;
        const FrameInfo& frame = capturer->frame();
        if (!frame.desktop_updated) {
            ++stats.pointer_only;
        }
        if (!encoder) {
            // The capture rebuilt: the output may have moved, rotated or
            // changed size, and the encoder may land on a different codec.
            encoder_config.width = frame.width;
            encoder_config.height = frame.height;
            const AdapterDevice rebuilt_adapter = capturer->adapter_device();
            encoder = create_encoder(rebuilt_adapter.device, encoder_config, error);
            if (!encoder) {
                *error = "encoder recreation failed: " + *error;
                drain_peer();
                return false;
            }
            // A rebuilt encoder starts from the configured bitrate again, so
            // the controller starts over with that value as its ceiling.
            abr = BitrateController(encoder->bitrate(), bitrate_floor(encoder->bitrate()));
            stats.target_bitrate = encoder->bitrate();
            const std::vector<DisplayInfo> refreshed = enumerate_displays();
            stream_info =
                describe_stream(refreshed, capturer->display(), *encoder, settings_.framerate,
                                settings_.allow_clipboard, audio_info);
            publish_stream(stream_info, find_display(refreshed, capturer->display()),
                           display_name_utf8(capturer->display()));
            if (config_.stream_announced) {
                config_.stream_announced(stream_info);
            }
            Error refreshed_err = session_->refresh_stream(stream_info);
            if (!refreshed_err.ok()) {
                if (refreshed_err.status() == Status::Unavailable ||
                    refreshed_err.status() == Status::WaitTimeout) {
                    *peer_left = true;
                } else {
                    *error = "refresh_stream: " + refreshed_err.to_string();
                }
                const bool clean = error->empty();
                drain_peer();
                return clean;
            }
            ++stats.stream_updates;
            log("stream changed to " + std::to_string(stream_info.width) + "x" +
                std::to_string(stream_info.height) + " at " +
                std::to_string(stream_info.origin_x) + "," +
                std::to_string(stream_info.origin_y) + "; re-announced");
            force_keyframe = true;
        }
        const uint64_t pts_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
        if (!process_frame(pts_ms, capture_clock_ms(captured_at))) {
            const bool clean = error->empty();
            drain_peer();
            return clean;
        }
        publish_cursor();
    }

    drain_peer();
    return true;
}

bool HostServer::sleep_unless_stopping(std::chrono::milliseconds duration) {
    const auto end = Clock::now() + duration;
    while (!stopping_.load()) {
        const auto left = end - Clock::now();
        if (left <= Clock::duration::zero()) {
            return true;
        }
        std::this_thread::sleep_for(std::min(
            std::chrono::duration_cast<std::chrono::milliseconds>(left),
            std::chrono::milliseconds(100)));
    }
    return false;
}

void HostServer::log(const std::string& message) {
    if (config_.log) {
        config_.log(message);
    }
}

void HostServer::publish_state(HostState state, const std::string& error) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = state;
    if (state == HostState::Listening || state == HostState::Streaming) {
        status_.error.clear();
    } else if (!error.empty()) {
        status_.error = error;
    }
}

void HostServer::publish_stats(const HostServerStats& stats) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.stats = stats;
}

void HostServer::publish_stream(const ServerInfo& info, size_t display_index,
                                const std::string& display_name) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.stream = info;
    status_.display_index = display_index;
    status_.display_name = display_name;
}

}  // namespace chast
