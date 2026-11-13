// Playback reference and SpeexDSP preprocessing for the audio paths.
//
// Echo cancellation needs two streams: the near end (microphone or loopback)
// and the far end (what this machine is playing). The far end is shared with
// the player thread, so AudioReference is a lock-protected ring rather than a
// callback: the render side pushes, the capture side reads a fixed distance
// behind the newest sample. That fixed delay is what makes the reference match
// the audio that is audible now instead of the block that was just decoded.
//
// SpeexDSP consumes exactly 10 ms frames. Capture blocks arrive at whatever
// size the endpoint chose, so AudioPreprocessor keeps an interleaved remainder
// between calls. The echo canceller uses a 200 ms adaptive filter, enough to
// cover the playback lead, the endpoint buffer and the acoustic path; the
// preprocessor adds residual echo suppression and, when requested, spectral
// noise suppression. AGC is deliberately left off: it pumps when the user
// switches between speakers and a headset.

#include "chast/audio.h"

extern "C" {
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace chast {

namespace {

constexpr uint32_t kDspRate = 48000;
constexpr size_t kDspFrame = kDspRate / 100;  // 10 ms
constexpr size_t kDspFilter = kDspRate * 200 / 1000;
constexpr size_t kReferenceCapacity = kDspRate * 2;  // two seconds
constexpr size_t kReferenceChannels = 2;

}  // namespace

struct AudioReference::Impl {
    mutable std::mutex mutex;
    std::vector<int16_t> ring =
        std::vector<int16_t>(kReferenceCapacity * kReferenceChannels, 0);
    // Untimed mode advances this exactly like the old append-only ring.
    uint64_t written = 0;
    // A reference may be owned by several players. The last release clears
    // the history, not every individual stop.
    uint32_t producers = 0;
    bool legacy_active = false;
    // Timed mode places each pushed block on the steady-clock playhead and
    // mixes overlapping producers.
    bool timed = false;
    int64_t origin_ms = 0;
    bool have_frames = false;
    int64_t first_frame = 0;
    int64_t last_frame = 0;
    int64_t last_push_steady_ms = 0;
    // The ring wraps after two seconds. A position that maps to the same slot
    // as a previous lap must start from zero before concurrent producers mix
    // into it; otherwise old playback leaks into the current reference.
    std::vector<int64_t> stamp = std::vector<int64_t>(kReferenceCapacity, -1);

    bool active_locked() const { return producers > 0 || legacy_active; }

    void reset_locked() {
        std::fill(ring.begin(), ring.end(), 0);
        written = 0;
        timed = false;
        origin_ms = 0;
        have_frames = false;
        first_frame = 0;
        last_frame = 0;
        last_push_steady_ms = 0;
        std::fill(stamp.begin(), stamp.end(), int64_t{-1});
    }

    void push_timed(const int16_t* samples, size_t frames, uint16_t channels,
                    int64_t audible_start_ms);
};

namespace {

int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t frames_from_ms(int64_t ms) {
    return ms * static_cast<int64_t>(kDspRate) / 1000;
}

void mix_frame(int16_t* ring, size_t index, int16_t left, int16_t right) {
    const int mixed_left = static_cast<int>(ring[index]) + left;
    const int mixed_right = static_cast<int>(ring[index + 1]) + right;
    ring[index] = static_cast<int16_t>(std::clamp(mixed_left, -32768, 32767));
    ring[index + 1] = static_cast<int16_t>(std::clamp(mixed_right, -32768, 32767));
}

}  // namespace

void AudioReference::Impl::push_timed(const int16_t* samples, size_t frames,
                                      uint16_t channels, int64_t audible_start_ms) {
    const int64_t raw_start = frames_from_ms(audible_start_ms - origin_ms);
    const int64_t start = (std::max)(int64_t{0}, raw_start);
    // A block that began before origin loses only its head; clamping the
    // whole block would shift every later sample earlier.
    const size_t first = raw_start < 0
        ? static_cast<size_t>((std::min)(static_cast<int64_t>(frames), -raw_start))
        : 0;
    if (first >= frames) {
        last_push_steady_ms = steady_ms();
        return;
    }
    if (!have_frames) {
        first_frame = start;
        have_frames = true;
    } else {
        first_frame = (std::min)(first_frame, start);
    }
    last_frame = (std::max)(last_frame, start + static_cast<int64_t>(frames - first));
    last_push_steady_ms = steady_ms();
    for (size_t frame = first; frame < frames; ++frame) {
        const int64_t position = raw_start + static_cast<int64_t>(frame);
        const int64_t slot = position % static_cast<int64_t>(kReferenceCapacity);
        const size_t index = static_cast<size_t>(slot) * kReferenceChannels;
        if (stamp[static_cast<size_t>(slot)] != position) {
            stamp[static_cast<size_t>(slot)] = position;
            ring[index] = 0;
            ring[index + 1] = 0;
        }
        const int16_t left = samples[frame * channels];
        const int16_t right = channels > 1 ? samples[frame * channels + 1] : left;
        mix_frame(ring.data(), index, left, right);
    }
}

AudioReference::AudioReference() : impl_(std::make_unique<Impl>()) {}
AudioReference::~AudioReference() = default;

void AudioReference::acquire() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->producers++ == 0 && !impl_->legacy_active) {
        impl_->reset_locked();
        impl_->origin_ms = steady_ms();
    }
}

void AudioReference::release() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->producers == 0) {
        return;
    }
    if (--impl_->producers == 0 && !impl_->legacy_active) {
        impl_->reset_locked();
    }
}

void AudioReference::push(const int16_t* samples, size_t frames, uint16_t channels) {
    if (!samples || frames == 0 || channels == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->active_locked()) {
        return;
    }
    if (impl_->timed) {
        impl_->push_timed(samples, frames, channels, steady_ms());
        return;
    }
    for (size_t frame = 0; frame < frames; ++frame) {
        const int16_t left = samples[frame * channels];
        const int16_t right = channels > 1 ? samples[frame * channels + 1] : left;
        const size_t index = static_cast<size_t>(impl_->written % kReferenceCapacity) *
                             kReferenceChannels;
        impl_->ring[index] = left;
        impl_->ring[index + 1] = right;
        ++impl_->written;
    }
}

void AudioReference::push_rendered(const int16_t* samples, size_t frames, uint16_t channels,
                                   int64_t audible_start_ms) {
    if (!samples || frames == 0 || channels == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->active_locked()) {
        return;
    }
    if (!impl_->timed) {
        // Mixing needs one timeline. A reference must not mix legacy appends
        // and timestamped renders, so switching modes starts a fresh window.
        impl_->reset_locked();
        impl_->origin_ms = steady_ms();
        impl_->timed = true;
    }
    impl_->push_timed(samples, frames, channels, audible_start_ms);
}

bool AudioReference::read_delayed(size_t frames, uint32_t delay_ms, int16_t* out) const {
    return read_delayed_frames(frames,
                               static_cast<size_t>(delay_ms) * kDspRate / 1000u, out);
}

bool AudioReference::read_delayed_frames(size_t frames, size_t delay_frames,
                                         int16_t* out) const {
    if (!out || frames == 0) {
        return true;
    }
    std::fill(out, out + frames * kReferenceChannels, 0);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->active_locked()) {
        return false;
    }
    if (!impl_->timed) {
        const uint64_t delay = delay_frames;
        const uint64_t total = impl_->written;
        const uint64_t end = total > delay ? total - delay : 0;
        if (end == 0) {
            return false;
        }
        const uint64_t wanted_start = end > frames ? end - frames : 0;
        const uint64_t oldest = total > kReferenceCapacity ? total - kReferenceCapacity : 0;
        const uint64_t start = (std::max)(wanted_start, oldest);
        // The block always ends at `end`, so missing history leaves zeros at its
        // front. This keeps the newest samples next to the capture block.
        for (uint64_t frame = start; frame < end; ++frame) {
            const size_t source =
                static_cast<size_t>(frame % kReferenceCapacity) * kReferenceChannels;
            const size_t target =
                static_cast<size_t>(frame + frames - end) * kReferenceChannels;
            out[target] = impl_->ring[source];
            out[target + 1] = impl_->ring[source + 1];
        }
        return start == wanted_start && end >= frames;
    }
    if (!impl_->have_frames || impl_->last_push_steady_ms == 0 ||
        steady_ms() - impl_->last_push_steady_ms > 500) {
        // A stalled stream must not keep feeding old samples to the canceller.
        return false;
    }
    const int64_t head =
        (std::max)(int64_t{0}, frames_from_ms(steady_ms() - impl_->origin_ms));
    const int64_t end = head - static_cast<int64_t>(delay_frames);
    if (end <= 0 || end > impl_->last_frame) {
        return false;
    }
    const int64_t wanted_start = end - static_cast<int64_t>(frames);
    const int64_t oldest =
        (std::max)(impl_->first_frame, end - static_cast<int64_t>(kReferenceCapacity));
    const int64_t start = (std::max)(wanted_start, oldest);
    for (int64_t frame = start; frame < end; ++frame) {
        const int64_t slot = frame % static_cast<int64_t>(kReferenceCapacity);
        if (impl_->stamp[static_cast<size_t>(slot)] != frame) {
            continue;
        }
        const size_t source = static_cast<size_t>(slot) * kReferenceChannels;
        const size_t target =
            static_cast<size_t>(frame + static_cast<int64_t>(frames) - end) *
            kReferenceChannels;
        out[target] = impl_->ring[source];
        out[target + 1] = impl_->ring[source + 1];
    }
    return start == wanted_start && end - start == static_cast<int64_t>(frames);
}

void AudioReference::set_active(bool active) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->legacy_active == active) {
        return;
    }
    impl_->legacy_active = active;
    if (impl_->producers == 0) {
        impl_->reset_locked();
        if (active) {
            impl_->origin_ms = steady_ms();
        }
    }
}

bool AudioReference::active() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->active_locked()) {
        return false;
    }
    if (impl_->timed && (impl_->last_push_steady_ms == 0 ||
                         steady_ms() - impl_->last_push_steady_ms > 500)) {
        return false;
    }
    return true;
}

struct AudioPreprocessor::Impl {
    AudioDspConfig config;
    SpeexEchoState* echo = nullptr;
    std::vector<SpeexPreprocessState*> preprocess;
    std::vector<int16_t> pending;
    size_t pending_frames = 0;
    std::vector<int16_t> output_pending;
    size_t output_frames = 0;
    std::vector<int16_t> far_end;
    std::vector<int16_t> processed;
    std::vector<int16_t> frame_out;
    std::vector<int16_t> channel;
    bool echo_was_active = false;
    bool reference_active = false;
    std::atomic<uint64_t> processed_frames{0};
    std::atomic<uint64_t> reference_underruns{0};
    std::atomic<uint64_t> resets{0};

    void destroy() {
        for (SpeexPreprocessState* state : preprocess) {
            if (state) {
                speex_preprocess_state_destroy(state);
            }
        }
        preprocess.clear();
        if (echo) {
            speex_echo_state_destroy(echo);
            echo = nullptr;
        }
    }

    void reset_filter() {
        if (echo) {
            speex_echo_state_reset(echo);
        }
        echo_was_active = false;
    }

    void process_frame(const int16_t* input, int16_t* output,
                       size_t newer_input_frames) {
        const size_t channels = config.near_channels;
        const size_t samples = kDspFrame * channels;
        const bool want_echo = echo != nullptr;
        const bool reference_live = want_echo && config.reference->active();

        if (want_echo && reference_live) {
            // The reference is read at "now"; this frame's last sample is
            // `newer_input_frames` older than the block that just arrived.
            // Adding that offset keeps the effective echo delay constant when
            // capture blocks are not whole DSP frames.
            const size_t delay =
                static_cast<size_t>(config.reference_delay_ms) * kDspRate / 1000u +
                newer_input_frames;
            if (!config.reference->read_delayed_frames(kDspFrame, delay,
                                                       far_end.data())) {
                reference_underruns.fetch_add(1);
            }
            speex_echo_cancellation(echo, input, far_end.data(), processed.data());
            echo_was_active = true;
        } else {
            if (want_echo && echo_was_active) {
                // Without a live reference the filter would train on silence
                // and then have to reconverge when playback returns.
                reset_filter();
                resets.fetch_add(1);
            }
            std::memcpy(processed.data(), input, samples * sizeof(int16_t));
        }

        if (!preprocess.empty()) {
            for (size_t ch = 0; ch < channels; ++ch) {
                for (size_t frame = 0; frame < kDspFrame; ++frame) {
                    channel[frame] = processed[frame * channels + ch];
                }
                speex_preprocess_run(preprocess[ch], channel.data());
                for (size_t frame = 0; frame < kDspFrame; ++frame) {
                    processed[frame * channels + ch] = channel[frame];
                }
            }
        }
        std::memcpy(output, processed.data(), samples * sizeof(int16_t));
    }
};

AudioPreprocessor::AudioPreprocessor() : impl_(std::make_unique<Impl>()) {}

AudioPreprocessor::~AudioPreprocessor() {
    if (impl_) {
        impl_->destroy();
    }
}

std::unique_ptr<AudioPreprocessor> AudioPreprocessor::create(const AudioDspConfig& config,
                                                             std::string* error) {
    if (config.near_channels < 1 || config.near_channels > 2) {
        if (error) {
            *error = "audio DSP needs one or two near-end channels";
        }
        return nullptr;
    }
    if (config.echo_cancellation && !config.reference) {
        if (error) {
            *error = "echo cancellation needs a playback reference";
        }
        return nullptr;
    }
    if (config.reference_delay_ms > 2000) {
        if (error) {
            *error = "the playback reference delay is out of range";
        }
        return nullptr;
    }

    std::unique_ptr<AudioPreprocessor> dsp(new AudioPreprocessor());
    Impl& impl = *dsp->impl_;
    impl.config = config;
    impl.reference_active = config.reference && config.reference->active();
    impl.far_end.resize(kDspFrame * kReferenceChannels);
    impl.processed.resize(kDspFrame * config.near_channels);
    impl.frame_out.resize(kDspFrame * config.near_channels);
    impl.channel.resize(kDspFrame);

    if (config.echo_cancellation) {
        impl.echo = speex_echo_state_init_mc(static_cast<int>(kDspFrame),
                                             static_cast<int>(kDspFilter),
                                             config.near_channels, kReferenceChannels);
        if (!impl.echo) {
            if (error) {
                *error = "speex echo canceller initialization failed";
            }
            return nullptr;
        }
        int rate = static_cast<int>(kDspRate);
        speex_echo_ctl(impl.echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    }

    // The preprocessor is the noise suppressor. Residual echo suppression is
    // tempting to attach even with denoise off, but it gates the near end by
    // the far-end energy and audibly eats double talk; the canceller alone
    // leaves the talker intact when the user only asked for AEC.
    if (config.noise_suppression) {
        impl.preprocess.reserve(config.near_channels);
        int denoise = config.noise_suppression ? 1 : 0;
        int agc = 0;
        int vad = 0;
        for (uint16_t ch = 0; ch < config.near_channels; ++ch) {
            SpeexPreprocessState* state =
                speex_preprocess_state_init(static_cast<int>(kDspFrame),
                                            static_cast<int>(kDspRate));
            if (!state) {
                if (error) {
                    *error = "speex preprocessor initialization failed";
                }
                return nullptr;
            }
            impl.preprocess.push_back(state);
            speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
            speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_AGC, &agc);
            speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_VAD, &vad);
            if (impl.echo) {
                speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_ECHO_STATE, impl.echo);
            }
        }
    }
    return dsp;
}

void AudioPreprocessor::process(int16_t* samples, size_t frames) {
    if (!samples || frames == 0) {
        return;
    }
    Impl& impl = *impl_;
    const size_t channels = impl.config.near_channels;
    const bool aec_live =
        impl.echo && impl.config.reference && impl.config.reference->active();
    if (impl.echo) {
        // Playback started or stopped between calls. The held partial frame
        // belongs to the old state and would splice unrelated audio into the
        // next one, so both transitions drop it and restart the filter.
        if (aec_live != impl.reference_active) {
            impl.reference_active = aec_live;
            impl.pending_frames = 0;
            impl.pending.clear();
            impl.output_frames = 0;
            impl.output_pending.clear();
            speex_echo_state_reset(impl.echo);
            impl.echo_was_active = aec_live;
            impl.resets.fetch_add(1);
        }
    }
    if (!aec_live && impl.preprocess.empty()) {
        // Nothing to do: the input buffer is already the output. Bypassing
        // here (instead of copying through the frame buffers) keeps an
        // inactive reference sample-exact for any block size.
        impl.echo_was_active = false;
        return;
    }

    const size_t old_frames = impl.pending_frames;
    impl.pending.resize((old_frames + frames) * channels);
    std::memcpy(impl.pending.data() + old_frames * channels, samples,
                frames * channels * sizeof(int16_t));
    impl.pending_frames += frames;

    size_t consumed = 0;
    while (impl.pending_frames - consumed >= kDspFrame) {
        const size_t newer_input_frames = impl.pending_frames - consumed - kDspFrame;
        impl.process_frame(impl.pending.data() + consumed * channels,
                           impl.frame_out.data(), newer_input_frames);
        impl.output_pending.insert(impl.output_pending.end(), impl.frame_out.begin(),
                                   impl.frame_out.end());
        consumed += kDspFrame;
        impl.processed_frames.fetch_add(kDspFrame);
    }
    if (consumed > 0) {
        const size_t left = impl.pending_frames - consumed;
        if (left > 0) {
            std::memmove(impl.pending.data(), impl.pending.data() + consumed * channels,
                         left * channels * sizeof(int16_t));
        }
        impl.pending.resize(left * channels);
        impl.pending_frames = left;
    }

    // The frame buffers introduce at most one DSP frame of startup latency.
    // Emitting exactly the requested count keeps every caller's buffer safe;
    // the short zero lead at the very first call is silence, not lost audio.
    const size_t available =
        impl.output_pending.size() / channels - impl.output_frames;
    const size_t emit = (std::min)(frames, available);
    if (emit > 0) {
        std::memcpy(samples,
                    impl.output_pending.data() + impl.output_frames * channels,
                    emit * channels * sizeof(int16_t));
        impl.output_frames += emit;
    }
    if (emit < frames) {
        std::memset(samples + emit * channels, 0,
                    (frames - emit) * channels * sizeof(int16_t));
    }
    if (impl.output_frames == impl.output_pending.size() / channels) {
        impl.output_pending.clear();
        impl.output_frames = 0;
    }
}

void AudioPreprocessor::reset() {
    Impl& impl = *impl_;
    impl.pending_frames = 0;
    impl.pending.clear();
    impl.output_frames = 0;
    impl.output_pending.clear();
    impl.reset_filter();
    impl.reference_active = impl.config.reference && impl.config.reference->active();
    impl.resets.fetch_add(1);
}

AudioDspStats AudioPreprocessor::stats() const {
    AudioDspStats stats;
    stats.processed_frames = impl_->processed_frames.load();
    stats.reference_underruns = impl_->reference_underruns.load();
    stats.resets = impl_->resets.load();
    return stats;
}

}  // namespace chast
