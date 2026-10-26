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
// The system-sound player schedules against the host's steady clock. Each
// packet carries the host capture time, and LatencyReply supplies the offset
// between the two monotonic clocks; a block is handed to the endpoint
// `lead_ms` after it was captured. That lead is the jitter buffer: large enough
// to hide network jitter, small enough that lip sync stays within a few tens of
// milliseconds. The microphone uplink has no shared clock, so its host-side
// player anchors each burst at arrival instead.
//
// Both directions use the device's own mix format and libswresample for the
// format and channel conversion, so a 44.1 kHz endpoint, a mono headset or a
// 5.1 device all work without a fixed intermediate rate. There is no CPU
// resampling loop in this project's own code. Opus itself is the standalone
// vcpkg library (Opus::opus), never FFmpeg's wrapper; AAC-LC remains available
// for older recordings and the codec tests.

#include <cstddef>
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
    uint32_t bitrate = 160000;  // bits per second
    // Opus only: OPUS_APPLICATION_VOIP instead of OPUS_APPLICATION_AUDIO.
    // Voice disables music tuning and keeps the bitrate spent on speech.
    bool voice = false;
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
// 2, and one packet is always 20 ms. The returned AudioInfo carries a 19-byte
// OpusHead whose pre-skip comes from OPUS_GET_LOOKAHEAD. This is the only
// encoder the product creates: system sound and microphone both use it.
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
// cancels its echo. Both directions need the same thing: the PCM that is (or
// is about to be) audible, kept long enough that a fixed read delay can line
// it up with the microphone. The format is fixed at 48 kHz stereo s16 because
// both capture paths already run at 48 kHz. All methods are thread-safe; push
// and read are called from different threads in practice.
class AudioReference {
public:
    AudioReference();
    ~AudioReference();
    AudioReference(const AudioReference&) = delete;
    AudioReference& operator=(const AudioReference&) = delete;

    // Appends playback PCM. `channels` is 1 or 2; mono is duplicated to both
    // sides. Ignored while inactive, so a paused player cannot seed stale
    // history for the next one.
    void push(const int16_t* samples, size_t frames, uint16_t channels);

    // Writes `frames` stereo samples ending `delay_ms` before the newest push.
    // Returns false (and zero-fills the missing part) while the reference is
    // inactive or shorter than the requested span; callers use that as an
    // underrun signal, not an error. The sample-accurate variant exists
    // because the DSP adds a partial-frame delay of its own.
    bool read_delayed(size_t frames, uint32_t delay_ms, int16_t* out) const;
    bool read_delayed_frames(size_t frames, size_t delay_frames, int16_t* out) const;

    // Opening/clearing the reference. Any transition drops the history: after
    // a pause the old samples no longer describe what is audible. The echo
    // canceller resets itself when this flips to false.
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
    uint64_t dropped = 0;          // packets dropped because the queue was full
    uint64_t silence_frames = 0;   // device frames detected as silence
    uint64_t capture_errors = 0;   // terminal read/encode failures
    uint64_t gate_transitions = 0; // silence gate opened or closed
    uint64_t gated_frames = 0;     // silent device frames skipped by the gate
    uint64_t recoveries = 0;       // reopened on a default/output-device change
    uint64_t processed_frames = 0; // near-end frames through the DSP
    uint64_t reference_underruns = 0;
    uint64_t dsp_resets = 0;
    bool recovering = false;       // endpoint lost, waiting for a new default
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
    // Viewer-side echo cancellation and noise suppression. The reference is
    // the system audio this viewer is playing; both switches are independent.
    std::shared_ptr<AudioReference> reference;
    bool echo_cancellation = false;
    bool noise_suppression = false;
};

// Captures one microphone on its own thread, converts it to 48 kHz mono s16
// and encodes 20 ms Opus packets. Unlike loopback there is no silence gate:
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

struct AudioPlayerConfig {
    // Empty follows the default render endpoint. A non-empty id pins that
    // device; when it disappears the player keeps retrying the same id and
    // reports `recovering` instead of silently moving to another endpoint.
    std::string device_id;
    // How far behind the sender timeline playback runs. This is the jitter
    // buffer; 80 ms covers a wide range of network conditions without a
    // noticeable lip-sync error.
    uint32_t lead_ms = 80;
    // Queued audio beyond this is dropped oldest-first, which keeps a network
    // burst from building an ever-growing delay.
    uint32_t queue_ms = 500;
    // Microphone uplink: packets carry the viewer's clock, which the host
    // cannot correlate with its own. The first block anchors the offset from
    // its arrival time, and a burst after a stall re-anchors instead of
    // replaying the stale timeline. System audio keeps this off and uses
    // LatencyReply.
    bool arrival_anchored = false;
};

struct AudioPlayerStats {
    uint64_t packets = 0;        // decoded blocks accepted
    uint64_t played_frames = 0;  // frames handed to the render endpoint
    uint64_t underruns = 0;      // times playback ran dry after audio had started
    uint64_t dropped = 0;        // stale or over-capacity blocks discarded
    uint64_t recoveries = 0;     // render endpoint reopened after a device change
    uint32_t buffer_ms = 0;      // queued audio plus endpoint padding
    uint32_t lead_ms = 0;
    bool recovering = false;     // endpoint lost, retrying the default device
};

class IAudioPlayer {
public:
    virtual ~IAudioPlayer() = default;

    virtual Error start() = 0;
    virtual void stop() = 0;

    // Queues one decoded block. The block's capture_ms places it on the host
    // timeline; the clock offset turns that into a local render time. Blocks
    // that are already older than one block are dropped rather than played
    // late, which would drag the timeline behind for the rest of the session.
    virtual Error push(const DecodedAudio& audio) = 0;

    // Host-minus-viewer steady-clock offset in milliseconds, from
    // LatencyReply. Updating it re-times everything still queued. Before the
    // first update the first pushed block anchors the offset, so playback
    // starts one lead behind its arrival.
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
