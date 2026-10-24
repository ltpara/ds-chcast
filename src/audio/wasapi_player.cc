// WASAPI shared-mode renderer with a timeline jitter buffer.
//
// Decoded PCM arrives on the session's media thread; this object converts it
// to the endpoint's mix format, positions each block on the host timeline and
// writes it when the endpoint's queue has run out to that point. The playback
// lead is the jitter buffer: blocks wait `lead_ms` after their host capture
// time before they are handed to the endpoint, which absorbs network jitter
// without letting latency grow. Blocks that miss their slot are dropped rather
// than played late; a gap the network cannot fill becomes silence (counted as
// an underrun) instead of a permanently delayed stream.
//
// If the endpoint disappears or the default output changes, the renderer
// rebuilds itself on the new default and keeps decoding: only this thread
// touches the endpoint, so the session and the audio stream stay up while it
// retries. The first block after every start, gap or reopen is faded in.

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
#include <cmath>
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

Error av_error(int code, const char* what) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return Error(Status::CaptureFailed,
                 std::string(what) + " failed: " + text + " (" + std::to_string(code) + ")");
}

class WasapiPlayer final : public IAudioPlayer {
public:
    explicit WasapiPlayer(AudioPlayerConfig config) : config_(config) {
        lead_ms_.store(config.lead_ms);
    }
    ~WasapiPlayer() override { close(); }

    bool start_thread(std::string* error);

    Error start() override;
    void stop() override;
    Error push(const DecodedAudio& audio) override;
    void set_clock_offset(int64_t host_minus_local_ms) override;
    void set_lead_ms(uint32_t lead_ms) override;
    void set_mute(bool mute) override;
    bool muted() const override { return muted_.load(); }
    AudioPlayerStats stats() const override;
    std::string error() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

private:
    struct Block {
        std::vector<uint8_t> pcm;
        size_t frames = 0;
        size_t written = 0;
        uint64_t host_capture_ms = 0;
    };

    void thread_main();
    bool open_endpoint(std::string* error);
    void close_endpoint();
    bool default_device_changed();
    bool render_loop_start();
    void render_loop();
    bool render();
    Error convert(const DecodedAudio& audio, std::vector<uint8_t>& pcm, size_t& frames);
    void apply_fade(uint8_t* data, size_t frames);
    void fail(const std::string& message);
    void close();
    bool stopping() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }
    size_t bytes_per_frame() const {
        return static_cast<size_t>(format_.channels) *
               (format_.format == AV_SAMPLE_FMT_FLT ? 4 : 2);
    }

    AudioPlayerConfig config_;
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
    ComPtr<IAudioRenderClient> render_;
    ComPtr<ISimpleAudioVolume> volume_;
    wasapi::MixFormat format_;
    UINT32 buffer_frames_ = 0;
    bool client_started_ = false;
    SwrContext* swr_ = nullptr;
    AVChannelLayout swr_input_{};
    int swr_input_rate_ = 0;
    uint16_t swr_input_channels_ = 0;
    std::vector<uint8_t> scratch_;

    // Serialises push()'s conversion against an endpoint rebuild. The packet
    // thread must never touch swr_ while the render thread replaces the mix
    // format; endpoint_ready_ tells it to drop a block instead of blocking.
    std::mutex format_mutex_;
    bool endpoint_ready_ = false;

    std::deque<Block> queue_;
    uint64_t queued_frames_ = 0;
    bool have_pushed_ = false;
    bool primed_ = false;
    int64_t last_end_local_ms_ = 0;
    size_t fade_frames_ = 0;
    size_t fade_remaining_ = 0;
    bool applied_mute_ = false;

    std::atomic<int64_t> offset_ms_{0};
    std::atomic<bool> have_offset_{false};
    std::atomic<uint32_t> lead_ms_{80};
    std::atomic<bool> muted_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> recovering_{false};
    std::atomic<uint64_t> packets_{0};
    std::atomic<uint64_t> played_frames_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> recoveries_{0};
    std::atomic<uint32_t> buffer_ms_{0};
};

bool WasapiPlayer::start_thread(std::string* error) {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        if (error) {
            *error = "CreateEvent for audio render failed, GetLastError=" +
                     std::to_string(GetLastError());
        }
        return false;
    }
    thread_ = std::thread(&WasapiPlayer::thread_main, this);
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

void WasapiPlayer::thread_main() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        const std::string message =
            wasapi::error_from_hresult(hr, "CoInitializeEx(audio render)").to_string();
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
    const bool opened = open_endpoint(&open_error);
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
            render_loop();
        }
    }

    close_endpoint();
    enumerator_.reset();
    CoUninitialize();
}

bool WasapiPlayer::open_endpoint(std::string* error) {
    if (!enumerator_) {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(enumerator_.put()));
        if (FAILED(hr)) {
            *error = wasapi::error_from_hresult(hr, "MMDeviceEnumerator").to_string();
            return false;
        }
    }

    ComPtr<IMMDevice> device;
    HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "GetDefaultAudioEndpoint").to_string();
        return false;
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

    // A deeper shared-mode buffer lets the renderer hold the jitter lead plus
    // a whole AAC frame, so a block is never submitted only just in time.
    // Shared mode may still clamp this to the engine period; GetBufferSize is
    // authoritative. Falling back to the default keeps a device that refuses
    // the size usable.
    constexpr REFERENCE_TIME kBufferHns = 1200000;  // 120 ms
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            kBufferHns, 0, format.wave(), nullptr);
    if (FAILED(hr)) {
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, format.wave(),
                                nullptr);
    }
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "IAudioClient::Initialize").to_string();
        return false;
    }
    hr = client->SetEventHandle(event_);
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "SetEventHandle").to_string();
        return false;
    }
    UINT32 buffer_frames = 0;
    hr = client->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "GetBufferSize").to_string();
        return false;
    }
    ComPtr<IAudioRenderClient> render;
    hr = client->GetService(IID_PPV_ARGS(render.put()));
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "IAudioRenderClient").to_string();
        return false;
    }
    ComPtr<ISimpleAudioVolume> volume;
    hr = client->GetService(IID_PPV_ARGS(volume.put()));
    if (FAILED(hr)) {
        // Muting still works by writing silence; the session volume service is
        // only the cleaner way to do it.
        volume.reset();
    }
    const std::wstring id = wasapi::device_id(device.get());

    // From here on the local objects cannot fail; hand them to the members
    // before marking the endpoint usable so a concurrent push() either sees a
    // complete format or drops its block.
    device_ = std::move(device);
    device_id_ = id;
    client_ = std::move(client);
    render_ = std::move(render);
    volume_ = std::move(volume);
    applied_mute_ = false;
    {
        std::lock_guard<std::mutex> lock(format_mutex_);
        if (swr_) {
            swr_free(&swr_);
        }
        av_channel_layout_uninit(&swr_input_);
        swr_input_rate_ = 0;
        swr_input_channels_ = 0;
        format_ = std::move(format);
        buffer_frames_ = buffer_frames;
        fade_frames_ = (std::max)(size_t{1},
                                  static_cast<size_t>(format_.sample_rate) * 5 / 1000);
        fade_remaining_ = 0;
        endpoint_ready_ = true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        queued_frames_ = 0;
        have_pushed_ = false;
        primed_ = false;
        last_end_local_ms_ = 0;
        buffer_ms_.store(0);
    }
    return true;
}

void WasapiPlayer::close_endpoint() {
    if (client_ && client_started_) {
        client_->Stop();
        client_started_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(format_mutex_);
        endpoint_ready_ = false;
        if (swr_) {
            swr_free(&swr_);
        }
        av_channel_layout_uninit(&swr_input_);
        swr_input_rate_ = 0;
        swr_input_channels_ = 0;
        format_ = wasapi::MixFormat{};
        buffer_frames_ = 0;
        fade_frames_ = 0;
        fade_remaining_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        queued_frames_ = 0;
        have_pushed_ = false;
        primed_ = false;
        last_end_local_ms_ = 0;
        buffer_ms_.store(0);
    }
    render_.reset();
    volume_.reset();
    client_.reset();
    device_.reset();
    device_id_.clear();
}

bool WasapiPlayer::default_device_changed() {
    if (!wasapi::device_active(device_.get())) {
        return true;
    }
    if (device_id_.empty()) {
        return false;
    }
    const std::wstring current = wasapi::default_render_device_id(enumerator_.get());
    return current.empty() || current != device_id_;
}

bool WasapiPlayer::render_loop_start() {
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

Error WasapiPlayer::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_.load()) {
            return Error(Status::Unavailable, error_);
        }
        if (started_ || stopping_) {
            return Error::success();
        }
        started_ = true;
        start_requested_ = true;
    }
    cv_.notify_all();
    return Error::success();
}

void WasapiPlayer::stop() {
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

void WasapiPlayer::close() {
    stop();
    if (swr_) {
        swr_free(&swr_);
    }
    av_channel_layout_uninit(&swr_input_);
    if (event_) {
        CloseHandle(event_);
        event_ = nullptr;
    }
}

void WasapiPlayer::render_loop() {
    bool endpoint_open = render_loop_start();
    if (!endpoint_open) {
        close_endpoint();
        recovering_.store(true);
    }
    int64_t next_retry = wasapi::now_ms() + 1000;
    int64_t next_check = wasapi::now_ms() + 1000;

    while (!stopping()) {
        if (!endpoint_open) {
            if (wasapi::now_ms() >= next_retry) {
                std::string open_error;
                if (open_endpoint(&open_error) && render_loop_start()) {
                    endpoint_open = true;
                    recovering_.store(false);
                    recoveries_.fetch_add(1);
                    next_check = wasapi::now_ms() + 1000;
                } else {
                    close_endpoint();
                    next_retry = wasapi::now_ms() + 1000;
                }
            }
            WaitForSingleObject(event_, 100);
            continue;
        }

        const DWORD wait = WaitForSingleObject(event_, 100);
        if (stopping()) {
            break;
        }
        if (wait == WAIT_OBJECT_0 && !render()) {
            // A device removed mid-stream (or an engine reset) is routine:
            // keep the viewer's audio pipeline alive and retry the new default.
            close_endpoint();
            endpoint_open = false;
            recovering_.store(true);
            next_retry = wasapi::now_ms() + 1000;
            continue;
        }
        if (wasapi::now_ms() >= next_check) {
            next_check = wasapi::now_ms() + 1000;
            if (default_device_changed()) {
                close_endpoint();
                endpoint_open = false;
                recovering_.store(true);
                next_retry = wasapi::now_ms();
            }
        }
    }

    if (endpoint_open) {
        close_endpoint();
    }
    recovering_.store(false);
}

bool WasapiPlayer::render() {
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
        return false;
    }

    // The mute request comes from the bridge thread; only this thread touches
    // the endpoint's volume service, so a rebuild cannot race the COM pointer.
    const bool mute = muted_.load();
    if (mute != applied_mute_) {
        if (volume_) {
            volume_->SetMute(mute ? TRUE : FALSE, nullptr);
        }
        applied_mute_ = mute;
    }

    const int64_t now = wasapi::now_ms();
    const int64_t device_rate = format_.sample_rate;
    const size_t frame_bytes = bytes_per_frame();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (have_pushed_ && !primed_) {
            // The lead only delays playback if the endpoint has something to
            // play while the queue waits. Prime it with silence once, up to
            // the lead, so blocks can be submitted a buffer early without
            // pulling the first sample forward.
            const uint64_t lead_frames = static_cast<uint64_t>(lead_ms_.load()) *
                                         static_cast<uint64_t>(device_rate) / 1000u;
            const UINT32 prime = static_cast<UINT32>(
                (std::min)(lead_frames, static_cast<uint64_t>(buffer_frames_)));
            while (padding < prime) {
                const UINT32 frames = (std::min)(prime - padding, buffer_frames_ - padding);
                BYTE* data = nullptr;
                hr = render_->GetBuffer(frames, &data);
                if (FAILED(hr)) {
                    return false;
                }
                std::memset(data, 0, static_cast<size_t>(frames) * frame_bytes);
                hr = render_->ReleaseBuffer(frames, 0);
                if (FAILED(hr)) {
                    return false;
                }
                padding += frames;
            }
            primed_ = true;
            // The endpoint sat in silence while the lead elapsed; ramp the
            // first audible block so the transition cannot click.
            fade_remaining_ = fade_frames_;
            last_end_local_ms_ =
                now + static_cast<int64_t>(prime) * 1000 / device_rate;
        }
        for (;;) {
            if (queue_.empty()) {
                if (have_pushed_) {
                    // Silence between scheduled blocks is the lead doing its
                    // job; silence past the last block's end is an underrun.
                    if (last_end_local_ms_ > 0 && now > last_end_local_ms_ + 30) {
                        underruns_.fetch_add(1);
                        last_end_local_ms_ = 0;
                        fade_remaining_ = fade_frames_;
                    }
                }
                break;
            }

            Block& head = queue_.front();
            const int64_t play_local =
                static_cast<int64_t>(head.host_capture_ms) - offset_ms_.load() +
                static_cast<int64_t>(lead_ms_.load());
            const int64_t duration_ms =
                static_cast<int64_t>(head.frames) * 1000 / device_rate;
            if (play_local + duration_ms <= now) {
                queued_frames_ -= head.frames - head.written;
                queue_.pop_front();
                dropped_.fetch_add(1);
                continue;
            }

            const UINT32 available = buffer_frames_ > padding ? buffer_frames_ - padding : 0;
            if (available == 0) {
                break;
            }
            const int64_t padding_ms = static_cast<int64_t>(padding) * 1000 / device_rate;
            // A block submitted exactly when it is due is only just in time:
            // one late render wake-up skips its tail. Submitting up to one
            // endpoint buffer early keeps the device fed while the padding
            // already committed still counts toward the same deadline.
            const int64_t endpoint_ms =
                static_cast<int64_t>(buffer_frames_) * 1000 / device_rate;
            if (play_local > now + padding_ms + endpoint_ms) {
                break;
            }

            const size_t remaining = head.frames - head.written;
            const size_t write_frames = std::min<size_t>(remaining, available);
            BYTE* data = nullptr;
            hr = render_->GetBuffer(static_cast<UINT32>(write_frames), &data);
            if (FAILED(hr)) {
                return false;
            }
            const size_t source_offset = head.written * frame_bytes;
            if (muted_.load()) {
                // Zeroing the block is authoritative even if the endpoint's
                // volume service is missing or refuses the mute call.
                std::memset(data, 0, write_frames * frame_bytes);
            } else {
                std::memcpy(data, head.pcm.data() + source_offset,
                            write_frames * frame_bytes);
                apply_fade(data, write_frames);
            }
            hr = render_->ReleaseBuffer(static_cast<UINT32>(write_frames), 0);
            if (FAILED(hr)) {
                return false;
            }

            head.written += write_frames;
            played_frames_.fetch_add(write_frames);
            queued_frames_ -= write_frames;
            padding += static_cast<UINT32>(write_frames);
            const int64_t end_local =
                play_local + static_cast<int64_t>(head.written) * 1000 / device_rate;
            last_end_local_ms_ = (std::max)(last_end_local_ms_, end_local);
            if (head.written == head.frames) {
                queue_.pop_front();
            }
        }
        buffer_ms_.store(static_cast<uint32_t>(
            static_cast<int64_t>(padding) * 1000 / device_rate +
            static_cast<int64_t>(queued_frames_) * 1000 / device_rate));
    }
    return true;
}

void WasapiPlayer::apply_fade(uint8_t* data, size_t frames) {
    if (fade_remaining_ == 0 || fade_frames_ == 0) {
        return;
    }
    const size_t channels = format_.channels;
    const size_t ramp = (std::min)(frames, fade_remaining_);
    if (format_.format == AV_SAMPLE_FMT_FLT) {
        auto* samples = reinterpret_cast<float*>(data);
        for (size_t frame = 0; frame < ramp; ++frame) {
            const float gain = 1.0f - static_cast<float>(fade_remaining_) /
                                           static_cast<float>(fade_frames_);
            for (size_t channel = 0; channel < channels; ++channel) {
                samples[frame * channels + channel] *= gain;
            }
            --fade_remaining_;
        }
        return;
    }
    auto* samples = reinterpret_cast<int16_t*>(data);
    for (size_t frame = 0; frame < ramp; ++frame) {
        const float gain = 1.0f - static_cast<float>(fade_remaining_) /
                                       static_cast<float>(fade_frames_);
        for (size_t channel = 0; channel < channels; ++channel) {
            samples[frame * channels + channel] = static_cast<int16_t>(
                std::lround(samples[frame * channels + channel] * gain));
        }
        --fade_remaining_;
    }
}

Error WasapiPlayer::convert(const DecodedAudio& audio, std::vector<uint8_t>& pcm,
                            size_t& frames) {
    if (audio.sample_rate == 0 || audio.channels == 0 || audio.channels > 2) {
        return Error(Status::InvalidCall, "the decoded audio has no usable format");
    }
    if (audio.samples.empty() ||
        audio.samples.size() % static_cast<size_t>(audio.channels) != 0) {
        return Error(Status::InvalidCall, "the decoded audio sample count is invalid");
    }
    const size_t input_frames = audio.samples.size() / audio.channels;
    if (swr_input_rate_ != static_cast<int>(audio.sample_rate) ||
        swr_input_channels_ != audio.channels) {
        if (swr_) {
            swr_free(&swr_);
        }
        av_channel_layout_uninit(&swr_input_);
        AVChannelLayout output;
        av_channel_layout_default(&swr_input_, audio.channels);
        av_channel_layout_default(&output, format_.channels);
        const int status = swr_alloc_set_opts2(
            &swr_, &output, format_.format, static_cast<int>(format_.sample_rate),
            &swr_input_, AV_SAMPLE_FMT_S16, static_cast<int>(audio.sample_rate), 0, nullptr);
        av_channel_layout_uninit(&output);
        if (status < 0 || !swr_) {
            return av_error(status < 0 ? status : AVERROR(ENOMEM),
                            "swr_alloc_set_opts2(audio render)");
        }
        const int initialized = swr_init(swr_);
        if (initialized < 0) {
            return av_error(initialized, "swr_init(audio render)");
        }
        swr_input_rate_ = static_cast<int>(audio.sample_rate);
        swr_input_channels_ = audio.channels;
    }

    const int output_capacity = static_cast<int>(av_rescale_rnd(
        swr_get_delay(swr_, static_cast<int>(audio.sample_rate)) +
            static_cast<int64_t>(input_frames),
        format_.sample_rate, audio.sample_rate, AV_ROUND_UP));
    if (output_capacity <= 0) {
        frames = 0;
        return Error::success();
    }
    scratch_.resize(static_cast<size_t>(output_capacity) * bytes_per_frame());
    uint8_t* output = scratch_.data();
    const uint8_t* input = reinterpret_cast<const uint8_t*>(audio.samples.data());
    const int converted =
        swr_convert(swr_, &output, output_capacity, &input, static_cast<int>(input_frames));
    if (converted < 0) {
        return av_error(converted, "swr_convert(audio render)");
    }
    frames = static_cast<size_t>(converted);
    scratch_.resize(frames * bytes_per_frame());
    pcm = std::move(scratch_);
    return Error::success();
}

Error WasapiPlayer::push(const DecodedAudio& audio) {
    if (failed_.load()) {
        return Error(Status::Unavailable, error());
    }
    std::vector<uint8_t> pcm;
    size_t frames = 0;
    {
        // The lock spans conversion and enqueue so a block can never be
        // queued against a mix format that the render thread is replacing.
        std::lock_guard<std::mutex> format_lock(format_mutex_);
        if (!endpoint_ready_) {
            // The endpoint is being rebuilt. The decoded block is discarded
            // rather than converted against a format that no longer exists;
            // the render thread clears the queue on every rebuild anyway.
            return Error::success();
        }
        Error converted = convert(audio, pcm, frames);
        if (!converted.ok()) {
            return converted;
        }
        if (frames == 0) {
            return Error::success();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_.load() || stopping_) {
            return Error(Status::Unavailable,
                         error_.empty() ? std::string("audio playback is stopping") : error_);
        }
        if (!have_offset_.load()) {
            // No LatencyReply yet: treat this block as arriving one lead
            // early, so playback starts after the jitter buffer instead of
            // waiting for the first probe.
            offset_ms_.store(static_cast<int64_t>(audio.capture_ms) - wasapi::now_ms());
            have_offset_.store(true);
        } else if (config_.arrival_anchored && last_end_local_ms_ > 0 &&
                   wasapi::now_ms() > last_end_local_ms_ + 100) {
            // Microphone bursts carry the sender's clock and have no
            // LatencyReply. After a stall the old offset would place every
            // queued block in the past; re-anchor the new burst at arrival and
            // let the renderer drop what is left of the stale one.
            offset_ms_.store(static_cast<int64_t>(audio.capture_ms) - wasapi::now_ms());
            queue_.clear();
            queued_frames_ = 0;
        }
        Block block;
        block.pcm = std::move(pcm);
        block.frames = frames;
        block.host_capture_ms = audio.capture_ms;
        queue_.push_back(std::move(block));
        queued_frames_ += frames;
        have_pushed_ = true;
        packets_.fetch_add(1);

        const uint64_t cap_frames =
            static_cast<uint64_t>(config_.queue_ms) * format_.sample_rate / 1000;
        while (queued_frames_ > cap_frames && queue_.size() > 1) {
            const Block& oldest = queue_.front();
            queued_frames_ -= oldest.frames - oldest.written;
            queue_.pop_front();
            dropped_.fetch_add(1);
        }
    }
    if (event_) {
        SetEvent(event_);
    }
    return Error::success();
}

void WasapiPlayer::set_clock_offset(int64_t host_minus_local_ms) {
    if (config_.arrival_anchored) {
        return;
    }
    offset_ms_.store(host_minus_local_ms);
    have_offset_.store(true);
    if (event_) {
        SetEvent(event_);
    }
}

void WasapiPlayer::set_lead_ms(uint32_t lead_ms) {
    lead_ms_.store(std::min<uint32_t>(lead_ms, 1000));
    if (event_) {
        SetEvent(event_);
    }
}

void WasapiPlayer::set_mute(bool mute) {
    muted_.store(mute);
    if (event_) {
        SetEvent(event_);
    }
}

AudioPlayerStats WasapiPlayer::stats() const {
    AudioPlayerStats stats;
    stats.packets = packets_.load();
    stats.played_frames = played_frames_.load();
    stats.underruns = underruns_.load();
    stats.dropped = dropped_.load();
    stats.recoveries = recoveries_.load();
    stats.buffer_ms = buffer_ms_.load();
    stats.lead_ms = lead_ms_.load();
    stats.recovering = recovering_.load();
    return stats;
}

void WasapiPlayer::fail(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!failed_.exchange(true)) {
            error_ = message;
        }
        stopping_ = true;
    }
    if (event_) {
        SetEvent(event_);
    }
    cv_.notify_all();
}

}  // namespace

std::unique_ptr<IAudioPlayer> create_audio_player(const AudioPlayerConfig& config,
                                                  std::string* error) {
    auto player = std::make_unique<WasapiPlayer>(config);
    if (!player->start_thread(error)) {
        return nullptr;
    }
    return player;
}

}  // namespace chast
