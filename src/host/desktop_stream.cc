#define NOMINMAX
#include "host/desktop_stream.h"
#include "host/display_topology.h"

#include <algorithm>
#include <deque>
#include <condition_variable>
#include <future>
#include <thread>
#include <atomic>
#include <type_traits>
#include <timeapi.h>

namespace chast {
namespace {
using Clock = std::chrono::steady_clock;
uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count());
}
struct Picture {
    FrameInfo frame;
    std::shared_ptr<const CursorInfo> cursor;
    DisplayInfo display;
    ComPtr<ID3D11Texture2D> nv12, bgra;
    std::vector<EncodedPacket> packets;
    VideoCodec codec = VideoCodec::H264;
    VideoChroma chroma = VideoChroma::Yuv420;
    uint64_t generation = 0;
    uint64_t sequence = 0;
    uint64_t capture_ms = 0;
    size_t bytes = 0;
    bool idr = false;
};
constexpr size_t kMaxSubscriberQueueFrames = 60;
constexpr size_t kMaxSubscriberQueueBytes = 8u * 1024u * 1024u;
struct Subscription {
    // Only the owning connection reads/replaces current. The service changes
    // queues on its thread, not metadata a connection might be inspecting.
    std::shared_ptr<const Picture> current;
    std::deque<std::shared_ptr<const Picture>> queue;
    size_t queued_bytes = 0;
    uint64_t generation = 0;
    bool waiting_idr = true;
    bool consumed = false;
    uint64_t delivered_capture_ms = 0;
    VideoCodecMask decoding_codecs = kVideoCodecAll;
    // RustDesk runs its pointer service beside the video service, so a slow
    // encode never delays the cursor. The source invokes this right after a
    // frame is captured, before encoding it.
    std::mutex cursor_mutex;
    std::function<void(const CursorInfo&)> cursor_sink;
};
} // namespace

struct DesktopStreamPool::Source {
    // The queue mutex protects only command dispatch. No capture, driver or
    // encoder call runs with it held. Commands are synchronous to their caller
    // but run between capture ticks on the one GPU-owning service thread.
    std::mutex command_mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> commands;
    bool stopping = false;
    std::thread worker;
    FollowDesktop follow_desktop;
    std::unique_ptr<IDxgiCapturer> capture;
    std::unique_ptr<IVideoEncoder> encoder;
    EncoderFactory factory;
    std::unique_ptr<StreamRecorder> recorder;
    EncoderConfig config;
    EncoderConfig base_config;
    std::shared_ptr<VideoQos> qos;
    std::atomic<UINT> effective_bitrate{0}, effective_fps{0};
    UINT audio_reserve = 0;
    UINT requested_rate = 0, requested_fps = 0;
    Clock::time_point rate_retry{};
    bool variable_quality = true;
    std::vector<std::weak_ptr<Subscription>> subscribers;
    std::shared_ptr<Picture> image;
    Error error = Error::success();
    bool configured = false;
    bool lost = false;
    bool fatal = false;
    bool keyframe = true;
    bool have_input = false;
    // A desktop change was captured while every subscriber queue was full and
    // has not been encoded yet. The next free tick must send a real frame
    // instead of repeating the encoder's older input.
    bool skipped_update = false;
    // RustDesk static_refresh: a bounded burst of repeats lets the rate
    // control refine a static picture. Counters restart on every new frame
    // and refinements must not count as motion for the shared QoS.
    Clock::time_point last_encode = Clock::now();
    unsigned repeat_outputs = 0, repeat_no_outputs = 0, repeat_failures = 0;
    bool source_ready = false;
    float policy_ratio = 0.67f;
    unsigned encode_failures = 0;
    bool software_fallback = false;
    UINT initial_bitrate = 0;
    VideoCodec preferred_codec = VideoCodec::H264;
    VideoCodecMask previous_common = 0;
    unsigned missing_nv12 = 0;
    uint64_t generation = 1;
    uint64_t sequence = 0;
    Clock::time_point started = Clock::now();
    uint64_t recording_generation = 0;
    Clock::time_point due{};
    bool global_codec = false, negotiating = false, needs_group_rebuild = false;
    VideoCodecMask global_common = kVideoCodecAll;
    struct Prepared {
        std::unique_ptr<IVideoEncoder> encoder;
        EncoderConfig config, base;
    } prepared;
    // Session telemetry must not queue behind a slow software encode. Publish
    // format and rate together so a new rate cannot describe the old codec.
    struct Metadata {
        std::shared_ptr<const CursorInfo> cursor;
        UINT width = 0, height = 0, bitrate = 0, fps = 0;
        VideoCodec codec = VideoCodec::H264;
        VideoChroma chroma = VideoChroma::Yuv420;
        bool encoder_recovery_pending = false;
    } metadata;
    std::mutex metadata_mutex;

    void publish_metadata() {
        Metadata next;
        if (image) next.cursor = image->cursor;
        next.encoder_recovery_pending = software_fallback && !encoder && image && !lost && !fatal;
        if (encoder && !lost && !fatal) {
            next.width = encoder->width(); next.height = encoder->height();
            next.codec = encoder->codec();
            next.chroma = encoder->chroma();
            next.bitrate = encoder->bitrate(); next.fps = encoder->framerate();
        }
        std::lock_guard lock(metadata_mutex);
        metadata = std::move(next);
    }
    Metadata read_metadata() {
        std::lock_guard lock(metadata_mutex);
        return metadata;
    }

    Source() { worker = std::thread([this] { run(); }); }
    ~Source() {
        {
            std::lock_guard<std::mutex> lock(command_mutex);
            stopping = true;
        }
        wake.notify_one();
        worker.join();
        if (qos) qos->remove_display(reinterpret_cast<uintptr_t>(this));
    }
    template<class Function> auto call(Function function) -> decltype(function()) {
        using Result = decltype(function());
        auto task = std::make_shared<std::packaged_task<Result()>>(
            [this, function = std::move(function)]() mutable -> Result {
                if constexpr (std::is_void_v<Result>) {
                    function();
                    publish_metadata();
                } else {
                    auto result = function();
                    publish_metadata();
                    return result;
                }
            });
        auto result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(command_mutex);
            commands.emplace_back([task] { (*task)(); });
        }
        wake.notify_one();
        return result.get();
    }
    void run() {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // Frame pacing matches the announced framerate; the default ~15.6 ms
        // wait granularity rounds a 30 fps tick up by another full frame.
        timeBeginPeriod(1);
        for (;;) {
            std::deque<std::function<void()>> pending;
            {
                std::unique_lock<std::mutex> lock(command_mutex);
                // Wait until the next tick instead of polling: a 5 ms cap made
                // every scheduled tick land up to one poll late. With no
                // pending tick (no capture yet) keep the idle poll.
                const auto now = Clock::now();
                auto delay = std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(5));
                if (due > now) delay = due - now;
                wake.wait_for(lock, delay, [&] { return stopping || !commands.empty(); });
                pending.swap(commands);
                if (stopping && pending.empty()) break;
            }
            for (auto& command : pending) command();
            try {
                pump();
            } catch (const std::exception& exception) {
                error = Error(Status::Unavailable, std::string("video service: ") + exception.what());
                fatal = true;
            }
            publish_metadata();
        }
        // GPU resources must be released on their owning thread as well.
        clear_queues();
        image.reset();
        prepared = {};
        encoder.reset();
        capture.reset();
        if (recorder) recorder->finish(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()));
        timeEndPeriod(1);
        if (SUCCEEDED(initialized)) CoUninitialize();
    }
    bool follow(std::string* failure) {
        return !follow_desktop || follow_desktop(failure, nullptr);
    }

    void clear_queues() {
        for (auto& weak : subscribers) if (auto sub = weak.lock()) {
            sub->queue.clear();
            sub->queued_bytes = 0;
            sub->waiting_idr = true;
        }
        keyframe = true;
    }
    void notify_cursor() {
        if (!image) return;
        for (const auto& weak : subscribers) {
            auto sub = weak.lock();
            if (!sub) continue;
            std::function<void(const CursorInfo&)> sink;
            {
                std::lock_guard<std::mutex> lock(sub->cursor_mutex);
                sink = sub->cursor_sink;
            }
            if (sink) sink(*image->cursor);
        }
    }
    void request_keyframe(const Subscription& sub) {
        // The connection may ask again while its first IDR is waiting to be
        // consumed. Coalesce that request instead of encoding another IDR on
        // the next tick after the first one has already been delivered.
        if (sub.current && !sub.consumed && sub.current->generation == generation && sub.current->idr)
            return;
        if (std::any_of(sub.queue.begin(), sub.queue.end(), [&](const auto& picture) {
                return picture->generation == generation && picture->idr;
            }))
            return;
        keyframe = true;
    }
    void invalidate() {
        ++generation;
        needs_group_rebuild = false;
        image.reset();
        encoder.reset();
        have_input = false;
        missing_nv12 = 0;
        encode_failures = 0;
        clear_queues();
    }
    void remember() {
        auto next = std::make_shared<Picture>();
        next->frame = capture->frame();
        next->cursor = std::make_shared<CursorInfo>(capture->cursor());
        next->display = capture->display();
        next->generation = generation;
        next->capture_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count());
        next->nv12 = ComPtr<ID3D11Texture2D>(next->frame.nv12_texture);
        next->nv12.add_ref();
        next->bgra = ComPtr<ID3D11Texture2D>(next->frame.texture);
        next->bgra.add_ref();
        // These are metadata views for the session, not a second GPU consumer.
        next->frame.texture = nullptr;
        next->frame.device = nullptr;
        next->frame.context = nullptr;
        image = std::move(next);
    }
    VideoCodecMask common_codecs(VideoCodecMask additional = kVideoCodecAll) const {
        additional &= global_common;
        for (const auto& weak : subscribers)
            if (auto sub = weak.lock()) additional &= sub->decoding_codecs;
        return additional;
    }
    bool has_subscribers() const {
        return std::any_of(subscribers.begin(), subscribers.end(),
                           [](const auto& weak) { return !weak.expired(); });
    }
    bool all_subscribers_full() const {
        bool any = false;
        for (const auto& weak : subscribers) {
            auto sub = weak.lock();
            if (!sub) continue;
            any = true;
            if (sub->queue.size() < kMaxSubscriberQueueFrames &&
                sub->queued_bytes < kMaxSubscriberQueueBytes) return false;
        }
        return any;
    }
    bool static_refresh_due(Clock::time_point now) const {
        if (!encoder || !image || !configured || !have_input || !source_ready || fatal || lost)
            return false;
        if (!has_subscribers()) return false;
        const auto codec = encoder->codec();
        if (codec == VideoCodec::Av1) return false;
        if (repeat_no_outputs >= 100) return false;
        // VP9 may legitimately drop a repeat frame without an encoding error.
        if (repeat_failures >= 3 && codec != VideoCodec::Vp9) return false;
        const auto budget = static_refresh_budget(codec, policy_ratio, encoder->backend(),
                                                  encoder->implementation());
        if (repeat_outputs >= budget) return false;
        const auto fps = std::max(1u, encoder->framerate());
        const auto step = std::max(std::chrono::microseconds(1000000 / fps),
                                   std::chrono::microseconds(1000000 / 30));
        const bool first = repeat_outputs == 0 && repeat_no_outputs == 0;
        const auto interval = codec == VideoCodec::Vp9 || first
            ? std::max(step, std::chrono::microseconds(100000)) : step;
        return now - last_encode >= interval;
    }
    UINT quality_bitrate(UINT width, UINT height, VideoCodec codec, float ratio,
                         const EncoderConfig& base) const {
        const auto rate = base.bitrate
            ? static_cast<UINT>(std::min<double>(base.bitrate, base.bitrate * ratio / std::max(0.1f, base.quality_ratio)))
            : hardware_bitrate(width, height, ratio, codec == VideoCodec::H264);
        return std::max(8000u, rate - std::min(audio_reserve, rate / 2));
    }
    UINT source_fps(UINT wanted) const {
        const auto limit = capture->max_framerate();
        return std::max(1u, limit ? std::min(wanted, limit) : wanted);
    }
    Error apply_qos() {
        if (!qos || !encoder || !configured || Clock::now() < rate_retry) return Error::success();
        const auto policy = qos->state(now_ms());
        policy_ratio = policy.ratio;
        const auto fps = source_fps(policy.fps);
        const auto rate = quality_bitrate(encoder->width(), encoder->height(), encoder->codec(), policy.ratio, base_config);
        // Drivers round bitrates. Compare requests, not a rounded readback,
        // or the service would reprogram an unchanged rate on every tick.
        if (rate == requested_rate && fps == requested_fps) return Error::success();
        bool required = false;
        auto result = encoder->set_rate(rate, fps, &required);
        if (result.ok()) {
            requested_rate = rate;
            requested_fps = fps;
            variable_quality = true;
            effective_bitrate = config.bitrate = encoder->bitrate();
            effective_fps = config.framerate = encoder->framerate();
            keyframe = keyframe || required;
            due = {};
        } else {
            variable_quality = false;
            rate_retry = Clock::now() + std::chrono::seconds(1);
        }
        qos->display(reinterpret_cast<uintptr_t>(this), 0, encoder->bitrate() / 1000,
                     variable_quality, now_ms());
        return result;
    }
    bool build(EncoderConfig wanted, std::string* failure, bool strict_codec = false,
               VideoCodecMask additional = kVideoCodecAll) {
        wanted.width = image->frame.nv12_width;
        wanted.height = image->frame.nv12_height;
        const auto base = configured ? base_config : wanted;
        policy_ratio = base.quality_ratio;
        const auto common = common_codecs(additional);
        if (!common || (strict_codec && !(common & video_codec_bit(wanted.codec)))) {
            *failure = "the requested codec is not supported by every display subscriber";
            return false;
        }
        const auto requested = wanted.codec;
        std::unique_ptr<IVideoEncoder> next;
        std::vector<VideoCodec> candidates{wanted.codec};
        if (!strict_codec)
            for (auto codec : {VideoCodec::Hevc, VideoCodec::H264, VideoCodec::Av1, VideoCodec::Vp9})
                if (codec != wanted.codec) candidates.push_back(codec);
        for (auto codec : candidates) {
            if (!(common & video_codec_bit(codec))) continue;
            if (software_fallback && codec != VideoCodec::H264) continue;
            wanted.codec = codec;
            if (!valid_video_format(codec, wanted.chroma)) wanted.chroma = VideoChroma::Yuv420;
            if (qos) {
                const auto policy = qos->state(now_ms());
                policy_ratio = policy.ratio;
                wanted.framerate = policy.fps;
                wanted.quality_ratio = policy.ratio;
                wanted.bitrate = quality_bitrate(wanted.width, wanted.height, codec, policy.ratio, base);
            }
            wanted.framerate = source_fps(wanted.framerate);
            auto actual = wanted;
            if (software_fallback) actual.backend = EncoderBackend::Software;
            auto candidate = factory(capture->adapter_device().device, actual, failure);
            if (!candidate || !(common & video_codec_bit(candidate->codec())) ||
                (strict_codec && candidate->codec() != requested)) continue;
            next = std::move(candidate);
            break;
        }
        if (!next) {
            if (failure->empty()) *failure = "no common encoder and decoder format";
            return false;
        }
        // Prove a replacement before changing the format used by live peers.
        // An unsupported new subscriber must never break the existing stream.
        std::vector<EncodedPacket> proof;
        auto proved = next->encode(next->bgra_input() ? image->bgra.get() : image->nv12.get(), 0, true, proof);
        if (!proved.ok() || proof.empty() ||
            std::none_of(proof.begin(), proof.end(), [](const auto& packet) { return packet.idr; })) {
            *failure = "new shared codec produced no usable IDR: " + proved.to_string();
            return false;
        }
        encoder = std::move(next);
        ++recording_generation;
        if (!configured) {
            base_config = base;
            initial_bitrate = base.bitrate ? base.bitrate :
                hardware_bitrate(wanted.width, wanted.height, base.quality_ratio, encoder->codec() == VideoCodec::H264);
            if (!qos) initial_bitrate = encoder->bitrate();
            preferred_codec = requested;
        }
        config = wanted;
        configured = true;
        requested_rate = wanted.bitrate;
        requested_fps = wanted.framerate;
        effective_bitrate = encoder->bitrate();
        effective_fps = encoder->framerate();
        variable_quality = true;
        have_input = false;
        encode_failures = 0;
        error = Error::success();
        clear_queues();
        failure->clear();
        return true;
    }

    bool prepare_codec(EncoderConfig wanted, VideoCodec codec, VideoChroma chroma,
                       bool allow_deferred, std::string* failure) {
        prepared = {};
        // Keep unavailable sources' decoder votes, but defer their GPU proof
        // to recovery. New targets must prove an IDR before being admitted.
        if (allow_deferred && configured &&
            (!image || lost || fatal || (!encoder && !software_fallback))) return true;
        if (!image || lost || fatal) {
            *failure = "display is not ready for codec negotiation";
            return false;
        }
        wanted.width = image->frame.nv12_width;
        wanted.height = image->frame.nv12_height;
        if (software_fallback && codec != VideoCodec::H264) {
            *failure = "the recovering display requires software H.264";
            return false;
        }
        if (chroma == VideoChroma::Yuv444 && !image->bgra) {
            *failure = "the source has no original full-chroma image";
            return false;
        }
        if (encoder && encoder->codec() == codec && encoder->chroma() == chroma && encoder->width() == wanted.width &&
            encoder->height() == wanted.height) return true;
        const auto base = configured ? base_config : wanted;
        wanted.codec = codec;
        wanted.chroma = chroma;
        if (qos) {
            const auto policy = qos->state(now_ms());
            wanted.framerate = policy.fps;
            wanted.quality_ratio = policy.ratio;
            wanted.bitrate = quality_bitrate(wanted.width, wanted.height, codec, policy.ratio, base);
        }
        wanted.framerate = source_fps(wanted.framerate);
        auto actual = wanted;
        if (software_fallback) actual.backend = EncoderBackend::Software;
        auto next = factory(capture->adapter_device().device, actual, failure);
        if (!next || next->codec() != codec || next->chroma() != chroma) {
            if (failure->empty()) *failure = "encoder cannot produce the negotiated format";
            return false;
        }
        std::vector<EncodedPacket> proof;
        auto result = next->encode(next->bgra_input() ? image->bgra.get() : image->nv12.get(), 0, true, proof);
        if (!result.ok() || std::none_of(proof.begin(), proof.end(), [](const auto& p) { return p.idr; })) {
            *failure = "candidate codec produced no usable IDR: " + result.to_string();
            return false;
        }
        prepared = {std::move(next), wanted, base};
        return true;
    }
    void commit_codec(VideoCodec codec, VideoChroma chroma, VideoCodecMask common) {
        global_codec = true;
        global_common = common;
        preferred_codec = codec;
        if (!prepared.encoder && (!encoder || !image || lost || fatal)) {
            encoder.reset();
            config.codec = codec;
            config.chroma = chroma;
            have_input = false;
            clear_queues();
        }
        if (prepared.encoder) {
            encoder = std::move(prepared.encoder);
            ++recording_generation;
            config = prepared.config;
            if (!configured) {
                base_config = prepared.base;
                initial_bitrate = base_config.bitrate ? base_config.bitrate :
                    hardware_bitrate(config.width, config.height, base_config.quality_ratio, codec == VideoCodec::H264);
            }
            configured = true;
            requested_rate = config.bitrate;
            requested_fps = config.framerate;
            effective_bitrate = encoder->bitrate();
            effective_fps = encoder->framerate();
            have_input = false;
            encode_failures = 0;
            error = Error::success();
            clear_queues();
        }
        prepared = {};
        needs_group_rebuild = image && !encoder;
        due = {};
    }
    void publish(const std::shared_ptr<Picture>& picture) {
        picture->sequence = ++sequence;
        if (recorder) recorder->submit(picture->codec, encoder->width(), encoder->height(),
                                       recording_generation, picture->packets, picture->chroma);
        for (auto it = subscribers.begin(); it != subscribers.end();) {
            auto sub = it->lock();
            if (!sub) { it = subscribers.erase(it); continue; }
            ++it;
            // Bound each slow connection independently. Never resume at a P
            // frame after dropping references: request a fresh IDR instead.
            if (sub->queue.size() >= kMaxSubscriberQueueFrames ||
                sub->queued_bytes + picture->bytes > kMaxSubscriberQueueBytes) {
                sub->queue.clear();
                sub->queued_bytes = 0;
                sub->waiting_idr = true;
                keyframe = true;
            }
            if (sub->waiting_idr && !picture->idr) {
                keyframe = true;
                continue;
            }
            // An IDR decodes on its own and makes every queued reference
            // obsolete. Pushing it behind pending refinements would leave a
            // waiting viewer with nothing usable for as long as the static
            // refresh keeps refilling the queue.
            if (picture->idr && !sub->queue.empty()) {
                sub->queue.clear();
                sub->queued_bytes = 0;
            }
            sub->waiting_idr = false;
            sub->queue.push_back(picture);
            sub->queued_bytes += picture->bytes;
        }
    }
    bool encode(bool repeat, bool force_update = false) {
        if (!image || !image->nv12) return true;
        if (!encoder || encoder->width() != image->frame.nv12_width ||
            encoder->height() != image->frame.nv12_height) {
            std::string failure;
            if (!build(config, &failure)) {
                error = Error(Status::Unavailable, "shared encoder: " + failure);
                fatal = true;
                return false;
            }
        }
        auto next = std::make_shared<Picture>(*image);
        if (repeat) next->capture_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
        next->packets.clear();
        next->codec = encoder->codec();
        next->chroma = encoder->chroma();
        const uint64_t pts = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
        Error result = Error::success();
        auto* input = encoder->bgra_input() ? next->bgra.get() : next->nv12.get();
        const bool updated = force_update ||
            (encoder->bgra_input() ? next->frame.desktop_updated : next->frame.nv12_updated);
        if (repeat) {
            // Refinement republishes the cached picture; it is not a new
            // desktop image for any subscriber.
            next->frame.nv12_updated = false;
            next->frame.desktop_updated = false;
        }
        if ((repeat || !updated) && have_input) {
            result = encoder->encode_repeat(pts, keyframe, next->packets);
            if (!result.ok() && result.status() != Status::DeviceRemoved &&
                result.status() != Status::DeviceReset) {
                next->packets.clear();
                keyframe = true;
                result = encoder->encode(input, pts, true, next->packets);
            }
        } else {
            result = encoder->encode(input, pts, keyframe, next->packets);
        }
        if (!result.ok()) {
            if (repeat) {
                ++repeat_no_outputs;
                ++repeat_failures;
            }
            error = result;
            if (result.status() == Status::DeviceRemoved || result.status() == Status::DeviceReset) {
                // A software readback still needs a live capture device.
                invalidate();
                lost = true;
                return false;
            }
            have_input = false;
            clear_queues();
            if (++encode_failures < 3) {
                // Retry even on a static desktop; clear_queues requests an IDR.
                due = Clock::now() + std::chrono::milliseconds(100);
                return true;
            }
            if (base_config.backend != EncoderBackend::Auto ||
                encoder->backend() == EncoderBackend::Software) {
                fatal = true;
                return false;
            }
            // Only this source changes backend. The codec is negotiated again
            // across all displays and viewers before any replacement commits.
            software_fallback = true;
            encoder.reset();
            prepared = {};
            ++generation;
            image = std::make_shared<Picture>(*image);
            image->generation = generation;
            needs_group_rebuild = global_codec;
            due = {};
            error = Error(Status::Unavailable,
                "hardware encoding failed repeatedly; negotiating software H.264: " + result.to_string());
            return false;
        }
        encode_failures = 0;
        error = Error::success();
        have_input = true;
        last_encode = Clock::now();
        const bool delivered = !next->packets.empty();
        const bool vpx = next->codec == VideoCodec::Vp9;
        const bool sent = (delivered || vpx) && has_subscribers();
        for (const auto& packet : next->packets) {
            next->bytes += packet.data.size();
            next->idr = next->idr || packet.idr;
        }
        if (next->idr) keyframe = false;
        if (repeat) {
            if (delivered) {
                ++repeat_outputs;
                repeat_failures = 0;
                source_ready = sent;
            } else {
                ++repeat_no_outputs;
            }
        } else {
            // RustDesk on_encoded(true) clears the failure count for a real
            // frame as well, so a later idle burst starts clean.
            repeat_failures = 0;
            source_ready = sent;
        }
        if (!next->packets.empty()) {
            publish(next);
            // Static refinement must not count as motion for adaptive bitrate.
            if (qos && !repeat) qos->display(reinterpret_cast<uintptr_t>(this), 1, encoder->bitrate() / 1000,
                                             variable_quality, now_ms());
        }
        return true;
    }
    bool pop(Subscription& sub) {
        if (sub.queue.empty()) return false;
        sub.current = std::move(sub.queue.front());
        sub.queue.pop_front();
        sub.queued_bytes -= sub.current->bytes;
        sub.consumed = false;
        return true;
    }
    CaptureResult read(Subscription& sub) {
        if (fatal) return CaptureResult::FatalError;
        if (lost || sub.generation != generation) return CaptureResult::ReinitNeeded;
        if (needs_group_rebuild && image) {
            sub.current = image;
            sub.consumed = false;
            return CaptureResult::Frame;
        }
        if (sub.current && !sub.consumed && !sub.current->packets.empty())
            return CaptureResult::Frame;
        if (pop(sub)) return CaptureResult::Frame;
        if (!configured && image) {
            sub.current = image;
            sub.consumed = false;
            return CaptureResult::Frame;
        }
        return CaptureResult::NoNewFrame;
    }
    void pump() {
        if (negotiating || needs_group_rebuild) return;
        if (!capture || fatal || lost || (!configured && image)) return;
        apply_qos();
        if (Clock::now() < due) return;
        if (recorder && recorder->needs_keyframe(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count())))
            keyframe = true;
        const auto common = common_codecs();
        if (!global_codec && configured && encoder && common != previous_common) {
            previous_common = common;
            // The last restrictive peer left: restore the chosen codec when
            // the remaining receivers and encoder can support it.
            if (common & video_codec_bit(preferred_codec) && encoder->codec() != preferred_codec) {
                auto wanted = config;
                wanted.codec = preferred_codec;
                std::string ignored;
                build(wanted, &ignored);
            }
        }
        // Schedule the next tick from this one: adding the interval after
        // encoding made every frame take interval + processing time, so a
        // 30 fps request delivered roughly half of it on real hardware.
        const auto tick = Clock::now();
        const UINT tick_fps = encoder ? encoder->framerate() : config.framerate;
        const auto interval = std::chrono::microseconds(1000000 / std::max(1u, tick_fps));
        due += interval;
        // An encoder slower than the interval must skip a beat instead of
        // bursting two frames back to back to catch up.
        if (due <= tick) due = tick + interval;
        // Every live connection is behind by a full window. Like RustDesk's
        // per-frame subscriber wait, stop encoding until one drains instead of
        // burning work for frames every queue would drop. The pointer is still
        // sampled: RustDesk's cursor services run beside video and keep moving
        // while a viewer's video queue is saturated.
        const bool full = all_subscribers_full();
        // Wait up to the next tick for a desktop change. A short fixed timeout
        // samples the present stream with a low duty cycle: when its cadence
        // and the tick cadence share a phase, whole intervals report no new
        // frame while others carry two. The target framerate is the cap.
        const auto capture_timeout = full
            ? std::chrono::milliseconds(0)
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::clamp<Clock::duration>(due - Clock::now(), std::chrono::milliseconds(1), interval));
        const auto result = capture->capture(capture_timeout);
        if (result == CaptureResult::ReinitNeeded || result == CaptureResult::FatalError) {
            error = capture->last_error();
            invalidate();
            lost = true;
            // Let the connection's desktop tracker follow UAC/lock desktops
            // before deciding whether capture can be recovered.
            return;
        }
        if (result == CaptureResult::Frame) {
            if (!capture->frame().nv12_texture || capture->frame().rotation_degrees) {
                if (full || ++missing_nv12 < 3) return;
                error = Error(Status::Unavailable, "capture produced no usable NV12 image");
                fatal = true;
                return;
            }
            missing_nv12 = 0;
            remember();
            notify_cursor();
            // Cursor-only wakes refresh the pointer but carry no new desktop
            // pixels. RustDesk's duplication reports the same wake as
            // WouldBlock and leaves cursor delivery to its cursor services;
            // encoding here would make every mouse move over a static desktop
            // a full frame and count it as motion for the shared QoS.
            const bool desktop_updated =
                image->frame.nv12_updated || image->frame.desktop_updated;
            if (full) {
                // Keep the newest picture for the first free tick; encoding it
                // now would only be dropped by every saturated queue.
                if (desktop_updated) skipped_update = true;
                return;
            }
            if (skipped_update && !desktop_updated) {
                // The last change arrived while every queue was full. Repeat
                // encoding would reuse the encoder's stale input, so encode
                // the remembered picture once before resuming normal pacing.
                skipped_update = false;
                if (configured) encode(false, true);
                return;
            }
            if (desktop_updated) {
                skipped_update = false;
                // Every changed desktop restarts RustDesk's static-refresh budget;
                // a cursor-only sample must not keep the refinement alive.
                repeat_outputs = 0;
                repeat_no_outputs = 0;
                source_ready = false;
            }
            if (global_codec && (!encoder || encoder->width() != image->frame.nv12_width ||
                                 encoder->height() != image->frame.nv12_height)) {
                needs_group_rebuild = true;
                return;
            }
            if (!configured) return;
            if (desktop_updated) encode(false);
            else if (keyframe) encode(true);
            else if (static_refresh_due(Clock::now())) encode(true);
        } else if (full) {
            // Every queue is saturated and no new frame arrived. Leave the
            // remembered desktop change and any forced refresh for the first
            // free tick; encoding now would only be dropped by every window
            // and the change would be lost.
            return;
        } else if (skipped_update && image && configured) {
            skipped_update = false;
            encode(false, true);
        } else if (keyframe && image && configured) {
            encode(true);
        } else if (static_refresh_due(Clock::now())) {
            encode(true);
        }
    }
};

class DesktopStreamPool::Capture final : public IDxgiCapturer {
public:
    std::shared_ptr<CodecPeer> peer;
    std::shared_ptr<Source> source;
    std::shared_ptr<Subscription> sub = std::make_shared<Subscription>();
    DisplayInfo initial_display;
    bool is_merged;
    size_t display_count;
    Error error = Error::success();
    FrameInfo empty_frame;
    CursorInfo empty_cursor;
    explicit Capture(std::shared_ptr<Source> value, VideoCodecMask codecs) : source(std::move(value)) {
        // Constructed on the service thread in the same command that checks
        // codec compatibility, so a capture tick cannot split admission.
        sub->decoding_codecs = codecs;
        initial_display = source->capture->display();
        is_merged = source->capture->merged();
        display_count = source->capture->merged_display_count();
        sub->generation = source->generation;
        source->subscribers.push_back(sub);
        source->keyframe = true;
    }
    ~Capture() override;
    CaptureResult capture(std::chrono::milliseconds timeout) override {
        const auto result = source->call([&] {
            const auto result = source->read(*sub);
            error = source->error;
            return result;
        });
        // Readers wait without occupying the GPU thread.
        if (result == CaptureResult::NoNewFrame)
            std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(5)));
        return result;
    }
    const FrameInfo& frame() const override { return sub->current ? sub->current->frame : empty_frame; }
    const CursorInfo& cursor() const override { return sub->current ? *sub->current->cursor : empty_cursor; }
    const DisplayInfo& display() const override { return sub->current ? sub->current->display : initial_display; }
    AdapterDevice adapter_device() const override {
        // The paired encoder adapter uses Source directly; no borrowed device
        // is allowed to escape while another subscriber can reset capture.
        return source->call([&] {
            auto adapter = source->capture->adapter_device();
            adapter.device = nullptr;
            return adapter;
        });
    }
    bool merged() const override { return is_merged; }
    size_t merged_display_count() const override { return display_count; }
    UINT nv12_scale_percent() const override {
        return source->call([&] { return source->capture->nv12_scale_percent(); });
    }
    Error set_nv12_scale(UINT percent) override {
        auto transaction = peer ? peer->lock_transaction() : std::unique_lock<std::mutex>{};
        return source->call([&] {
            if (source->capture->nv12_scale_percent() == percent) return Error::success();
            return source->capture->set_nv12_scale(percent);
        });
    }
    Error reinitialize() override {
        auto transaction = peer ? peer->lock_transaction() : std::unique_lock<std::mutex>{};
        return source->call([&] {
            if (source->lost || sub->generation == source->generation) {
                const auto identity = initial_display;
                std::string failure;
                if (!source->follow(&failure)) return Error(Status::Unavailable, failure);
                auto result = source->capture->reinitialize();
                if (!result.ok()) return result;
                if (!is_merged && !same_display_identity(identity, source->capture->display())) {
                    source->invalidate(); source->lost = true;
                    source->error = Error(Status::Unavailable, "the display identity changed");
                    return source->error;
                }
                source->invalidate();
                source->lost = source->fatal = false;
                source->error = Error::success();
                source->due = {};
            }
            sub->generation = source->generation;
            initial_display = source->capture->display();
            sub->current.reset();
            sub->consumed = false;
            return Error::success();
        });
    }
    const Error& last_error() const override { return error; }
};

Error DesktopStreamPool::refresh_display(IDxgiCapturer& capture, const DisplayInfo& display, bool* changed) {
    auto& shared = static_cast<Capture&>(capture);
    auto transaction = shared.peer ? shared.peer->lock_transaction() : std::unique_lock<std::mutex>{};
    return shared.source->call([&] {
        auto& source = *shared.source;
        *changed = false;
        if (!same_display_identity(source.capture->display(), display))
            return Error(Status::Unavailable, "the display identity changed");
        if (!same_display_geometry(source.capture->display(), display)) {
            source.invalidate();
            source.lost = true;
            *changed = true;
            std::string failure;
            if (!source.follow(&failure)) return Error(Status::Unavailable, failure);
            auto result = source.capture->reinitialize();
            if (!result.ok()) { source.error = result; return result; }
            if (!same_display_identity(source.capture->display(), display)) {
                source.error = Error(Status::Unavailable, "the display identity changed");
                return source.error;
            }
            source.lost = source.fatal = false;
            source.error = Error::success();
            source.due = {};
        }
        if (shared.sub->generation != source.generation) {
            *changed = true;
            shared.sub->generation = source.generation;
            shared.sub->current.reset(); shared.sub->consumed = false;
        }
        shared.initial_display = source.capture->display();
        return Error::success();
    });
}

std::shared_ptr<const CursorInfo> DesktopStreamPool::cursor_snapshot(const IDxgiCapturer& capture) {
    const auto& shared = static_cast<const Capture&>(capture);
    return shared.source->read_metadata().cursor;
}

void DesktopStreamPool::set_cursor_sink(IDxgiCapturer& capture,
                                        std::function<void(const CursorInfo&)> sink) {
    auto& shared = static_cast<Capture&>(capture);
    std::lock_guard<std::mutex> lock(shared.sub->cursor_mutex);
    shared.sub->cursor_sink = std::move(sink);
}

class DesktopStreamPool::Encoder final : public IVideoEncoder {
public:
    std::shared_ptr<Source> source;
    std::shared_ptr<Subscription> sub;
    UINT width_, height_, rate_, fps_;
    VideoCodec codec_;
    VideoChroma chroma_;
    EncoderBackend backend_;
    std::string name_;
    bool imported_;
    Encoder(Capture& capture) : source(capture.source), sub(capture.sub) {
        // Runs on the service thread. Freeze the format this connection
        // announced until it sees and announces the next picture's format.
        const auto& raw = *source->encoder;
        width_ = raw.width(); height_ = raw.height(); codec_ = raw.codec(); chroma_ = raw.chroma();
        if (sub->current && !sub->current->packets.empty()) {
            width_ = sub->current->frame.nv12_width;
            height_ = sub->current->frame.nv12_height;
            codec_ = sub->current->codec;
            chroma_ = sub->current->chroma;
        }
        rate_ = raw.bitrate(); fps_ = raw.framerate();
        name_ = raw.implementation(); imported_ = raw.shared_import();
        backend_ = raw.backend();
    }
    Error encode(ID3D11Texture2D*, uint64_t pts, bool force,
                 std::vector<EncodedPacket>& out) override {
        return source->call([&] {
            if (!sub->current || sub->current->generation != source->generation)
                return Error::success();
            if (sub->consumed) {
                if (force) source->request_keyframe(*sub);
                return Error::success();
            }
            auto pending = sub->current;
            if (pending->packets.empty() && !sub->queue.empty()) {
                pending = std::move(sub->queue.front());
                sub->queue.pop_front();
                sub->queued_bytes -= pending->bytes;
            }
            const auto& picture = *pending;
            sub->delivered_capture_ms = picture.capture_ms;
            if (force && !picture.idr) {
                source->request_keyframe(*sub);
                sub->consumed = true;
                return Error::success();
            }
            for (auto packet : picture.packets) {
                packet.pts_ms = pts; // connection-local clock survives subscription changes
                out.push_back(std::move(packet));
            }
            sub->consumed = true;
            return Error::success();
        });
    }
    Error encode_repeat(uint64_t pts, bool force, std::vector<EncodedPacket>& out) override {
        return encode(nullptr, pts, force, out);
    }
    Error set_rate(UINT rate, UINT fps, bool* key = nullptr) override {
        return source->call([&] {
            if (source->qos) {
                if (key) *key = false;
                return source->apply_qos();
            }
            bool required = false;
            auto result = source->encoder ? source->encoder->set_rate(rate, fps, &required) : Error::success();
            if (result.ok()) {
                rate_ = source->config.bitrate = rate;
                fps_ = source->config.framerate = fps;
                source->effective_bitrate = rate;
                source->effective_fps = fps;
                source->keyframe = source->keyframe || required;
            }
            if (key) *key = required;
            return result;
        });
    }
    UINT bitrate() const noexcept override { return source->effective_bitrate; }
    UINT framerate() const noexcept override { return source->effective_fps; }
    VideoCodec codec() const noexcept override { return codec_; }
    VideoChroma chroma() const noexcept override { return chroma_; }
    UINT width() const noexcept override { return width_; }
    UINT height() const noexcept override { return height_; }
    const std::string& implementation() const noexcept override { return name_; }
    bool shared_import() const noexcept override { return imported_; }
    EncoderBackend backend() const noexcept override { return backend_; }
};

struct DesktopStreamPool::CodecGroup {
    struct User {
        VideoCodecMask decoders;
        std::optional<VideoCodec> preference;
        VideoCodecMask decoders_444 = 0;
        VideoChroma chroma_preference = VideoChroma::Yuv420;
        std::map<uintptr_t, std::weak_ptr<Source>> subscriptions;
    };
    std::mutex mutex;
    std::map<uint64_t, User> users;

    // Reference Codec::update counts only usable preferences. Its hash-map
    // tie order is unspecified; use the oldest connection for stable ties.
    static VideoCodec choose(const std::map<uint64_t, User>& peers, VideoCodecMask usable) {
        std::map<VideoCodec, unsigned> counts;
        for (const auto& [id, peer] : peers)
            if (peer.preference && (usable & video_codec_bit(*peer.preference))) ++counts[*peer.preference];
        unsigned maximum = 0;
        for (const auto& [codec, count] : counts) maximum = std::max(maximum, count);
        if (maximum)
            for (const auto& [id, peer] : peers)
                if (peer.preference && counts[*peer.preference] == maximum &&
                    (usable & video_codec_bit(*peer.preference))) return *peer.preference;
        for (auto codec : {VideoCodec::Hevc, VideoCodec::H264, VideoCodec::Av1, VideoCodec::Vp9})
            if (usable & video_codec_bit(codec)) return codec;
        return VideoCodec::H264;
    }

    // Runs on a connection thread with mutex held. Source commands never
    // acquire this mutex. Prepare every display before committing any of them.
    bool apply(const std::map<uint64_t, User>& peers, const std::shared_ptr<Source>& target,
               const EncoderConfig& initial, std::string* failure) {
        failure->clear();
        VideoCodecMask common = kVideoCodecAll;
        std::vector<std::shared_ptr<Source>> sources;
        for (const auto& [id, peer] : peers) {
            common &= peer.decoders;
            for (const auto& [token, weak] : peer.subscriptions)
                if (auto source = weak.lock()) {
                    if (std::find(sources.begin(), sources.end(), source) == sources.end()) sources.push_back(source);
                }
        }
        if (!common) { *failure = "viewers have no common decoding format across displays"; return false; }
        if (sources.empty()) return true;
        for (auto& source : sources) source->call([&] { source->negotiating = true; });
        const auto finish = [&] {
            for (auto& source : sources) source->call([&] {
                source->prepared = {};
                source->negotiating = false;
            });
        };
        try {
            auto usable = common;
            while (usable) {
                const auto codec = choose(peers, usable);
                const bool full_color = std::all_of(peers.begin(), peers.end(), [&](const auto& entry) {
                    const auto& peer = entry.second;
                    return peer.chroma_preference == VideoChroma::Yuv444 &&
                           (peer.decoders_444 & video_codec_bit(codec));
                });
                auto chroma = full_color ? VideoChroma::Yuv444 : VideoChroma::Yuv420;
                for (;;) {
                    bool ready = true;
                    for (auto& source : sources) {
                        if (!source->call([&] {
                            auto config = source == target && !source->configured ? initial : source->config;
                            return source->prepare_codec(config, codec, chroma, source != target, failure);
                        })) { ready = false; break; }
                    }
                    if (ready) {
                        for (auto& source : sources) source->call([&] { source->commit_codec(codec, chroma, common); });
                        finish();
                        failure->clear();
                        return true;
                    }
                    for (auto& source : sources) source->call([&] { source->prepared = {}; });
                    // A camera or another source may only supply 4:2:0. Keep
                    // the chosen codec and prove 4:2:0 before trying another.
                    if (chroma == VideoChroma::Yuv420) break;
                    chroma = VideoChroma::Yuv420;
                }
                usable &= ~video_codec_bit(codec);
            }
            if (failure->empty()) *failure = "no common encoder works on every active display";
            finish();
            return false;
        } catch (...) {
            finish();
            throw;
        }
    }
    bool configure(CodecPeer& peer, uintptr_t subscription, const std::shared_ptr<Source>& source, const EncoderConfig& config,
                   std::optional<VideoCodec> preference, std::string* failure) {
        auto proposed = users;
        auto& user = proposed[peer.id_];
        user.decoders = peer.decoders_;
        user.preference = preference;
        user.decoders_444 = peer.decoders_444_;
        user.chroma_preference = config.chroma;
        user.subscriptions[subscription] = source;
        if (!apply(proposed, source, config, failure)) return false;
        users = std::move(proposed);
        peer.preference_ = preference;
        peer.chroma_preference_ = config.chroma;
        return true;
    }
    void detach(uint64_t id, uintptr_t subscription) {
        std::lock_guard lock(mutex);
        const auto found = users.find(id);
        if (found == users.end() || !found->second.subscriptions.erase(subscription)) return;
        // RustDesk's decoding participant belongs to the connection, including
        // while CaptureDisplays has an empty set. Only CodecPeer removes it.
        std::string ignored;
        apply(users, {}, {}, &ignored);
    }
    void remove(uint64_t id) {
        std::lock_guard lock(mutex);
        if (!users.erase(id)) return;
        std::string ignored;
        // Removal only widens compatibility. If a desktop is temporarily
        // unavailable, its existing streams keep their last working format.
        apply(users, {}, {}, &ignored);
    }
};

DesktopStreamPool::Capture::~Capture() {
    if (peer) peer->group_->detach(peer->id_, reinterpret_cast<uintptr_t>(sub.get()));
}

DesktopStreamPool::CodecPeer::CodecPeer(std::shared_ptr<CodecGroup> group, uint64_t id,
        VideoCodecMask decoders, std::optional<VideoCodec> preference,
        VideoCodecMask decoders_444, VideoChroma chroma)
    : group_(std::move(group)), id_(id), decoders_(decoders), preference_(preference),
      decoders_444_(decoders_444), chroma_preference_(chroma) {}
DesktopStreamPool::CodecPeer::~CodecPeer() { group_->remove(id_); }
bool DesktopStreamPool::vote_peer(CodecPeer& peer, std::optional<VideoCodec> preference,
                                  std::string* error, VideoChroma chroma) {
    std::lock_guard lock(peer.group_->mutex);
    auto proposed = peer.group_->users;
    auto& user = proposed[peer.id_];
    user.decoders = peer.decoders_;
    user.preference = preference;
    user.decoders_444 = peer.decoders_444_;
    user.chroma_preference = chroma;
    if (!peer.group_->apply(proposed, {}, {}, error)) return false;
    peer.group_->users = std::move(proposed);
    peer.preference_ = preference;
    peer.chroma_preference_ = chroma;
    return true;
}
std::unique_lock<std::mutex> DesktopStreamPool::CodecPeer::lock_transaction() {
    return std::unique_lock<std::mutex>(group_->mutex);
}

std::shared_ptr<DesktopStreamPool::CodecPeer> DesktopStreamPool::connect_codecs(
        VideoCodecMask decoders, std::optional<VideoCodec> preference,
        VideoCodecMask decoders_444, VideoChroma chroma) {
    if (!decoders || (decoders & ~kVideoCodecAll) ||
        (decoders_444 & ~(decoders & kVideoCodecVp9)) || static_cast<unsigned>(chroma) > 1) return {};
    std::lock_guard lock(mutex_);
    if (!codecs_) codecs_ = std::make_shared<CodecGroup>();
    return std::shared_ptr<CodecPeer>(new CodecPeer(codecs_, ++next_peer_, decoders, preference, decoders_444, chroma));
}

std::unique_ptr<IDxgiCapturer> DesktopStreamPool::subscribe(const std::wstring& key,
        CaptureFactory capture, EncoderFactory encoder, std::string* error, FollowDesktop follow_desktop,
        VideoCodecMask decoding_codecs, std::shared_ptr<CodecPeer> peer, HostRecordingOptions recording) {
    if (!decoding_codecs || (decoding_codecs & ~kVideoCodecAll)) {
        *error = "the viewer advertised no valid video decoder";
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sources_.begin(); it != sources_.end();)
        if (it->second.expired()) it = sources_.erase(it); else ++it;
    auto source = sources_[key].lock();
    if (!source) {
        source = std::make_shared<Source>();
        const bool ready = source->call([&] {
            source->qos = qos_;
            source->global_codec = peer != nullptr;
            source->follow_desktop = std::move(follow_desktop);
            if (!source->follow(error)) return false;
            source->capture = capture(error);
            source->factory = std::move(encoder);
            if (source->capture && recording.enabled)
                source->recorder = std::make_unique<StreamRecorder>(std::move(recording), key);
            return source->capture != nullptr;
        });
        if (!ready) { sources_.erase(key); return nullptr; }
        sources_[key] = source;
    }
    return source->call([&]() -> std::unique_ptr<IDxgiCapturer> {
        if (peer) {
            auto subscription = std::make_unique<Capture>(source, decoding_codecs);
            subscription->peer = std::move(peer);
            return subscription;
        }
        const auto common = source->common_codecs(decoding_codecs);
        if (!common) { *error = "viewers have no common decoding format"; return nullptr; }
        if (source->encoder && !(common & video_codec_bit(source->encoder->codec()))) {
            if (!source->build(source->config, error, false, decoding_codecs)) return nullptr;
        }
        auto subscription = std::make_unique<Capture>(source, decoding_codecs);
        source->previous_common = common;
        return subscription;
    });
}

std::unique_ptr<IVideoEncoder> DesktopStreamPool::encoder(IDxgiCapturer& capturer,
        const EncoderConfig& config, std::string* error, bool change_codec) {
    auto& capture = dynamic_cast<Capture&>(capturer);
    if (capture.peer) return configure_encoder(capture, config, config.codec, !change_codec, error);
    auto& source = *capture.source;
    return source.call([&]() -> std::unique_ptr<IVideoEncoder> {
        if (!source.image) { *error = "the display has no usable frame"; return nullptr; }
        if (!source.encoder || (change_codec && (source.encoder->codec() != config.codec || source.encoder->chroma() != config.chroma))) {
            if (!source.build(config, error, change_codec)) return nullptr;
            if (change_codec) source.preferred_codec = config.codec;
            if (!source.encode(false)) { *error = source.error.to_string(); return nullptr; }
            if (change_codec) source.pop(*capture.sub);
        }
        return std::make_unique<Encoder>(capture);
    });
}

std::unique_ptr<IVideoEncoder> DesktopStreamPool::vote_codec(IDxgiCapturer& capturer,
        const EncoderConfig& config, std::optional<VideoCodec> preference, std::string* error) {
    auto& capture = dynamic_cast<Capture&>(capturer);
    return configure_encoder(capture, config, preference, false, error);
}

std::unique_ptr<IVideoEncoder> DesktopStreamPool::configure_encoder(Capture& capture,
        const EncoderConfig& config, std::optional<VideoCodec> preference,
        bool preserve_preference, std::string* error) {
    auto& peer = *capture.peer;
    std::lock_guard lock(peer.group_->mutex);
    // Desktop control and camera consumption share this peer across threads.
    // Read its current vote inside the same transaction that applies it.
    auto wanted = config;
    if (preserve_preference) {
        preference = peer.preference_;
        wanted.chroma = peer.chroma_preference_;
    }
    if (!peer.group_->configure(peer, reinterpret_cast<uintptr_t>(capture.sub.get()),
                                capture.source, wanted, preference, error)) return nullptr;
    return capture.source->call([&]() -> std::unique_ptr<IVideoEncoder> {
        // Commit invalidated all old queued references. Publish and take the
        // new-format IDR before constructing the connection's format adapter.
        const auto& raw = capture.source->encoder;
        if (!raw || capture.source->lost || capture.source->fatal) {
            *error = "display became unavailable during codec negotiation";
            return nullptr;
        }
        if (!capture.sub->current || capture.sub->current->generation != capture.source->generation ||
            capture.sub->current->codec != raw->codec() || capture.sub->current->chroma != raw->chroma() ||
            capture.sub->current->frame.nv12_width != raw->width() ||
            capture.sub->current->frame.nv12_height != raw->height() ||
            capture.sub->current->packets.empty()) {
            capture.sub->consumed = true;
            if (!capture.source->pop(*capture.sub)) {
                capture.source->request_keyframe(*capture.sub);
                if (!capture.source->encode(false)) { *error = capture.source->error.to_string(); return nullptr; }
                capture.source->pop(*capture.sub);
            }
        }
        return std::make_unique<Encoder>(capture);
    });
}

VideoCodec DesktopStreamPool::codec(const IDxgiCapturer& capturer) {
    const auto& capture = dynamic_cast<const Capture&>(capturer);
    return capture.sub->current ? capture.sub->current->codec : VideoCodec::H264;
}

VideoChroma DesktopStreamPool::chroma(const IDxgiCapturer& capturer) {
    const auto& capture = dynamic_cast<const Capture&>(capturer);
    return capture.sub->current ? capture.sub->current->chroma : VideoChroma::Yuv420;
}

UINT DesktopStreamPool::configured_bitrate(const IDxgiCapturer& capturer) {
    const auto& capture = dynamic_cast<const Capture&>(capturer);
    return capture.source->call([&] { return capture.source->initial_bitrate; });
}

bool DesktopStreamPool::read_rate(const IVideoEncoder& encoder, UINT& bitrate, UINT& fps) {
    const auto& adapter = dynamic_cast<const Encoder&>(encoder);
    const auto state = adapter.source->read_metadata();
    if (!state.width || state.width != adapter.width() || state.height != adapter.height() ||
        state.codec != adapter.codec() || state.chroma != adapter.chroma()) return false;
    bitrate = state.bitrate;
    fps = state.fps;
    return true;
}

bool DesktopStreamPool::encoder_recovery_pending(const IDxgiCapturer& capturer) {
    const auto& capture = dynamic_cast<const Capture&>(capturer);
    return capture.source->read_metadata().encoder_recovery_pending;
}

void DesktopStreamPool::request_keyframe(IDxgiCapturer& capturer) {
    auto& capture = dynamic_cast<Capture&>(capturer);
    capture.source->call([&] { capture.source->request_keyframe(*capture.sub); });
}
void DesktopStreamPool::discard_backlog(IDxgiCapturer& capturer) {
    auto& capture = dynamic_cast<Capture&>(capturer);
    capture.source->call([&] {
        auto& sub = *capture.sub;
        sub.queue.clear();
        sub.queued_bytes = 0;
        sub.consumed = true;
        sub.waiting_idr = true;
        capture.source->keyframe = true;
    });
}

uint64_t DesktopStreamPool::capture_time(const IDxgiCapturer& capturer) {
    return dynamic_cast<const Capture&>(capturer).sub->delivered_capture_ms;
}

unsigned DesktopStreamPool::static_refresh_budget(VideoCodec codec, float quality,
                                                  EncoderBackend backend,
                                                  const std::string& implementation) {
    // RustDesk video_service/static_refresh.rs max_repeat_frames(): measured
    // per codec and hardware backend. AV1 has no static refinement.
    enum class Vendor { Nvenc, Qsv, Amf, Other };
    auto vendor = [&] {
        if (backend == EncoderBackend::Vpl) return Vendor::Qsv;
        if (backend == EncoderBackend::Ffmpeg) {
            auto named = [&](const char* name) {
                return implementation.find(name) != std::string::npos;
            };
            if (named("nvenc")) return Vendor::Nvenc;
            if (named("amf")) return Vendor::Amf;
            if (named("qsv")) return Vendor::Qsv;
        }
        return Vendor::Other;
    }();
    unsigned low = 0, low_to_balanced = 0, balanced_to_best = 0, best = 0;
    switch (codec) {
    case VideoCodec::Vp9:
        low = 40; low_to_balanced = 20; balanced_to_best = 20; best = 20;
        break;
    case VideoCodec::H264:
        switch (vendor) {
        case Vendor::Nvenc: low = 80; low_to_balanced = 50; balanced_to_best = 60; best = 30; break;
        case Vendor::Qsv: low = 80; low_to_balanced = 70; balanced_to_best = 130; best = 110; break;
        case Vendor::Amf: low = 150; low_to_balanced = 150; balanced_to_best = 170; best = 110; break;
        case Vendor::Other: low = 90; low_to_balanced = 60; balanced_to_best = 60; best = 30; break;
        }
        break;
    case VideoCodec::Hevc:
        switch (vendor) {
        case Vendor::Nvenc: low = 160; low_to_balanced = 90; balanced_to_best = 60; best = 30; break;
        case Vendor::Qsv: low = 150; low_to_balanced = 30; balanced_to_best = 30; best = 30; break;
        case Vendor::Amf: low = 200; low_to_balanced = 200; balanced_to_best = 200; best = 120; break;
        case Vendor::Other: low = 90; low_to_balanced = 60; balanced_to_best = 60; best = 30; break;
        }
        break;
    case VideoCodec::Av1:
        return 0;
    }
    if (quality >= 1.5f) return best;
    if (quality >= 0.67f) return balanced_to_best;
    if (quality >= 0.5f) return low_to_balanced;
    return low;
}

DesktopStreamPool::QualityPeer::QualityPeer(std::shared_ptr<VideoQos> policy, uint64_t id)
    : policy_(std::move(policy)), id_(id) { policy_->open(id_, now_ms()); }
DesktopStreamPool::QualityPeer::~QualityPeer() { policy_->close(id_, now_ms()); }
void DesktopStreamPool::QualityPeer::options(uint32_t fps, std::optional<VideoQos::Quality> quality) {
    policy_->options(id_, fps, quality, now_ms());
}
void DesktopStreamPool::QualityPeer::feedback(uint32_t delay, uint64_t pending, bool new_reply) {
    if (new_reply) policy_->delay(id_, delay, now_ms());
    policy_->pending(id_, pending, now_ms());
}
std::shared_ptr<DesktopStreamPool::QualityPeer> DesktopStreamPool::connect_quality(
        uint32_t max_fps, float ratio, bool abr, bool adaptive_fps) {
    std::lock_guard lock(mutex_);
    if (!qos_) qos_ = std::make_shared<VideoQos>(max_fps, ratio, abr, adaptive_fps);
    return std::make_shared<QualityPeer>(qos_, ++next_peer_);
}
Error DesktopStreamPool::reserve_audio(IDxgiCapturer& capturer, UINT bitrate) {
    auto& capture = dynamic_cast<Capture&>(capturer);
    return capture.source->call([&] {
        capture.source->audio_reserve = bitrate;
        return capture.source->apply_qos();
    });
}
} // namespace chast
