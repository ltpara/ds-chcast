// WASAPI loopback capture on its own thread.
//
// The default render endpoint's mix is captured in shared mode with event
// callbacks, resampled to the fixed wire format (48 kHz stereo s16) and handed
// to the AAC encoder. Nothing here touches the video thread: the capture
// thread wakes on the endpoint's cadence, and only finished packets cross into
// the bounded queue the host drains.
//
// The device is opened during create(), before the stream is announced, so a
// machine with no usable endpoint never advertises audio. If the endpoint
// disappears or the default changes later, the thread follows the new default
// and reopens in place: the announced AudioInfo never changes, so the viewer
// keeps its stream and only hears a short gap. Digital silence that lasts
// longer than the configured gate stops feeding the encoder, and audio that
// resumes is ramped in to avoid a click.

#include "chast/audio.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

extern "C" {
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

// The encoded format is fixed for the session. A default-output change then
// only replaces the device and resampler; AudioInfo stays valid.
constexpr uint32_t kCaptureRate = 48000;
constexpr uint16_t kCaptureChannels = 2;
constexpr size_t kFadeMs = 5;

class WasapiLoopbackCapture final : public ILoopbackCapture {
public:
    explicit WasapiLoopbackCapture(LoopbackCaptureConfig config)
        : config_(std::move(config)) {
        if ((config_.echo_cancellation && config_.reference) ||
            config_.noise_suppression) {
            AudioDspConfig dsp_config;
            dsp_config.near_channels = kCaptureChannels;
            dsp_config.echo_cancellation = config_.echo_cancellation;
            dsp_config.noise_suppression = config_.noise_suppression;
            dsp_config.reference = config_.reference;
            std::string dsp_error;
            // A DSP that cannot open must not take system sound down: the
            // capture falls back to sending the untouched mix.
            dsp_ = AudioPreprocessor::create(dsp_config, &dsp_error);
        }
    }
    ~WasapiLoopbackCapture() override { close(); }

    // Spawns the worker and waits for it to open the endpoint.
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
        stats.gate_transitions = gate_transitions_.load();
        stats.gated_frames = gated_frames_.load();
        stats.recoveries = recoveries_.load();
        stats.recovering = recovering_.load();
        if (dsp_) {
            const AudioDspStats dsp = dsp_->stats();
            stats.processed_frames = dsp.processed_frames;
            stats.reference_underruns = dsp.reference_underruns;
            stats.dsp_resets = dsp.resets;
        }
        return stats;
    }

private:
    void thread_main();
    bool open_encoder(std::string* error);
    bool open_device(std::string* error);
    void close_device();
    bool start_client();
    bool default_device_changed();
    void close();
    void capture_loop();
    bool drain();
    bool process(const BYTE* data, UINT32 frames, DWORD flags);
    void encode_block(const int16_t* samples, size_t frames, int64_t capture_ms);
    void apply_fade(int16_t* samples, size_t frames);
    void fail(const std::string& message, bool count_error);
    static size_t fade_frames() {
        return static_cast<size_t>(kCaptureRate) * kFadeMs / 1000;
    }
    bool stopping() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

    LoopbackCaptureConfig config_;
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
    std::unique_ptr<AudioPreprocessor> dsp_;
    SwrContext* swr_ = nullptr;
    std::vector<int16_t> pcm_;
    std::vector<uint8_t> zero_input_;
    AudioInfo info_;

    int64_t silent_ms_ = 0;
    int64_t last_capture_end_ms_ = 0;
    size_t fade_remaining_ = 0;
    bool gated_ = false;

    std::mutex queue_mutex_;
    std::deque<EncodedAudio> queue_;

    std::atomic<bool> failed_{false};
    std::atomic<bool> recovering_{false};
    std::atomic<uint64_t> packets_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> silence_frames_{0};
    std::atomic<uint64_t> capture_errors_{0};
    std::atomic<uint64_t> gate_transitions_{0};
    std::atomic<uint64_t> gated_frames_{0};
    std::atomic<uint64_t> recoveries_{0};
};

bool WasapiLoopbackCapture::start_thread(std::string* error) {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        if (error) {
            *error = "CreateEvent for audio capture failed, GetLastError=" +
                     std::to_string(GetLastError());
        }
        return false;
    }
    thread_ = std::thread(&WasapiLoopbackCapture::thread_main, this);
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

void WasapiLoopbackCapture::start() {
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

void WasapiLoopbackCapture::stop() {
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

void WasapiLoopbackCapture::close() {
    stop();
    if (event_) {
        CloseHandle(event_);
        event_ = nullptr;
    }
}

bool WasapiLoopbackCapture::take_packet(EncodedAudio& packet) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.empty()) {
        return false;
    }
    packet = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void WasapiLoopbackCapture::thread_main() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        const std::string message =
            wasapi::error_from_hresult(hr, "CoInitializeEx(audio capture)").to_string();
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

bool WasapiLoopbackCapture::open_encoder(std::string* error) {
    AudioEncoderConfig encoder_config;
    encoder_config.codec = AudioCodec::Opus;
    encoder_config.sample_rate = kCaptureRate;
    encoder_config.channels = kCaptureChannels;
    encoder_config.bitrate = config_.bitrate;
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

bool WasapiLoopbackCapture::open_device(std::string* error) {
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
        const std::wstring wanted = wasapi::widen_utf8(config_.device_id);
        if (wanted.empty()) {
            *error = "the selected audio output id is not valid UTF-8";
            return false;
        }
        hr = enumerator_->GetDevice(wanted.c_str(), device.put());
        if (FAILED(hr)) {
            *error = wasapi::error_from_hresult(hr, "GetDevice(audio output)").to_string();
            return false;
        }
        if (!wasapi::device_active(device.get())) {
            *error = "the selected audio output is not active";
            return false;
        }
    } else {
        hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
        if (FAILED(hr)) {
            *error = wasapi::error_from_hresult(hr, "GetDefaultAudioEndpoint").to_string();
            return false;
        }
    }
    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.put()));
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "IAudioClient activate").to_string();
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "GetMixFormat").to_string();
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

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK |
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            0, 0, format.wave(), nullptr);
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "IAudioClient::Initialize").to_string();
        return false;
    }
    hr = client->SetEventHandle(event_);
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "SetEventHandle").to_string();
        return false;
    }
    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(IID_PPV_ARGS(capture.put()));
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "IAudioCaptureClient").to_string();
        return false;
    }

    // The device may mix at any rate or channel count; the wire format does
    // not follow it. Keeping 48 kHz stereo here is what makes a device switch
    // possible without renegotiating AudioInfo.
    SwrContext* swr = nullptr;
    AVChannelLayout input = wasapi::input_layout(format);
    AVChannelLayout output;
    av_channel_layout_default(&output, kCaptureChannels);
    const int status = swr_alloc_set_opts2(
        &swr, &output, AV_SAMPLE_FMT_S16, kCaptureRate, &input, format.format,
        static_cast<int>(format.sample_rate), 0, nullptr);
    av_channel_layout_uninit(&input);
    av_channel_layout_uninit(&output);
    if (status < 0 || !swr) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(status < 0 ? status : AVERROR(ENOMEM), text, sizeof(text));
        *error = std::string("swr_alloc_set_opts2(audio capture) failed: ") + text;
        return false;
    }
    const int initialized = swr_init(swr);
    if (initialized < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(initialized, text, sizeof(text));
        swr_free(&swr);
        *error = std::string("swr_init(audio capture) failed: ") + text;
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

void WasapiLoopbackCapture::close_device() {
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
    silent_ms_ = 0;
    last_capture_end_ms_ = 0;
    fade_remaining_ = 0;
    gated_ = false;
    if (dsp_) {
        dsp_->reset();
    }
}

bool WasapiLoopbackCapture::start_client() {
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

bool WasapiLoopbackCapture::default_device_changed() {
    if (!wasapi::device_active(device_.get())) {
        return true;
    }
    if (!config_.device_id.empty()) {
        // Pinned endpoint: keep retrying it instead of silently moving the
        // session's audio to another device.
        return false;
    }
    if (device_id_.empty()) {
        return false;
    }
    const std::wstring current = wasapi::default_render_device_id(enumerator_.get());
    return current.empty() || current != device_id_;
}

void WasapiLoopbackCapture::capture_loop() {
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
                    silent_ms_ = 0;
                    last_capture_end_ms_ = 0;
                    gated_ = false;
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
            // The endpoint vanished (unplug, disable, driver reset). Keep the
            // announced audio stream alive and follow the new default.
            close_device();
            endpoint_open = false;
            recovering_.store(true);
            next_retry = wasapi::now_ms() + 1000;
            continue;
        }
        if (wasapi::now_ms() >= next_check) {
            next_check = wasapi::now_ms() + 1000;
            if (default_device_changed()) {
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

bool WasapiLoopbackCapture::drain() {
    for (;;) {
        UINT32 next = 0;
        HRESULT hr = capture_->GetNextPacketSize(&next);
        if (FAILED(hr)) {
            // Endpoint loss is recoverable; the loop reopens the new default.
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

bool WasapiLoopbackCapture::process(const BYTE* data, UINT32 frames, DWORD flags) {
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
        // WASAPI forbids reading the buffer when the silent flag is set. Feed
        // zeros through the resampler so its phase state and the output length
        // stay aligned with the audible path.
        const size_t bytes = static_cast<size_t>(frames) * format_.channels *
                             static_cast<size_t>(av_get_bytes_per_sample(format_.format));
        zero_input_.assign(bytes, 0);
        input = zero_input_.data();
    }

    const int out_capacity = static_cast<int>(av_rescale_rnd(
        swr_get_delay(swr_, static_cast<int>(format_.sample_rate)) + frames, kCaptureRate,
        format_.sample_rate, AV_ROUND_UP));
    if (out_capacity <= 0) {
        last_capture_end_ms_ = capture_ms + duration_ms;
        return true;
    }
    pcm_.resize(static_cast<size_t>(out_capacity) * kCaptureChannels);
    uint8_t* output = reinterpret_cast<uint8_t*>(pcm_.data());
    const int converted = swr_convert(swr_, &output, out_capacity, &input,
                                      static_cast<int>(frames));
    if (converted < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(converted, text, sizeof(text));
        fail(std::string("swr_convert(audio capture) failed: ") + text, true);
        return false;
    }
    const size_t out_frames = static_cast<size_t>(converted);
    if (dsp_ && out_frames > 0) {
        // Cancellation runs on the fixed 48 kHz stereo stream, after the
        // device conversion and before the silence gate.
        dsp_->process(pcm_.data(), out_frames);
    }

    bool silent = silent_flag;
    if (!silent && out_frames > 0) {
        silent = true;
        const int16_t threshold = config_.echo_cancellation ? 32 : 0;
        const int16_t* samples = pcm_.data();
        const size_t count = out_frames * kCaptureChannels;
        for (size_t i = 0; i < count; ++i) {
            if (samples[i] > threshold || samples[i] < -threshold) {
                silent = false;
                break;
            }
        }
    }

    if (silent) {
        silence_frames_.fetch_add(frames);
        if (gated_) {
            gated_frames_.fetch_add(frames);
            last_capture_end_ms_ = capture_ms + duration_ms;
            return !failed_.load();
        }
        silent_ms_ += duration_ms;
        if (config_.silence_gate_ms > 0 &&
            silent_ms_ >= static_cast<int64_t>(config_.silence_gate_ms)) {
            gated_ = true;
            gate_transitions_.fetch_add(1);
            // This block is still encoded so a partial AAC frame holding the
            // last real samples is completed; later silent blocks are skipped.
        }
    } else {
        silent_ms_ = 0;
        if (gated_) {
            gated_ = false;
            gate_transitions_.fetch_add(1);
            fade_remaining_ = fade_frames();
        }
        if (last_capture_end_ms_ > 0 && capture_ms > last_capture_end_ms_ + 100) {
            fade_remaining_ = fade_frames();
        }
    }

    if (out_frames > 0) {
        if (!silent && fade_remaining_ > 0) {
            apply_fade(pcm_.data(), out_frames);
        }
        encode_block(pcm_.data(), out_frames, capture_ms);
    }
    last_capture_end_ms_ = capture_ms + duration_ms;
    return !failed_.load();
}

void WasapiLoopbackCapture::apply_fade(int16_t* samples, size_t frames) {
    if (fade_remaining_ == 0) {
        return;
    }
    const size_t total = fade_frames();
    const size_t ramp = (std::min)(frames, fade_remaining_);
    for (size_t frame = 0; frame < ramp; ++frame) {
        const float gain =
            1.0f - static_cast<float>(fade_remaining_) / static_cast<float>(total);
        for (size_t channel = 0; channel < kCaptureChannels; ++channel) {
            samples[frame * kCaptureChannels + channel] = static_cast<int16_t>(
                samples[frame * kCaptureChannels + channel] * gain);
        }
        --fade_remaining_;
    }
}

void WasapiLoopbackCapture::encode_block(const int16_t* samples, size_t frames,
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

void WasapiLoopbackCapture::fail(const std::string& message, bool count_error) {
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

std::unique_ptr<ILoopbackCapture> create_loopback_capture(const LoopbackCaptureConfig& config,
                                                          std::string* error) {
    auto capture = std::make_unique<WasapiLoopbackCapture>(config);
    if (!capture->start_thread(error)) {
        return nullptr;
    }
    return capture;
}

}  // namespace chast
