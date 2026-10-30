#define NOMINMAX

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "chast/camera.h"
#include "chast/com_ptr.h"
#include "chast/protocol.h"

namespace chast {

namespace {

// Media Foundation declares the stream selectors as a signed enum, which MSVC
// refuses in a constant expression; the reader API takes a DWORD. Values come
// from mfreadwrite.h.
constexpr DWORD kFirstVideoStream = 0xFFFFFFFC;  // MF_SOURCE_READER_FIRST_VIDEO_STREAM
constexpr DWORD kAllStreams = 0xFFFFFFFE;        // MF_SOURCE_READER_ALL_STREAMS

// One Media Foundation platform per process. MFShutdown runs at exit; no
// other chast component uses Media Foundation, so the static lifetime covers
// every camera.
struct MfPlatform {
    MfPlatform() { result = MFStartup(MF_VERSION); }
    ~MfPlatform() {
        if (SUCCEEDED(result)) {
            MFShutdown();
        }
    }
    HRESULT result = E_FAIL;
};

MfPlatform& mf_platform() {
    static MfPlatform platform;
    return platform;
}

// Balances CoInitializeEx for a call that only needs COM while it runs.
struct ComScope {
    ComScope() : initialized(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() {
        if (initialized) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    bool initialized;
};

std::string hresult_text(const char* what, HRESULT hr) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(hr));
    return std::string(what) + " failed, HRESULT " + code;
}

std::string utf8(const wchar_t* text) {
    if (!text || !*text) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    return out;
}

// What the reader callback writes and read() consumes.
struct ReaderState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<uint8_t> frame;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint64_t pts_ms = 0;
    bool frame_ready = false;
    bool request_pending = false;
    bool failed = false;
    bool stopping = false;
    std::string failure;
};

// Copies an NV12 sample into a tight buffer. Frames are small enough that the
// row copy is cheaper than negotiating locked 2D buffers with every camera.
bool copy_nv12(IMFSample* sample, uint32_t width, uint32_t height, uint32_t stride,
               std::vector<uint8_t>& out, std::string* error) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->ConvertToContiguousBuffer(buffer.put());
    if (FAILED(hr)) {
        *error = hresult_text("ConvertToContiguousBuffer", hr);
        return false;
    }
    if (width == 0 || height == 0) {
        *error = "the camera delivered a frame before its media type was known";
        return false;
    }

    const BYTE* data = nullptr;
    LONG pitch = 0;
    ComPtr<IMF2DBuffer> buffer2d = com_query<IMF2DBuffer>(buffer);
    if (buffer2d) {
        hr = buffer2d->Lock2D(const_cast<BYTE**>(&data), &pitch);
        if (FAILED(hr)) {
            *error = hresult_text("IMF2DBuffer::Lock2D", hr);
            return false;
        }
        ULONG contiguous = 0;
        const bool whole = SUCCEEDED(buffer2d->GetContiguousLength(&contiguous)) &&
                           contiguous >= static_cast<ULONG>(pitch) * height * 3 / 2;
        if (!whole || pitch < static_cast<LONG>(width)) {
            buffer2d->Unlock2D();
            *error = "the camera buffer is not a contiguous NV12 surface";
            return false;
        }
    } else {
        DWORD length = 0;
        hr = buffer->Lock(const_cast<BYTE**>(&data), nullptr, &length);
        if (FAILED(hr)) {
            *error = hresult_text("IMFMediaBuffer::Lock", hr);
            return false;
        }
        // A 1D media buffer has no row padding of its own; the media type's
        // default stride is the only layout hint.
        pitch = stride >= width ? static_cast<LONG>(stride) : static_cast<LONG>(width);
        if (length < static_cast<DWORD>(pitch) * height * 3 / 2) {
            buffer->Unlock();
            *error = "the camera buffer is smaller than its NV12 frame";
            return false;
        }
    }

    out.resize(static_cast<size_t>(width) * height * 3 / 2);
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(out.data() + static_cast<size_t>(row) * width,
                    data + static_cast<size_t>(pitch) * row, width);
    }
    const BYTE* chroma = data + static_cast<size_t>(pitch) * height;
    uint8_t* target = out.data() + static_cast<size_t>(width) * height;
    for (uint32_t row = 0; row < height / 2; ++row) {
        std::memcpy(target + static_cast<size_t>(row) * width,
                    chroma + static_cast<size_t>(pitch) * row, width);
    }
    if (buffer2d) {
        buffer2d->Unlock2D();
    } else {
        buffer->Unlock();
    }
    return true;
}

class ReaderCallback final : public IMFSourceReaderCallback {
public:
    explicit ReaderCallback(std::shared_ptr<ReaderState> state) : state_(std::move(state)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) {
            return E_POINTER;
        }
        if (id == __uuidof(IUnknown) || id == __uuidof(IMFSourceReaderCallback)) {
            *out = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT status, DWORD, DWORD flags,
                                           LONGLONG timestamp, IMFSample* sample) override {
        std::vector<uint8_t> frame;
        std::string failure;
        if (FAILED(status)) {
            failure = hresult_text("ReadSample", status);
        } else if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
            failure = "the camera reported a streaming error";
        } else if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            failure = "the camera stream ended";
        } else if (sample) {
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t stride = 0;
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                width = state_->width;
                height = state_->height;
                stride = state_->stride;
            }
            if (!copy_nv12(sample, width, height, stride, frame, &failure)) {
                frame.clear();
            }
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->request_pending = false;
            if (!failure.empty()) {
                state_->failed = true;
                state_->failure = failure;
            } else if (!frame.empty()) {
                state_->frame = std::move(frame);
                state_->pts_ms = static_cast<uint64_t>(std::max<LONGLONG>(timestamp, 0) / 10000);
                state_->frame_ready = true;
            }
        }
        state_->cv.notify_all();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFlush(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    ~ReaderCallback() = default;

    LONG refs_ = 1;
    std::shared_ptr<ReaderState> state_;
};

class MfCamera final : public ICameraCapture {
public:
    MfCamera(CameraDevice device, ComPtr<IMFSourceReader> reader,
             ComPtr<ReaderCallback> callback, std::shared_ptr<ReaderState> state,
             uint32_t framerate, bool com_initialized)
        : device_(std::move(device)),
          reader_(std::move(reader)),
          callback_(std::move(callback)),
          state_(std::move(state)),
          framerate_(framerate),
          com_initialized_(com_initialized) {}

    ~MfCamera() override {
        stop();
        // Releasing the reader cancels whatever request is still parked in the
        // device; the callback only touches the shared state, which outlives
        // this object through the callback's reference.
        reader_.reset();
        callback_.reset();
        if (com_initialized_) {
            CoUninitialize();
        }
    }

    Error read(std::chrono::milliseconds timeout, const FrameCallback& frame) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            bool issue = false;
            {
                std::unique_lock<std::mutex> lock(state_->mutex);
                if (state_->stopping) {
                    return Error(Status::Unavailable, "the camera was stopped");
                }
                if (state_->failed) {
                    return Error(Status::CaptureFailed, state_->failure);
                }
                if (state_->frame_ready) {
                    CameraFrame out;
                    out.data = std::move(state_->frame);
                    out.width = state_->width;
                    out.height = state_->height;
                    out.timestamp_ms = state_->pts_ms;
                    state_->frame_ready = false;
                    lock.unlock();
                    frame(std::move(out));
                    return Error::success();
                }
                if (!state_->request_pending) {
                    state_->request_pending = true;
                    issue = true;
                }
            }
            if (issue) {
                const HRESULT hr = reader_->ReadSample(kFirstVideoStream, 0,
                                                       nullptr, nullptr, nullptr, nullptr);
                if (FAILED(hr)) {
                    std::lock_guard<std::mutex> lock(state_->mutex);
                    state_->request_pending = false;
                    return Error(status_from_hresult(hr), hresult_text("ReadSample", hr));
                }
            }
            std::unique_lock<std::mutex> lock(state_->mutex);
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return Error(Status::WaitTimeout, "no camera frame arrived in time");
            }
            state_->cv.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(100)));
        }
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->stopping) {
                return;
            }
            state_->stopping = true;
        }
        state_->cv.notify_all();
        if (reader_) {
            // Documented way to drop the pending request; the camera keeps
            // delivering until the reader is released, so this only has to
            // unblock the parked ReadSample.
            reader_->Flush(kFirstVideoStream);
        }
    }

    uint32_t width() const override { return state_->width; }
    uint32_t height() const override { return state_->height; }
    uint32_t framerate() const override { return framerate_; }
    const CameraDevice& device() const override { return device_; }

private:
    CameraDevice device_;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<ReaderCallback> callback_;
    std::shared_ptr<ReaderState> state_;
    uint32_t framerate_ = 0;
    bool com_initialized_ = false;
};

// S_OK and S_FALSE both count as initialized and must be balanced by the
// destructor; RPC_E_CHANGED_MODE means the thread is an STA, which a source
// reader must not be created on.
bool init_com(std::string* error) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        return true;
    }
    if (error) {
        *error = hr == RPC_E_CHANGED_MODE
                     ? "the calling thread is initialized as STA; camera capture needs MTA"
                     : hresult_text("CoInitializeEx", hr);
    }
    return false;
}

}  // namespace

std::vector<CameraDevice> enumerate_cameras() {
    std::vector<CameraDevice> cameras;
    if (FAILED(mf_platform().result)) {
        return cameras;
    }
    // Device activation goes through COM. The caller may be the host session
    // thread, which never touches COM otherwise.
    const ComScope com;
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(attributes.put(), 1))) {
        return cameras;
    }
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attributes.get(), &devices, &count)) || !devices) {
        return cameras;
    }
    for (UINT32 i = 0; i < count && cameras.size() < kMaxCameras; ++i) {
        wchar_t* link = nullptr;
        UINT32 length = 0;
        if (FAILED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                  &link, &length)) ||
            !link) {
            continue;
        }
        CameraDevice device;
        device.symbolic_link = utf8(link);
        CoTaskMemFree(link);
        if (device.symbolic_link.empty()) {
            continue;
        }
        wchar_t* name = nullptr;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name,
                                                     &length)) &&
            name) {
            device.name = utf8(name);
            CoTaskMemFree(name);
        }
        if (device.name.empty()) {
            device.name = "Camera " + std::to_string(cameras.size() + 1);
        }
        device.id = static_cast<uint16_t>(cameras.size() + 1);
        device.is_default = cameras.empty();
        cameras.push_back(std::move(device));
    }
    for (UINT32 i = 0; i < count; ++i) {
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    return cameras;
}

std::unique_ptr<ICameraCapture> open_camera(const CameraDevice& device, uint32_t framerate,
                                            std::string* error) {
    const auto fail = [&](const std::string& reason) -> std::unique_ptr<ICameraCapture> {
        if (error) {
            *error = reason;
        }
        return nullptr;
    };
    if (FAILED(mf_platform().result)) {
        return fail(hresult_text("MFStartup", mf_platform().result));
    }
    bool com_initialized = false;
    {
        std::string com_error;
        if (!init_com(&com_error)) {
            return fail(com_error);
        }
        com_initialized = true;
    }

    const std::wstring link = wide(device.symbolic_link);
    if (link.empty()) {
        CoUninitialize();
        return fail("the camera has no symbolic link");
    }
    ComPtr<IMFAttributes> source_attributes;
    HRESULT hr = MFCreateAttributes(source_attributes.put(), 2);
    if (SUCCEEDED(hr)) {
        hr = source_attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    }
    if (SUCCEEDED(hr)) {
        hr = source_attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                          link.c_str());
    }
    ComPtr<IMFMediaSource> source;
    if (SUCCEEDED(hr)) {
        hr = MFCreateDeviceSource(source_attributes.get(), source.put());
    }
    if (FAILED(hr)) {
        CoUninitialize();
        return fail(hresult_text("opening the camera", hr));
    }

    auto state = std::make_shared<ReaderState>();
    ComPtr<ReaderCallback> callback(new ReaderCallback(state));
    ComPtr<IMFAttributes> reader_attributes;
    hr = MFCreateAttributes(reader_attributes.put(), 2);
    if (SUCCEEDED(hr)) {
        hr = reader_attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.get());
    }
    if (SUCCEEDED(hr)) {
        // Lets the reader insert the video processor: webcams rarely offer
        // NV12 at the requested rate themselves.
        hr = reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(hr)) {
        hr = MFCreateSourceReaderFromMediaSource(source.get(), reader_attributes.get(),
                                                 reader.put());
    }
    if (FAILED(hr)) {
        CoUninitialize();
        return fail(hresult_text("creating the camera reader", hr));
    }
    reader->SetStreamSelection(kAllStreams, FALSE);
    reader->SetStreamSelection(kFirstVideoStream, TRUE);

    ComPtr<IMFMediaType> type;
    MFCreateMediaType(type.put());
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (framerate > 0) {
        MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, framerate, 1);
    }
    hr = reader->SetCurrentMediaType(kFirstVideoStream, nullptr, type.get());
    if (FAILED(hr)) {
        // A device that cannot produce the requested rate still gives NV12 at
        // its own; the encoder takes whatever rate arrives.
        ComPtr<IMFMediaType> native;
        MFCreateMediaType(native.put());
        native->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        native->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        hr = reader->SetCurrentMediaType(kFirstVideoStream, nullptr,
                                         native.get());
    }
    ComPtr<IMFMediaType> current;
    if (SUCCEEDED(hr)) {
        hr = reader->GetCurrentMediaType(kFirstVideoStream, current.put());
    }
    if (FAILED(hr)) {
        CoUninitialize();
        return fail(hresult_text("negotiating NV12 with the camera", hr));
    }
    UINT32 width = 0;
    UINT32 height = 0;
    MFGetAttributeSize(current.get(), MF_MT_FRAME_SIZE, &width, &height);
    if (width == 0 || height == 0) {
        CoUninitialize();
        return fail("the camera reported an empty frame size");
    }
    UINT32 rate_numerator = 0;
    UINT32 rate_denominator = 1;
    if (FAILED(MFGetAttributeRatio(current.get(), MF_MT_FRAME_RATE, &rate_numerator,
                                   &rate_denominator)) ||
        rate_denominator == 0 || rate_numerator == 0) {
        rate_numerator = framerate > 0 ? framerate : 30;
        rate_denominator = 1;
    }
    UINT32 stride = 0;
    current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->width = width;
        state->height = height;
        state->stride = stride;
    }
    const uint32_t actual_rate =
        std::max<UINT32>(1, static_cast<UINT32>(static_cast<double>(rate_numerator) /
                                                rate_denominator + 0.5));
    return std::make_unique<MfCamera>(device, std::move(reader), std::move(callback),
                                      std::move(state), actual_rate, com_initialized);
}

}  // namespace chast
