#pragma once

#include <functional>
#include <map>
#include <memory>
#include <vector>
#include "chast/decoder.h"
#include "chast/protocol.h"

namespace chast_viewer {
class FrameSink;

struct DisplayVideoStatus {
    chast::DisplayStreamInfo stream;
    uint64_t received = 0, decoded = 0, published = 0, dropped = 0, decode_errors = 0;
    size_t queued_frames = 0, queued_bytes = 0;
    bool bound = false, waiting_idr = true, ready = false;
    std::string error;
};

struct DisplayVideoTotals {
    uint64_t decoded = 0, published = 0, decode_errors = 0;
};

// Called by the session thread. Each source owns a bounded queue and a worker
// that creates, uses and destroys its decoder. Sinks stay alive until the last
// in-flight GPU callback returns, including when a view is removed.
class DisplayVideoReceiver {
public:
    using DecoderFactory = std::function<std::unique_ptr<chast::IVideoDecoder>(
        const chast::DecoderConfig&, std::string*)>;
    explicit DisplayVideoReceiver(DecoderFactory factory = chast::create_decoder);
    ~DisplayVideoReceiver();
    DisplayVideoReceiver(const DisplayVideoReceiver&) = delete;
    DisplayVideoReceiver& operator=(const DisplayVideoReceiver&) = delete;
    bool Apply(const chast::CaptureDisplaysResult&, std::string* error);
    bool Update(const chast::DisplayStreamInfo&, std::string* error);
    bool Bind(uint32_t stream_id, std::shared_ptr<FrameSink> sink);
    bool Push(const chast::FrameHeader&, std::vector<uint8_t> payload);
    void Recover(uint32_t stream_id);
    // Executes readback on the stream worker, serialized with GPU presentation.
    // A removed/rebound source calls the completion with nullptr.
    bool WithFrame(uint32_t stream_id, std::function<void(FrameSink*)> completion);
    std::vector<uint32_t> TakeKeyframeRequests();
    std::vector<DisplayVideoStatus> Snapshot() const;
    DisplayVideoTotals Totals() const;
    void Reset();
    bool Contains(uint32_t stream_id) const;
    bool empty() const { return streams_.empty(); }
private:
    struct Stream;
    DecoderFactory factory_;
    std::map<uint32_t, std::unique_ptr<Stream>> streams_;
    DisplayVideoTotals retired_;
};
} // namespace chast_viewer
