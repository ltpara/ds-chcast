#pragma once

// Client-side viewer engine for the Flutter shell:
//
//   QUIC client -> VPL decoder (D3D11 NV12) -> GPU surface texture
//
// The engine owns one worker thread and is deliberately independent of
// Flutter. The UI polls Snapshot() for state and counters; decoded pictures are
// pushed to the sink from the worker thread as D3D11 textures while the
// decoder still owns the surface.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "chast/encoder.h"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace chast_viewer {

class FrameSink {
public:
    virtual ~FrameSink() = default;
    // The D3D11 device shared with the decoder and the renderer.
    virtual ID3D11Device* device() = 0;
    // Called on the session thread with the decoder's NV12 texture and the
    // array slice that holds the picture. Returns true when the frame was
    // published to Flutter, false when it was dropped. Fatal display failures
    // are reported through error().
    virtual bool OnGpuFrame(ID3D11Texture2D* nv12, uint32_t subresource, uint32_t width,
                            uint32_t height, uint32_t crop_x, uint32_t crop_y,
                            uint64_t pts_ms) = 0;
    virtual std::string error() const = 0;
};

enum class ViewerState {
    kIdle,
    kConnecting,
    kHandshaking,
    kStreaming,
    kEnded,
    kFailed,
};

struct ViewerStatus {
    ViewerState state = ViewerState::kIdle;
    std::string error;
    std::string host;
    uint16_t port = 0;
    chast::VideoCodec codec = chast::VideoCodec::H264;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t framerate = 0;
    uint32_t server_bitrate = 0;
    std::string decoder;
    std::string server_encoder;
    uint64_t received = 0;
    uint64_t decoded = 0;
    uint64_t keyframes = 0;
    uint64_t published = 0;
    uint64_t crc_errors = 0;
    uint64_t decode_errors = 0;
    uint64_t protocol_errors = 0;
    uint64_t gaps = 0;
    uint64_t bytes = 0;
    double fps = 0.0;
    double kbps = 0.0;
    uint64_t elapsed_ms = 0;
    bool video_finished = false;
};

class ViewerEngine {
public:
    explicit ViewerEngine(FrameSink* sink);
    ~ViewerEngine();

    ViewerEngine(const ViewerEngine&) = delete;
    ViewerEngine& operator=(const ViewerEngine&) = delete;

    // Starts a session thread. Fails when one is already running; call Stop()
    // first to switch targets.
    bool Start(const std::string& host, uint16_t port);
    // Requests the session to end and waits for the worker to tear down the
    // QUIC connection. Safe to call when no session is running.
    void Stop();

    ViewerStatus Snapshot() const;

private:
    void Run(std::string host, uint16_t port);
    void SetState(ViewerState state, std::string error = {});

    FrameSink* sink_;
    mutable std::mutex mutex_;
    ViewerStatus status_;
    std::chrono::steady_clock::time_point started_at_;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
};

}  // namespace chast_viewer
