#pragma once

// System-sound path: WASAPI loopback on the host, Opus on the wire, WASAPI
// render on the viewer. The viewer's microphone takes the opposite route
// through a CMIC stream to the host's default speakers.
//
// Audio is the one media path that cannot stay on the GPU: the endpoint hands
// over PCM and Opus consumes PCM. The threads and the timeline are therefore
// the design surface. Capture and encode run on a dedicated thread at the
// device's pace, so the video loop's 30 fps cadence never stretches an audio
// frame; encoded packets land in a bounded queue that the host drains while it
// is between video frames. On the viewer the same split exists: decode on the
// packet thread, schedule and render on the audio thread.
//
// The viewer's system-sound player plays decoded audio as it arrives: there is
// no fixed lead and no A/V scheduling, so audio cannot trail the video by a
// jitter buffer it does not need. Each burst anchors at its arrival, and the
// capture stamps only space the blocks inside that burst; blocks that miss
// their slot are dropped, and the queue cap bounds the delay after a stall.
// With `adaptive_jitter` a windowed controller adds a bounded lead only while
// arrivals miss their slots, so a degraded link trades some latency for
// continuity while a healthy one keeps the zero-lead relationship to the
// video and gives any learned lead back. The microphone uplink's host-side
// player anchors on arrival the same way, with the default 80 ms lead as its
// jitter buffer. A caller that does know the host clock can leave
// `arrival_anchored` off and rely on LatencyReply.
//
// Both directions use the device's own mix format and libswresample for the
// format and channel conversion, so a 44.1 kHz endpoint, a mono headset or a
// 5.1 device all work without a fixed intermediate rate. There is no CPU
// resampling loop in this project's own code. Opus itself is the standalone
// vcpkg library (Opus::opus), never FFmpeg's wrapper; AAC-LC remains available
// for older recordings and the codec tests.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chast/error.h"
#include "chast/protocol.h"

namespace chast {

// ----- audio codec -----

struct AudioEncoderConfig {
    AudioCodec codec = AudioCodec::Opus;
    uint32_t sample_rate = 48000;
    uint16_t channels = 2;
    uint32_t bitrate = 160000;  // bits per second, set explicitly on the encoder
};

// One encoded access unit plus the steady-clock time of its first sample. The
// session layer turns capture_ms into both AudioHeader timestamps; everything
// below it stays on the local clock.
struct EncodedAudio {
    std::vector<uint8_t> data;
    uint64_t capture_ms = 0;
};

class IAudioEncoder {
public:
    virtual ~IAudioEncoder() = default;

    // `samples` holds `frames` interleaved s16 frames (channels * frames
    // values). Input is accumulated internally until a full codec frame is
    // available, so callers may pass any block size. `capture_ms` is the
    // steady-clock time of the first sample in this call.
    //
    // A gap of more than 100 ms since the previous call re-anchors the
    // timeline: the partial frame is padded with silence and emitted, and the
    // new samples start at `capture_ms`. Loopback delivers no packets while
    // the endpoint is idle, and without re-anchoring that idle time would turn
    // into a growing delay on the receiver.
    virtual Error encode(const int16_t* samples, size_t frames, uint64_t capture_ms,
                         std::vector<EncodedAudio>& out) = 0;

    // Pads and emits the final partial frame. Safe to call once at the end of
    // a session; later encode() calls are invalid.
    virtual Error flush(std::vector<EncodedAudio>& out) = 0;

    virtual uint32_t sample_rate() const noexcept = 0;
    virtual uint16_t channels() const noexcept = 0;
    virtual uint16_t frame_samples() const noexcept = 0;
    virtual uint32_t bitrate() const noexcept = 0;
    // OpusHead, as announced in AudioInfo.
    virtual const std::vector<uint8_t>& extradata() const noexcept = 0;
};

// Opens the standalone libopus encoder. Only 48 kHz is accepted, channels 1 or
// 2, and one packet is always 10 ms in the restricted low-delay application,
// matching RustDesk. The returned AudioInfo carries a 19-byte OpusHead whose
// pre-skip comes from OPUS_GET_LOOKAHEAD. This is the only encoder the product
// creates: system sound and microphone both use it.
std::unique_ptr<IAudioEncoder> create_opus_encoder(const AudioEncoderConfig& config,
                                                   std::string* error);

// Dispatches on config.codec. AAC encoding is intentionally unavailable; the
// enum stays on the wire so older AudioInfo values still decode.
std::unique_ptr<IAudioEncoder> create_audio_encoder(const AudioEncoderConfig& config,
                                                    std::string* error);

// One decoded block of PCM, interleaved s16. `pts_ms` and `capture_ms` are
// passed through from the AudioHeader so the player can schedule on the
// sender's timeline without a second side channel.
struct DecodedAudio {
    std::vector<int16_t> samples;
    uint64_t pts_ms = 0;
    uint64_t capture_ms = 0;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
};

class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    // Decodes one raw access unit. The decoder is primed from
    // AudioInfo::extradata, so packets from a session that joined mid-stream
    // can be fed as soon as they arrive. Encoder priming (AAC) and pre-skip
    // (Opus) are removed from the first output.
    virtual Error decode(const AudioHeader& header, const uint8_t* data, size_t size,
                         std::vector<DecodedAudio>& out) = 0;
    // Synthesizes one frame lost from an unreliable audio path. `next` is
    // the packet that revealed the gap; `frames_back` counts from it (1 is
    // the packet immediately before `next`). Opus uses its built-in
    // concealment; decoders without it return Unsupported. The receiver
    // calls this in order for a bounded gap before decoding `next`, so the
    // returned PCM carries an estimated capture time.
    virtual Error conceal(const AudioHeader& next, uint32_t frames_back,
                          std::vector<DecodedAudio>& out) = 0;
    virtual Error flush(std::vector<DecodedAudio>& out) = 0;

    virtual uint32_t sample_rate() const noexcept = 0;
    virtual uint16_t channels() const noexcept = 0;
};

std::unique_ptr<IAudioDecoder> create_aac_decoder(const AudioInfo& info, std::string* error);
std::unique_ptr<IAudioDecoder> create_opus_decoder(const AudioInfo& info, std::string* error);

// Dispatches on info.codec.
std::unique_ptr<IAudioDecoder> create_audio_decoder(const AudioInfo& info, std::string* error);

// ----- audio DSP (echo cancellation and noise suppression) -----

// Playback reference shared by the side that renders audio and the side that
// cancels its echo. Both directions need the same thing: the PCM that is
// audible, kept long enough that a fixed read delay can line it up with the
// microphone. The format is fixed at 48 kHz stereo s16 because both capture
// paths already run at 48 kHz. All methods are thread-safe; push and read are
// called from different threads in practice.
//
// A reference can be shared by several players (the host plays every viewer's
// microphone through the same speakers). Producers call acquire() before the
// first push and release() when their player stops; the last release clears
// the history, so one viewer closing its microphone no longer blinds the AEC
// of another. Timed pushes are mixed on a common steady-clock timeline, which
// is what makes two concurrent players cancel correctly instead of appending
// one stream behind the other.
class AudioReference {
public:
    AudioReference();
    ~AudioReference();
    AudioReference(const AudioReference&) = delete;
    AudioReference& operator=(const AudioReference&) = delete;

    // Producer lifetime for a shared reference. acquire() activates and keeps
    // the history while at least one producer is attached. release() clears it
    // only when the last producer leaves. Balanced calls are required.
    void acquire();
    void release();

    // Appends playback PCM to the untimed stream. `channels` is 1 or 2; mono
    // is duplicated to both sides. Ignored while inactive, so a paused player
    // cannot seed stale history for the next one. Callers that know when the
    // PCM becomes audible should use push_rendered() instead.
    void push(const int16_t* samples, size_t frames, uint16_t channels);

    // Appends PCM that becomes audible at `audible_start_ms` on the steady
    // clock. Concurrent producers are mixed sample-by-sample (saturating) at
    // that position. A player calls this from its render thread after the
    // endpoint accepted a block, so blocks discarded during a rebuild, the
    // startup gather or queue overflow never enter the reference.
    void push_rendered(const int16_t* samples, size_t frames, uint16_t channels,
                       int64_t audible_start_ms);

    // Writes `frames` stereo samples ending `delay_ms` before the current
    // playhead (timed mode) or the newest push (untimed mode). Returns false
    // (and zero-fills the missing part) while the reference is inactive,
    // stalled or shorter than the requested span; callers use that as an
    // underrun signal, not an error. The sample-accurate variant exists
    // because the DSP adds a partial-frame delay of its own.
    bool read_delayed(size_t frames, uint32_t delay_ms, int16_t* out) const;
    bool read_delayed_frames(size_t frames, size_t delay_frames, int16_t* out) const;

    // Single-owner activation, kept for the DSP tests and simple callers.
    // Acquire/release is the lifetime mechanism for shared players; a legacy
    // deactivation does not clear history while producers remain attached.
    // Any transition drops the history: after a pause the old samples no
    // longer describe what is audible. The echo canceller resets itself when
    // this flips to false. Timed history also reports inactive after 500 ms
    // without a push, so a stalled stream cannot keep training the filter.
    void set_active(bool active);
    bool active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct AudioDspConfig {
    // Near-end channels: microphone upling is mono, loopback is stereo.
    uint16_t near_channels = 1;
    // Echo cancellation needs a reference. Enabling it without one makes
    // create() fail instead of silently passing audio through.
    bool echo_cancellation = false;
    bool noise_suppression = false;
    // How far back the reference is read, and the adaptive filter length
    // (200 ms, fixed in the implementation). The default lines the reference
    // up with the player's lead and the endpoint buffer.
    uint32_t reference_delay_ms = 120;
    std::shared_ptr<AudioReference> reference;
};

struct AudioDspStats {
    uint64_t processed_frames = 0;
    uint64_t reference_underruns = 0;  // 10 ms blocks without a full reference
    uint64_t resets = 0;               // filter resets after reference loss
};

// 48 kHz s16 echo cancellation and noise suppression on 10 ms frames. Input
// may be any size: short blocks are accumulated and the remainder is carried
// to the next call. Processing is in place and never fails once created; when
// the reference is inactive the echo path is bypassed (and reset), while noise
// suppression, if enabled, keeps working.
class AudioPreprocessor {
public:
    static std::unique_ptr<AudioPreprocessor> create(const AudioDspConfig& config,
                                                     std::string* error);
    ~AudioPreprocessor();
    AudioPreprocessor(const AudioPreprocessor&) = delete;
    AudioPreprocessor& operator=(const AudioPreprocessor&) = delete;

    // `samples` holds `frames` interleaved near-end frames. `in` and `out` may
    // be the same buffer. The number of samples is unchanged; a trailing
    // partial frame is held until the next call.
    void process(int16_t* samples, size_t frames);

    // Drops the partial frame and resets the adaptive filter. Used when the
    // capture endpoint is reopened.
    void reset();

    AudioDspStats stats() const;

private:
    AudioPreprocessor();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ----- host capture -----

struct LoopbackCaptureConfig {
    uint32_t bitrate = 128000;
    // Encoded packets the queue may hold before the oldest is dropped. The
    // host drains once per video frame, so a few hundred milliseconds covers a
    // stalled capture/encode loop without turning into unbounded latency.
    size_t queue_packets = 32;
    // After this much digital silence the encoder stops receiving samples, so
    // an idle endpoint costs no Opus bitrate. Zero disables the gate. The
    // timeline is unaffected: the next audible block re-anchors the encoder at
    // its capture time.
    uint32_t silence_gate_ms = 2000;
    // Empty follows the default render endpoint; a non-empty id pins that
    // device and keeps retrying it if it disappears.
    std::string device_id;
    // Echo cancellation against the session's microphone playback. The
    // reference must be fed with the PCM the host renders to the same
    // endpoint; without it the capture passes audio through untouched.
    std::shared_ptr<AudioReference> reference;
    bool echo_cancellation = false;
    // Reserved for symmetry with the microphone path; loopback noise
    // suppression is off by default because it would also shape music.
    bool noise_suppression = false;
};

struct AudioCaptureStats {
    uint64_t packets = 0;          // packets queued for the session
    uint64_t dropped = 0;          // packets dropped because a queue was full
    uint64_t stale_dropped = 0;    // packets discarded after waiting past the
                                   // shared queue's age budget
    uint64_t silence_frames = 0;   // device frames detected as silence
    uint64_t capture_errors = 0;   // terminal read/encode failures
    uint64_t gate_transitions = 0; // silence gate opened or closed
    uint64_t gated_frames = 0;     // silent device frames skipped by the gate
    uint64_t recoveries = 0;       // reopened on a default/output-device change
    uint64_t processed_frames = 0; // near-end frames through the DSP
    uint64_t reference_underruns = 0;
    uint64_t dsp_resets = 0;
    bool recovering = false;       // endpoint lost, waiting for a new default
    // Echo cancellation / noise suppression requested by the settings and
    // whether the DSP object could actually be created. A missing SpeexDSP
    // build must be visible here instead of silently sending raw PCM.
    bool dsp_requested = false;
    bool dsp_active = false;
    std::string dsp_error;
};

// Captures the default render endpoint's loopback mix on its own thread,
// converts it to stereo s16 at 48 kHz and encodes it to Opus. The encoded
// format is fixed for the session, so a device change can resume on a different
// endpoint without renegotiating AudioInfo.
//
// The device is opened and the encoder created in create() (the startup
// handshake is synchronous, so a machine without a usable endpoint is known
// before the stream is announced), but capture does not start until start().
class ILoopbackCapture {
public:
    virtual ~ILoopbackCapture() = default;

    virtual void start() = 0;
    // Stops the capture thread and releases the endpoint. Safe to call twice.
    virtual void stop() = 0;

    // Moves the oldest encoded packet into `packet`. False when the queue is
    // empty. Call from one thread (the host loop).
    virtual bool take_packet(EncodedAudio& packet) = 0;

    // True once capture stopped on a device or encoder failure; error() then
    // says what happened. The host turns audio off for the rest of the session.
    virtual bool failed() const = 0;
    virtual std::string error() const = 0;

    // The format actually opened, including the AAC decoder config. `enabled`
    // is true when capture is available; callers announce this after a
    // successful create().
    virtual AudioInfo info() const = 0;
    virtual AudioCaptureStats stats() const = 0;
};

std::unique_ptr<ILoopbackCapture> create_loopback_capture(const LoopbackCaptureConfig& config,
                                                          std::string* error);

// ----- microphone capture (viewer) -----

struct MicrophoneDevice {
    // The WASAPI endpoint id, as UTF-8. Pass it back to select the device.
    std::string id;
    std::string name;
    bool is_default = false;
};

// Active capture endpoints, default communications device first. Returns an
// empty list (with `error`) when COM or the enumerator is unavailable.
std::vector<MicrophoneDevice> list_microphones(std::string* error);

struct MicrophoneCaptureConfig {
    // Empty selects the default communications capture device and follows it
    // if the user changes it. A non-empty id pins that device; if it
    // disappears, capture keeps retrying it and reports `recovering`.
    std::string device_id;
    uint32_t bitrate = 32000;
    size_t queue_packets = 16;
    // Host-side echo cancellation and noise suppression for the uplink. The
    // host plays every viewer's microphone through its speakers, so the
    // reference is host-wide and may have several producers; both switches
    // are independent.
    std::shared_ptr<AudioReference> reference;
    bool echo_cancellation = false;
    bool noise_suppression = false;
};

// Captures one microphone on its own thread, converts it to 48 kHz mono s16
// and encodes 10 ms Opus packets. Unlike loopback there is no silence gate:
// the viewer's explicit on/off switch is the only privacy boundary, and a
// paused stream would look like a device failure to the host.
class IMicrophoneCapture {
public:
    virtual ~IMicrophoneCapture() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool take_packet(EncodedAudio& packet) = 0;

    virtual bool failed() const = 0;
    virtual std::string error() const = 0;
    virtual AudioInfo info() const = 0;
    virtual AudioCaptureStats stats() const = 0;
};

// Opens the selected capture endpoint and creates the Opus encoder in
// create(); returns nullptr (with `error`) when this machine cannot record.
std::unique_ptr<IMicrophoneCapture> create_microphone_capture(
    const MicrophoneCaptureConfig& config, std::string* error);

// ----- viewer playback -----

// Active render endpoints, default console device first. Returns an empty
// list (with `error`) when COM or the enumerator is unavailable.
struct AudioOutputDevice {
    std::string id;
    std::string name;
    bool is_default = false;
};

std::vector<AudioOutputDevice> list_audio_outputs(std::string* error);

// Adaptive playout lead for arrival-anchored audio. The viewer's system
// sound starts with no fixed lead so it cannot trail the video on a healthy
// link. This controller adds lead only after a window of arrivals shows
// blocks missing their slots (negative slack), and gives it back once
// arrivals are consistently early again. Observations are grouped into
// windows, so one late or early block cannot move the lead back and forth and
// at most one change happens per window. Startup and stall re-anchors are
// excluded: their first blocks reflect queue fill and recovery bursts rather
// than link jitter, and would otherwise pin a short session at a high lead.
class JitterLeadController {
public:
    struct Config {
        int64_t max_lead_ms = 120;
        // A window raises the lead only when at least `raise_min_blocks`
        // blocks land at or below `raise_at_ms` slack. A single reordered
        // packet is left to concealment instead of delaying the session.
        int64_t raise_at_ms = -20;
        int64_t raise_min_blocks = 2;
        int64_t raise_margin_ms = 10;
        int64_t raise_step_max_ms = 40;
        // Consecutive windows with every block more than `decay_above_ms`
        // early give the lead back, one step per window after the hold.
        int64_t decay_above_ms = 20;
        int64_t decay_hold_windows = 2;
        int64_t decay_step_ms = 20;
        int64_t window_ms = 500;
        // Windows that start before these deadlines never change the lead:
        // session start and the burst after a stall release are not link
        // conditions. Once the deadline passes, normal adaptation resumes.
        int64_t startup_grace_ms = 1000;
        int64_t release_grace_ms = 500;
    };

    JitterLeadController() = default;
    explicit JitterLeadController(const Config& config) : config_(config) {}

    // Observes (scheduled play time - arrival time) for one block and returns
    // the lead to schedule with from now on.
    int64_t observe(int64_t slack_ms, int64_t now_ms) {
        if (!window_open_) {
            grace_until_ms_ = now_ms + config_.startup_grace_ms;
            open(now_ms, slack_ms);
            return lead_ms_;
        }
        window_min_ms_ = (std::min)(window_min_ms_, slack_ms);
        if (slack_ms <= config_.raise_at_ms) {
            ++window_late_;
        }
        if (now_ms - window_start_ms_ < config_.window_ms) {
            return lead_ms_;
        }
        const bool settling = window_start_ms_ < grace_until_ms_;
        if (!settling && window_late_ >= config_.raise_min_blocks) {
            int64_t want = (-window_min_ms_) + config_.raise_margin_ms;
            want = (std::min)(want, config_.raise_step_max_ms);
            want = (std::min)(want, config_.max_lead_ms - lead_ms_);
            if (want > 0) {
                lead_ms_ += want;
                ++raises_;
            }
            early_windows_ = 0;
        } else if (!settling && window_min_ms_ > config_.decay_above_ms) {
            if (early_windows_ < config_.decay_hold_windows) {
                ++early_windows_;
            } else if (lead_ms_ > 0) {
                lead_ms_ = (std::max)(int64_t{0}, lead_ms_ - config_.decay_step_ms);
                ++decays_;
            }
        } else {
            early_windows_ = 0;
        }
        open(now_ms, slack_ms);
        return lead_ms_;
    }

    int64_t lead_ms() const { return lead_ms_; }
    uint64_t raises() const { return raises_; }
    uint64_t decays() const { return decays_; }

    // Drops the learned lead and starts a fresh window without forgetting
    // the session's raise/decay counters. The player calls this when a stall
    // re-anchors the timeline.
    void release(int64_t now_ms) {
        lead_ms_ = 0;
        early_windows_ = 0;
        grace_until_ms_ = (std::max)(grace_until_ms_, now_ms + config_.release_grace_ms);
        open(now_ms, 0);
    }

    void reset() {
        lead_ms_ = 0;
        raises_ = 0;
        decays_ = 0;
        window_open_ = false;
        window_start_ms_ = 0;
        window_min_ms_ = 0;
        window_late_ = 0;
        early_windows_ = 0;
        grace_until_ms_ = 0;
    }

private:
    void open(int64_t now_ms, int64_t slack_ms) {
        window_open_ = true;
        window_start_ms_ = now_ms;
        window_min_ms_ = slack_ms;
        window_late_ = slack_ms <= config_.raise_at_ms ? 1 : 0;
    }

    Config config_{};
    int64_t lead_ms_ = 0;
    int64_t window_start_ms_ = 0;
    int64_t window_min_ms_ = 0;
    int64_t window_late_ = 0;
    int64_t early_windows_ = 0;
    int64_t grace_until_ms_ = 0;
    uint64_t raises_ = 0;
    uint64_t decays_ = 0;
    bool window_open_ = false;
};

// Long-session drift for capture-anchored playout. The configured lead is
// intentional, so this does not chase an absolute phase: it learns the
// best-case arrival phase of the first window and returns later windows to
// it. The best case is the largest slack (smallest one-way delay), which a
// burst of queued blocks cannot drag down the way a window minimum can.
// A persistent departure means the local scheduling timeline is drifting
// against the host capture clock (or the path's base delay moved); without
// correction the lead slowly drains until blocks miss their slots (negative
// departure) or latency grows (positive departure). Steps change the
// host/local offset and are capped per window; a departure must repeat for
// `confirm_windows` windows so one jittery window cannot move the timeline.
// Callers should subtract any intentional lead from slack first, so a lead
// change is not mistaken for drift. The first closed window after a start or
// re-anchor is skipped: it can contain the arrival anchor's transient, which
// says nothing about the path. Only used with a live clock offset, never with
// the arrival anchor, and disabled by default.
class PlayoutDriftController {
public:
    struct Config {
        int64_t window_ms = 5000;
        int64_t confirm_windows = 2;
        int64_t dead_band_ms = 15;
        int64_t max_step_ms = 20;
        // Closed windows skipped before the reference is learned.
        int64_t warmup_windows = 1;
    };

    PlayoutDriftController() = default;
    explicit PlayoutDriftController(const Config& config) : config_(config) {}

    // Observes the lead-independent arrival phase (scheduled play time minus
    // arrival time, with the intentional lead removed) and returns the offset
    // step in ms to add to the host-minus-local offset: positive plays
    // earlier, negative later. At most one step per window.
    int64_t observe(int64_t phase_ms, int64_t now_ms) {
        if (!window_open_) {
            open(now_ms);
            add(phase_ms);
            return 0;
        }
        add(phase_ms);
        if (now_ms - window_start_ms_ < config_.window_ms) {
            return 0;
        }
        const int64_t window_best = window_best_ms_;
        // The closing block belongs to the window it helped close; the next
        // window starts empty so one sample cannot count twice.
        open(now_ms);
        if (!have_reference_) {
            if (windows_skipped_ < config_.warmup_windows) {
                ++windows_skipped_;
                pending_ = 0;
                return 0;
            }
            reference_ms_ = window_best;
            have_reference_ = true;
            pending_ = 0;
            return 0;
        }
        const int64_t deviation = window_best - reference_ms_;
        if (deviation >= -config_.dead_band_ms &&
            deviation <= config_.dead_band_ms) {
            pending_ = 0;
            return 0;
        }
        const int64_t sign = deviation > 0 ? 1 : -1;
        if (pending_ > 0 && sign == pending_sign_) {
            ++pending_;
        } else {
            pending_sign_ = sign;
            pending_ = 1;
        }
        if (pending_ < config_.confirm_windows) {
            return 0;
        }
        const int64_t step =
            deviation > 0 ? (std::min)(deviation, config_.max_step_ms)
                          : (std::max)(deviation, -config_.max_step_ms);
        correction_ms_ += step;
        ++corrections_;
        // Keep the confirmation while the departure persists, so a sustained
        // drift is corrected once per window instead of pausing to re-confirm.
        pending_ = config_.confirm_windows;
        return step;
    }

    // Cumulative offset applied since the timeline was learned. Positive
    // means playback was moved earlier; `corrections` counts the steps.
    int64_t correction_ms() const { return correction_ms_; }
    uint64_t corrections() const { return corrections_; }

    // Forgets the reference and the open window, including the accumulated
    // correction. Called when the timeline is rebuilt (endpoint reopen), so
    // the old reference cannot leak into the new one.
    void reset() {
        window_open_ = false;
        window_has_sample_ = false;
        window_start_ms_ = 0;
        window_best_ms_ = 0;
        reference_ms_ = 0;
        have_reference_ = false;
        windows_skipped_ = 0;
        pending_ = 0;
        pending_sign_ = 0;
        correction_ms_ = 0;
        corrections_ = 0;
    }

private:
    void open(int64_t now_ms) {
        window_open_ = true;
        window_has_sample_ = false;
        window_start_ms_ = now_ms;
        window_best_ms_ = 0;
    }

    void add(int64_t phase_ms) {
        if (!window_has_sample_) {
            window_best_ms_ = phase_ms;
            window_has_sample_ = true;
        } else {
            window_best_ms_ = (std::max)(window_best_ms_, phase_ms);
        }
    }

    Config config_{};
    int64_t window_start_ms_ = 0;
    int64_t window_best_ms_ = 0;
    int64_t reference_ms_ = 0;
    int64_t pending_ = 0;
    int64_t pending_sign_ = 0;
    int64_t correction_ms_ = 0;
    uint64_t corrections_ = 0;
    bool window_open_ = false;
    bool window_has_sample_ = false;
    bool have_reference_ = false;
    int64_t windows_skipped_ = 0;
};

struct AudioPlayerConfig {
    // Empty follows the default render endpoint. A non-empty id pins that
    // device; when it disappears the player keeps retrying the same id and
    // reports `recovering` instead of silently moving to another endpoint.
    std::string device_id;
    // How far behind the sender timeline playback runs. For a caller that
    // knows the host clock this is the jitter buffer; 80 ms covers typical
    // network jitter. The viewer's system audio sets this to 0 and anchors on
    // arrival instead, so it carries no fixed lead over the video.
    uint32_t lead_ms = 80;
    // Queued audio beyond this is dropped oldest-first, which keeps a network
    // burst from building an ever-growing delay.
    uint32_t queue_ms = 500;
    // Anchor each burst at its arrival instead of using a reported clock
    // offset. The microphone uplink needs this (no shared clock) and the
    // viewer's system audio uses it to play decoded blocks as they arrive.
    // Capture stamps still space blocks inside a burst; a burst after a stall
    // re-anchors instead of replaying the stale timeline. `set_clock_offset`
    // is ignored in this mode.
    bool arrival_anchored = false;
    // Collect this many blocks before setting the first arrival anchor, then
    // anchor on the fastest one plus a small safety margin. The gathered
    // blocks are skipped, so playback starts on the corrected timeline
    // instead of carrying the startup handshake delay for the whole session.
    // 0 anchors on the first block. Only used with `arrival_anchored`.
    uint32_t anchor_gather_blocks = 0;
    // Reports PCM that the endpoint accepted, after ReleaseBuffer succeeds and
    // only for actual playback. `audible_start_ms` is the steady-clock time
    // the first frame is scheduled to become audible. Blocks dropped by the
    // startup gather, queue overflow, an expired slot or an endpoint rebuild
    // never reach this callback, so echo cancellation cannot train on audio
    // that was never played. Empty when no reference is needed.
    std::function<void(const int16_t*, size_t, uint16_t, int64_t)> render_sink;
    // Grow the playout lead past `lead_ms` when arrivals miss their slots,
    // and return it after the link settles. Works on both timelines: with
    // `arrival_anchored` it starts from zero, and with a host capture offset
    // it adds up to its ceiling on top of `lead_ms`. The microphone uplink
    // keeps its fixed lead.
    bool adaptive_jitter = false;
    // Correct long-session clock drift on a capture-anchored timeline. The
    // controller learns the best-case arrival phase of the first window and
    // slowly moves the host/local offset when it departs, so the configured
    // lead stays intact instead of draining (late drift) or growing (early
    // drift). Needs a live clock offset from LatencyReply; ignored with
    // `arrival_anchored`. Experimental and off by default.
    bool clock_drift_correction = false;
};

struct AudioPlayerStats {
    uint64_t packets = 0;        // decoded blocks accepted
    uint64_t played_frames = 0;  // frames handed to the render endpoint
    uint64_t underruns = 0;      // times playback ran dry after audio had started
    // Total scheduled silence between consecutive written blocks. Underruns
    // count events past a 30 ms threshold and stay blind to streams that keep
    // dropping blocks; this is the actual dry time on the playout timeline,
    // and the metric to compare playout strategies with.
    uint64_t starved_ms = 0;
    uint64_t dropped = 0;        // stale or over-capacity blocks discarded
    uint64_t recoveries = 0;     // render endpoint reopened after a device change
    uint32_t buffer_ms = 0;      // queued audio plus endpoint padding
    uint32_t lead_ms = 0;
    // Extra lead the adaptive controller added on top of `lead_ms`, and how
    // often it raised or returned part of it.
    uint32_t jitter_ms = 0;
    uint64_t jitter_raises = 0;
    uint64_t jitter_decays = 0;
    // Cumulative offset the drift controller added to the host/local clock
    // mapping (positive moved playback earlier) and how many steps it took.
    int64_t drift_ms = 0;
    uint64_t drift_corrections = 0;
    bool recovering = false;     // endpoint lost, retrying the default device
    // The most recently rendered block's host capture stamp and the local
    // steady-clock time it was scheduled to play. Both stay 0 until a block
    // reaches the endpoint and are cleared when the endpoint is rebuilt.
    // A viewer holding a LatencyReply offset turns the pair into the audio
    // playout latency it can compare with frame age.
    uint64_t playout_capture_ms = 0;
    int64_t playout_scheduled_ms = 0;
    // Range of (scheduled local play time - arrival local time) over the
    // blocks queued since the last anchor or startup gather. A consistently
    // positive average means the anchor implies more delay than the blocks
    // need; a negative minimum means the anchor was optimistic and blocks
    // risk being dropped.
    int64_t slack_min_ms = 0;
    int64_t slack_max_ms = 0;
    double slack_avg_ms = 0.0;
    uint64_t slack_early = 0;  // blocks arriving more than 20 ms early
    uint64_t slack_late = 0;   // blocks arriving more than 20 ms late
    // How far the startup anchor gather moved playback earlier than the
    // first-block anchor would have. Positive means the gather removed that
    // much avoidable delay; 0 means it did not run or found no profit.
    int64_t anchor_shift_ms = 0;
};

// Playout latency of an audio block in the host's clock: the local scheduled
// time adjusted by the host-minus-local offset from LatencyReply, minus the
// block's capture stamp. Returns a negative value when the pair is unset or
// the result is implausible (a stale offset or a replay), so callers can
// report "not measured" instead of a wild number.
inline double audio_playout_latency_ms(uint64_t capture_ms, int64_t scheduled_ms,
                                       int64_t host_minus_local_us) {
    if (capture_ms == 0 || scheduled_ms == 0) {
        return -1.0;
    }
    const int64_t latency_us = scheduled_ms * 1000 + host_minus_local_us -
                               static_cast<int64_t>(capture_ms) * 1000;
    if (latency_us <= -100000 || latency_us >= 10000000) {
        return -1.0;
    }
    return static_cast<double>(latency_us > 0 ? latency_us : 0) / 1000.0;
}

class IAudioPlayer {
public:
    virtual ~IAudioPlayer() = default;

    virtual Error start() = 0;
    virtual void stop() = 0;

    // Queues one decoded block. The block's capture_ms places it on the host
    // timeline; the clock offset turns that into a local render time, or the
    // arrival anchor does when `arrival_anchored` is set. Blocks that are
    // already older than one block are dropped rather than played late,
    // which would drag the timeline behind for the rest of the session.
    virtual Error push(const DecodedAudio& audio) = 0;

    // Host-minus-viewer steady-clock offset in milliseconds, from
    // LatencyReply. Updating it re-times everything still queued. Before the
    // first update the first pushed block anchors the offset, so playback
    // starts one lead behind its arrival. Ignored when `arrival_anchored` is
    // set: that mode anchors each burst on arrival instead.
    virtual void set_clock_offset(int64_t host_minus_local_ms) = 0;
    virtual void set_lead_ms(uint32_t lead_ms) = 0;

    // Mutes the render session without pausing decode or the timeline.
    virtual void set_mute(bool mute) = 0;
    virtual bool muted() const = 0;

    virtual AudioPlayerStats stats() const = 0;
    // Empty while playback is healthy. Set when the endpoint was invalidated
    // or a render call failed; playback then stops.
    virtual std::string error() const = 0;
};

// Opens the default render endpoint in shared mode and prepares its thread.
// Returns nullptr (with `error`) when this machine cannot play audio; the
// viewer then simply keeps running without it. Once running, a lost endpoint
// or a default-device change is recovered in place: playback retries the new
// default every second and keeps error() empty while recovering.
std::unique_ptr<IAudioPlayer> create_audio_player(const AudioPlayerConfig& config,
                                                  std::string* error);

}  // namespace chast
