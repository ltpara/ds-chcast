// WASAPI microphone capture for the viewer's uplink.
//
// One capture endpoint, one thread, 48 kHz mono s16 on the wire as 20 ms Opus
// packets. The queue and the recovery policy mirror the loopback capture, but
// there is no silence gate: opening the CMIC stream is the user's consent, and
// a stream that pauses on silence would look like a failure to the host.
//
// If the selected device disappears, the thread keeps retrying the same id and
// reports `recovering`; an empty selection follows the default communications
// device. The announced AudioInfo never changes, so the host keeps its decoder
// across a device swap.

#include "chast/audio.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "chast/com_ptr.h"
#include "wasapi_common.h"

namespace chast {

namespace {

constexpr uint32_t kMicRate = 48000;
constexpr uint16_t kMicChannels = 1;
constexpr size_t kFadeMs = 5;

// PKEY_Device_FriendlyName, spelled out so this file does not need the
// function-discovery header's full property-key list.
constexpr PROPERTYKEY kFriendlyNameKey = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string narrow_utf8(const wchar_t* text) {
    if (!text || !*text) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string narrow(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), size, nullptr, nullptr);
    return narrow;
}

std::string friendly_name(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put()))) {
        return {};
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name;
    if (SUCCEEDED(store->GetValue(kFriendlyNameKey, &value)) &&
        value.vt == VT_LPWSTR) {
        name = narrow_utf8(value.pwszVal);
    }
    PropVariantClear(&value);
    return name;
}

ComPtr<IMMDevice> default_capture_device(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, device.put()))) {
        device.reset();
        enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device.put());
    }
    return device;
}

class WasapiMicrophoneCapture final : public IMicrophoneCapture {
public:
    explicit WasapiMicrophoneCapture(MicrophoneCaptureConfig config)
        : config_(std::move(config)) {}
    ~WasapiMicrophoneCapture() override { close(); }

    bool start_thread(std::string* error);

    void start() override;
    void stop() override;
    bool take_packet(EncodedAudio& packet) override;

    bool failed() const override { return failed_.load(); }
    std::string error() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }
    AudioInfo info() const override { return info_; }
    AudioCaptureStats stats() const override {
        AudioCaptureStats stats;
        stats.packets = packets_.load();
        stats.dropped = dropped_.load();
        stats.silence_frames = silence_frames_.load();
        stats.capture_errors = capture_errors_.load();
        stats.recoveries = recoveries_.load();
        stats.recovering = recovering_.load();
        return stats;
    }

private:
    void thread_main();
    bool open_encoder(std::string* error);
    bool open_device(std::string* error);
    void close_device();
    bool start_client();
    bool device_lost();
    void close();
    void capture_loop();
    bool drain();
    bool process(const BYTE* data, UINT32 frames, DWORD flags);
    void encode_block(const int16_t* samples, size_t frames, int64_t capture_ms);
    void apply_fade(int16_t* samples, size_t frames);
    void fail(const std::string& message, bool count_error);
    static size_t fade_frames() {
        return static_cast<size_t>(kMicRate) * kFadeMs / 1000;
    }
    bool stopping() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

    MicrophoneCaptureConfig config_;
    std::thread thread_;
    HANDLE event_ = nullptr;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_ = false;
    bool start_requested_ = false;
    bool started_ = false;
    bool stopping_ = false;
    std::string error_;

    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice> device_;
    std::wstring device_id_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    bool client_started_ = false;
    wasapi::MixFormat format_;
    std::unique_ptr<IAudioEncoder> encoder_;
    SwrContext* swr_ = nullptr;
    std::vector<int16_t> pcm_;
    std::vector<uint8_t> zero_input_;
    AudioInfo info_;

    int64_t last_capture_end_ms_ = 0;
    size_t fade_remaining_ = 0;

    std::mutex queue_mutex_;
    std::deque<EncodedAudio> queue_;

    std::atomic<bool> failed_{false};
    std::atomic<bool> recovering_{false};
    std::atomic<uint64_t> packets_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> silence_frames_{0};
    std::atomic<uint64_t> capture_errors_{0};
    std::atomic<uint64_t> recoveries_{0};
};

bool WasapiMicrophoneCapture::start_thread(std::string* error) {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        if (error) {
            *error = "CreateEvent for microphone capture failed, GetLastError=" +
                     std::to_string(GetLastError());
        }
        return false;
    }
    thread_ = std::thread(&WasapiMicrophoneCapture::thread_main, this);
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return ready_; });
    if (failed_.load()) {
        std::string message = error_;
        lock.unlock();
        thread_.join();
        if (error) {
            *error = message;
        }
        return false;
    }
    return true;
}

void WasapiMicrophoneCapture::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_.load() || stopping_ || started_) {
            return;
        }
        started_ = true;
        start_requested_ = true;
    }
    cv_.notify_all();
}

void WasapiMicrophoneCapture::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (event_) {
        SetEvent(event_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void WasapiMicrophoneCapture::close() {
    stop();
    if (event_) {
        CloseHandle(event_);
        event_ = nullptr;
    }
}

bool WasapiMicrophoneCapture::take_packet(EncodedAudio& packet) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.empty()) {
        return false;
    }
    packet = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void WasapiMicrophoneCapture::thread_main() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        const std::string message =
            wasapi::error_from_hresult(hr, "CoInitializeEx(microphone capture)").to_string();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready_ = true;
            stopping_ = true;
            error_ = message;
            failed_.store(true);
        }
        cv_.notify_all();
        return;
    }

    std::string open_error;
    const bool opened = open_encoder(&open_error) && open_device(&open_error);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_ = true;
        if (!opened) {
            stopping_ = true;
            error_ = open_error;
            failed_.store(true);
        }
    }
    cv_.notify_all();

    if (opened) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return start_requested_ || stopping_; });
        }
        if (!stopping()) {
            capture_loop();
        }
    }

    close_device();
    encoder_.reset();
    enumerator_.reset();
    CoUninitialize();
}

bool WasapiMicrophoneCapture::open_encoder(std::string* error) {
    AudioEncoderConfig encoder_config;
    encoder_config.codec = AudioCodec::Opus;
    encoder_config.sample_rate = kMicRate;
    encoder_config.channels = kMicChannels;
    encoder_config.bitrate = config_.bitrate;
    encoder_config.voice = true;
    encoder_ = create_audio_encoder(encoder_config, error);
    if (!encoder_) {
        return false;
    }
    info_.enabled = true;
    info_.codec = encoder_config.codec;
    info_.sample_rate = encoder_->sample_rate();
    info_.channels = encoder_->channels();
    info_.bitrate = encoder_->bitrate();
    info_.frame_samples = encoder_->frame_samples();
    info_.extradata = encoder_->extradata();
    return true;
}

bool WasapiMicrophoneCapture::open_device(std::string* error) {
    if (!enumerator_) {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(enumerator_.put()));
        if (FAILED(hr)) {
            *error = wasapi::error_from_hresult(hr, "MMDeviceEnumerator").to_string();
            return false;
        }
    }

    ComPtr<IMMDevice> device;
    HRESULT hr = S_OK;
    if (!config_.device_id.empty()) {
        const std::wstring wanted = widen_utf8(config_.device_id);
        if (wanted.empty()) {
            *error = "the selected microphone id is not valid UTF-8";
            return false;
        }
        hr = enumerator_->GetDevice(wanted.c_str(), device.put());
        if (FAILED(hr)) {
            *error = wasapi::error_from_hresult(hr, "GetDevice(microphone)").to_string();
            return false;
        }
    } else {
        device = default_capture_device(enumerator_.get());
        if (!device) {
            *error = "this machine has no default capture endpoint";
            return false;
        }
    }
    if (!wasapi::device_active(device.get())) {
        *error = "the microphone is not active";
        return false;
    }

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.put()));
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "IAudioClient activate(microphone)").to_string();
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "GetMixFormat(microphone)").to_string();
        return false;
    }
    wasapi::MixFormat format;
    std::string format_error;
    const bool parsed = wasapi::parse_mix_format(mix, format, &format_error);
    CoTaskMemFree(mix);
    if (!parsed) {
        *error = format_error;
        return false;
    }

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            0, 0, format.wave(), nullptr);
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "IAudioClient::Initialize(microphone)")
                     .to_string();
        return false;
    }
    hr = client->SetEventHandle(event_);
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "SetEventHandle(microphone)").to_string();
        return false;
    }
    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(IID_PPV_ARGS(capture.put()));
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "IAudioCaptureClient").to_string();
        return false;
    }

    SwrContext* swr = nullptr;
    AVChannelLayout input = wasapi::input_layout(format);
    AVChannelLayout output;
    av_channel_layout_default(&output, kMicChannels);
    const int status = swr_alloc_set_opts2(
        &swr, &output, AV_SAMPLE_FMT_S16, kMicRate, &input, format.format,
        static_cast<int>(format.sample_rate), 0, nullptr);
    av_channel_layout_uninit(&input);
    av_channel_layout_uninit(&output);
    if (status < 0 || !swr) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(status < 0 ? status : AVERROR(ENOMEM), text, sizeof(text));
        *error = std::string("swr_alloc_set_opts2(microphone) failed: ") + text;
        return false;
    }
    const int initialized = swr_init(swr);
    if (initialized < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(initialized, text, sizeof(text));
        swr_free(&swr);
        *error = std::string("swr_init(microphone) failed: ") + text;
        return false;
    }
    const std::wstring id = wasapi::device_id(device.get());

    device_ = std::move(device);
    client_ = std::move(client);
    capture_ = std::move(capture);
    format_ = std::move(format);
    device_id_ = id;
    if (swr_) {
        swr_free(&swr_);
    }
    swr_ = swr;
    client_started_ = false;
    pcm_.clear();
    zero_input_.clear();
    return true;
}

void WasapiMicrophoneCapture::close_device() {
    if (client_ && client_started_) {
        client_->Stop();
        client_started_ = false;
    }
    if (swr_) {
        swr_free(&swr_);
    }
    capture_.reset();
    client_.reset();
    device_.reset();
    format_ = wasapi::MixFormat{};
    device_id_.clear();
    pcm_.clear();
    zero_input_.clear();
    last_capture_end_ms_ = 0;
    fade_remaining_ = 0;
}

bool WasapiMicrophoneCapture::start_client() {
    if (!client_) {
        return false;
    }
    const HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        return false;
    }
    client_started_ = true;
    return true;
}

bool WasapiMicrophoneCapture::device_lost() {
    if (!wasapi::device_active(device_.get())) {
        return true;
    }
    if (!config_.device_id.empty()) {
        return false;
    }
    ComPtr<IMMDevice> current = default_capture_device(enumerator_.get());
    if (!current) {
        return true;
    }
    const std::wstring id = wasapi::device_id(current.get());
    return id.empty() || id != device_id_;
}

void WasapiMicrophoneCapture::capture_loop() {
    bool endpoint_open = start_client();
    if (!endpoint_open) {
        close_device();
        recovering_.store(true);
    }
    int64_t next_retry = wasapi::now_ms() + 1000;
    int64_t next_check = wasapi::now_ms() + 1000;

    while (!stopping()) {
        if (!endpoint_open) {
            if (wasapi::now_ms() >= next_retry) {
                std::string open_error;
                if (open_device(&open_error) && start_client()) {
                    endpoint_open = true;
                    recovering_.store(false);
                    recoveries_.fetch_add(1);
                    last_capture_end_ms_ = 0;
                    fade_remaining_ = fade_frames();
                    next_check = wasapi::now_ms() + 1000;
                } else {
                    close_device();
                    next_retry = wasapi::now_ms() + 1000;
                }
            }
            WaitForSingleObject(event_, 100);
            continue;
        }

        const DWORD wait = WaitForSingleObject(event_, 200);
        if (stopping()) {
            break;
        }
        if (wait == WAIT_OBJECT_0 && !drain()) {
            close_device();
            endpoint_open = false;
            recovering_.store(true);
            next_retry = wasapi::now_ms() + 1000;
            continue;
        }
        if (wasapi::now_ms() >= next_check) {
            next_check = wasapi::now_ms() + 1000;
            if (device_lost()) {
                close_device();
                endpoint_open = false;
                recovering_.store(true);
                next_retry = wasapi::now_ms();
            }
        }
    }

    if (endpoint_open) {
        close_device();
    }
    recovering_.store(false);
}

bool WasapiMicrophoneCapture::drain() {
    for (;;) {
        UINT32 next = 0;
        HRESULT hr = capture_->GetNextPacketSize(&next);
        if (FAILED(hr)) {
            return false;
        }
        if (next == 0) {
            return true;
        }
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) {
            return false;
        }
        const bool processed = process(data, frames, flags);
        capture_->ReleaseBuffer(frames);
        if (!processed) {
            return false;
        }
    }
}

bool WasapiMicrophoneCapture::process(const BYTE* data, UINT32 frames, DWORD flags) {
    if (frames == 0) {
        return true;
    }
    const int64_t duration_ms =
        static_cast<int64_t>(frames) * 1000 / static_cast<int64_t>(format_.sample_rate);
    const int64_t end_ms = wasapi::now_ms();
    const int64_t capture_ms = end_ms > duration_ms ? end_ms - duration_ms : 0;
    const bool silent_flag = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data;

    const uint8_t* input = data;
    if (silent_flag) {
        const size_t bytes = static_cast<size_t>(frames) * format_.channels *
                             static_cast<size_t>(av_get_bytes_per_sample(format_.format));
        zero_input_.assign(bytes, 0);
        input = zero_input_.data();
    }

    const int out_capacity = static_cast<int>(av_rescale_rnd(
        swr_get_delay(swr_, static_cast<int>(format_.sample_rate)) + frames, kMicRate,
        format_.sample_rate, AV_ROUND_UP));
    if (out_capacity <= 0) {
        last_capture_end_ms_ = capture_ms + duration_ms;
        return true;
    }
    pcm_.resize(static_cast<size_t>(out_capacity) * kMicChannels);
    uint8_t* output = reinterpret_cast<uint8_t*>(pcm_.data());
    const int converted = swr_convert(swr_, &output, out_capacity, &input,
                                      static_cast<int>(frames));
    if (converted < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(converted, text, sizeof(text));
        fail(std::string("swr_convert(microphone) failed: ") + text, true);
        return false;
    }
    const size_t out_frames = static_cast<size_t>(converted);

    bool silent = silent_flag;
    if (!silent && out_frames > 0) {
        silent = true;
        for (size_t i = 0; i < out_frames * kMicChannels; ++i) {
            if (pcm_[i] != 0) {
                silent = false;
                break;
            }
        }
    }
    if (silent) {
        silence_frames_.fetch_add(frames);
    }
    if (last_capture_end_ms_ > 0 && capture_ms > last_capture_end_ms_ + 100) {
        fade_remaining_ = fade_frames();
    }

    if (out_frames > 0) {
        if (fade_remaining_ > 0) {
            apply_fade(pcm_.data(), out_frames);
        }
        encode_block(pcm_.data(), out_frames, capture_ms);
    }
    last_capture_end_ms_ = capture_ms + duration_ms;
    return !failed_.load();
}

void WasapiMicrophoneCapture::apply_fade(int16_t* samples, size_t frames) {
    if (fade_remaining_ == 0) {
        return;
    }
    const size_t total = fade_frames();
    const size_t ramp = (std::min)(frames, fade_remaining_);
    for (size_t frame = 0; frame < ramp; ++frame) {
        const float gain =
            1.0f - static_cast<float>(fade_remaining_) / static_cast<float>(total);
        for (size_t channel = 0; channel < kMicChannels; ++channel) {
            samples[frame * kMicChannels + channel] = static_cast<int16_t>(
                samples[frame * kMicChannels + channel] * gain);
        }
        --fade_remaining_;
    }
}

void WasapiMicrophoneCapture::encode_block(const int16_t* samples, size_t frames,
                                           int64_t capture_ms) {
    std::vector<EncodedAudio> packets;
    Error encoded = encoder_->encode(samples, frames, static_cast<uint64_t>(capture_ms),
                                     packets);
    if (!encoded.ok()) {
        fail(encoded.to_string(), true);
        return;
    }
    std::lock_guard<std::mutex> lock(queue_mutex_);
    for (EncodedAudio& packet : packets) {
        if (queue_.size() >= config_.queue_packets) {
            queue_.pop_front();
            dropped_.fetch_add(1);
        }
        queue_.push_back(std::move(packet));
        packets_.fetch_add(1);
    }
}

void WasapiMicrophoneCapture::fail(const std::string& message, bool count_error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!failed_.exchange(true)) {
            error_ = message;
        }
        stopping_ = true;
    }
    if (count_error) {
        capture_errors_.fetch_add(1);
    }
    cv_.notify_all();
    if (event_) {
        SetEvent(event_);
    }
}

}  // namespace

std::vector<MicrophoneDevice> list_microphones(std::string* error) {
    std::vector<MicrophoneDevice> devices;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        if (error) {
            *error = wasapi::error_from_hresult(hr, "CoInitializeEx(microphones)").to_string();
        }
        return devices;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(enumerator.put()));
    if (FAILED(hr)) {
        if (error) {
            *error = wasapi::error_from_hresult(hr, "MMDeviceEnumerator").to_string();
        }
        if (uninitialize) {
            CoUninitialize();
        }
        return devices;
    }
    ComPtr<IMMDevice> default_device = default_capture_device(enumerator.get());
    const std::wstring default_id = wasapi::device_id(default_device.get());

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, collection.put());
    if (FAILED(hr)) {
        if (error) {
            *error = wasapi::error_from_hresult(hr, "EnumAudioEndpoints(capture)").to_string();
        }
        if (uninitialize) {
            CoUninitialize();
        }
        return devices;
    }
    UINT32 count = 0;
    if (SUCCEEDED(collection->GetCount(&count))) {
        devices.reserve(count);
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, device.put()))) {
                continue;
            }
            MicrophoneDevice entry;
            entry.id = narrow_utf8(wasapi::device_id(device.get()).c_str());
            entry.name = friendly_name(device.get());
            if (entry.name.empty()) {
                entry.name = "Microphone " + std::to_string(i + 1);
            }
            entry.is_default = !default_id.empty() && wasapi::device_id(device.get()) == default_id;
            devices.push_back(std::move(entry));
        }
    }
    std::stable_sort(devices.begin(), devices.end(),
                     [](const MicrophoneDevice& left, const MicrophoneDevice& right) {
                         return left.is_default && !right.is_default;
                     });
    if (uninitialize) {
        CoUninitialize();
    }
    return devices;
}

std::unique_ptr<IMicrophoneCapture> create_microphone_capture(
    const MicrophoneCaptureConfig& config, std::string* error) {
    auto capture = std::make_unique<WasapiMicrophoneCapture>(config);
    if (!capture->start_thread(error)) {
        return nullptr;
    }
    return capture;
}

}  // namespace chast
