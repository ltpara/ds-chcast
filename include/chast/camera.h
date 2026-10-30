#pragma once

// Camera capture for the remote-camera stream (protocol v29).
//
// Media Foundation is the only webcam provider here: DXGI has no camera path,
// and the Media Foundation source reader hands frames over in system memory.
// This is therefore the one capture path in chast that starts in RAM; the
// host relay uploads each NV12 frame into a shared D3D11 texture on the
// capture adapter and the hardware encoder takes it from there, so encoding
// stays on the GPU and on the adapter the desktop stream already uses.
//
// As with desktop capture there is no software encoder fallback: a machine
// whose GPU cannot hardware-encode fails the camera start instead of silently
// moving the camera onto the CPU.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "chast/error.h"

namespace chast {

struct CameraDevice {
    // 1-based position in the enumeration. The host answers StartCamera
    // against the list it last sent to the viewer, so an id always names the
    // device the viewer saw.
    uint16_t id = 0;
    // UTF-8 friendly name; localized names are not ASCII, so no byte copy.
    std::string name;
    // Device interface path (\\?\usb#vid_...#{...}); opens the device. The
    // name is only for display.
    std::string symbolic_link;
    // The first camera of the enumeration. Windows has no default-camera
    // concept; the viewer offers this one as the initial choice.
    bool is_default = false;
};

// Every video capture source Media Foundation can activate, in its
// enumeration order. A source without a symbolic link cannot be opened and is
// skipped. Ids restart at 1 for every call.
std::vector<CameraDevice> enumerate_cameras();

// One tightly packed NV12 image: width*height luma bytes, then
// width*height/2 interleaved chroma bytes (U,V pairs), every row unpadded.
struct CameraFrame {
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    // Source presentation time on the reader timeline, milliseconds.
    uint64_t timestamp_ms = 0;
};

// One opened camera. The source reader runs asynchronously, so stop() may be
// called from another thread and interrupts a parked read(); every other
// method belongs to the thread that called open_camera().
class ICameraCapture {
public:
    using FrameCallback = std::function<void(CameraFrame&& frame)>;

    virtual ~ICameraCapture() = default;

    // Waits up to `timeout` for the next frame and calls `frame` with it.
    // WaitTimeout is normal (no frame in that window; call again),
    // Unavailable means stop() or the device went away, CaptureFailed is a
    // Media Foundation failure and ends the capture.
    virtual Error read(std::chrono::milliseconds timeout, const FrameCallback& frame) = 0;

    // Wakes a parked read() and cancels the outstanding reader request. Safe
    // from any thread; the next read() reports Unavailable.
    virtual void stop() = 0;

    // Size and rate the reader is actually delivering (native size, the rate
    // negotiated from the requested one).
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
    virtual uint32_t framerate() const = 0;
    virtual const CameraDevice& device() const = 0;
};

// Opens `device` and makes the reader deliver NV12 at its native size,
// converting on the way when the device does not offer NV12 or the requested
// rate itself. The calling thread must be initialized as MTA (source readers
// are not usable from an STA); the camera worker calls CoInitializeEx
// COINIT_MULTITHREADED before opening. Returns nullptr with *error set on
// failure.
std::unique_ptr<ICameraCapture> open_camera(const CameraDevice& device, uint32_t framerate,
                                            std::string* error = nullptr);

}  // namespace chast
