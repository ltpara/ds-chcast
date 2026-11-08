#pragma once

// Host half of the remote camera (protocol v29). Subscribes to a camera-owned
// video service and relays compressed pictures over the session's CCAM stream.
//
// The split follows TerminalRelay: pump() runs on the host session thread and
// is the only code that touches the QuicSession; a worker thread blocks on
// its shared video subscription. Encoded frames wait in a small bounded
// queue, so a viewer that cannot keep up costs camera frames, never desktop
// frames. Dropping an encoded frame breaks the reference chain, so the worker
// asks the encoder for an IDR before the next packet: the viewer recovers
// instead of showing corruption until the camera stops.
//
// The camera has its own D3D11 device on the selected adapter. Capture,
// encoding and global codec negotiation run on the video service thread.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chast/camera.h"
#include "chast/encoder.h"
#include "chast/quic.h"

namespace chast {

struct CameraRelayConfig {
    VideoCodec codec = VideoCodec::H264;
    // RustDesk's Quality knob, applied to hardware_bitrate() for the camera's
    // own size.
    float quality_ratio = 0.67f;
    EncoderBackend backend = EncoderBackend::Auto;
    // Requested capture rate. A device may deliver fewer frames; the shared
    // encoder follows the active video policy and timestamps each picture.
    uint32_t framerate = 30;
    std::function<std::vector<CameraDevice>()> devices = enumerate_cameras;
    std::function<std::unique_ptr<IDxgiCapturer>(const CameraDevice&, LUID, uint32_t,
                                               std::string*)> capture = create_camera_capturer;
    bool stream_options = false; // Camera-only sessions own their video options.
};

class CameraRelay {
public:
    // Supplies the adapter of the current desktop capturer. Queried at every
    // camera start, so a capturer rebuilt after a display switch or a device
    // reset is picked up instead of encoding on a card that is gone.
    using AdapterProvider = std::function<LUID()>;
    using CaptureFactory = std::function<std::unique_ptr<IDxgiCapturer>(std::string*)>;
    // The host binds this to its shared pool and the connection's codec peer.
    // Desktop plus camera is one vote; a camera-only viewer has its own peer.
    struct Subscription {
        // Keeps a camera-only quality participant alive until capture closes.
        std::function<void(const StreamOptions&)> quality;
        std::unique_ptr<IDxgiCapturer> capture;
    };
    using Subscribe = std::function<Subscription(
        const std::wstring&, CaptureFactory, std::string*)>;

    CameraRelay(QuicSession& session, AdapterProvider adapter, const CameraRelayConfig& config,
                Subscribe subscribe);
    ~CameraRelay();
    CameraRelay(const CameraRelay&) = delete;
    CameraRelay& operator=(const CameraRelay&) = delete;

    // Handles every queued list, start and stop request and sends the frames
    // the worker produced. Never blocks on the camera. Returns human-readable
    // state changes for the host log.
    std::vector<std::string> pump();

    // True while a camera is open or opening. The host loop shortens its
    // capture wait while this holds, so an idle desktop does not throttle the
    // camera to the idle capture cadence.
    bool streaming() const;

    uint64_t cameras_started() const { return started_; }
    uint64_t failures() const { return failures_; }
    uint64_t frames_sent() const { return frames_; }

private:
    struct Worker;

    void send_list();
    void start_camera(uint16_t camera_id, std::vector<std::string>& events);
    void drive(std::vector<std::string>& events);
    void send_frames(std::vector<std::string>& events);
    void stream_options();

    QuicSession& session_;
    AdapterProvider adapter_;
    CameraRelayConfig config_;
    Subscribe subscribe_;
    std::optional<StreamOptions> options_;
    // The list the viewer last received. An id names a device there, not in a
    // newer enumeration that may have shifted after a hot-plug.
    std::vector<CameraDevice> listed_;
    std::unique_ptr<Worker> worker_;
    uint64_t started_ = 0;
    uint64_t failures_ = 0;
    uint64_t frames_ = 0;
};

}  // namespace chast
