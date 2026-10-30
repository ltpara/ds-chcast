#pragma once

// Host half of the remote camera (protocol v29). It owns the Media Foundation
// capture, the camera's own hardware encoder on the capture adapter, and the
// queue between them, and relays encoded access units over the QuicSession's
// CCAM stream.
//
// The split follows TerminalRelay: pump() runs on the host session thread and
// is the only code that touches the QuicSession; a worker thread blocks on
// the camera and on the encoder. Encoded frames wait in a small bounded
// queue, so a viewer that cannot keep up costs camera frames, never desktop
// frames. Dropping an encoded frame breaks the reference chain, so the worker
// asks the encoder for an IDR before the next packet: the viewer recovers
// instead of showing corruption until the camera stops.
//
// The camera has its own D3D11 device on the adapter of the running desktop
// capturer (the same LUID, so encoding stays on the capture card) because the
// capturer's immediate context belongs to the session thread.

#include <cstdint>
#include <functional>
#include <memory>
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
    // Requested capture rate. The camera may deliver another one; the
    // encoder is told what actually arrives.
    uint32_t framerate = 30;
};

class CameraRelay {
public:
    // Supplies the adapter of the current desktop capturer. Queried at every
    // camera start, so a capturer rebuilt after a display switch or a device
    // reset is picked up instead of encoding on a card that is gone.
    using AdapterProvider = std::function<LUID()>;

    CameraRelay(QuicSession& session, AdapterProvider adapter, const CameraRelayConfig& config);
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

    QuicSession& session_;
    AdapterProvider adapter_;
    CameraRelayConfig config_;
    // The list the viewer last received. An id names a device there, not in a
    // newer enumeration that may have shifted after a hot-plug.
    std::vector<CameraDevice> listed_;
    std::unique_ptr<Worker> worker_;
    uint64_t started_ = 0;
    uint64_t failures_ = 0;
    uint64_t frames_ = 0;
};

}  // namespace chast
