#define NOMINMAX
#include "host/stream_recorder.h"
#include "chast/host_recording.h"
#include "mp4_recorder.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

namespace chast {
namespace {
constexpr size_t kQueueBytes = 32 * 1024 * 1024;
constexpr size_t kQueueFrames = 120;
constexpr uint64_t kSegmentMs = 10 * 60 * 1000;
// A segment may grow large between checks; this keeps the floor meaningful
// without querying the volume on every frame.
constexpr uint64_t kDiskCheckMs = 5000;
namespace fs = std::filesystem;
std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}
std::string source_label(const std::wstring& key) {
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t c : key) { hash ^= static_cast<uint16_t>(c); hash *= 1099511628211ull; }
    const char* type = key.starts_with(L"camera:") ? "camera" :
                       key.starts_with(L"merged:") ? "merged" : "display";
    char label[64];
    std::snprintf(label, sizeof(label), "%s-%016llx", type, static_cast<unsigned long long>(hash));
    return label;
}
} // namespace

struct StreamRecorder::Impl {
    struct Frame {
        VideoCodec codec;
        VideoChroma chroma;
        uint32_t width, height;
        uint64_t generation, epoch;
        size_t bytes;
        std::vector<EncodedPacket> packets;
    };
    HostRecordingOptions options;
    std::string label;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Frame> queue;
    size_t queued_bytes = 0;
    bool stopping = false;
    uint64_t end_pts_ms = 0;
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> dropped{0};
    std::string pending_error;
    // The following fields belong exclusively to the producer/source thread.
    bool waiting_idr = true;
    uint64_t epoch = 0, generation = 0, rotate_at = 0;
    std::thread worker;

    Impl(HostRecordingOptions value, const std::wstring& source)
        : options(std::move(value)), label(source_label(source)), worker([this] { run(); }) {}

    void log(const std::string& message) noexcept {
        try { if (options.log) options.log("host recording [" + label + "]: " + message); }
        catch (...) {} // A logging callback must not terminate the disk worker.
    }
    fs::path create_directory() {
        if (options.directory.empty() || options.directory.find_first_of("\r\n") != std::string::npos ||
            options.directory.find('\0') != std::string::npos)
            throw std::runtime_error("choose an absolute recording directory in local settings");
        const fs::path root(std::u8string(options.directory.begin(), options.directory.end()));
        if (!root.is_absolute()) throw std::runtime_error("recording directory must be absolute");
        fs::create_directories(root);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        static std::atomic<uint64_t> serial{0};
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            char name[160];
            std::snprintf(name, sizeof(name), "%04u%02u%02u-%02u%02u%02u-%03u-%lu-%llu-%s",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                static_cast<unsigned long>(GetCurrentProcessId()),
                static_cast<unsigned long long>(++serial), label.c_str());
            const auto path = root / name;
            // Reserve a fresh directory atomically; reconnects and concurrent
            // workers cannot overwrite an earlier recording or its segments.
            if (fs::create_directory(path)) return path;
        }
        throw std::runtime_error("could not allocate a unique recording directory");
    }
    void fail(const std::string& message) {
        {
            std::lock_guard lock(mutex);
            failed = true;
            queue.clear();
            queued_bytes = 0;
        }
        log("ERROR: " + message + "; recording stopped for this source");
    }
    void run() noexcept {
        std::unique_ptr<chast_tools::Mp4Recorder> recorder;
        try {
            fs::path directory;
            unsigned part = 0;
            uint64_t active_generation = 0, active_epoch = 0;
            uint64_t reported_drops = 0;
            const auto report_drops = [&] {
                const auto total = dropped.load();
                if (total != reported_drops) {
                    log("WARNING: writer backlog dropped " + std::to_string(total - reported_drops) +
                        " frame(s); resuming in a new segment at a keyframe");
                    reported_drops = total;
                }
            };
            VideoCodec codec = VideoCodec::H264;
            uint32_t width = 0, height = 0;
            uint64_t next_disk_check = 0;
            const auto close = [&](uint64_t end) {
                if (!recorder) return;
                recorder->close(end);
                if (recorder->failed()) throw std::runtime_error(recorder->error());
                for (const auto& path : recorder->segments()) log("saved " + path);
                recorder.reset();
            };
            for (;;) {
                Frame frame;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [&] { return stopping || !queue.empty() || !pending_error.empty(); });
                    if (!pending_error.empty()) throw std::runtime_error(pending_error);
                    if (queue.empty()) break;
                    frame = std::move(queue.front());
                    queue.pop_front();
                    queued_bytes -= frame.bytes;
                }
                report_drops();
                if (recorder && frame.packets.front().pts_ms >= next_disk_check) {
                    next_disk_check = frame.packets.front().pts_ms + kDiskCheckMs;
                    std::string reason;
                    if (!recording_disk_available(utf8(directory), kMinRecordingFreeBytes, &reason))
                        throw std::runtime_error(reason);
                }
                if (recorder && (active_generation != frame.generation || active_epoch != frame.epoch ||
                    codec != frame.codec || width != frame.width || height != frame.height)) {
                    close(frame.packets.front().pts_ms);
                }
                for (const auto& packet : frame.packets) {
                    if (!recorder) {
                        if (!packet.idr) continue;
                        if (directory.empty()) {
                            std::string reason;
                            if (!recording_disk_available(options.directory, kMinRecordingFreeBytes,
                                                          &reason))
                                throw std::runtime_error(reason);
                            directory = create_directory();
                        }
                        next_disk_check = packet.pts_ms + kDiskCheckMs;
                        char name[48];
                        std::snprintf(name, sizeof(name), "part-%04u.mp4", ++part);
                        const auto path = utf8(directory / name);
                        recorder = std::make_unique<chast_tools::Mp4Recorder>();
                        std::string error;
                        if (!recorder->open(path, frame.codec, frame.width, frame.height, &error))
                            throw std::runtime_error(error);
                        active_generation = frame.generation; active_epoch = frame.epoch;
                        codec = frame.codec; width = frame.width; height = frame.height;
                        log("started " + path);
                    }
                    FrameHeader header;
                    header.codec = frame.codec; header.chroma = frame.chroma; header.width = frame.width; header.height = frame.height;
                    header.pts_ms = packet.pts_ms; header.keyframe = packet.keyframe;
                    recorder->write(header, packet.data.data(), packet.data.size());
                    if (recorder->failed()) throw std::runtime_error(recorder->error());
                }
            }
            report_drops();
            close(end_pts_ms);
        } catch (const std::exception& error) {
            // Still try to write a trailer for any already accepted samples.
            if (recorder) recorder->close();
            fail(error.what());
        } catch (...) {
            if (recorder) recorder->close();
            fail("unexpected recording failure");
        }
    }
};

StreamRecorder::StreamRecorder(HostRecordingOptions options, const std::wstring& source)
    : impl_(std::make_unique<Impl>(std::move(options), source)) {}
StreamRecorder::~StreamRecorder() { finish(0); }
bool StreamRecorder::needs_keyframe(uint64_t pts_ms) const {
    return !impl_->failed && (impl_->waiting_idr || (impl_->rotate_at && pts_ms >= impl_->rotate_at));
}
void StreamRecorder::submit(VideoCodec codec, uint32_t width, uint32_t height, uint64_t generation,
                            const std::vector<EncodedPacket>& packets, VideoChroma chroma) {
    auto& state = *impl_;
    if (state.failed || packets.empty()) return;
    size_t bytes = 0;
    bool idr = false;
    for (const auto& packet : packets) { bytes += packet.data.size(); idr |= packet.idr; }
    std::lock_guard lock(state.mutex);
    if (state.stopping || state.failed) return;
    if (bytes > kQueueBytes) {
        state.pending_error = "a compressed frame exceeded the 32 MiB recording buffer";
        state.failed = true;
        state.wake.notify_one();
        return;
    }
    if (state.generation != generation) {
        state.generation = generation;
        state.waiting_idr = true;
        ++state.epoch;
    }
    if (state.queue.size() >= kQueueFrames || state.queued_bytes + bytes > kQueueBytes) {
        state.dropped += state.queue.size();
        state.queue.clear(); state.queued_bytes = 0;
        state.waiting_idr = true;
        ++state.epoch;
    }
    if (state.waiting_idr && !idr) { ++state.dropped; return; }
    const uint64_t pts = packets.front().pts_ms;
    if (state.waiting_idr || (state.rotate_at && pts >= state.rotate_at && idr)) {
        ++state.epoch;
        state.rotate_at = pts + kSegmentMs;
    }
    state.waiting_idr = false;
    state.queue.push_back({codec, chroma, width, height, generation, state.epoch, bytes, packets});
    state.queued_bytes += bytes;
    state.wake.notify_one();
}
void StreamRecorder::finish(uint64_t end_pts_ms) {
    auto& state = *impl_;
    {
        std::lock_guard lock(state.mutex);
        if (!state.stopping) { state.stopping = true; state.end_pts_ms = end_pts_ms; }
    }
    state.wake.notify_one();
    if (state.worker.joinable()) state.worker.join();
}
} // namespace chast
