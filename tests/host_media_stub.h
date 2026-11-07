#pragma once

// Deterministic media boundaries: no display, sound device or input side effects.
// Packets exercise the real QUIC transport, not a hardware codec's bitstream.
#include <atomic>
#include <chrono>
#include <thread>
#include "chast/audio.h"
#include "chast/com_ptr.h"
#include "chast/host_server.h"

namespace host_test {
using namespace chast;

struct MediaState {
    std::atomic<unsigned> captures{0};
    std::atomic<unsigned> encodes{0};
    std::atomic<unsigned> repeats{0};
    std::atomic<unsigned> audio_sources{0};
    std::atomic<bool> repeat_supported{true};
    std::atomic<bool> reset_requested{false};
    std::atomic<bool> hold_frames{false};
    std::atomic<unsigned> resets{0};
};

class Texture final : public ID3D11Texture2D {
public:
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
        out->Width = 640;
        out->Height = 480;
        out->Format = DXGI_FORMAT_NV12;
    }
private:
    std::atomic<ULONG> refs_{1};
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
    explicit Capture(std::shared_ptr<MediaState> state) : state_(std::move(state)) {}
    CaptureResult capture(std::chrono::milliseconds timeout) override {
        ++state_->captures;
        if (state_->reset_requested.exchange(false)) return CaptureResult::ReinitNeeded;
        if (!first_ || state_->hold_frames) {
            // Deliberately clear borrowed metadata. The host must retain its
            // own COM reference and frame description when capture is idle.
            frame_ = {};
            texture_.reset();
            std::this_thread::sleep_for(timeout);
            return CaptureResult::NoNewFrame;
        }
        first_ = false;
        texture_ = ComPtr<ID3D11Texture2D>(new Texture);
        frame_ = {};
        frame_.width = frame_.nv12_width = frame_.nv12_surface_width = 640;
        frame_.height = frame_.nv12_height = frame_.nv12_surface_height = 480;
        frame_.nv12_texture = texture_.get();
        frame_.nv12_updated = true;
        return CaptureResult::Frame;
    }
    const FrameInfo& frame() const override { return frame_; }
    const CursorInfo& cursor() const override { return cursor_; }
    const DisplayInfo& display() const override { return display_; }
    AdapterDevice adapter_device() const override { return {}; }
    Error reinitialize() override {
        first_ = true;
        texture_.reset();
        frame_ = {};
        ++state_->resets;
        return Error::success();
    }
    Error set_nv12_scale(UINT) override { return Error(Status::NotCurrentlyAvailable, "fixed test size"); }
    const Error& last_error() const override { return error_; }
private:
    std::shared_ptr<MediaState> state_;
    ComPtr<ID3D11Texture2D> texture_;
    FrameInfo frame_;
    CursorInfo cursor_;
    DisplayInfo display_ = host_test::display();
    Error error_ = Error::success();
    bool first_ = true;
};

class Encoder final : public IVideoEncoder {
public:
    Encoder(std::shared_ptr<MediaState> state, EncoderConfig config)
        : state_(std::move(state)), config_(config) {}
    Error encode(ID3D11Texture2D* texture, uint64_t pts, bool key,
                 std::vector<EncodedPacket>& out) override {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc); // Also verifies the host retained a live COM object.
        if (desc.Width != width()) return Error(Status::InvalidCall, "wrong texture");
        ++state_->encodes;
        have_input_ = true;
        out.push_back({{0, 0, 0, 1, 0x65}, pts, key, key});
        return Error::success();
    }
    Error encode_repeat(uint64_t pts, bool key, std::vector<EncodedPacket>& out) override {
        if (!have_input_ || !state_->repeat_supported) {
            return Error(Status::NotCurrentlyAvailable, "repeat unsupported");
        }
        ++state_->repeats;
        out.push_back({{0, 0, 0, 1, 0x65}, pts, key, key});
        return Error::success();
    }
    Error set_rate(UINT rate, UINT fps, bool* key = nullptr) override {
        config_.bitrate = rate;
        config_.framerate = fps;
        if (key) *key = false;
        return Error::success();
    }
    UINT bitrate() const noexcept override { return config_.bitrate ? config_.bitrate : 2000000; }
    UINT framerate() const noexcept override { return config_.framerate; }
    VideoCodec codec() const noexcept override { return config_.codec; }
    UINT width() const noexcept override { return config_.width; }
    UINT height() const noexcept override { return config_.height; }
    const std::string& implementation() const noexcept override { return name_; }
    bool shared_import() const noexcept override { return true; }
private:
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
    config.capture_factory = [state](const DisplayInfo&, const CaptureConfig&, std::string*) {
        return std::make_unique<Capture>(state);
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
