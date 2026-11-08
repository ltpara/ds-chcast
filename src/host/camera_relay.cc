// Winsock 2 must precede windows.h, which chast/quic.h pulls in through
// chast/error.h; otherwise the SDK sees winsock 1 first and ws2tcpip.h fails.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "chast/camera_relay.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "host/desktop_stream.h"

namespace chast {

namespace {

using Clock = std::chrono::steady_clock;

// Each connection bounds its relay independently of the shared source.
constexpr size_t kMaxQueuedPackets = 4;
constexpr size_t kMaxQueuedBytes = 4u * 1024u * 1024u;
constexpr auto kReadTimeout = std::chrono::milliseconds(5);
constexpr auto kStartupTimeout = std::chrono::seconds(10);

std::optional<VideoCodec> codec_vote(StreamCodecPreference preference) {
    switch (preference) {
        case StreamCodecPreference::H264: return VideoCodec::H264;
        case StreamCodecPreference::Hevc: return VideoCodec::Hevc;
        case StreamCodecPreference::Av1: return VideoCodec::Av1;
        case StreamCodecPreference::Vp9: return VideoCodec::Vp9;
        default: return std::nullopt;
    }
}

const char* codec_name(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::H264:
            return "h264";
        case VideoCodec::Hevc:
            return "hevc";
        case VideoCodec::Vp9:
            return "VP9";
        case VideoCodec::Av1:
            return "av1";
    }
    return "h264";
}

}  // namespace

struct CameraRelay::Worker {
    struct Packet {
        FrameHeader header;
        std::vector<uint8_t> payload;
    };

    uint16_t camera_id = 0;
    std::string name;
    std::thread thread;
    std::mutex mutex;
    std::deque<Packet> frames;
    size_t frame_bytes = 0;
    bool resolved = false;
    bool ready = false;
    bool answered = false;
    bool failed = false;
    bool ended = false;
    bool waiting_idr = true;
    bool options_inflight = false;
    std::optional<StreamOptions> pending_options;
    std::optional<StreamOptions> accepted_options;
    std::optional<StreamOptionsResult> options_result;
    std::string failure;
    std::string detail;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t dropped = 0;
    std::atomic<bool> stop{false};
    // Set whenever an encoded frame was lost (queue overflow or a transport
    // drop). The next encoded frame must be an IDR or the viewer cannot
    // decode anything that follows.
    std::atomic<bool> force_keyframe{false};

    void run(LUID adapter, CameraRelayConfig config, CameraDevice camera, Subscribe subscribe);
    void resolve_failed(const std::string& reason);
    void mark_ended(const std::string& reason, bool bad);
    void publish(Packet packet);
    void resync();
};

void CameraRelay::Worker::resolve_failed(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex);
    resolved = true;
    ready = false;
    failed = true;
    ended = true;
    failure = reason;
}

void CameraRelay::Worker::mark_ended(const std::string& reason, bool bad) {
    std::lock_guard<std::mutex> lock(mutex);
    ended = true;
    failed = bad;
    failure = reason;
}

void CameraRelay::Worker::resync() {
    std::lock_guard<std::mutex> lock(mutex);
    frames.clear();
    frame_bytes = 0;
    waiting_idr = true;
    force_keyframe = true;
}

void CameraRelay::Worker::publish(Packet packet) {
    std::lock_guard<std::mutex> lock(mutex);
    if (frames.size() >= kMaxQueuedPackets ||
        frame_bytes + packet.payload.size() > kMaxQueuedBytes) {
        dropped += frames.size();
        frames.clear();
        frame_bytes = 0;
        waiting_idr = true;
        force_keyframe = true;
    }
    if ((waiting_idr && !packet.header.idr) || packet.payload.size() > kMaxQueuedBytes) {
        ++dropped;
        return;
    }
    waiting_idr = false;
    frame_bytes += packet.payload.size();
    frames.push_back(std::move(packet));
}

void CameraRelay::Worker::run(LUID adapter, CameraRelayConfig config, CameraDevice camera,
                              Subscribe subscribe) {
    const auto fail = [&](const std::string& reason) {
        // Also resolves a failed first-frame wait; drive() must always be able
        // to answer a pending StartCamera before collecting the worker.
        std::lock_guard<std::mutex> lock(mutex);
        if (!resolved) { resolved = true; ready = false; }
        failed = ended = true;
        failure = reason;
    };
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        camera.symbolic_link.data(), static_cast<int>(camera.symbolic_link.size()), nullptr, 0);
    if (length <= 0 || !subscribe || !config.capture) {
        fail("camera has no valid capture service or device identity");
        return;
    }
    std::wstring key(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, camera.symbolic_link.data(),
        static_cast<int>(camera.symbolic_link.size()), key.data(), length);
    std::string reason;
    auto subscription = subscribe(L"camera:" + key, [config, camera, adapter](std::string* error) {
        return config.capture(camera, adapter, config.framerate, error);
    }, &reason);
    auto& capture = subscription.capture;
    if (!capture) { fail(reason); return; }
    // The relay copies the last accepted settings before this thread starts.
    const auto initial_options = accepted_options;
    if (initial_options && subscription.quality) subscription.quality(*initial_options);

    EncoderConfig wanted;
    wanted.codec = config.codec;
    if (initial_options) wanted.chroma = initial_options->chroma;
    wanted.framerate = config.framerate;
    wanted.quality_ratio = config.quality_ratio;
    wanted.backend = config.backend;
    std::unique_ptr<IVideoEncoder> encoder;
    const auto started = Clock::now();
    auto retry = Clock::time_point{};
    auto missing_since = started;
    bool first = true, need_idr = true, reinitialize = false;
    StreamOptionsResult effective;
    effective.fps = static_cast<uint16_t>(config.framerate);
    effective.quality = config.quality_ratio >= 1.0f ? StreamQuality::High :
        config.quality_ratio >= 0.6f ? StreamQuality::Balanced : StreamQuality::Smooth;
    if (initial_options) effective.quality = initial_options->quality;
    while (!stop.load()) {
        std::optional<StreamOptions> options;
        { std::lock_guard<std::mutex> lock(mutex); options.swap(pending_options); }
        if (options) {
            auto result = effective;
            auto requested = wanted;
            requested.chroma = options->chroma;
            auto replacement = DesktopStreamPool::vote_codec(*capture, requested, codec_vote(options->codec), &reason);
            if (replacement) {
                wanted.chroma = options->chroma;
                const bool changed = !encoder || encoder->codec() != replacement->codec() || encoder->chroma() != replacement->chroma() ||
                    encoder->width() != replacement->width() || encoder->height() != replacement->height();
                encoder = std::move(replacement);
                if (subscription.quality) subscription.quality(*options);
                encoder->set_rate(0, 0); // Applies the shared policy; retries are owned by the service.
                effective.codec = encoder->codec();
                effective.chroma = encoder->chroma();
                effective.fps = static_cast<uint16_t>(encoder->framerate());
                effective.quality = options->quality;
                result = effective;
                if (changed) resync();
            } else {
                result.code = StreamOptionsCode::Failed;
                result.reason = reason;
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (result.code == StreamOptionsCode::Ok) accepted_options = *options;
            options_result = std::move(result);
        }
        if (Clock::now() < retry) {
            std::this_thread::sleep_for(kReadTimeout);
            continue;
        }
        if (force_keyframe.exchange(false)) {
            DesktopStreamPool::discard_backlog(*capture);
            need_idr = true;
        }
        if (reinitialize) {
            const auto result = capture->reinitialize();
            if (!result.ok()) {
                if (Clock::now() - missing_since >= kStartupTimeout) {
                    fail("capture recovery: " + result.to_string());
                    return;
                }
                retry = Clock::now() + std::chrono::seconds(1);
                continue;
            }
            reinitialize = false;
        }
        const auto result = capture->capture(kReadTimeout);
        if (result == CaptureResult::ReinitNeeded) {
            resync();
            encoder.reset();
            reinitialize = true;
            continue;
        }
        if (result == CaptureResult::FatalError) {
            fail("video service: " + capture->last_error().to_string());
            return;
        }
        if (result != CaptureResult::Frame) {
            if (first && Clock::now() - missing_since >= kStartupTimeout) {
                fail("camera produced no usable first frame");
                return;
            }
            continue;
        }
        const auto frame = capture->frame();
        if (!encoder || encoder->codec() != DesktopStreamPool::codec(*capture) ||
            encoder->chroma() != DesktopStreamPool::chroma(*capture) ||
            encoder->width() != frame.nv12_width || encoder->height() != frame.nv12_height) {
            encoder = first && initial_options
                ? DesktopStreamPool::vote_codec(*capture, wanted, codec_vote(initial_options->codec), &reason)
                : DesktopStreamPool::encoder(*capture, wanted, &reason);
            if (!encoder) {
                if (!first && DesktopStreamPool::encoder_recovery_pending(*capture)) {
                    retry = Clock::now() + std::chrono::seconds(1);
                    continue;
                }
                fail("encoder: " + reason);
                return;
            }
            // A new generation starts with a fresh reference chain, including
            // when another viewer changes the global codec.
            { std::lock_guard<std::mutex> lock(mutex);
              frames.clear(); frame_bytes = 0; waiting_idr = true; }
            need_idr = true;
        }
        effective.codec = encoder->codec();
        effective.chroma = encoder->chroma();
        effective.fps = static_cast<uint16_t>(encoder->framerate());
        std::vector<EncodedPacket> packets;
        const auto pts = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started).count());
        const auto encoded = encoder->encode(nullptr, pts, need_idr, packets);
        if (!encoded.ok()) { fail("shared camera encoder: " + encoded.to_string()); return; }
        for (auto& packet : packets) {
            if (packet.data.empty() || (need_idr && !packet.idr)) continue;
            if (packet.data.size() > kMaxQueuedBytes) {
                fail("camera access unit exceeds the relay buffer limit");
                return;
            }
            need_idr = false;
            if (first) {
                std::lock_guard<std::mutex> lock(mutex);
                resolved = ready = true;
                width = encoder->width(); height = encoder->height();
                detail = std::to_string(width) + "x" + std::to_string(height) + ", " +
                    codec_name(encoder->codec()) + ", " + encoder->implementation();
                first = false;
            }
            FrameHeader header;
            header.pts_ms = packet.pts_ms;
            header.width = encoder->width(); header.height = encoder->height();
            header.codec = encoder->codec();
            header.chroma = encoder->chroma();
            header.keyframe = packet.keyframe; header.idr = packet.idr;
            header.capture_ms = DesktopStreamPool::capture_time(*capture) & 0xFFFFFFFFFFFFull;
            publish(Packet{header, std::move(packet.data)});
            missing_since = Clock::now();
        }
        if (first && Clock::now() - started >= kStartupTimeout) {
            fail("camera encoder produced no usable first IDR");
            return;
        }
    }
    mark_ended({}, false);
}

CameraRelay::CameraRelay(QuicSession& session, AdapterProvider adapter,
                         const CameraRelayConfig& config, Subscribe subscribe)
    : session_(session), adapter_(std::move(adapter)), config_(config),
      subscribe_(std::move(subscribe)) {}

CameraRelay::~CameraRelay() {
    if (!worker_) {
        return;
    }
    worker_->stop = true;
    if (worker_->thread.joinable()) {
        worker_->thread.join();
    }
    worker_.reset();
}

bool CameraRelay::streaming() const {
    return worker_ != nullptr && !worker_->stop.load();
}

void CameraRelay::send_list() {
    CameraList list;
    if ((session_.permissions() & kPermissionCamera) == 0) {
        list.code = CameraCode::Refused;
        list.reason = "camera access is not permitted by the host";
    } else {
        listed_ = config_.devices ? config_.devices() : std::vector<CameraDevice>{};
        for (const CameraDevice& device : listed_) {
            list.cameras.push_back(CameraDescription{device.id, device.name, device.is_default});
        }
    }
    session_.send_camera_list(list);
}

void CameraRelay::start_camera(uint16_t camera_id, std::vector<std::string>& events) {
    if ((session_.permissions() & kPermissionCamera) == 0) {
        session_.send_camera_result(
            CameraResult{camera_id, CameraCode::Refused, "camera access is not permitted"});
        return;
    }
    const CameraDevice* chosen = nullptr;
    for (const CameraDevice& device : listed_) {
        if (device.id == camera_id) {
            chosen = &device;
            break;
        }
    }
    if (!chosen) {
        ++failures_;
        session_.send_camera_result(
            CameraResult{camera_id, CameraCode::NotFound, "the camera is not in the host's list"});
        events.push_back("camera " + std::to_string(camera_id) +
                         ": start refused, it is not in the announced list");
        return;
    }
    if (worker_) {
        // The transport refuses a second camera; this is a request that
        // crossed the previous camera's teardown.
        session_.send_camera_result(
            CameraResult{camera_id, CameraCode::Busy, "another camera is still closing"});
        return;
    }
    worker_ = std::make_unique<Worker>();
    worker_->camera_id = camera_id;
    worker_->name = chosen->name;
    worker_->accepted_options = options_;
    Worker* worker = worker_.get();
    const CameraDevice camera = *chosen;
    const LUID adapter = adapter_();
    const CameraRelayConfig config = config_;
    worker->thread = std::thread([worker, adapter, config, camera, subscribe = subscribe_] {
        try { worker->run(adapter, config, camera, subscribe); }
        catch (const std::exception& error) { worker->resolve_failed(error.what()); }
        catch (...) { worker->resolve_failed("unexpected camera service failure"); }
    });
    events.push_back("camera \"" + chosen->name + "\": opening");
}

void CameraRelay::drive(std::vector<std::string>& events) {
    Worker& worker = *worker_;
    bool resolved = false;
    bool ready = false;
    bool failed = false;
    bool ended = false;
    bool answered = false;
    std::string failure;
    std::string detail;
    {
        std::lock_guard<std::mutex> lock(worker.mutex);
        resolved = worker.resolved;
        ready = worker.ready;
        failed = worker.failed;
        ended = worker.ended;
        answered = worker.answered;
        failure = worker.failure;
        detail = worker.detail;
    }
    if (worker.stop.load()) {
        if (!ended) {
            return;
        }
        // A viewer stop already finished the stream in the transport and the
        // destructor cannot reach here; only the thread is left to collect.
        if (worker_->thread.joinable()) {
            worker_->thread.join();
        }
        worker_.reset();
        return;
    }
    if (!resolved) {
        return;
    }
    if (!answered) {
        if (!ready) {
            ++failures_;
            session_.send_camera_result(
                CameraResult{worker.camera_id, CameraCode::Failed, failure});
            events.push_back("camera \"" + worker.name + "\": cannot start: " + failure);
            worker.stop = true;
            return;
        }
        const Error sent = session_.send_camera_result(
            CameraResult{worker.camera_id, CameraCode::Ok, {}});
        if (!sent.ok()) {
            // The viewer cancelled while the camera opened, the permission was
            // revoked, or the session is going away.
            events.push_back("camera \"" + worker.name + "\": cannot open the stream: " +
                             sent.to_string());
            worker.stop = true;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(worker.mutex);
            worker.answered = true;
        }
        ++started_;
        events.push_back("camera \"" + worker.name + "\": streaming " + detail);
        answered = true;
    }
    if (ended) {
        if (failed) {
            ++failures_;
            events.push_back("camera \"" + worker.name + "\": " + failure);
        } else {
            events.push_back("camera \"" + worker.name + "\": the device stopped");
        }
        // The viewer learns about the end from the stream FIN; finish_camera
        // is idempotent when the transport already closed it.
        session_.finish_camera();
        if (worker_->thread.joinable()) {
            worker_->thread.join();
        }
        worker_.reset();
    }
}

void CameraRelay::send_frames(std::vector<std::string>& events) {
    Worker& worker = *worker_;
    if (!worker.answered || worker.stop.load()) {
        return;
    }
    for (;;) {
        Worker::Packet packet;
        {
            std::lock_guard<std::mutex> lock(worker.mutex);
            if (worker.frames.empty()) {
                break;
            }
            packet = std::move(worker.frames.front());
            worker.frames.pop_front();
            worker.frame_bytes -= packet.payload.size();
        }
        const Error sent =
            session_.send_camera_frame(packet.header, packet.payload.data(),
                                       packet.payload.size());
        if (sent.ok()) {
            ++frames_;
            continue;
        }
        if (sent.status() == Status::WaitTimeout) {
            // The transport dropped this packet; the reference chain is broken
            // from here, so the encoder has to produce an IDR next.
            worker.resync();
            break;
        }
        events.push_back("camera \"" + worker.name + "\": cannot send a frame: " +
                         sent.to_string());
        worker.stop = true;
        break;
    }
}

void CameraRelay::stream_options() {
    if (!config_.stream_options) return;
    std::optional<StreamOptionsResult> answer;
    if (worker_) {
        std::lock_guard<std::mutex> lock(worker_->mutex);
        answer.swap(worker_->options_result);
        if (answer && answer->code == StreamOptionsCode::Ok) options_ = worker_->accepted_options;
        if (!answer && worker_->options_inflight && (worker_->ended || worker_->stop.load())) {
            answer = StreamOptionsResult{};
            answer->code = StreamOptionsCode::Failed;
            answer->reason = "the camera stopped while changing video options";
        }
        if (answer) worker_->options_inflight = false;
    }
    if (answer) session_.send_stream_options_result(*answer);
    StreamOptions options;
    if (!session_.take_stream_options_request(options)) return;
    if (worker_) {
        std::lock_guard<std::mutex> lock(worker_->mutex);
        if (worker_->ready && !worker_->ended && !worker_->stop.load() && !worker_->options_inflight) {
            worker_->pending_options = options;
            worker_->options_inflight = true;
            return;
        }
    }
    StreamOptionsResult busy;
    busy.code = StreamOptionsCode::Busy;
    busy.reason = "wait for the camera to stream and its previous options to finish";
    session_.send_stream_options_result(busy);
}

std::vector<std::string> CameraRelay::pump() {
    std::vector<std::string> events;
    if (worker_ && (session_.permissions() & kPermissionCamera) == 0) worker_->stop = true;
    if (session_.take_camera_list_request()) {
        send_list();
    }
    uint16_t camera_id = 0;
    while (session_.take_camera_start(camera_id)) {
        start_camera(camera_id, events);
    }
    uint16_t stopped = 0;
    while (session_.take_camera_stop(stopped)) {
        if (worker_ && worker_->camera_id == stopped) {
            worker_->stop = true;
            events.push_back("camera \"" + worker_->name + "\": stopped by the viewer");
        }
    }
    stream_options();
    if (worker_) {
        drive(events);
    }
    if (worker_) {
        send_frames(events);
    }
    return events;
}

}  // namespace chast
