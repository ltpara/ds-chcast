#pragma once

// Deterministic media boundaries: no display, sound device or input side effects.
// Packets exercise the real QUIC transport, not a hardware codec's bitstream.
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include "chast/audio.h"
#include "chast/com_ptr.h"
#include "chast/host_server.h"

namespace host_test {
using namespace chast;

struct MediaState {
    std::atomic<unsigned> captures{0};
    std::atomic<unsigned> capture_fps{0};
    std::atomic<unsigned> capture_created{0}, capture_destroyed{0};
    std::atomic<unsigned> encoder_created{0}, encoder_destroyed{0};
    std::atomic<unsigned> software_created{0}, hardware_errors{0};
    std::atomic<bool> fail_hardware{false}, fail_hardware_once{false};
    std::atomic<Status> hardware_error{Status::Unavailable};
    std::atomic<bool> moving{false};
    std::atomic<bool> original_bgra{false}, fail_444{false};
    std::atomic<unsigned> wrong_thread{0};
    std::atomic<DWORD> capture_thread{0}, encoder_thread{0};
    std::atomic<unsigned> encodes{0};
    std::atomic<unsigned> idrs{0};
    std::atomic<unsigned> encode_delay_ms{0};
    std::atomic<bool> encoding_delayed{false};
    std::atomic<unsigned> repeats{0};
    std::atomic<unsigned> audio_sources{0};
    std::atomic<bool> repeat_supported{true};
    std::atomic<bool> reset_requested{false};
    std::atomic<bool> hold_frames{false};
    std::atomic<VideoCodecMask> fail_encode_codecs{0}, silent_codecs{0};
    std::atomic<unsigned> resets{0};
    std::atomic<bool> reinit_fails{false};
    std::mutex display_mutex;
    std::optional<DisplayInfo> next_display;
    void set_display(DisplayInfo display) {
        std::lock_guard lock(display_mutex); next_display = std::move(display);
    }
    std::mutex cursor_mutex;
    CursorInfo pointer;
    uint64_t pointer_revision = 0;
    void set_cursor(CursorInfo cursor) {
        std::lock_guard<std::mutex> lock(cursor_mutex);
        pointer = std::move(cursor);
        ++pointer_revision;
    }
};

class Texture final : public ID3D11Texture2D {
public:
    explicit Texture(UINT width = 640, UINT height = 480) : width_(width), height_(height) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(ID3D11DeviceChild) &&
            id != __uuidof(ID3D11Resource) && id != __uuidof(ID3D11Texture2D)) return E_NOINTERFACE;
        *out = static_cast<ID3D11Texture2D*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** out) override { *out = nullptr; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* out) override {
        *out = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* out) override {
        *out = {};
        out->Width = width_;
        out->Height = height_;
        out->Format = DXGI_FORMAT_NV12;
    }
private:
    std::atomic<ULONG> refs_{1};
    UINT width_, height_;
};

inline DisplayInfo display() {
    DisplayInfo info;
    info.device_name = L"test-display";
    info.width = 640;
    info.height = 480;
    info.rect = {0, 0, 640, 480};
    info.primary = true;
    return info;
}

class Capture final : public IDxgiCapturer {
public:
    explicit Capture(std::shared_ptr<MediaState> state, DisplayInfo display = host_test::display())
        : state_(std::move(state)), display_(std::move(display)) {
        state_->capture_thread = GetCurrentThreadId();
        ++state_->capture_created;
    }
    ~Capture() override { check_thread(); ++state_->capture_destroyed; }
    CaptureResult capture(std::chrono::milliseconds timeout) override {
        check_thread();
        ++state_->captures;
        if (state_->reset_requested.exchange(false)) return CaptureResult::ReinitNeeded;
        bool pointer_changed = false;
        {
            std::lock_guard<std::mutex> lock(state_->cursor_mutex);
            pointer_changed = pointer_revision_ != state_->pointer_revision;
            pointer_revision_ = state_->pointer_revision;
            cursor_ = state_->pointer;
        }
        if ((!first_ && !state_->moving && !pointer_changed) || state_->hold_frames) {
            // Deliberately clear borrowed metadata. The host must retain its
            // own COM reference and frame description when capture is idle.
            frame_ = {};
            texture_.reset();
            std::this_thread::sleep_for(timeout);
            return CaptureResult::NoNewFrame;
        }
        const bool desktop_changed = first_ || state_->moving;
        first_ = false;
        texture_ = ComPtr<ID3D11Texture2D>(new Texture(display_.width, display_.height));
        frame_ = {};
        frame_.width = frame_.nv12_width = frame_.nv12_surface_width = display_.width;
        frame_.height = frame_.nv12_height = frame_.nv12_surface_height = display_.height;
        frame_.nv12_texture = texture_.get();
        if (state_->original_bgra) frame_.texture = texture_.get();
        frame_.nv12_updated = frame_.desktop_updated = desktop_changed;
        return CaptureResult::Frame;
    }
    const FrameInfo& frame() const override { return frame_; }
    const CursorInfo& cursor() const override { return cursor_; }
    const DisplayInfo& display() const override { return display_; }
    AdapterDevice adapter_device() const override { return {}; }
    UINT max_framerate() const override { return state_->capture_fps; }
    Error reinitialize() override {
        check_thread();
        ++state_->resets;
        if (state_->reinit_fails) return Error(Status::Unavailable, "test display temporarily unavailable");
        { std::lock_guard lock(state_->display_mutex); if (state_->next_display) display_ = *state_->next_display; }
        first_ = true;
        texture_.reset();
        frame_ = {};
        return Error::success();
    }
    Error set_nv12_scale(UINT) override {
        check_thread();
        return Error(Status::NotCurrentlyAvailable, "fixed test size");
    }
    const Error& last_error() const override { return error_; }
private:
    void check_thread() { if (GetCurrentThreadId() != state_->capture_thread) ++state_->wrong_thread; }
    std::shared_ptr<MediaState> state_;
    ComPtr<ID3D11Texture2D> texture_;
    FrameInfo frame_;
    CursorInfo cursor_;
    DisplayInfo display_ = host_test::display();
    Error error_ = Error::success();
    bool first_ = true;
    uint64_t pointer_revision_ = 0;
};

class Encoder final : public IVideoEncoder {
public:
    Encoder(std::shared_ptr<MediaState> state, EncoderConfig config)
        : state_(std::move(state)), config_(config) {
        if (config_.backend == EncoderBackend::Software) {
            config_.codec = VideoCodec::H264;
            name_ = "deterministic software encoder";
            ++state_->software_created;
        }
        state_->encoder_thread = GetCurrentThreadId();
        ++state_->encoder_created;
    }
    ~Encoder() override { check_thread(); ++state_->encoder_destroyed; }
    Error encode(ID3D11Texture2D* texture, uint64_t pts, bool key,
                 std::vector<EncodedPacket>& out) override {
        check_thread();
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc); // Also verifies the host retained a live COM object.
        if (desc.Width != width()) return Error(Status::InvalidCall, "wrong texture");
        ++state_->encodes;
        if (auto error = hardware_failure(); !error.ok()) return error;
        if (const auto delay = state_->encode_delay_ms.load()) {
            state_->encoding_delayed = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            state_->encoding_delayed = false;
        }
        if ((state_->fail_444 && config_.chroma == VideoChroma::Yuv444) ||
            (state_->fail_encode_codecs & video_codec_bit(config_.codec)))
            return Error(Status::Unavailable, "simulated encoder failure");
        if (state_->silent_codecs & video_codec_bit(config_.codec)) return Error::success();
        have_input_ = true;
        if (key) ++state_->idrs;
        out.push_back({{0, 0, 0, 1, 0x65, static_cast<uint8_t>(width() / 16)}, pts, key, key});
        return Error::success();
    }
    Error encode_repeat(uint64_t pts, bool key, std::vector<EncodedPacket>& out) override {
        check_thread();
        if (!have_input_ || !state_->repeat_supported) {
            return Error(Status::NotCurrentlyAvailable, "repeat unsupported");
        }
        ++state_->repeats;
        if (auto error = hardware_failure(); !error.ok()) return error;
        if (key) ++state_->idrs;
        out.push_back({{0, 0, 0, 1, 0x65, static_cast<uint8_t>(width() / 16)}, pts, key, key});
        return Error::success();
    }
    Error set_rate(UINT rate, UINT fps, bool* key = nullptr) override {
        check_thread();
        config_.bitrate = rate;
        config_.framerate = fps;
        if (key) *key = false;
        return Error::success();
    }
    UINT bitrate() const noexcept override { return config_.bitrate ? config_.bitrate : 2000000; }
    UINT framerate() const noexcept override { return config_.framerate; }
    VideoCodec codec() const noexcept override { return config_.codec; }
    VideoChroma chroma() const noexcept override { return config_.chroma; }
    bool bgra_input() const noexcept override { return config_.chroma == VideoChroma::Yuv444; }
    UINT width() const noexcept override { return config_.width; }
    UINT height() const noexcept override { return config_.height; }
    const std::string& implementation() const noexcept override { return name_; }
    bool shared_import() const noexcept override { return true; }
    EncoderBackend backend() const noexcept override {
        return config_.backend == EncoderBackend::Auto ? EncoderBackend::Vpl : config_.backend;
    }
private:
    Error hardware_failure() {
        if (backend() != EncoderBackend::Software &&
            (state_->fail_hardware || state_->fail_hardware_once.exchange(false))) {
            ++state_->hardware_errors;
            return Error(state_->hardware_error, "simulated hardware encoding failure");
        }
        return Error::success();
    }
    void check_thread() { if (GetCurrentThreadId() != state_->encoder_thread) ++state_->wrong_thread; }
    std::shared_ptr<MediaState> state_;
    EncoderConfig config_;
    bool have_input_ = false;
    std::string name_ = "deterministic test encoder";
};

class Audio final : public ILoopbackCapture {
public:
    void start() override { running_ = true; next_ = Clock::now(); }
    void stop() override { running_ = false; }
    bool take_packet(EncodedAudio& packet) override {
        if (!running_ || Clock::now() < next_) return false;
        next_ = Clock::now() + std::chrono::milliseconds(20);
        packet.capture_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
        packet.data = {0xf8, 0xff, 0xfe};
        ++stats_.packets;
        return true;
    }
    bool failed() const override { return false; }
    std::string error() const override { return {}; }
    AudioInfo info() const override {
        AudioInfo info;
        info.enabled = true;
        info.codec = AudioCodec::Opus;
        info.frame_samples = 960;
        info.bitrate = 96000;
        info.extradata = {'O','p','u','s','H','e','a','d',1,2,0,0,0x80,0xbb,0,0,0,0,0};
        return info;
    }
    AudioCaptureStats stats() const override { return stats_; }
private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point next_;
    bool running_ = false;
    AudioCaptureStats stats_;
};

inline std::shared_ptr<MediaState> use_media(HostServerConfig& config) {
    auto state = std::make_shared<MediaState>();
    config.displays = [] { return std::vector<DisplayInfo>{display()}; };
    config.capture_factory = [state](const DisplayInfo& display, const CaptureConfig&, std::string*) {
        return std::make_unique<Capture>(state, display);
    };
    config.encoder_factory = [state](ID3D11Device*, const EncoderConfig& cfg, std::string*) {
        return std::make_unique<Encoder>(state, cfg);
    };
    config.desktop_status = [] { return InputDesktopStatus{InputDesktopKind::Default, false, "Default", {}}; };
    config.follow_desktop = [](std::string*, bool* switched) {
        if (switched) *switched = false;
        return true;
    };
    config.audio_capture = [state](std::string*) {
        ++state->audio_sources;
        return std::make_unique<Audio>();
    };
    config.dry_run_input = true;
    config.adaptive_bitrate = false;
    config.keyframe_interval_ms = 0;
    return state;
}
} // namespace host_test
