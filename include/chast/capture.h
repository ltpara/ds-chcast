#pragma once

// Public capture surface. Everything below is GPU-resident: a captured frame
// is an ID3D11Texture2D owned by the capturer, valid until the next capture,
// and reaches the encoder without a round trip through system memory.
//
// Zero-copy rules this API encodes:
//   * FrameInfo::texture lives on FrameInfo::device. An encoder on another
//     device imports it through the shared handle; one on the same device uses
//     it directly.
//   * The desktop image is never mapped on the CPU. Pointer sprites are the
//     only pixels that cross into system memory, and DXGI hands them over as
//     small shapes.
//   * Frames are delivered whole. There is no dirty/move-region tracking, so a
//     consumer that needs regions has to compute them from the textures.
//
// Hardware encoders consume NV12. The optional NV12 stage converts the BGRA8
// frame with the D3D11 video processor (one GPU blit, the same conversion
// RustDesk's hwcodec does), so the encoded frame never touches system memory
// either.
//
// There is intentionally no CPU fallback: outputs that cannot be duplicated
// with Desktop Duplication fail to create a capturer instead of silently
// degrading to GDI Screen BitBlt, which would put every frame through sysmem.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chast/com_ptr.h"
#include "chast/error.h"

namespace chast {

struct DisplayRect {
    LONG left = 0;
    LONG top = 0;
    LONG right = 0;
    LONG bottom = 0;
};

// An attached output, described the way Desktop Duplication will see it.
struct DisplayInfo {
    std::wstring device_name;      // \\.\DISPLAY1
    std::wstring adapter_name;     // adapter description from DXGI
    DisplayRect rect;              // virtual-desktop coordinates, native orientation
    UINT width = 0;                // pixels in a captured frame (rotation applied)
    UINT height = 0;
    UINT rotation_degrees = 0;     // 0/90/180/270, the panel's DXGI rotation
    bool primary = false;
    bool hdr = false;              // composited as scene-referred scRGB (FP16)
    bool advanced_color_sdr = false;  // WCG SDR: display-referred FP16
    UINT sdr_white_level = 0;      // DISPLAYCONFIG units, 1000 == 80 nits; 0 unknown
    float max_luminance_nits = 0.f;
    LUID adapter_luid{};
    UINT adapter_index = 0;
    UINT output_index = 0;
    // Adapter PCI id, from DXGI_ADAPTER_DESC1::VendorId.
    UINT adapter_vendor_id = 0;
    // Monitor handle of the output, for APIs that work per monitor (the
    // Windows Magnification API used for privacy capture takes a rect, but a
    // consumer mapping screen coordinates to a display wants this).
    HMONITOR monitor = nullptr;

    // True when the desktop is composited in FP16 and therefore needs the
    // HDR normalization pass to reach BGRA8.
    bool fp16_composition() const { return hdr || advanced_color_sdr; }
};

// Enumerates every attached output of every adapter, in the order the OS lists
// them (EnumDisplayDevices order), which is the order RustDesk's display list
// uses. Detached outputs and outputs with no area are skipped: Win+P "show
// only on 1" still enumerates the other outputs as attached but zero-sized.
std::vector<DisplayInfo> enumerate_displays();

// DisplayInfo::device_name as UTF-8, for the protocol's display list. Device
// names are normally ASCII, but the conversion is explicit so a localized
// name cannot reach the wire in UTF-16 bytes.
std::string display_name_utf8(const DisplayInfo& display);

// How the NV12 stage turns RGB into YCbCr. RustDesk's VRAM encoder converts
// with a fixed matrix and range for every frame (hwcodec's ffmpeg_vram sets
// bt709_ = false, full_range_ = false), so the stream is always BT.601 limited
// regardless of the monitor; these defaults match that, and the matrices are
// exposed because the choice is a compatibility knob, not something the
// capture path can infer.
enum class Nv12Colorimetry {
    Bt601Limited,  // RustDesk's VRAM path (and its default)
    Bt601Full,
    Bt709Limited,
    Bt709Full,
};

struct Nv12Config {
    bool enabled = false;
    Nv12Colorimetry colorimetry = Nv12Colorimetry::Bt601Limited;
};

struct CaptureConfig {
    // Ask DXGI for FP16 frames on HDR/advanced-color desktops and normalize
    // them to BGRA8 on the GPU. When false, DXGI's legacy clipped BGRA8
    // conversion is used (washed out on HDR hosts, but cheaper).
    bool want_hdr = true;
    bool capture_cursor = true;
    // Convert each frame to NV12 on the GPU for hardware encoders. The BGRA8
    // frame stays available either way.
    Nv12Config nv12;
};

// Pixel source of the current frame.
struct FrameInfo {
    ID3D11Texture2D* texture = nullptr;  // capturer-owned, valid until next capture()
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HANDLE shared_handle = nullptr;      // DXGI shared handle of `texture`, never closed
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    LUID adapter_luid{};
    uint64_t frame_number = 0;           // monotonic count of captured frames
    // Rotation the consumer still has to apply: 0 when the GPU rotated the
    // image into screen orientation, non-zero when it could not (no video
    // processor rotation caps). In that case `texture` is panel-native and
    // `width`/`height` describe the unrotated surface; an encoder must carry
    // the rotation itself or reject the frame.
    UINT rotation_degrees = 0;
    // False when DXGI left the pixel contents untouched this frame (pointer
    // only); the texture still holds the last desktop image.
    bool desktop_updated = true;
    // NV12 version of the same frame (CaptureConfig::nv12), on the same device,
    // converted from the texture above so the two always hold the same image.
    // `nv12_texture` is null when NV12 was not requested, could not be created
    // on this device, or the conversion failed for this frame; `nv12_updated`
    // tells a new conversion from a pointer-only repeat of the previous one.
    //
    // The image is the top-left nv12_width x nv12_height of the texture.
    // Those are the encoded picture size and may be smaller than width/height
    // after set_nv12_scale(); the video processor scales during the conversion.
    // The texture itself is 16-aligned (the alignment Intel oneVPL requires for
    // frames in video memory) and carries black in the padding, so a hardware
    // encoder imports it directly with nv12_width/nv12_height as its crop.
    // Consumers that read the texture on the CPU must use nv12_surface_width
    // and nv12_surface_height as the row stride instead.
    ID3D11Texture2D* nv12_texture = nullptr;
    HANDLE nv12_shared_handle = nullptr;
    bool nv12_updated = false;
    UINT nv12_width = 0;
    UINT nv12_height = 0;
    UINT nv12_surface_width = 0;
    UINT nv12_surface_height = 0;
};

struct CursorInfo {
    bool visible = false;
    bool position_valid = false;
    POINT position{};  // in the published frame's space (rotation applied); add
                       // DisplayInfo::rect origin for screen space
    bool shape_valid = false;
    // Zero when no shape is cached (the SDK enum has no "none" value).
    DXGI_OUTDUPL_POINTER_SHAPE_TYPE shape_type =
        static_cast<DXGI_OUTDUPL_POINTER_SHAPE_TYPE>(0);
    // Dimensions of the sprite as drawn, i.e. bitmap.size() / 4 == width*height.
    UINT width = 0;
    UINT height = 0;
    UINT pitch = 0;
    // Premultiplied BGRA sprite, width*height*4 bytes.
    std::vector<uint8_t> bitmap;
    // 1 byte per pixel, non-zero where the sprite must be blended with
    // DST_INVERT instead of alpha. Populated for masked-color pointers and for
    // monochrome pointers whose AND and XOR masks both cover a pixel, where
    // the desktop shows through inverted.
    std::vector<uint8_t> invert;
    // Where the shape's first pixel sits inside a drawn sprite. DXGI places the
    // shape's top-left at `position`, so the sprite is drawn with its top-left
    // at position - origin_offset(). Non-zero only for an outlined sprite,
    // where a one-pixel ring wraps the shape.
    POINT origin_offset{};
    // Hot spot within the sprite: the pixel that lands on the pointer position.
    // DXGI reports it relative to the shape it handed over, so the outline
    // offset (1,1) is already added here.
    POINT hotspot{};
    // True when the sprite was outlined: it contained inverted pixels, which
    // were made black and surrounded by a white ring so the pointer stays
    // visible over any desktop. `invert` is empty in that case.
    bool outlined = false;
    // Identity of this sprite (53-bit hash of its pixels), stable across
    // revisits. Equal ids mean an identical image; a consumer that has already
    // sent one for an id can skip the transfer and just reposition.
    uint64_t content_id = 0;
    // Increments whenever the shape or position changes.
    uint64_t revision = 0;
};

enum class CaptureResult {
    Frame,       // FrameInfo and CursorInfo describe the newest state
    NoNewFrame,  // nothing changed within the timeout
    ReinitNeeded,  // AccessLost or device reset: call reinitialize()
    FatalError,
};

// The D3D11 device a capturer runs on, exported so an encoder can pick the
// right device: import the frame's shared handle into `device` to stay
// on-GPU, and choose a hardware encoder for `vendor_id` (1 = AMD, 0x10DE =
// NVIDIA, 0x8086 = Intel) or fall back to the software one.
struct AdapterDevice {
    ID3D11Device* device = nullptr;  // capturer-owned, valid for its lifetime
    UINT vendor_id = 0;
    LUID luid{};
};

class IDxgiCapturer {
public:
    virtual ~IDxgiCapturer() = default;

    // Blocks up to `timeout` for desktop activity. After Frame, frame() and
    // cursor() are valid until the next capture().
    virtual CaptureResult capture(std::chrono::milliseconds timeout) = 0;

    virtual const FrameInfo& frame() const = 0;
    virtual const CursorInfo& cursor() const = 0;
    virtual const DisplayInfo& display() const = 0;
    virtual AdapterDevice adapter_device() const = 0;

    // Recreates device + duplication after ReinitNeeded. Display, cursor and
    // frame state are reset; the next capture() returns a full frame.
    virtual Error reinitialize() = 0;

    // Scales the published NV12 image to `percent` of the frame size (100
    // restores it). The video processor that converts BGRA to NV12 also does
    // the downscale, so the image never leaves video memory and FrameInfo's
    // BGRA texture stays full size; only the encoded picture shrinks. The next
    // capture() is a full conversion even if the desktop did not change.
    //
    // The scale survives reinitialize(). On failure the previous scale stays
    // in effect and the error says why. A capturer created without NV12
    // remembers the value for when NV12 is configured.
    virtual Error set_nv12_scale(UINT percent) = 0;

    // Last error from capture(), for logging alongside FatalError.
    virtual const Error& last_error() const = 0;
};

// False on sessions where Desktop Duplication cannot work at all (RDP,
// non-interactive sessions on older builds). There is no CPU fallback.
bool duplication_supported();

// Creates a capturer for the display with the given index into
// enumerate_displays(), or for the primary display when index is out of range.
// Returns nullptr with the error stored in `*error` (if non-null) when the
// output cannot be duplicated.
std::unique_ptr<IDxgiCapturer> create_capturer(size_t display_index, const CaptureConfig& config,
                                               std::string* error = nullptr);
std::unique_ptr<IDxgiCapturer> create_capturer(const DisplayInfo& display,
                                               const CaptureConfig& config,
                                               std::string* error = nullptr);

}  // namespace chast
