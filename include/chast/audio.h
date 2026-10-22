#pragma once

// System-sound path: WASAPI loopback on the host, AAC-LC on the wire, WASAPI
// render on the viewer.
//
// Audio is the one media path that cannot stay on the GPU: the endpoint hands
// over PCM and AAC consumes PCM. The threads and the timeline are therefore
// the design surface. Capture and encode run on a dedicated thread at the
// device's pace, so the video loop's 30 fps cadence never stretches an audio
// frame; encoded packets land in a bounded queue that the host drains while it
// is between video frames. On the viewer the same split exists: decode on the
// packet thread, schedule and render on the audio thread.
//
// The player schedules against the host's steady clock. Each packet carries
// the host capture time, and LatencyReply supplies the offset between the two
// monotonic clocks; a block is handed to the endpoint `lead_ms` after it was
// captured. That lead is the jitter buffer: large enough to hide network
// jitter, small enough that lip sync stays within a few tens of milliseconds.
//
// Both directions use the device's own mix format and libswresample for the
// format and channel conversion, so a 44.1 kHz endpoint, a mono headset or a
// 5.1 device all work without a fixed intermediate rate. There is no CPU
// resampling loop in this project's own code.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chast/error.h"
#include "chast/protocol.h"

namespace chast {

// ----- AAC codec -----

struct AudioEncoderConfig {
    uint32_t sample_rate = 48000;
    uint16_t channels = 2;
    uint32_t bitrate = 160000;  // bits per second
};

// One AAC access unit plus the host time of its first sample. The session
// layer turns capture_ms into both AudioHeader timestamps; everything below it
// stays on the steady clock.
struct EncodedAudio {
    std::vector<uint8_t> data;
    uint64_t capture_ms = 0;
};

class IAudioEncoder {
public:
    virtual ~IAudioEncoder() = default;

    // `samples` holds `frames` interleaved s16 frames (channels * frames
    // values). Input is accumulated internally until a full AAC frame is
    // available, so callers may pass any block size. `capture_ms` is the host
    // steady-clock time of the first sample in this call.
    //
    // A gap of more than 100 ms since the previous call re-anchors the
    // timeline: the partial frame is padded with silence and emitted, and the
    // new samples start at `capture_ms`. Loopback delivers no packets while
    // the endpoint is idle, and without re-anchoring that idle time would turn
    // into a growing delay on the viewer.
    virtual Error encode(const int16_t* samples, size_t frames, uint64_t capture_ms,
                         std::vector<EncodedAudio>& out) = 0;

    // Pads and emits the final partial frame. Safe to call once at the end of
    // a session; later encode() calls are invalid.
    virtual Error flush(std::vector<EncodedAudio>& out) = 0;

    virtual uint32_t sample_rate() const noexcept = 0;
    virtual uint16_t channels() const noexcept = 0;
    virtual uint16_t frame_samples() const noexcept = 0;
    virtual uint32_t bitrate() const noexcept = 0;
    // AAC AudioSpecificConfig (usually two bytes), as announced in AudioInfo.
    virtual const std::vector<uint8_t>& extradata() const noexcept = 0;
};

// Opens FFmpeg's native AAC-LC encoder. The rate must be one AAC supports
// (44.1 or 48 kHz in practice); channels 1 or 2.
std::unique_ptr<IAudioEncoder> create_aac_encoder(const AudioEncoderConfig& config,
                                                  std::string* error);

// One decoded block of PCM, interleaved s16. `pts_ms` and `capture_ms` are
// passed through from the AudioHeader so the player can schedule on the host
// timeline without a second side channel.
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

    // Decodes one raw AAC access unit. The decoder is primed from
    // AudioInfo::extradata, so packets from a session that joined mid-stream
    // can be fed as soon as they arrive. Encoder priming samples are removed
    // from the first output.
    virtual Error decode(const AudioHeader& header, const uint8_t* data, size_t size,
                         std::vector<DecodedAudio>& out) = 0;
    virtual Error flush(std::vector<DecodedAudio>& out) = 0;

    virtual uint32_t sample_rate() const noexcept = 0;
    virtual uint16_t channels() const noexcept = 0;
};

std::unique_ptr<IAudioDecoder> create_aac_decoder(const AudioInfo& info, std::string* error);

// ----- host capture -----

struct LoopbackCaptureConfig {
    uint32_t bitrate = 160000;
    // Encoded packets the queue may hold before the oldest is dropped. The
    // host drains once per video frame, so a few hundred milliseconds covers a
    // stalled capture/encode loop without turning into unbounded latency.
    size_t queue_packets = 32;
    // After this much digital silence the encoder stops receiving samples, so
    // an idle endpoint costs no AAC bitrate. Zero disables the gate. The
    // timeline is unaffected: the next audible block re-anchors the encoder at
    // its capture time.
    uint32_t silence_gate_ms = 2000;
};

struct AudioCaptureStats {
    uint64_t packets = 0;          // packets queued for the session
    uint64_t dropped = 0;          // packets dropped because the queue was full
    uint64_t silence_frames = 0;   // device frames detected as silence
    uint64_t capture_errors = 0;   // terminal read/encode failures
    uint64_t gate_transitions = 0; // silence gate opened or closed
    uint64_t gated_frames = 0;     // silent device frames skipped by the gate
    uint64_t recoveries = 0;       // reopened on a default/output-device change
    bool recovering = false;       // endpoint lost, waiting for a new default
};

// Captures the default render endpoint's loopback mix on its own thread,
// converts it to stereo s16 at 48 kHz and encodes it to AAC. The encoded
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

// ----- viewer playback -----

struct AudioPlayerConfig {
    // How far behind the host timeline playback runs. This is the jitter
    // buffer; 80 ms covers a wide range of network conditions without a
    // noticeable lip-sync error.
    uint32_t lead_ms = 80;
    // Queued audio beyond this is dropped oldest-first, which keeps a network
    // burst from building an ever-growing delay.
    uint32_t queue_ms = 500;
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
