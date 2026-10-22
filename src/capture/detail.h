#pragma once

// Internal capture building blocks. Public callers only see chast/capture.h;
// the pieces below exist so the capturer stays an orchestrator and the GPU
// stages can be exercised against a known texture.

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>  // ID3D11VideoContext1, the color-space-1 setters
#include <dxgi1_6.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "chast/capture.h"
#include "chast/com_ptr.h"
#include "chast/error.h"

namespace chast::detail {

// Color state of an output as the compositor is currently using it.
struct ColorState {
    bool hdr = false;          // scRGB, scene-referred, 1.0 == 80 nits
    bool wcg = false;          // advanced-color SDR, FP16, display-referred
    UINT sdr_white_level = 0;  // DISPLAYCONFIG units, 1000 == 80 nits; 0 unknown
    float max_luminance = 0.f; // nits; 0 unknown

    bool fp16() const { return hdr || wcg; }
};

// Fills the output's DXGI description and, where the stack reports it, its
// color state. Reading through IDXGIOutput6 keeps this correct for both HDR
// and Windows 11 advanced-color SDR desktops, which share FP16 composition.
struct OutputDesc {
    DXGI_OUTPUT_DESC desc{};
    DXGI_OUTPUT_DESC1 desc1{};
    bool has_desc1 = false;
    ColorState color;
};

OutputDesc describe_output(IDXGIOutput* output);

// State of a live duplication plus the color state of the output behind it.
// The rotation is only in the output description, so it is carried alongside
// the duplication description here.
struct DuplicationDesc {
    DXGI_OUTDUPL_DESC desc{};
    ColorState color;
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
};
DuplicationDesc describe_duplication(IDXGIOutputDuplication* duplication, IDXGIOutput* output);

std::string to_utf8(const std::wstring& wide);

// SDR white level of the output with the given GDI name, in DISPLAYCONFIG
// units (1000 == 80 nits == scRGB 1.0). Zero when the platform does not
// report one (before Windows 10 1709) or the display is not active.
UINT query_sdr_white_level(const WCHAR* device_name);

// Re-reads the color state of a named output. Advanced Color is dynamic: HDR
// can be toggled, or an advanced-color SDR desktop can become an HDR one,
// without the duplication being lost, and a DXGI output's description is a
// snapshot taken by the factory that enumerated it. Returns false when the
// output is gone (display disconnected or topology changed), which the
// capturer treats as a hint to recreate everything.
bool query_color_state(const std::wstring& device_name, ColorState& out);

// Rate-limits the color-state queries. Advanced Color state changes at human
// speed and each query walks the whole display topology, so once a second is
// plenty; force=true is used when a reinit already proved the state moved.
class ColorStateMonitor {
public:
    explicit ColorStateMonitor(std::wstring device_name);

    // Returns the current state, or the last known one when the query fails
    // or is still rate-limited. `changed` is set when the returned state
    // differs from the previous successful query.
    ColorState refresh(bool force, bool* changed);

    const std::wstring& device_name() const noexcept { return device_name_; }

private:
    std::wstring device_name_;
    ColorState state_{};
    std::chrono::steady_clock::time_point last_query_{};
    bool initialized_ = false;
};

// Compiles and owns the FP16 -> BGRA8 normalization pass. Only constructed on
// outputs that compose in FP16; SDR outputs never pay for it, and a machine
// without D3DCompile simply never creates one.
class HdrPass {
public:
    // True when this process can build HDR passes: D3DCompile is present and
    // no pass has failed permanently yet. Callers read it before asking DXGI
    // for FP16 frames, so a machine that already failed once does not pay the
    // conversion cost -- or the failed duplication -- again.
    static bool available();
    // False once this process has proven it cannot build HDR passes anywhere
    // (d3dcompiler missing, shaders rejected). Callers read it before asking
    // DXGI for FP16 frames, so a machine that already failed once does not pay
    // the conversion cost -- or the failed duplication -- again.
    static bool unavailable();
    // Latches unavailable(): set when a pass fails for a reason retrying
    // cannot fix (Unsupported, InvalidCall).
    static void mark_unavailable();

    HdrPass(ID3D11Device* device, ID3D11DeviceContext* context);

    struct Params {
        bool hdr = false;
        // DISPLAYCONFIG units, 1000 == 80 nits == scRGB 1.0. Applies only to
        // HDR desktops: advanced-color SDR is already display-referred and
        // only needs the scRGB -> sRGB transfer.
        UINT sdr_white_level = 1000;
        float max_luminance = 0.f;
    };

    // Renders source into an owned BGRA8 texture (created with
    // D3D11_RESOURCE_MISC_SHARED so the encoder can import it) and returns it.
    // The texture is reused until the next call or a size change.
    Error convert(ID3D11Texture2D* source, const Params& params, ID3D11Texture2D** out);

    // DXGI shared handle of the current target texture; nullptr before the
    // first convert().
    HANDLE shared_handle() const;

private:
    Error ensure_target(UINT width, UINT height);
    Error ensure_source_view(ID3D11Texture2D* source);
    void update_params(const Params& params);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11Texture2D> target_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11ShaderResourceView> srv_;
    ID3D11Texture2D* srv_source_ = nullptr;  // srv_ keeps it alive
    float inv_white_ = 0.0f;
    bool params_dirty_ = true;
    UINT width_ = 0;
    UINT height_ = 0;
};

// Rotates a duplicated desktop image on the GPU through the video processor,
// so portrait displays never force a CPU rotation. When the device lacks the
// rotation feature the pass stays not-ready and the capturer reports the
// pending rotation alongside the unrotated texture.
class RotationPass {
public:
    RotationPass() = default;

    Error initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                     DXGI_MODE_ROTATION rotation, UINT input_width, UINT input_height,
                     UINT output_width, UINT output_height);

    bool ready() const noexcept { return static_cast<bool>(processor_); }

    // Runs the rotation for this frame; the result stays valid until the next
    // call.
    Error rotate(ID3D11Texture2D* source, ID3D11Texture2D** out);

    HANDLE shared_handle() const;

private:
    Error ensure_target(const D3D11_TEXTURE2D_DESC& source_desc);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11Texture2D> target_;
    UINT target_width_ = 0;
    UINT target_height_ = 0;
    DXGI_FORMAT target_format_ = DXGI_FORMAT_UNKNOWN;
    D3D11_VIDEO_PROCESSOR_ROTATION rotation_ = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
    UINT input_width_ = 0;
    UINT input_height_ = 0;
    UINT output_width_ = 0;
    UINT output_height_ = 0;
};

// Converts the BGRA8 frame to NV12 through the D3D11 video processor, which is
// the same path RustDesk's hwcodec takes (NativeDevice::BgraToNv12): one blit
// from the RGB texture into an NV12 render target, colorspace in/out set on
// the stream and the output. This is the only supported conversion -- the
// image stays on the GPU, and a device without the video processor leaves the
// pass not-ready so the capturer just doesn't publish NV12.
//
// The same blit can also downscale: the stream's source rect stays the whole
// input frame while the destination rect covers a smaller target. That is how
// adaptive resolution shrinks the encoded picture without a second pass and
// without leaving the GPU.
class Nv12Pass {
public:
    Nv12Pass() = default;

    // `colorimetry` picks the output matrix/range, RustDesk's BT.601 limited
    // unless told otherwise. `output_width`/`output_height` are the image size
    // the encoder will see; equal to the input size when not scaling. Returns
    // Unsupported when the device has no video processor for these dimensions,
    // in which case the pass stays not-ready.
    Error initialize(ID3D11Device* device, ID3D11DeviceContext* context, UINT input_width,
                     UINT input_height, UINT output_width, UINT output_height,
                     Nv12Colorimetry colorimetry);

    bool ready() const noexcept { return static_cast<bool>(processor_); }

    // Converts `source` into the owned NV12 texture; the result stays valid
    // until the next call. `source` is the input frame size the pass was
    // initialized with; the output is output_width() x output_height().
    Error convert(ID3D11Texture2D* source, ID3D11Texture2D** out);

    // The display image is the top-left width() x height() of the texture;
    // the texture itself is rounded up to 16 pixels so a hardware encoder
    // (oneVPL wants 16-aligned surfaces in video memory) can import it
    // directly with the frame size as its crop.
    UINT width() const noexcept { return width_; }
    UINT height() const noexcept { return height_; }
    UINT surface_width() const noexcept { return surface_width_; }
    UINT surface_height() const noexcept { return surface_height_; }

    HANDLE shared_handle() const;

    // DXGI color spaces the pass applies, for diagnostics.
    DXGI_COLOR_SPACE_TYPE input_color_space() const noexcept { return input_space_; }
    DXGI_COLOR_SPACE_TYPE output_color_space() const noexcept { return output_space_; }

private:
    Error ensure_target();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoContext1> video_context1_;  // color-space setters, Windows 8+
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11Texture2D> target_;
    DXGI_COLOR_SPACE_TYPE input_space_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    DXGI_COLOR_SPACE_TYPE output_space_ = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
    UINT input_width_ = 0;   // frame the pass reads
    UINT input_height_ = 0;
    UINT width_ = 0;         // image the encoder sees, may be scaled down
    UINT height_ = 0;
    UINT surface_width_ = 0;   // width_ rounded up to 16
    UINT surface_height_ = 0;  // height_ rounded up to 16
};

// Decides what the capturer must do after a stage failed. Status::Unsupported
// and InvalidCall are permanent: the same pass will fail on every frame, so
// retrying is pointless. Everything else may be transient (device removed,
// surface lost).
inline bool stage_failure_is_permanent(Status status) noexcept {
    return status == Status::Unsupported || status == Status::InvalidCall;
}

// Decodes the IDXGIOutputDuplication pointer into a shape, position and bitmap
// the encoder can composite itself. Shape pixels are the only data in the
// capture path that reaches system memory; the desktop image never does.
//
// The exported sprite is premultiplied BGRA plus, for masked-color pointer
// shapes, a 1-byte-per-pixel invert mask: an inverted desktop pixel cannot be
// represented as a sprite, so consumers composite it with DST_INVERT.
struct CursorBitmap {
    // Size of the sprite as drawn. Its bitmap is width*height*4 bytes (and one
    // invert byte per pixel), so these grow by two after an outline.
    UINT width = 0;
    UINT height = 0;
    std::vector<uint8_t> color;   // premultiplied BGRA
    std::vector<uint8_t> invert;  // 1 == blend with DST_INVERT
    // The shape needed the inversion treatment (see outline_cursor): `color`
    // is then the outlined sprite, two pixels larger in each dimension with
    // the shape drawn at (1,1), and every inverted pixel is opaque black.
    bool outlined = false;
};

size_t cursor_buffer_size(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape);

// Content id of a sprite, modelled on RustDesk's cursor_content_id: a 53-bit
// hash of its dimensions, hotspot and pixels (53 bits so it survives a JS
// number, which its web client needs). Equal ids mean the same sprite.
uint64_t cursor_content_id(UINT width, UINT height, POINT hotspot,
                           const std::vector<uint8_t>& color);

// RustDesk's cursor outline, applied to sprites that contain inverted desktop
// pixels. Those cannot be sent as an image, so the inverted pixels are made
// black and a one-pixel white outline is drawn around the sprite to keep it
// visible on dark backgrounds. Input is `bitmap` (premultiplied BGRA),
// output is the sprite two pixels larger in each dimension. Returns false
// when the shape does not need an outline (it carries no inversion), in
// which case `out` is left untouched.
bool outline_cursor(const CursorBitmap& bitmap, UINT width, UINT height, CursorBitmap& out);

// The SDK has no "no shape" enumerator: zero is never assigned by DXGI, so it
// marks the "no pointer shape cached" state.
constexpr UINT kNoShapeType = 0;

inline bool has_pointer_shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape) noexcept {
    return shape.Type != kNoShapeType;
}

class CursorDecoder {
public:
    explicit CursorDecoder(UINT max_bitmap_bytes = 4u * 1024u * 1024u)
        : max_bitmap_bytes_(max_bitmap_bytes) {}

    void update_position(const POINT& position, bool visible);
    void update_shape(IDXGIOutputDuplication* duplication);
    void clear_shape();
    void reset();

    const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape() const noexcept { return shape_; }
    const CursorBitmap& bitmap() const noexcept { return bitmap_; }
    bool has_shape() const noexcept { return has_pointer_shape(shape_); }
    bool has_position() const noexcept { return position_valid_; }
    POINT position() const noexcept { return position_; }
    bool visible() const noexcept { return visible_; }
    // Where the shape's first pixel sits inside the drawn sprite. DXGI places
    // the shape's top-left at the pointer position, so a consumer draws the
    // sprite at position - origin_offset(). It is (1,1) for an outlined sprite,
    // whose ring wraps around the shape.
    POINT origin_offset() const noexcept { return origin_offset_; }
    // 53-bit identity of the sprite: equal ids mean an identical image, so a
    // consumer can send each shape once however often the pointer swaps back
    // to it.
    uint64_t content_id() const noexcept { return content_id_; }
    // Hot spot within the drawn sprite (outline offset included). For DXGI's
    // shapes this comes from DXGI_OUTDUPL_POINTER_SHAPE_INFO::HotSpot, which
    // is documented for drawing the sprite against a pointer position.
    POINT hotspot() const noexcept { return hotspot_; }
    // Bumped on every shape or position change so consumers can skip work.
    uint64_t revision() const noexcept { return revision_; }

private:
    UINT max_bitmap_bytes_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_{};
    CursorBitmap bitmap_;
    std::vector<uint8_t> raw_;
    POINT position_{};
    POINT origin_offset_{};
    POINT hotspot_{};
    uint64_t content_id_ = 0;
    bool position_valid_ = false;
    bool visible_ = false;
    uint64_t revision_ = 0;
};

// Process-wide cursor shape cache, modelled on RustDesk's CURSOR_SHAPES map:
// every sprite the capturer has published, by content id, so a consumer that
// asks again (a late-joining viewer, one whose decoder cache was evicted) gets
// the bytes without the host decoding them again. Bounded by entry count and
// total bytes, oldest entries evicted first.
//
// The cache is process-wide, so several capturers (or a capturer and a
// consumer thread) can reach it at once. find() returns a shared_ptr: the
// entry stays alive for the caller even if another thread evicts it.
class CursorShapeCache {
public:
    static constexpr size_t kMaxEntries = 4096;
    static constexpr size_t kMaxBytes = 32u * 1024u * 1024u;

    // Stores a shape under its content id (no-op for a shape still cached).
    void put(const CursorInfo& info);
    std::shared_ptr<const CursorInfo> find(uint64_t content_id) const;
    size_t size() const;
    size_t bytes() const;
    void clear();

private:
    void evict_oldest();

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<const CursorInfo>> entries_;
    std::deque<uint64_t> order_;  // insertion order, for eviction
    size_t bytes_ = 0;
};

// The process-wide cache. Shapes outlive a capturer: a reinit or a display
// switch keeps them, as RustDesk's does.
CursorShapeCache& cursor_shape_cache();

}  // namespace chast::detail

