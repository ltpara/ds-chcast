// WASAPI shared-mode renderer with a timeline jitter buffer.
//
// Decoded PCM arrives on the session's media thread; this object converts it
// to the endpoint's mix format, positions each block on the host timeline and
// writes it when the endpoint's queue has run out to that point. In the
// default mode the playback lead is the jitter buffer: blocks wait `lead_ms`
// after their host capture time before they are handed to the endpoint, which
// absorbs network jitter without letting latency grow. In arrival-anchored
// mode each burst is anchored when it arrives and plays immediately; capture
// stamps then only space blocks inside the burst. Expired blocks are dropped
// rather than played arbitrarily late. Arrival-anchored callers may opt into
// a small lateness budget; gaps the network cannot fill become silence instead
// of a permanently delayed stream.
//
// If the endpoint disappears or the default output changes, the renderer
// rebuilds itself on the new default and keeps decoding: only this thread
// touches the endpoint, so the session and the audio stream stay up while it
// retries. A pinned device is retried by id instead, so a user's explicit
// output choice never follows Windows' default. The first block after every
// start, gap or reopen is faded in.

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
#include <array>
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

// RustDesk rebuilds an output stream whose callback has not started within
// this window. In shared-mode WASAPI the equivalent signal is the endpoint
// consuming the audio handed to it, so this doubles as the stall window.
constexpr int64_t kStartupConfirmationMs = 5000;

// The first arrival-anchored blocks carry session setup and decoder warm-up,
// so anchoring on the first one schedules the whole session behind. Gather a
// few blocks, anchor on the fastest arrival and keep only this margin for
// jitter. The gathered blocks never had a slot and are skipped, so playback
// begins on the corrected timeline instead of replaying the startup backlog.
constexpr int64_t kAnchorGatherMarginMs = 5;

void update_max(std::atomic<uint64_t>& maximum, uint64_t value) {
    uint64_t observed = maximum.load(std::memory_order_relaxed);
    while (observed < value &&
           !maximum.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
    }
}
constexpr int64_t kAnchorGatherMaxWaitMs = 300;
// The measured startup pessimism is 33-58 ms; a single unusually fast block
// must not move the anchor further than that or later blocks miss their slots.
constexpr int64_t kAnchorGatherMaxShiftMs = 60;
constexpr std::array<size_t, 4> kAudioTimingEventKindLimits{256, 128, 128, 64};

Error av_error(int code, const char* what) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return Error(Status::CaptureFailed,
                 std::string(what) + " failed: " + text + " (" + std::to_string(code) + ")");
}

class WasapiPlayer final : public IAudioPlayer {
public:
    explicit WasapiPlayer(AudioPlayerConfig config)
        : config_(config), jitter_(config.adaptive_jitter_config) {
        lead_ms_.store(config.lead_ms);
        if (config_.arrival_anchored && config_.arrival_phase_correction) {
            PlayoutDriftController::Config phase_config;
            phase_config.window_ms = 500;
            phase_config.confirm_windows = 2;
            phase_config.dead_band_ms = 20;
            phase_config.max_step_ms = 20;
            phase_config.warmup_windows = 1;
            drift_ = PlayoutDriftController(phase_config);
        }
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
        int64_t queued_ms = 0;
        uint64_t diagnostic_sequence = 0;
        uint64_t diagnostic_received_steady_us = 0;
        uint64_t diagnostic_reordered_steady_us = 0;
        uint64_t diagnostic_revealed_by_sequence = 0;
        bool diagnostic_concealed = false;
        uint64_t queued_steady_us = 0;
        // Lead fixed when the block was queued. An adaptive raise must not
        // delay blocks that are already on their way to the endpoint, or a
        // partial block would hold half of its samples back and the endpoint
        // could run dry between halves.
        int64_t play_lead_ms = 0;
        // Canonical 48 kHz reference PCM for AEC. Kept only when the decoded
        // block already has the fixed reference format; the render sink gets
        // it after ReleaseBuffer succeeds, never at push time.
        std::vector<int16_t> reference;
        uint16_t reference_channels = 0;
        bool late_tolerance_counted = false;
    };

    void thread_main();
    bool open_endpoint(std::string* error);
    void close_endpoint();
    bool default_device_changed();
    bool render_loop_start();
    void render_loop();
    bool render();
    void record_recovery(AudioRecoveryCause cause, HRESULT hr,
                         bool render_failure = false);
    void record_timing_event(const AudioTimingEvent& event);
    Error convert(const DecodedAudio& audio, std::vector<uint8_t>& pcm, size_t& frames);
    void apply_fade(uint8_t* data, size_t frames);
    void fail(const std::string& message);
    // Wakes the render thread through the handle under format_mutex_, the same
    // lock close() holds while it retires the handle.
    void signal_render();
    void close();
    bool stopping() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }
    size_t bytes_per_frame() const {
        return static_cast<size_t>(format_.channels) *
               (format_.format == AV_SAMPLE_FMT_FLT ? 4 : 2);
    }
    int64_t playout_lead_ms() const {
        return static_cast<int64_t>(lead_ms_.load()) + jitter_ms_.load();
    }
    // Base clock offset plus the slow drift correction the controller added.
    int64_t effective_offset_ms() const {
        return offset_ms_.load() + drift_step_ms_.load();
    }

    AudioPlayerConfig config_;
    std::thread thread_;
    HANDLE event_ = nullptr;

    mutable std::mutex mutex_;
    mutable std::mutex timing_events_mutex_;
    std::vector<AudioTimingEvent> timing_events_;
    std::array<size_t, kAudioTimingEventKindLimits.size()> timing_event_counts_{};
    uint64_t next_timing_event_id_ = 1;
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
    std::vector<int16_t> reference_silence_;

    // Serialises push()'s conversion against an endpoint rebuild. The packet
    // thread must never touch swr_ while the render thread replaces the mix
    // format; endpoint_ready_ tells it to drop a block instead of blocking.
    std::mutex format_mutex_;
    bool endpoint_ready_ = false;

    std::deque<Block> queue_;
    uint64_t queued_frames_ = 0;
    bool have_pushed_ = false;
    bool primed_ = false;
    // Startup anchor gather state; guarded by mutex_. Only one gather runs,
    // before the first offset exists.
    size_t gather_blocks_ = 0;
    int64_t gather_first_latency_ms_ = 0;
    int64_t gather_first_arrival_ms_ = 0;
    int64_t gather_best_latency_ms_ = 0;
    int64_t last_input_ms_ = 0;
    int64_t last_active_render_ms_ = 0;
    int64_t last_end_local_ms_ = 0;
    // End of the audio handed to the endpoint on the playout timeline. Unlike
    // last_end_local_ms_ it survives underrun resets, so a dry stretch counts
    // once in starved_ms_ no matter how many render wake-ups it spans.
    int64_t played_end_ms_ = 0;
    AudioRenderAccounting render_accounting_;
    AudioStallReanchor stall_reanchor_;
    // Adaptive playout lead; only push() touches the controller, guarded by
    // mutex_, and the render thread reads the atomic copy.
    JitterLeadController jitter_;
    PlayoutDriftController drift_;
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
    std::atomic<uint64_t> starved_ms_{0};
    std::atomic<uint64_t> device_starved_ms_{0};
    std::atomic<uint64_t> device_max_gap_ms_{0};
    std::atomic<uint64_t> input_max_gap_ms_{0};
    std::atomic<uint64_t> input_to_render_max_ms_{0};
    std::atomic<uint64_t> render_wake_max_gap_ms_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> late_drops_{0};
    std::atomic<uint64_t> tolerated_late_blocks_{0};
    std::atomic<uint64_t> overflow_drops_{0};
    std::atomic<uint64_t> reanchor_dropped_blocks_{0};
    std::atomic<uint64_t> reanchor_dropped_ms_{0};
    std::atomic<uint64_t> recoveries_{0};
    std::atomic<uint64_t> render_failures_{0};
    std::atomic<uint64_t> stalled_recoveries_{0};
    std::atomic<uint64_t> device_change_recoveries_{0};
    std::atomic<AudioRecoveryCause> last_recovery_cause_{AudioRecoveryCause::None};
    std::atomic<int32_t> last_recovery_hresult_{0};
    std::atomic<uint64_t> recovery_open_failures_{0};
    std::atomic<uint32_t> buffer_ms_{0};
    std::atomic<uint32_t> endpoint_buffer_ms_{0};
    std::atomic<bool> shared_low_latency_{false};
    std::atomic<int32_t> shared_low_latency_result_{0};
    std::atomic<uint32_t> engine_period_us_{0};
    std::atomic<uint32_t> stream_latency_us_{0};
    std::atomic<uint64_t> playout_capture_ms_{0};
    std::atomic<int64_t> playout_scheduled_ms_{0};
    std::atomic<int64_t> playout_target_ms_{0};
    std::atomic<int64_t> slack_min_ms_{0};
    std::atomic<int64_t> slack_max_ms_{0};
    std::atomic<int64_t> slack_sum_ms_{0};
    std::atomic<uint64_t> slack_blocks_{0};
    std::atomic<uint64_t> slack_early_{0};
    std::atomic<uint64_t> slack_late_{0};
    std::atomic<int64_t> anchor_shift_ms_{0};
    std::atomic<int64_t> jitter_ms_{0};
    std::atomic<uint64_t> jitter_raises_{0};
    std::atomic<uint64_t> jitter_decays_{0};
    std::atomic<int64_t> drift_step_ms_{0};
    std::atomic<uint64_t> drift_corrections_{0};
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
    HRESULT hr = S_OK;
    if (!config_.device_id.empty()) {
        const std::wstring wanted = wasapi::widen_utf8(config_.device_id);
        if (wanted.empty()) {
            *error = "the selected playback device id is not valid UTF-8";
            return false;
        }
        hr = enumerator_->GetDevice(wanted.c_str(), device.put());
        if (FAILED(hr)) {
            *error = wasapi::error_from_hresult(hr, "GetDevice(audio player)").to_string();
            return false;
        }
        if (!wasapi::device_active(device.get())) {
            *error = "the selected playback device is not active";
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

    bool initialized = false;
    HRESULT low_latency_result = S_OK;
    if (config_.render_buffer_ms < 120) {
        ComPtr<IAudioClient3> client3 = com_query<IAudioClient3>(client);
        low_latency_result = E_NOINTERFACE;
        if (client3) {
            UINT32 default_period = 0;
            UINT32 fundamental_period = 0;
            UINT32 minimum_period = 0;
            UINT32 maximum_period = 0;
            hr = client3->GetSharedModeEnginePeriod(
                format.wave(), &default_period, &fundamental_period,
                &minimum_period, &maximum_period);
            low_latency_result = hr;
            if (SUCCEEDED(hr) && fundamental_period > 0 &&
                minimum_period <= maximum_period) {
                const uint64_t requested_frames =
                    static_cast<uint64_t>(format.sample_rate) *
                    config_.render_buffer_ms / 1000;
                uint64_t period_frames =
                    requested_frames / fundamental_period * fundamental_period;
                period_frames = (std::max)(period_frames,
                                           static_cast<uint64_t>(minimum_period));
                period_frames = (std::min)(period_frames,
                                           static_cast<uint64_t>(maximum_period));
                hr = client3->InitializeSharedAudioStream(
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    static_cast<UINT32>(period_frames), format.wave(), nullptr);
                initialized = SUCCEEDED(hr);
                low_latency_result = hr;
            }
        }
    }
    if (!initialized) {
        const REFERENCE_TIME buffer_hns =
            static_cast<REFERENCE_TIME>(config_.render_buffer_ms) * 10000;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK, buffer_hns, 0,
                                format.wave(), nullptr);
        if (FAILED(hr)) {
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0,
                                    format.wave(), nullptr);
        }
    }
    if (FAILED(hr)) {
        *error = wasapi::error_from_hresult(hr, "IAudioClient::Initialize").to_string();
        return false;
    }
    uint32_t engine_period_us = 0;
    if (ComPtr<IAudioClient3> client3 = com_query<IAudioClient3>(client)) {
        WAVEFORMATEX* current_mix = nullptr;
        UINT32 current_period = 0;
        if (SUCCEEDED(client3->GetCurrentSharedModeEnginePeriod(
                &current_mix, &current_period)) && current_mix &&
            current_mix->nSamplesPerSec > 0) {
            engine_period_us = static_cast<uint32_t>(
                static_cast<uint64_t>(current_period) * 1000000 /
                current_mix->nSamplesPerSec);
        }
        CoTaskMemFree(current_mix);
    }
    REFERENCE_TIME stream_latency = 0;
    client->GetStreamLatency(&stream_latency);
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
        endpoint_buffer_ms_.store(static_cast<uint32_t>(
            (static_cast<uint64_t>(buffer_frames_) * 1000 + format_.sample_rate - 1) /
            format_.sample_rate));
        shared_low_latency_.store(initialized);
        shared_low_latency_result_.store(static_cast<int32_t>(low_latency_result));
        engine_period_us_.store(engine_period_us);
        stream_latency_us_.store(static_cast<uint32_t>(stream_latency / 10));
        fade_frames_ = (std::max)(size_t{1},
                                  static_cast<size_t>(format_.sample_rate) * 5 / 1000);
        fade_remaining_ = 0;
        endpoint_ready_ = true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        last_active_render_ms_ = 0;
        queued_frames_ = 0;
        have_pushed_ = false;
        primed_ = false;
        gather_blocks_ = 0;
        gather_first_latency_ms_ = 0;
        gather_first_arrival_ms_ = 0;
        gather_best_latency_ms_ = 0;
        last_end_local_ms_ = 0;
        stall_reanchor_.reset();
        played_end_ms_ = 0;
        render_accounting_.reopen();
        jitter_.reset();
        jitter_ms_.store(0);
        jitter_raises_.store(0);
        jitter_decays_.store(0);
        drift_.reset();
        drift_step_ms_.store(0);
        drift_corrections_.store(0);
        buffer_ms_.store(0);
        playout_capture_ms_.store(0);
        playout_scheduled_ms_.store(0);
        playout_target_ms_.store(0);
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
        endpoint_buffer_ms_.store(0);
        shared_low_latency_.store(false);
        shared_low_latency_result_.store(0);
        engine_period_us_.store(0);
        stream_latency_us_.store(0);
        fade_frames_ = 0;
        fade_remaining_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        last_active_render_ms_ = 0;
        queued_frames_ = 0;
        have_pushed_ = false;
        primed_ = false;
        last_end_local_ms_ = 0;
        stall_reanchor_.reset();
        played_end_ms_ = 0;
        render_accounting_.reopen();
        buffer_ms_.store(0);
        playout_capture_ms_.store(0);
        playout_scheduled_ms_.store(0);
        playout_target_ms_.store(0);
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
    if (!config_.device_id.empty()) {
        // Pinned endpoint: retry the same device rather than moving playback
        // somewhere the user did not choose.
        return false;
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
        record_recovery(AudioRecoveryCause::ClientStart, hr, true);
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

void WasapiPlayer::signal_render() {
    std::lock_guard<std::mutex> lock(format_mutex_);
    if (event_) {
        SetEvent(event_);
    }
}

void WasapiPlayer::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    signal_render();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void WasapiPlayer::close() {
    stop();
    HANDLE event = nullptr;
    {
        // push() and the setters signal through this lock, so retiring the
        // handle here cannot race a SetEvent on a closed handle.
        std::lock_guard<std::mutex> lock(format_mutex_);
        endpoint_ready_ = false;
        if (swr_) {
            swr_free(&swr_);
        }
        av_channel_layout_uninit(&swr_input_);
        swr_input_rate_ = 0;
        swr_input_channels_ = 0;
        format_ = wasapi::MixFormat{};
        event = event_;
        event_ = nullptr;
    }
    if (event) {
        CloseHandle(event);
    }
}

void WasapiPlayer::render_loop() {
    int64_t last_progress = wasapi::now_ms();
    bool endpoint_open = render_loop_start();
    if (!endpoint_open) {
        close_endpoint();
        recovering_.store(true);
    }
    int64_t next_retry = wasapi::now_ms() + 1000;
    int64_t next_check = wasapi::now_ms() + 1000;
    uint64_t last_played = played_frames_.load();

    while (!stopping()) {
        if (!endpoint_open) {
            if (wasapi::now_ms() >= next_retry) {
                std::string open_error;
                const bool endpoint_ready = open_endpoint(&open_error);
                if (endpoint_ready && render_loop_start()) {
                    endpoint_open = true;
                    recovering_.store(false);
                    recoveries_.fetch_add(1);
                    next_check = wasapi::now_ms() + 1000;
                    last_progress = wasapi::now_ms();
                    last_played = played_frames_.load();
                } else {
                    if (!endpoint_ready) {
                        recovery_open_failures_.fetch_add(1);
                        std::fprintf(stderr, "audio-player: endpoint reopen failed: %s\n",
                                     open_error.c_str());
                    }
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
        if (wait == WAIT_FAILED) {
            record_recovery(AudioRecoveryCause::WaitForEvent,
                            HRESULT_FROM_WIN32(GetLastError()), true);
            close_endpoint();
            endpoint_open = false;
            recovering_.store(true);
            next_retry = wasapi::now_ms() + 1000;
            continue;
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
                record_recovery(AudioRecoveryCause::DefaultDeviceChanged, S_OK);
                device_change_recoveries_.fetch_add(1);
                close_endpoint();
                endpoint_open = false;
                recovering_.store(true);
                next_retry = wasapi::now_ms();
            } else if (!render()) {
                // The periodic render also refreshes the padding while idle,
                // so a drained endpoint reads as empty to the stall check
                // below; failing here means the engine is already gone.
                close_endpoint();
                endpoint_open = false;
                recovering_.store(true);
                next_retry = wasapi::now_ms() + 1000;
            }
        }

        const uint64_t played = played_frames_.load();
        if (played != last_played) {
            last_played = played;
            last_progress = wasapi::now_ms();
        } else if (buffer_ms_.load() > 0 &&
                   wasapi::now_ms() - last_progress >= kStartupConfirmationMs) {
            // RustDesk rebuilds an output stream whose callback has not run
            // within this window. Start() can succeed on an engine that never
            // consumes anything, so the callback here is the endpoint taking
            // audio: queued or buffered audio that stays unwritten for the
            // whole window means the stream is not running. An idle endpoint
            // has no buffered audio and never trips this.
            record_recovery(AudioRecoveryCause::ConsumptionStalled, S_OK);
            stalled_recoveries_.fetch_add(1);
            close_endpoint();
            endpoint_open = false;
            recovering_.store(true);
            next_retry = wasapi::now_ms() + 1000;
        }
    }

    if (endpoint_open) {
        close_endpoint();
    }
    recovering_.store(false);
}

void WasapiPlayer::record_recovery(AudioRecoveryCause cause, HRESULT hr,
                                   bool render_failure) {
    last_recovery_cause_.store(cause);
    last_recovery_hresult_.store(static_cast<int32_t>(hr));
    if (render_failure) {
        render_failures_.fetch_add(1);
    }
}

void WasapiPlayer::record_timing_event(const AudioTimingEvent& event) {
    if (!config_.collect_timing_events) {
        return;
    }
    if (event.kind == AudioTimingEventKind::LateDrop && event.lateness_us < 50000) {
        return;
    }
    const size_t kind_index = static_cast<size_t>(event.kind);
    if (kind_index >= kAudioTimingEventKindLimits.size()) {
        return;
    }
    std::lock_guard<std::mutex> lock(timing_events_mutex_);
    size_t& kind_count = timing_event_counts_[kind_index];
    if (kind_count >= kAudioTimingEventKindLimits[kind_index]) {
        const auto oldest = std::find_if(
            timing_events_.begin(), timing_events_.end(),
            [&event](const AudioTimingEvent& recorded) {
                return recorded.kind == event.kind;
            });
        if (oldest != timing_events_.end()) {
            timing_events_.erase(oldest);
            --kind_count;
        }
    }
    timing_events_.push_back(event);
    ++kind_count;
}

bool WasapiPlayer::render() {
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
        record_recovery(AudioRecoveryCause::GetCurrentPadding, hr, true);
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

    const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t now = now_us / 1000;
    const int64_t device_rate = format_.sample_rate;
    const size_t frame_bytes = bytes_per_frame();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            last_active_render_ms_ = 0;
        } else {
            if (last_active_render_ms_ > 0 && now > last_active_render_ms_) {
                update_max(render_wake_max_gap_ms_,
                           static_cast<uint64_t>(now - last_active_render_ms_));
            }
            last_active_render_ms_ = now;
        }
        if (have_pushed_ && !primed_) {
            // The lead only delays playback if the endpoint has something to
            // play while the queue waits. Prime it with silence once, up to
            // the lead, so blocks can be submitted a buffer early without
            // pulling the first sample forward.
            const uint64_t lead_frames = static_cast<uint64_t>(playout_lead_ms()) *
                                         static_cast<uint64_t>(device_rate) / 1000u;
            const UINT32 prime = static_cast<UINT32>(
                (std::min)(lead_frames, static_cast<uint64_t>(buffer_frames_)));
            while (padding < prime) {
                const UINT32 frames = (std::min)(prime - padding, buffer_frames_ - padding);
                BYTE* data = nullptr;
                hr = render_->GetBuffer(frames, &data);
                if (FAILED(hr)) {
                    record_recovery(AudioRecoveryCause::PrimeGetBuffer, hr, true);
                    return false;
                }
                std::memset(data, 0, static_cast<size_t>(frames) * frame_bytes);
                hr = render_->ReleaseBuffer(frames, 0);
                if (FAILED(hr)) {
                    record_recovery(AudioRecoveryCause::PrimeReleaseBuffer, hr, true);
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
            // The priming silence is the lead doing its job, not starvation;
            // a zero-length prime leaves the timeline unset so the first block
            // is not charged for the scheduling gap before it.
            if (prime > 0) {
                played_end_ms_ = (std::max)(played_end_ms_, last_end_local_ms_);
            }
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
                static_cast<int64_t>(head.host_capture_ms) - effective_offset_ms() +
                head.play_lead_ms;
            const int64_t duration_ms =
                static_cast<int64_t>(head.frames) * 1000 / device_rate;
            const AudioLateBlockDecision lateness = audio_late_block_decision(
                now, play_local + duration_ms, config_.late_tolerance_ms);
            if (lateness.drop && head.written == 0) {
                record_timing_event({
                    AudioTimingEventKind::LateDrop,
                    head.diagnostic_sequence,
                    head.host_capture_ms,
                    head.diagnostic_received_steady_us,
                    head.diagnostic_reordered_steady_us,
                    head.queued_steady_us,
                    static_cast<uint64_t>(now_us),
                    (play_local + duration_ms) * 1000,
                    lateness.late_ms * 1000,
                    0,
                    head.diagnostic_revealed_by_sequence,
                    head.diagnostic_concealed,
                });
                queued_frames_ -= head.frames - head.written;
                queue_.pop_front();
                dropped_.fetch_add(1);
                late_drops_.fetch_add(1);
                continue;
            }
            const bool tolerate_late =
                lateness.late_ms > 0 && !head.late_tolerance_counted;

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
            const int64_t written_ms =
                static_cast<int64_t>(head.written) * 1000 / device_rate;
            const int64_t audible_us = now_us +
                static_cast<int64_t>(padding) * 1000000 / device_rate;
            BYTE* data = nullptr;
            hr = render_->GetBuffer(static_cast<UINT32>(write_frames), &data);
            if (FAILED(hr)) {
                record_recovery(AudioRecoveryCause::GetBuffer, hr, true);
                return false;
            }
            const size_t source_offset = head.written * frame_bytes;
            if (mute) {
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
                record_recovery(AudioRecoveryCause::ReleaseBuffer, hr, true);
                return false;
            }
            if (head.written == 0 && head.queued_ms > 0 && now > head.queued_ms) {
                update_max(input_to_render_max_ms_,
                           static_cast<uint64_t>(now - head.queued_ms));
            }
            if (head.written == 0 && head.queued_steady_us > 0 && now_us > 0 &&
                static_cast<uint64_t>(now_us) - head.queued_steady_us >= 100000) {
                record_timing_event({
                    AudioTimingEventKind::FirstWrite,
                    head.diagnostic_sequence,
                    head.host_capture_ms,
                    head.diagnostic_received_steady_us,
                    head.diagnostic_reordered_steady_us,
                    head.queued_steady_us,
                    static_cast<uint64_t>(now_us),
                    play_local * 1000,
                    0,
                    0,
                    head.diagnostic_revealed_by_sequence,
                    head.diagnostic_concealed,
                });
            }
            if (tolerate_late) {
                head.late_tolerance_counted = true;
                tolerated_late_blocks_.fetch_add(1);
            }

            // Publish only after ReleaseBuffer accepted the samples. Their
            // audible time follows endpoint padding, not the requested slot.
            playout_capture_ms_.store(head.host_capture_ms + written_ms);
            playout_scheduled_ms_.store(audible_us / 1000);
            playout_target_ms_.store(play_local + written_ms);
            const int64_t previous_device_end_us = render_accounting_.end_us();
            render_accounting_.write(
                audible_us, static_cast<int64_t>(write_frames) * 1000000 / device_rate);
            device_starved_ms_.store(render_accounting_.starved_ms());
            device_max_gap_ms_.store(render_accounting_.max_gap_ms());
            const size_t reference_frames =
                head.reference.empty() ? 0 : head.reference.size() / head.reference_channels;
            const size_t reference_offset = reference_frames == 0
                ? 0
                : head.written * reference_frames / head.frames;
            head.written += write_frames;
            played_frames_.fetch_add(write_frames);
            queued_frames_ -= write_frames;
            padding += static_cast<UINT32>(write_frames);
            if (config_.render_sink && head.reference_channels != 0 &&
                reference_frames != 0) {
                const size_t reference_end = (std::min)(
                    reference_frames, head.written * reference_frames / head.frames);
                const int64_t audible_start = audible_us / 1000;
                if (mute) {
                    reference_silence_.assign(
                        (reference_end - reference_offset) * head.reference_channels, 0);
                    config_.render_sink(reference_silence_.data(),
                                        reference_end - reference_offset,
                                        head.reference_channels, audible_start);
                } else {
                    config_.render_sink(
                        head.reference.data() + reference_offset,
                        reference_end - reference_offset, head.reference_channels,
                        audible_start);
                }
            }
            const int64_t end_local =
                play_local + static_cast<int64_t>(head.written) * 1000 / device_rate;
            if (previous_device_end_us > 0 && audible_us > previous_device_end_us &&
                audible_us - previous_device_end_us >= 20000) {
                record_timing_event({
                    AudioTimingEventKind::DeviceGap,
                    head.diagnostic_sequence,
                    head.host_capture_ms,
                    head.diagnostic_received_steady_us,
                    head.diagnostic_reordered_steady_us,
                    head.queued_steady_us,
                    static_cast<uint64_t>(audible_us),
                    end_local * 1000,
                    0,
                    static_cast<uint64_t>(audible_us - previous_device_end_us),
                    head.diagnostic_revealed_by_sequence,
                    head.diagnostic_concealed,
                });
            }
            // Anything between the last written audio and this block's slot
            // plays as silence. Counting the timeline gap here charges a long
            // dry stretch once, whatever the render wake-up schedule was.
            if (played_end_ms_ > 0 && play_local > played_end_ms_) {
                starved_ms_.fetch_add(static_cast<uint64_t>(play_local - played_end_ms_));
            }
            played_end_ms_ = (std::max)(played_end_ms_, end_local);
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
        const int64_t arrival_ms = wasapi::now_ms();
        const uint64_t queued_steady_us = config_.collect_timing_events
            ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count())
            : 0;
        if (last_input_ms_ > 0 && arrival_ms > last_input_ms_) {
            update_max(input_max_gap_ms_,
                       static_cast<uint64_t>(arrival_ms - last_input_ms_));
        }
        last_input_ms_ = arrival_ms;
        const size_t gather_target =
            config_.arrival_anchored ? config_.anchor_gather_blocks : 0;
        if (!have_offset_.load() && gather_target > 0) {
            const int64_t latency = arrival_ms - static_cast<int64_t>(audio.capture_ms);
            if (gather_blocks_ == 0) {
                gather_first_latency_ms_ = latency;
                gather_first_arrival_ms_ = arrival_ms;
            }
            if (gather_blocks_ == 0 || latency < gather_best_latency_ms_) {
                gather_best_latency_ms_ = latency;
            }
            ++gather_blocks_;
            if (gather_blocks_ < gather_target &&
                arrival_ms - gather_first_arrival_ms_ < kAnchorGatherMaxWaitMs) {
                // Skipping the gathered blocks keeps the startup backlog from
                // becoming a permanent lead over the video.
                stall_reanchor_.observe(arrival_ms);
                return Error::success();
            }
            const int64_t margin = gather_blocks_ > 1 ? kAnchorGatherMarginMs : 0;
            const int64_t anchor_latency =
                (std::max)(gather_best_latency_ms_ + margin,
                           gather_first_latency_ms_ - kAnchorGatherMaxShiftMs);
            offset_ms_.store(-anchor_latency);
            have_offset_.store(true);
            anchor_shift_ms_.store((std::max)(
                int64_t{0}, gather_first_latency_ms_ - anchor_latency));
            slack_min_ms_.store(0);
            slack_max_ms_.store(0);
            slack_sum_ms_.store(0);
            slack_blocks_.store(0);
            slack_early_.store(0);
            slack_late_.store(0);
        }
        if (!have_offset_.load()) {
            // No offset yet: anchor this block at its arrival, so playback
            // starts after the jitter buffer (default mode) or immediately
            // (arrival-anchored mode) instead of waiting for the first probe.
            offset_ms_.store(static_cast<int64_t>(audio.capture_ms) - arrival_ms);
            have_offset_.store(true);
            if (config_.arrival_anchored) {
                stall_reanchor_.observe(arrival_ms);
            }
        } else if (config_.arrival_anchored && stall_reanchor_.observe(arrival_ms)) {
            // Arrival-anchored callers have no live clock offset (the mic
            // uplink has no LatencyReply, system audio plays on arrival).
            // After a stall the old offset can place the new burst in the
            // past; anchor it at arrival and apply the caller's queue policy.
            const int64_t previous_target_ms = static_cast<int64_t>(audio.capture_ms) -
                                               effective_offset_ms() + playout_lead_ms();
            const uint64_t queued_audio_ms = format_.sample_rate == 0
                ? 0
                : static_cast<uint64_t>(queued_frames_) * 1000 / format_.sample_rate;
            if (config_.collect_timing_events) {
                record_timing_event({
                    AudioTimingEventKind::StallReanchor,
                    audio.diagnostic_sequence,
                    audio.capture_ms,
                    audio.diagnostic_received_steady_us,
                    audio.diagnostic_reordered_steady_us,
                    queued_steady_us,
                    queued_steady_us,
                    previous_target_ms * 1000,
                    0,
                    0,
                    audio.diagnostic_revealed_by_sequence,
                    audio.diagnostic_concealed,
                    (arrival_ms - previous_target_ms) * 1000,
                    queued_audio_ms,
                    static_cast<uint64_t>(queue_.size()),
                });
            }
            offset_ms_.store(static_cast<int64_t>(audio.capture_ms) - arrival_ms);
            if (config_.arrival_phase_correction) {
                drift_.reanchor();
            } else {
                drift_.reset();
            }
            drift_step_ms_.store(0);
            if (!config_.preserve_queue_on_reanchor) {
                reanchor_dropped_blocks_.fetch_add(queue_.size());
                reanchor_dropped_ms_.fetch_add(queued_audio_ms);
                dropped_.fetch_add(queue_.size());
                queue_.clear();
                queued_frames_ = 0;
            }
            last_active_render_ms_ = 0;
            playout_capture_ms_.store(0);
            playout_scheduled_ms_.store(0);
            playout_target_ms_.store(0);
            slack_min_ms_.store(0);
            slack_max_ms_.store(0);
            slack_sum_ms_.store(0);
            slack_blocks_.store(0);
            slack_early_.store(0);
            slack_late_.store(0);
            if (config_.adaptive_jitter) {
                // The stall broke the timeline the learned lead was measured
                // against; start the new anchor at zero lead so playback
                // resumes at the lowest latency. Session counters stay.
                jitter_.release(arrival_ms);
                jitter_ms_.store(0);
            }
        }
        int64_t schedule_local = static_cast<int64_t>(audio.capture_ms) -
                                 effective_offset_ms() + playout_lead_ms();
        int64_t slack = schedule_local - arrival_ms;
        // The capture-anchored timeline can adapt too: `lead_ms` is the base
        // jitter buffer and the controller adds up to its own ceiling on top
        // when arrivals miss the scheduled slot.
        if (config_.adaptive_jitter) {
            const int64_t observed = jitter_.observe(slack, arrival_ms);
            if (observed != jitter_ms_.load()) {
                jitter_ms_.store(observed);
                jitter_raises_.store(jitter_.raises());
                jitter_decays_.store(jitter_.decays());
                // The reporting block moves with the new lead instead of
                // being dropped for the deficit it just measured.
                schedule_local = static_cast<int64_t>(audio.capture_ms) -
                                 effective_offset_ms() + playout_lead_ms();
                slack = schedule_local - arrival_ms;
            }
        }
        if (config_.phase_correction_enabled()) {
            // The lead is removed so an adaptive raise cannot look like drift;
            // the offset (including earlier drift steps) still shows up in the
            // phase, which is what makes the controller converge.
            const int64_t step =
                drift_.observe(slack - playout_lead_ms(), arrival_ms);
            if (step != 0) {
                drift_step_ms_.fetch_add(step);
                if (config_.arrival_anchored) {
                    drift_corrections_.fetch_add(1);
                } else {
                    drift_corrections_.store(drift_.corrections());
                }
                // The reporting block follows the corrected offset instead of
                // reporting the departure that was just applied.
                schedule_local = static_cast<int64_t>(audio.capture_ms) -
                                 effective_offset_ms() + playout_lead_ms();
                slack = schedule_local - arrival_ms;
            }
        }
        if (slack < slack_min_ms_.load()) {
            slack_min_ms_.store(slack);
        }
        if (slack > slack_max_ms_.load()) {
            slack_max_ms_.store(slack);
        }
        slack_sum_ms_.fetch_add(slack);
        slack_blocks_.fetch_add(1);
        if (slack > 20) {
            slack_early_.fetch_add(1);
        } else if (slack < -20) {
            slack_late_.fetch_add(1);
        }
        Block block;
        block.pcm = std::move(pcm);
        block.frames = frames;
        block.host_capture_ms = audio.capture_ms;
        block.queued_ms = arrival_ms;
        if (config_.collect_timing_events) {
            block.diagnostic_sequence = audio.diagnostic_sequence;
            block.diagnostic_received_steady_us = audio.diagnostic_received_steady_us;
            block.diagnostic_reordered_steady_us = audio.diagnostic_reordered_steady_us;
            block.diagnostic_revealed_by_sequence = audio.diagnostic_revealed_by_sequence;
            block.diagnostic_concealed = audio.diagnostic_concealed;
        }
        block.queued_steady_us = queued_steady_us;
        block.play_lead_ms = playout_lead_ms();
        if (config_.render_sink && audio.sample_rate == 48000 && audio.channels >= 1 &&
            audio.channels <= 2 && !audio.samples.empty()) {
            block.reference = audio.samples;
            block.reference_channels = audio.channels;
        }
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
            overflow_drops_.fetch_add(1);
        }
        // Signal under format_mutex_ so close() cannot free the handle first.
        if (event_) {
            SetEvent(event_);
        }
    }
    return Error::success();
}

void WasapiPlayer::set_clock_offset(int64_t host_minus_local_ms) {
    if (config_.arrival_anchored) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool had_offset = have_offset_.load();
        const int64_t previous = offset_ms_.load();
        // A large base change is a re-anchor rather than clock tracking: the
        // drift reference belongs to the old timeline and the accumulated
        // step is superseded by the fresh offset.
        const int64_t change = host_minus_local_ms > previous
                                   ? host_minus_local_ms - previous
                                   : previous - host_minus_local_ms;
        if (had_offset && change > 50) {
            drift_.reset();
            drift_step_ms_.store(0);
            drift_corrections_.store(0);
        }
        offset_ms_.store(host_minus_local_ms);
        have_offset_.store(true);
    }
    signal_render();
}

void WasapiPlayer::set_lead_ms(uint32_t lead_ms) {
    lead_ms_.store(std::min<uint32_t>(lead_ms, 1000));
    signal_render();
}

void WasapiPlayer::set_mute(bool mute) {
    muted_.store(mute);
    signal_render();
}

AudioPlayerStats WasapiPlayer::stats() const {
    AudioPlayerStats stats;
    stats.packets = packets_.load();
    stats.played_frames = played_frames_.load();
    stats.underruns = underruns_.load();
    stats.starved_ms = starved_ms_.load();
    stats.device_starved_ms = device_starved_ms_.load();
    stats.device_max_gap_ms = device_max_gap_ms_.load();
    stats.input_max_gap_ms = input_max_gap_ms_.load();
    stats.input_to_render_max_ms = input_to_render_max_ms_.load();
    stats.render_wake_max_gap_ms = render_wake_max_gap_ms_.load();
    if (config_.collect_timing_events) {
        std::lock_guard<std::mutex> lock(timing_events_mutex_);
        stats.timing_events = timing_events_;
    }
    stats.dropped = dropped_.load();
    stats.late_drops = late_drops_.load();
    stats.tolerated_late_blocks = tolerated_late_blocks_.load();
    stats.overflow_drops = overflow_drops_.load();
    stats.reanchor_dropped_blocks = reanchor_dropped_blocks_.load();
    stats.reanchor_dropped_ms = reanchor_dropped_ms_.load();
    stats.recoveries = recoveries_.load();
    stats.render_failures = render_failures_.load();
    stats.stalled_recoveries = stalled_recoveries_.load();
    stats.device_change_recoveries = device_change_recoveries_.load();
    stats.last_recovery_cause = last_recovery_cause_.load();
    stats.last_recovery_hresult = last_recovery_hresult_.load();
    stats.recovery_open_failures = recovery_open_failures_.load();
    stats.buffer_ms = buffer_ms_.load();
    stats.endpoint_buffer_ms = endpoint_buffer_ms_.load();
    stats.shared_low_latency = shared_low_latency_.load();
    stats.shared_low_latency_result = shared_low_latency_result_.load();
    stats.engine_period_us = engine_period_us_.load();
    stats.stream_latency_us = stream_latency_us_.load();
    stats.lead_ms = lead_ms_.load();
    stats.recovering = recovering_.load();
    stats.playout_capture_ms = playout_capture_ms_.load();
    stats.playout_scheduled_ms = playout_scheduled_ms_.load();
    stats.playout_target_ms = playout_target_ms_.load();
    stats.slack_min_ms = slack_min_ms_.load();
    stats.slack_max_ms = slack_max_ms_.load();
    const uint64_t slack_blocks = slack_blocks_.load();
    if (slack_blocks > 0) {
        stats.slack_avg_ms =
            static_cast<double>(slack_sum_ms_.load()) / static_cast<double>(slack_blocks);
    }
    stats.slack_early = slack_early_.load();
    stats.slack_late = slack_late_.load();
    stats.anchor_shift_ms = anchor_shift_ms_.load();
    stats.jitter_ms = static_cast<uint32_t>((std::max)(int64_t{0}, jitter_ms_.load()));
    stats.jitter_raises = jitter_raises_.load();
    stats.jitter_decays = jitter_decays_.load();
    stats.drift_ms = drift_step_ms_.load();
    stats.drift_corrections = drift_corrections_.load();
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
    signal_render();
    cv_.notify_all();
}

}  // namespace

std::vector<AudioOutputDevice> list_audio_outputs(std::string* error) {
    std::vector<AudioOutputDevice> devices;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        if (error) {
            *error = wasapi::error_from_hresult(hr, "CoInitializeEx(outputs)").to_string();
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
    const std::wstring default_id = wasapi::default_render_device_id(enumerator.get());

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put());
    if (FAILED(hr)) {
        if (error) {
            *error = wasapi::error_from_hresult(hr, "EnumAudioEndpoints(render)").to_string();
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
            AudioOutputDevice entry;
            const std::wstring id = wasapi::device_id(device.get());
            entry.id = wasapi::narrow_utf8(id.c_str());
            entry.name = wasapi::friendly_name(device.get());
            if (entry.name.empty()) {
                entry.name = "Output " + std::to_string(i + 1);
            }
            entry.is_default = !default_id.empty() && id == default_id;
            devices.push_back(std::move(entry));
        }
    }
    std::stable_sort(devices.begin(), devices.end(),
                     [](const AudioOutputDevice& left, const AudioOutputDevice& right) {
                         return left.is_default && !right.is_default;
                     });
    if (uninitialize) {
        CoUninitialize();
    }
    return devices;
}

std::unique_ptr<IAudioPlayer> create_audio_player(const AudioPlayerConfig& config,
                                                  std::string* error) {
    if (config.render_buffer_ms < 10 || config.render_buffer_ms > 500) {
        if (error) {
            *error = "the render buffer must be between 10 and 500 ms";
        }
        return nullptr;
    }
    auto player = std::make_unique<WasapiPlayer>(config);
    if (!player->start_thread(error)) {
        return nullptr;
    }
    return player;
}

}  // namespace chast
