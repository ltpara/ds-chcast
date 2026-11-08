#include "display_video_receiver.h"
#include "viewer_engine.h"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace chast_viewer {
namespace {
using Clock = std::chrono::steady_clock;
bool valid(const chast::DisplayStreamInfo& stream) {
    const auto& info = stream.info;
    return info.mode == chast::SessionMode::Desktop && !info.control_only && !info.files_only &&
        chast::valid_video_format(info.codec, info.chroma) && info.width && info.height && info.width <= 8192 && info.height <= 8192 &&
        uint64_t(info.width) * info.height <= 7680ull * 4320;
}
bool format_changed(const chast::ServerInfo& a, const chast::ServerInfo& b) {
    return a.codec != b.codec || a.chroma != b.chroma || a.width != b.width || a.height != b.height;
}
}

struct DisplayVideoReceiver::Stream {
    struct Packet { chast::FrameHeader header; std::vector<uint8_t> bytes; };
    mutable std::mutex mutex;
    std::condition_variable cv;
    DisplayVideoStatus status;
    std::shared_ptr<FrameSink> sink;
    std::deque<Packet> queue;
    DecoderFactory factory;
    uint64_t revision = 1;
    bool stop = false, keyframe = true;
    Clock::time_point last_request = Clock::now() - std::chrono::seconds(1);
    std::thread worker;
    std::function<void(FrameSink*)> frame_job;
    uint64_t frame_job_revision = 0;

    Stream(chast::DisplayStreamInfo info, DecoderFactory make) : factory(std::move(make)) {
        status.stream = std::move(info);
        worker = std::thread([this] { Run(); });
    }
    ~Stream() { Stop(); }
    void Stop() {
        { std::lock_guard lock(mutex); stop = true; }
        cv.notify_one();
        if (worker.joinable()) worker.join();
    }
    void Clear() {
        status.dropped += queue.size(); queue.clear();
        status.queued_frames = status.queued_bytes = 0;
        status.waiting_idr = true; keyframe = true;
    }
    void Run() {
        std::unique_ptr<chast::IVideoDecoder> decoder;
        std::shared_ptr<FrameSink> output;
        uint64_t version = 0;
        unsigned failures = 0, rebuilds = 0;
        for (;;) {
            Packet packet;
            chast::DisplayStreamInfo info;
            std::function<void(FrameSink*)> job;
            bool job_valid = false;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return stop || revision != version || !queue.empty() || frame_job; });
                if (stop) {
                    job = std::move(frame_job);
                    lock.unlock();
                    if (job) job(nullptr);
                    break;
                }
                if (revision != version) {
                    lock.unlock(); decoder.reset(); lock.lock();
                    version = revision; output = sink; failures = rebuilds = 0;
                }
                if (frame_job) {
                    job = std::move(frame_job);
                    job_valid = frame_job_revision == version && status.ready;
                } else {
                    if (queue.empty()) continue;
                    packet = std::move(queue.front()); queue.pop_front();
                    status.queued_bytes -= packet.bytes.size(); status.queued_frames = queue.size();
                }
                info = status.stream;
            }
            if (job) { job(job_valid ? output.get() : nullptr); continue; }
            if (!output) continue;
            std::string error;
            if (!decoder) {
                chast::DecoderConfig config;
                config.codec = info.info.codec; config.chroma = info.info.chroma; config.d3d11_device = output->device();
                config.gpu_handler = [this, output, version](const chast::GpuFrame& frame) {
                    {
                        std::lock_guard lock(mutex);
                        if (stop || revision != version) return chast::Error::success();
                        ++status.decoded;
                    }
                    if (!output->OnGpuFrame(frame.texture, frame.subresource, frame.width,
                            frame.height, frame.crop_x, frame.crop_y, frame.pts_ms))
                        return chast::Error(chast::Status::Unavailable, "display: " + output->error());
                    std::lock_guard lock(mutex);
                    if (revision == version) { ++status.published; status.ready = true; status.error.clear(); }
                    return chast::Error::success();
                };
                if (config.d3d11_device) decoder = factory(config, &error);
                else error = "display GPU is unavailable";
            }
            std::vector<chast::DecodedFrame> cpu_frames;
            auto result = decoder ? decoder->decode(packet.bytes.data(), packet.bytes.size(),
                packet.header.pts_ms, packet.header.keyframe, cpu_frames) :
                chast::Error(chast::Status::Unavailable, error);
            if (result.ok() && !cpu_frames.empty())
                result = chast::Error(chast::Status::Unsupported, "desktop decoder returned CPU frames");
            if (result.ok()) { failures = 0; continue; }
            decoder.reset();
            const bool rebuild = output->fatal() && rebuilds++ == 0;
            if (rebuild) output->Reset();
            std::lock_guard lock(mutex);
            if (revision != version) continue;
            ++status.decode_errors;
            if (rebuild) status.ready = false;
            status.error = result.to_string();
            Clear();
            if (++failures >= 30) keyframe = false;
        }
        // Decoder destruction happens here on the same worker as decode.
    }
};

DisplayVideoReceiver::DisplayVideoReceiver(DecoderFactory factory) : factory_(std::move(factory)) {}
DisplayVideoReceiver::~DisplayVideoReceiver() = default;

bool DisplayVideoReceiver::Apply(const chast::CaptureDisplaysResult& result, std::string* error) {
    std::set<uint32_t> ids;
    std::set<uint16_t> displays;
    for (const auto& stream : result.streams) {
        if (!valid(stream) || !ids.insert(stream.stream_id).second ||
            !displays.insert(stream.display_index).second || ids.size() > chast::kMaxDisplayCount) {
            if (error) *error = "invalid desktop subscription description";
            return false;
        }
    }
    for (auto it = streams_.begin(); it != streams_.end();) {
        if (ids.count(it->first)) { ++it; continue; }
        it->second->Stop();
        retired_.decoded += it->second->status.decoded;
        retired_.published += it->second->status.published;
        retired_.decode_errors += it->second->status.decode_errors;
        it = streams_.erase(it);
    }
    for (const auto& stream : result.streams) {
        if (!Contains(stream.stream_id)) streams_.emplace(stream.stream_id, std::make_unique<Stream>(stream, factory_));
        else Update(stream, error);
    }
    return true;
}
bool DisplayVideoReceiver::Update(const chast::DisplayStreamInfo& info, std::string* error) {
    auto it = streams_.find(info.stream_id);
    if (it == streams_.end()) return true;
    if (!valid(info)) { if (error) *error = "invalid desktop stream geometry"; return false; }
    auto& stream = *it->second;
    {
        std::lock_guard lock(stream.mutex);
        if (format_changed(stream.status.stream.info, info.info)) {
            ++stream.revision; stream.Clear(); stream.status.error.clear(); stream.status.ready = false;
        }
        stream.status.stream = info;
    }
    stream.cv.notify_one();
    return true;
}
bool DisplayVideoReceiver::Bind(uint32_t id, std::shared_ptr<FrameSink> sink) {
    const auto it = streams_.find(id);
    if (it == streams_.end()) return false;
    if (sink) for (const auto& [other_id, other] : streams_) {
        std::lock_guard lock(other->mutex);
        if (other_id != id && other->sink == sink) return false;
    }
    auto& stream = *it->second;
    {
        std::lock_guard lock(stream.mutex);
        if (stream.sink == sink) return true;
        stream.sink = std::move(sink); stream.status.bound = stream.sink != nullptr;
        ++stream.revision; stream.Clear(); stream.status.error.clear(); stream.status.ready = false;
    }
    stream.cv.notify_one();
    return true;
}
bool DisplayVideoReceiver::Push(const chast::FrameHeader& header, std::vector<uint8_t> payload) {
    const auto it = streams_.find(header.stream_id);
    if (it == streams_.end()) return false;
    auto& stream = *it->second;
    {
        std::lock_guard lock(stream.mutex);
        ++stream.status.received;
        const auto& info = stream.status.stream.info;
        if (!stream.sink) { ++stream.status.dropped; return false; }
        if (header.width != info.width || header.height != info.height || header.codec != info.codec || header.chroma != info.chroma ||
            payload.empty() || payload.size() > chast::kMaxFramePayload ||
            chast::crc32(payload.data(), payload.size()) != header.payload_crc32) {
            ++stream.status.dropped; stream.Clear(); return false;
        }
        if (stream.queue.size() >= 60 || stream.status.queued_bytes + payload.size() > 8u * 1024u * 1024u)
            stream.Clear();
        if (stream.status.waiting_idr && !header.idr) { ++stream.status.dropped; return false; }
        if (header.idr) { stream.status.waiting_idr = false; stream.keyframe = false; }
        stream.status.queued_bytes += payload.size();
        stream.queue.push_back({header, std::move(payload)}); stream.status.queued_frames = stream.queue.size();
    }
    stream.cv.notify_one();
    return true;
}
void DisplayVideoReceiver::Recover(uint32_t id) {
    const auto it = streams_.find(id);
    if (it == streams_.end()) return;
    std::lock_guard lock(it->second->mutex);
    it->second->Clear();
}
bool DisplayVideoReceiver::WithFrame(uint32_t id, std::function<void(FrameSink*)> completion) {
    const auto it = streams_.find(id);
    if (it == streams_.end()) return false;
    auto& stream = *it->second;
    {
        std::lock_guard lock(stream.mutex);
        if (!stream.status.ready || !stream.sink || stream.frame_job) return false;
        stream.frame_job = std::move(completion); stream.frame_job_revision = stream.revision;
    }
    stream.cv.notify_one();
    return true;
}
std::vector<uint32_t> DisplayVideoReceiver::TakeKeyframeRequests() {
    std::vector<uint32_t> requests;
    const auto now = Clock::now();
    for (const auto& [id, stream] : streams_) {
        std::lock_guard lock(stream->mutex);
        if (stream->keyframe && stream->sink && now - stream->last_request >= std::chrono::seconds(1)) {
            requests.push_back(id); stream->last_request = now;
        }
    }
    return requests;
}
std::vector<DisplayVideoStatus> DisplayVideoReceiver::Snapshot() const {
    std::vector<DisplayVideoStatus> result;
    for (const auto& [id, stream] : streams_) {
        std::lock_guard lock(stream->mutex); result.push_back(stream->status);
    }
    return result;
}
bool DisplayVideoReceiver::Contains(uint32_t id) const { return streams_.count(id) != 0; }
DisplayVideoTotals DisplayVideoReceiver::Totals() const {
    auto result = retired_;
    for (const auto& stream : Snapshot()) {
        result.decoded += stream.decoded; result.published += stream.published; result.decode_errors += stream.decode_errors;
    }
    return result;
}
void DisplayVideoReceiver::Reset() { Apply({}, nullptr); }
} // namespace chast_viewer
