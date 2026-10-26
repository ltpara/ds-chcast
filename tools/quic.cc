// QUIC transport harness: capture -> encode -> MsQuic -> decode.
//
//   chast-quic serve   --port 4433 --codec h264 --seconds 10 [--display N] [--save sent.h264] [--input] [--clipboard] [--audio] [--encoder auto|vpl|ffmpeg]
//   chast-quic connect --host 127.0.0.1 --port 4433 [--display N] [--save received.h264] [--input] [--clipboard] [--audio] [--play]
//   chast-quic verify  --port 45433 --seconds 5 [--display N] [--merged] [--input-real] [--display-mode]
//                      [--file-upload] [--file-download] [--microphone] [--privacy]
//                      [--sas] [--stream-options] [--encoder auto|vpl|ffmpeg]
//
// serve runs the sender: one client connects, the actual encoder codec and
// picture size are announced, and every access unit is written to a
// server-initiated unidirectional stream. connect runs the receiver: it
// reassembles the frames, checks the per-frame CRC32, decodes each access unit
// on the hardware VPL decoder, and reports counts plus a digest of the decoded
// picture content.
//
// verify runs both ends in one process and closes the loop: it replays the
// stream the server actually sent through chast-decode's access-unit splitter
// and a fresh decoder, and requires the receiver's frame count and content
// digest to match. That is the repeatable proof that the bytes crossing QUIC
// decoded into the same pictures the sender encoded. No display is involved.
//
// With --input the serve end accepts InputEvents and injects them, connect
// replays a scripted pointer/key sequence (for real: the pointer moves and a
// Shift+A is typed), and verify also closes that loop: the events the server
// received must equal the ones the client sent, in order, and every one must
// have been injected. verify injects in dry-run mode unless --input-real is
// given, so a normal test run never touches the real pointer; keys are left
// out of the script in that mode, because a test must not type into whatever
// window happens to be focused.
//
// Hello carries a display index: connect --display N asks serve to capture the
// Nth output of the announced list, and serve creates its capturer only after
// Hello arrives so the choice can be honoured. Without --display the client
// leaves the choice to the server, whose own --display is the fallback
// (primary when unset). verify always asks for display 0 and checks that the
// announced origin and picture size are that display's rect; verify --merged
// asks for the merged layout of every output instead.
//
// Access control: serve --token T requires Hello to carry T and spends it on
// the first accepted viewer unless --token-reuse is given; --token auto
// generates and prints one. --approve asks on stdin before letting a viewer
// in. serve --password P installs a permanent password and
// --auth-mode password|click|both picks the policy; --totp-secret B enables
// 2FA. connect --token/--password/--totp (or --totp-secret to compute the
// code) send those credentials, and a refusal prints the server's reason.
// verify exercises the refusal paths on throwaway servers.
//
// With --clipboard the serve end mirrors the clipboard and connect prints what
// the host sends. verify always covers this path with an in-memory clipboard
// shared by both ends: the viewer uploads one item carrying text, HTML, RTF
// and a DIB, the host writes all four, and only then does the emulated host
// clipboard change so the viewer must receive exactly one update. Exact counts
// prove neither side echoes the other's content back.
//
// verify also covers viewer file uploads when --file-upload is given: a fresh
// 300 KiB file, a deliberately truncated first attempt that leaves a resumable
// prefix, the same offer resumed from that prefix, one file placed below a
// relative directory, and a file whose advertised digest cannot match. The
// host must finalize the three complete files (the folder one in its nested
// directory), keep the prefix (its answer names the offset), and delete the
// refused upload's partial file.
//
// --file-download runs the same channel the other way: the host offers a
// deterministic file, the viewer accepts it, checks the bytes against the
// offered digest and reports the result back to the host.
//
// With --audio the serve end captures the system sound through WASAPI and
// connect decodes the second uni-stream, printing packet/sample/energy
// counters. With --play connect also renders the decoded blocks through WASAPI
// and prints the player's buffer depth and underrun count, which is the only
// way the tool exercises the viewer's real render path. verify substitutes a
// generated 440 Hz tone (encoded by the real AAC codec) so it can assert packet
// counts, CRCs, decoded duration and energy without a sound card, and probes a
// server whose audio gate is closed.
//
// With --microphone verify also covers the viewer uplink: a synthetic 440 Hz
// mono Opus source opens a CMIC stream on a host that granted the microphone,
// and an injected in-memory player on the host records the decoded PCM so the
// check can assert packet/frame counts, the tone's frequency and its RMS. The
// viewer then closes the stream, and a second probe connects to a host whose
// microphone gate is closed to prove CMIC is refused without disturbing video.
//
// With --stream-options verify walks the viewer's picture preferences on the
// running loopback stream: an explicit request for the encoder already in use,
// then Balanced at a 15 fps ceiling, the restore to High at the configured
// rate, a quality-only Balancing at that same rate (which must not add an IDR
// on H.264), and finally the other encoder family. The host must answer each
// one with the effective values, send a StreamInfo after every change, and
// report the other family either as produced (then restored) or as
// Unsupported. The flag is named after the channel because --quality already
// sets the VPL encoder quality for serve/connect/verify.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "annexb.h"
#include "chast/audio.h"
#include "chast/capture.h"
#include "chast/carrier.h"
#include "chast/decoder.h"
#include "chast/display_mode.h"
#include "chast/encoder.h"
#include "chast/host_server.h"
#include "chast/host_settings.h"
#include "chast/identity.h"
#include "chast/protocol.h"
#include "chast/quic.h"
#include "chast/security.h"
#include "crash_handler.h"

namespace {

// A four-format clipboard item for verify: text, HTML, RTF and a 2x2 32bpp
// DIB. The sizes stay far below the protocol caps; the point is that every
// format survives both the CCLB stream and the host's write-back.
chast::ClipboardContent rich_clipboard_probe(const std::string& text) {
    chast::ClipboardContent content;
    chast::ClipboardFormatData plain;
    plain.format = chast::ClipboardFormat::Text;
    plain.data.assign(text.begin(), text.end());
    content.formats.push_back(std::move(plain));
    chast::ClipboardFormatData html;
    html.format = chast::ClipboardFormat::Html;
    const std::string html_text = "<p>" + text + "</p>";
    html.data.assign(html_text.begin(), html_text.end());
    content.formats.push_back(std::move(html));
    chast::ClipboardFormatData rtf;
    rtf.format = chast::ClipboardFormat::Rtf;
    const std::string rtf_text = "{\\rtf1\\ansi " + text + "}";
    rtf.data.assign(rtf_text.begin(), rtf_text.end());
    content.formats.push_back(std::move(rtf));
    chast::ClipboardFormatData dib;
    dib.format = chast::ClipboardFormat::Dib;
    // BITMAPINFOHEADER (40 bytes) + two rows of two 32bpp pixels.
    dib.data.assign(40 + 16, 0);
    auto put32 = [&](size_t offset, uint32_t value) {
        dib.data[offset] = static_cast<uint8_t>(value);
        dib.data[offset + 1] = static_cast<uint8_t>(value >> 8);
        dib.data[offset + 2] = static_cast<uint8_t>(value >> 16);
        dib.data[offset + 3] = static_cast<uint8_t>(value >> 24);
    };
    put32(0, 40);
    put32(4, 2);
    put32(8, 2);
    dib.data[12] = 1;
    dib.data[14] = 32;
    put32(20, 16);
    for (size_t i = 40; i < dib.data.size(); ++i) {
        dib.data[i] = static_cast<uint8_t>(0x10 + i);
    }
    content.formats.push_back(std::move(dib));
    return content;
}

inline constexpr char kClipboardAfterOverflow[] =
    "chast clipboard: after over-limit \xE2\x9C\x93";

// In-memory clipboard shared by verify's server and its test driver. The
// baseline is only there to be primed; the changed item is released after the
// viewer's upload was written, so both directions have a deterministic order.
struct FakeClipboard {
    bool read(chast::ClipboardContent& content) {
        std::lock_guard<std::mutex> lock(mutex);
        ++reads;
        if (update_enabled && !update_sent) {
            update_sent = true;
            current = updated;
        }
        content = current;
        return true;
    }

    bool write(const chast::ClipboardContent& content) {
        std::lock_guard<std::mutex> lock(mutex);
        ++writes;
        written.push_back(content);
        last_write = content;
        current = content;
        cv.notify_all();
        return true;
    }

    bool wait_for_write(std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [this] { return writes > 0; });
    }

    void enable_update() {
        std::lock_guard<std::mutex> lock(mutex);
        update_enabled = true;
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::string baseline_text = "chast clipboard: host baseline";
    std::string updated_text = "chast clipboard: host update \xE2\x9C\x93";
    chast::ClipboardContent baseline = rich_clipboard_probe(baseline_text);
    chast::ClipboardContent updated = rich_clipboard_probe(updated_text);
    chast::ClipboardContent current = baseline;
    chast::ClipboardContent last_write;
    std::vector<chast::ClipboardContent> written;
    uint64_t reads = 0;
    uint64_t writes = 0;
    bool update_enabled = false;
    bool update_sent = false;
};

// In-memory chat transcript shared by verify's host and its test driver. Each
// side records exactly what the other sent, so a missing, duplicated or
// reordered line fails the loopback's exact-match checks.
struct ChatProbe {
    std::mutex mutex;
    std::vector<std::string> host_received;
};

inline constexpr char kHostChatLine[] = "chast chat: host to viewer";
inline constexpr char kViewerChatLine[] = "chast chat: viewer to host";

// Deterministic payload for verify --file-upload. Several 64 KiB CFIL chunks
// of pseudo-random bytes, so a truncated or reordered transfer cannot match.
std::vector<uint8_t> file_probe_content(size_t size) {
    std::vector<uint8_t> bytes(size);
    uint32_t state = 0x9E3779B9u;
    for (size_t i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        bytes[i] = static_cast<uint8_t>(state >> 24);
    }
    return bytes;
}

// The upload probe's five offers: a complete fresh file, the prefix of the
// same payload under another name, the rest of that file, a complete file with
// a relative directory, and an honest payload with a digest that cannot match.
inline constexpr size_t kFileProbeBytes = 300u * 1024u;
inline constexpr size_t kFileResumePrefix = 100u * 1024u;
inline constexpr size_t kFileDigestBytes = 64u * 1024u;

bool same_clipboard_content(const chast::ClipboardContent& left,
                            const chast::ClipboardContent& right) {
    if (left.formats.size() != right.formats.size()) {
        return false;
    }
    for (size_t i = 0; i < left.formats.size(); ++i) {
        if (left.formats[i].format != right.formats[i].format ||
            left.formats[i].data != right.formats[i].data) {
            return false;
        }
    }
    return true;
}

const char* audio_codec_name(chast::AudioCodec codec) {
    switch (codec) {
        case chast::AudioCodec::AAC:
            return "AAC-LC";
        case chast::AudioCodec::Opus:
            return "Opus";
    }
    return "unknown";
}

// verify's stand-in for a real capture endpoint: a 440 Hz tone at the
// encoder's rate, paced against the steady clock and encoded with the real
// codec. It makes the loopback check cover encode -> wire -> decode without
// depending on a sound card. The system-sound probe uses the loopback interface
// with an AAC stereo encoder; the microphone probe uses the capture interface
// with a mono Opus encoder.
template <typename Base>
class SyntheticToneCapture final : public Base {
public:
    SyntheticToneCapture(std::unique_ptr<chast::IAudioEncoder> encoder,
                         chast::AudioCodec codec)
        : encoder_(std::move(encoder)) {
        info_.enabled = true;
        info_.codec = codec;
        info_.sample_rate = encoder_->sample_rate();
        info_.channels = encoder_->channels();
        info_.bitrate = encoder_->bitrate();
        info_.frame_samples = encoder_->frame_samples();
        info_.extradata = encoder_->extradata();
    }

    ~SyntheticToneCapture() override { stop(); }

    void start() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_.joinable() || stopping_.load()) {
            return;
        }
        thread_ = std::thread(&SyntheticToneCapture::run, this);
    }

    void stop() override {
        stopping_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool take_packet(chast::EncodedAudio& packet) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        packet = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    bool failed() const override { return failed_.load(); }

    std::string error() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

    chast::AudioInfo info() const override { return info_; }

    chast::AudioCaptureStats stats() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

private:
    static constexpr size_t kQueuePackets = 32;
    static constexpr double kToneHz = 440.0;
    static constexpr double kAmplitude = 0.30;
    static constexpr double kTwoPi = 6.28318530717958647692;

    void run() {
        using Clock = std::chrono::steady_clock;
        const uint32_t rate = encoder_->sample_rate();
        const uint16_t channels = encoder_->channels();
        const size_t frame_samples = encoder_->frame_samples();
        if (rate == 0 || channels == 0 || frame_samples == 0) {
            fail("the encoder reported an empty format");
            return;
        }
        auto steady_ms = [](Clock::time_point time) {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch())
                    .count());
        };
        const auto start = Clock::now();
        const uint64_t start_ms = steady_ms(start);
        const double step = kTwoPi * kToneHz / static_cast<double>(rate);
        std::vector<int16_t> samples(frame_samples * channels);
        double phase = 0.0;
        uint64_t produced = 0;
        // The frame count follows the wall clock, not the sleep cadence: a
        // late wake-up emits the frames it missed so the tone's duration
        // always matches the session instead of drifting slow on coarse
        // Windows timers.
        while (!stopping_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (stopping_.load()) {
                break;
            }
            const uint64_t due = (steady_ms(Clock::now()) - start_ms) * rate / 1000;
            while (!stopping_.load() && produced + frame_samples <= due) {
                for (size_t i = 0; i < frame_samples; ++i) {
                    const int16_t value = static_cast<int16_t>(
                        std::lround(std::sin(phase) * kAmplitude * 32767.0));
                    for (uint16_t ch = 0; ch < channels; ++ch) {
                        samples[i * channels + ch] = value;
                    }
                    phase += step;
                }
                phase = std::fmod(phase, kTwoPi);
                const uint64_t capture_ms = start_ms + produced * 1000 / rate;
                std::vector<chast::EncodedAudio> encoded;
                const chast::Error result =
                    encoder_->encode(samples.data(), frame_samples, capture_ms, encoded);
                if (!result.ok()) {
                    fail(result.to_string());
                    return;
                }
                produced += frame_samples;
                for (chast::EncodedAudio& packet : encoded) {
                    enqueue(std::move(packet));
                }
            }
        }
    }

    void enqueue(chast::EncodedAudio packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_.load()) {
            return;
        }
        while (queue_.size() >= kQueuePackets) {
            queue_.pop_front();
            ++stats_.dropped;
        }
        queue_.push_back(std::move(packet));
        ++stats_.packets;
    }

    void fail(std::string message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!failed_.exchange(true)) {
                error_ = std::move(message);
            }
        }
        stopping_.store(true);
    }

    std::unique_ptr<chast::IAudioEncoder> encoder_;
    chast::AudioInfo info_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> failed_{false};
    mutable std::mutex mutex_;
    std::deque<chast::EncodedAudio> queue_;
    chast::AudioCaptureStats stats_;
    std::string error_;
};

using SyntheticCapture = SyntheticToneCapture<chast::ILoopbackCapture>;
using SyntheticMicrophoneCapture = SyntheticToneCapture<chast::IMicrophoneCapture>;

std::unique_ptr<chast::ILoopbackCapture> make_synthetic_capture(std::string* error) {
    // Mirrors WasapiLoopbackCapture: 48 kHz stereo Opus at the loopback path's
    // default bitrate. AAC is still supported by the library, but the tool and
    // the host's real capture must exercise the same codec.
    chast::AudioEncoderConfig config;
    config.codec = chast::AudioCodec::Opus;
    config.sample_rate = 48000;
    config.channels = 2;
    config.bitrate = 128000;
    std::unique_ptr<chast::IAudioEncoder> encoder = chast::create_opus_encoder(config, error);
    if (!encoder) {
        return nullptr;
    }
    return std::make_unique<SyntheticCapture>(std::move(encoder), chast::AudioCodec::Opus);
}

std::unique_ptr<chast::IMicrophoneCapture> make_synthetic_microphone_capture(
    std::string* error) {
    chast::AudioEncoderConfig config;
    config.codec = chast::AudioCodec::Opus;
    config.sample_rate = 48000;
    config.channels = 1;
    config.bitrate = 32000;
    config.voice = true;
    std::unique_ptr<chast::IAudioEncoder> encoder =
        chast::create_opus_encoder(config, error);
    if (!encoder) {
        return nullptr;
    }
    return std::make_unique<SyntheticMicrophoneCapture>(std::move(encoder),
                                                       chast::AudioCodec::Opus);
}

// Host-side stand-in for the speaker renderer, injected through
// HostServerConfig::microphone_player. It keeps the decoded PCM so verify can
// assert the microphone stream carried the expected tone instead of trusting
// counters alone.
class MicrophoneRecorder {
public:
    void record(const chast::DecodedAudio& block) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++packets_;
        const size_t channels = block.channels == 0 ? 1 : block.channels;
        sample_rate_ = block.sample_rate;
        channels_ = block.channels;
        sample_count_ += block.samples.size() / channels;
        if (samples_.size() < kMaxSamples) {
            const size_t room = kMaxSamples - samples_.size();
            const size_t take = (std::min)(room, block.samples.size());
            samples_.insert(samples_.end(), block.samples.begin(),
                            block.samples.begin() + static_cast<std::ptrdiff_t>(take));
        }
    }

    uint64_t packets() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return packets_;
    }

    struct Result {
        uint64_t packets = 0;
        uint64_t samples = 0;
        uint32_t sample_rate = 0;
        uint16_t channels = 0;
        double rms = 0.0;
        double frequency_hz = 0.0;
    };

    Result result() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Result out;
        out.packets = packets_;
        out.samples = sample_count_;
        out.sample_rate = sample_rate_;
        out.channels = channels_;
        if (channels_ != 1 || samples_.size() < 2 || sample_rate_ == 0) {
            return out;
        }
        // Ignore the first and last 50 ms so the codec's priming and the
        // truncation at stop cannot bias the estimate.
        const size_t margin = (std::min)(samples_.size() / 4,
                                         static_cast<size_t>(sample_rate_ / 20));
        double energy = 0.0;
        size_t counted = 0;
        uint64_t positive_crossings = 0;
        for (size_t i = 0; i < samples_.size(); ++i) {
            const double value = static_cast<double>(samples_[i]);
            if (i < margin || i + margin >= samples_.size()) {
                continue;
            }
            energy += value * value;
            ++counted;
            if (i > 0 && samples_[i - 1] <= 0 && samples_[i] > 0) {
                ++positive_crossings;
            }
        }
        if (counted == 0) {
            return out;
        }
        out.rms = std::sqrt(energy / static_cast<double>(counted));
        out.frequency_hz = static_cast<double>(positive_crossings) *
                           static_cast<double>(sample_rate_) / static_cast<double>(counted);
        return out;
    }

private:
    static constexpr size_t kMaxSamples = 48000 * 20;
    mutable std::mutex mutex_;
    uint64_t packets_ = 0;
    uint64_t sample_count_ = 0;
    uint32_t sample_rate_ = 0;
    uint16_t channels_ = 0;
    std::vector<int16_t> samples_;
};

class RecordingMicrophonePlayer final : public chast::IAudioPlayer {
public:
    explicit RecordingMicrophonePlayer(std::shared_ptr<MicrophoneRecorder> recorder)
        : recorder_(std::move(recorder)) {}

    chast::Error start() override { return chast::Error::success(); }
    void stop() override {}
    chast::Error push(const chast::DecodedAudio& audio) override {
        recorder_->record(audio);
        return chast::Error::success();
    }
    void set_clock_offset(int64_t) override {}
    void set_lead_ms(uint32_t) override {}
    void set_mute(bool mute) override { muted_ = mute; }
    bool muted() const override { return muted_; }

    chast::AudioPlayerStats stats() const override {
        chast::AudioPlayerStats stats;
        stats.packets = recorder_->packets();
        return stats;
    }
    std::string error() const override { return {}; }

private:
    std::shared_ptr<MicrophoneRecorder> recorder_;
    bool muted_ = false;
};

struct Options {
    std::string mode;
    std::string bind = "127.0.0.1";
    std::string host = "127.0.0.1";
    uint16_t port = 4433;
    // connect/verify: how the QUIC datagrams reach the peer. "udp" dials the
    // host directly; "tcp"/"ws" tunnel them through the host's carrier, which
    // still runs the same QUIC handshake on both ends. serve always listens on
    // both UDP and the carrier.
    std::string transport = "udp";
    // connect/verify: carrier endpoint override for a relay or a manually
    // forwarded port. Empty means the host's QUIC port + 1.
    std::string carrier;
    // connect/verify: select this host on a shared multiplexing carrier.
    // Empty host id means the carrier endpoint serves one host on its own
    // port, which is what a relay port does.
    std::string carrier_host_id;
    std::string carrier_token;
    // serve: fallback output when the client does not ask for one.
    // connect: the output to ask the server for.
    size_t display = 0;
    bool display_set = false;
    chast::VideoCodec codec = chast::VideoCodec::H264;
    // serve/verify: pin the hardware encoder family instead of Auto's
    // oneVPL-then-FFmpeg order.
    chast::EncoderBackend encoder = chast::EncoderBackend::Auto;
    bool encoder_set = false;
    UINT bitrate_kbps = 0;
    UINT framerate = 30;
    float quality = 0.67f;
    UINT keyframe_ms = 0;
    // Receiver-side: ask for an IDR this often, exercising the control stream
    // and the sender's key-frame-on-request path.
    UINT keyframe_request_ms = 0;
    // serve: accept and inject InputEvents. connect: send the scripted input
    // sequence once the handshake reports input enabled. verify: the same,
    // and check that the server observed exactly the events the client sent.
    bool input = false;
    // serve: mirror clipboard text through the real clipboard. connect: print
    // the texts the host sends. verify: the same as serve with a fake backend
    // and checks in both directions.
    bool clipboard = false;
    // serve: capture the system sound through WASAPI. connect: require and
    // decode the audio stream, printing its counters. verify always turns
    // this on with a synthetic source.
    bool audio = false;
    // connect only: render the decoded audio through WASAPI as well, so the
    // real playback path reports its buffer and underruns. Implies --audio.
    bool play = false;
    // verify only: the host encodes a generated 440 Hz tone instead of opening
    // the real loopback endpoint, so the check needs no sound card.
    bool synthetic_audio = false;
    // verify only: cover the viewer microphone uplink. The host grants the
    // microphone and gets an in-memory player; the viewer sends a synthetic
    // mono Opus tone through CMIC. A second probe checks the closed gate.
    bool microphone = false;
    // verify only: cover privacy mode on a host that allows it. The viewer
    // turns the overlay on, checks the stream still carries desktop content,
    // then turns it off; a second probe checks the closed gate.
    bool privacy = false;
    // verify only: cover Ctrl+Alt+Del. The host gets an in-memory send callback
    // that reports success, so the request/answer channel runs without touching
    // the real secure desktop; a second probe checks a host with no callback
    // answers Unsupported instead.
    bool sas = false;
    // verify only: walk the viewer's picture preferences. A quality/frame-rate
    // change and its restore must be answered with the effective values and
    // followed by a StreamInfo; the other encoder family is requested to prove
    // that Ok and Unsupported both describe what the host can really produce.
    bool stream_options = false;
    // The in-memory host player the microphone probe injects, shared with
    // run_verify so it can inspect the decoded PCM after the session.
    std::shared_ptr<MicrophoneRecorder> microphone_recorder;
    // connect only: upload this text once after the handshake, for checking a
    // host's real clipboard write path. Implies --clipboard.
    std::string send_clipboard;
    // verify only: the server's clipboard backend and the test driver's view.
    // When set, the real clipboard is never touched.
    std::shared_ptr<FakeClipboard> fake_clipboard;
    // verify only: the shared chat transcript. The host greets the viewer when
    // the stream is announced and records the viewer's reply; the viewer sends
    // its line once the stream is live.
    std::shared_ptr<ChatProbe> chat_probe;
    // serve only: accept input but stop at the OS boundary. Lets the Flutter
    // viewer exercise its full send path against a server on the same machine
    // without the injected events looping back into the viewer's own window.
    bool input_dry_run = false;
    // serve: print every InputEvent the server observes, for diagnosing
    // the viewer's mapping.
    bool dump_input = false;
    // serve/verify: scale the published NV12 picture to this percent once,
    // 2.5 s into the session, to exercise the live resize path without a lossy
    // link. 0 leaves the host's own ladder alone; verify defaults to 75.
    uint32_t resolution = 0;
    // serve and verify: send a StreamInfo with the current description once,
    // two seconds in. serve uses it to exercise a viewer's update path without
    // changing the host's display mode; verify turns it on to cover the wire
    // and session layers.
    bool reannounce = false;
    // verify only: really call SendInput instead of stopping at the OS
    // boundary. Keys are left out of the script in that mode, because a test
    // run must not type into whatever window happens to be focused; the key
    // path is covered by the dry run and the injector's own checks.
    bool input_real = false;
    // verify only: also exercise the display-mode channel end to end. The
    // probe lists the streamed output's modes, sends a no-op request, changes
    // to another mode, restores the original and finally asks for a mode the
    // display does not report. The session runs long enough for the whole
    // walk because every step waits for the previous answer.
    bool display_mode = false;
    // verify only: ask for the merged layout (kDisplayAll) instead of one
    // output, then check the union geometry and the no-op merge switch.
    bool merged = false;
    // verify only: upload a deterministic 300 KiB payload through CFIL and
    // check the host wrote it, byte for byte, into the receive directory.
    bool file_upload = false;
    // verify only: where the local host writes uploads. Set to a temporary
    // folder so the check never touches the operator's Downloads.
    std::string file_receive_dir;
    // verify only: send a deterministic file from the host to the viewer and
    // check the bytes the viewer received against the offered digest.
    bool file_download = false;
    // The file the host offers when --file-download is given; verify writes
    // it to a temporary folder before the session starts.
    std::string download_source;
    // connect/verify: SHA-256 fingerprint to pin. verify fills it from the
    // server it starts when the option is absent.
    std::string fingerprint;
    // connect: accept any certificate. Development only, and never combined
    // with a pin.
    bool insecure = false;
    // serve: Hello must carry this token. connect: the token to send. verify:
    // shared by the local server and the receiver.
    std::string token;
    // serve --token auto: generate a token and print it. Cleared once the
    // token is resolved, so later code only sees the value.
    bool token_auto = false;
    // serve: let the same token serve later sessions instead of spending it
    // on the first accepted viewer.
    bool token_reuse = false;
    // serve: ask on stdin before letting a viewer in.
    bool approve = false;
    // serve: the permanent password Hello must carry. connect: the password to
    // send. An empty password with click/both mode asks for approval instead.
    std::string password;
    // serve: require this base32 TOTP secret in addition to the password.
    // connect: compute the current six-digit code from it, or send `totp`.
    std::string totp_secret;
    std::string totp;
    // serve: access mode. Empty keeps the flag-derived default (click with
    // --approve, password with --password, token otherwise).
    chast::HostAuthMode auth_mode = chast::HostAuthMode::Both;
    bool auth_mode_set = false;
    double seconds = 0.0;
    bool seconds_set = false;
    double wait_seconds = 30.0;
    std::string save;
    chast::CaptureConfig config;
    bool quiet = false;
};

bool parse_host_port(const std::string& text, std::string& host, uint16_t& port) {
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == text.size()) {
        return false;
    }
    host = text.substr(0, colon);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    unsigned long value = 0;
    try {
        value = std::stoul(text.substr(colon + 1));
    } catch (const std::exception&) {
        return false;
    }
    if (host.empty() || value == 0 || value > 65535) {
        return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
}

bool parse_args(int argc, char** argv, Options& options) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: chast-quic serve|connect|verify [options]\n");
        return false;
    }
    options.mode = argv[1];
    if (options.mode != "serve" && options.mode != "connect" && options.mode != "verify") {
        std::fprintf(stderr, "unknown mode: %s (want serve, connect or verify)\n",
                     options.mode.c_str());
        return false;
    }
    try {
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--port" && i + 1 < argc) {
                options.port = static_cast<uint16_t>(std::stoul(argv[++i]));
            } else if (arg == "--bind" && i + 1 < argc) {
                options.bind = argv[++i];
            } else if (arg == "--host" && i + 1 < argc) {
                options.host = argv[++i];
            } else if (arg == "--transport" && i + 1 < argc) {
                std::string value = argv[++i];
                std::transform(value.begin(), value.end(), value.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (value == "quic") {
                    value = "udp";
                } else if (value == "websocket") {
                    value = "ws";
                }
                if (value != "udp" && value != "tcp" && value != "ws") {
                    std::fprintf(stderr, "--transport wants udp, tcp or ws\n");
                    return false;
                }
                options.transport = value;
            } else if (arg == "--carrier" && i + 1 < argc) {
                options.carrier = argv[++i];
            } else if (arg == "--carrier-id" && i + 1 < argc) {
                options.carrier_host_id = argv[++i];
            } else if (arg == "--carrier-token" && i + 1 < argc) {
                options.carrier_token = argv[++i];
            } else if (arg == "--display" && i + 1 < argc) {
                const unsigned long value = std::stoul(argv[++i]);
                if (value >= chast::kMaxDisplayCount) {
                    std::fprintf(stderr, "display index %lu is past the %u-display limit\n",
                                 value, static_cast<unsigned>(chast::kMaxDisplayCount));
                    return false;
                }
                options.display = static_cast<size_t>(value);
                options.display_set = true;
            } else if (arg == "--codec" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value == "h264" || value == "avc") {
                    options.codec = chast::VideoCodec::H264;
                } else if (value == "hevc" || value == "h265") {
                    options.codec = chast::VideoCodec::Hevc;
                } else {
                    std::fprintf(stderr, "unknown codec: %s\n", value.c_str());
                    return false;
                }
            } else if (arg == "--encoder" && i + 1 < argc) {
                if (!chast::parse_encoder_backend(argv[++i], options.encoder)) {
                    std::fprintf(stderr, "--encoder wants auto, vpl or ffmpeg\n");
                    return false;
                }
                options.encoder_set = true;
            } else if (arg == "--bitrate" && i + 1 < argc) {
                options.bitrate_kbps = static_cast<UINT>(std::stoul(argv[++i]));
            } else if (arg == "--framerate" && i + 1 < argc) {
                options.framerate = static_cast<UINT>(std::stoul(argv[++i]));
            } else if (arg == "--quality" && i + 1 < argc) {
                options.quality = std::stof(argv[++i]);
            } else if (arg == "--keyframe-interval" && i + 1 < argc) {
                options.keyframe_ms = static_cast<UINT>(std::stoul(argv[++i]));
            } else if (arg == "--request-keyframe-ms" && i + 1 < argc) {
                options.keyframe_request_ms = static_cast<UINT>(std::stoul(argv[++i]));
            } else if (arg == "--seconds" && i + 1 < argc) {
                options.seconds = std::stod(argv[++i]);
                options.seconds_set = true;
            } else if (arg == "--wait" && i + 1 < argc) {
                options.wait_seconds = std::stod(argv[++i]);
            } else if (arg == "--save" && i + 1 < argc) {
                options.save = argv[++i];
            } else if (arg == "--input") {
                options.input = true;
            } else if (arg == "--clipboard") {
                options.clipboard = true;
            } else if (arg == "--audio") {
                options.audio = true;
            } else if (arg == "--play") {
                options.audio = true;
                options.play = true;
            } else if (arg == "--microphone") {
                options.microphone = true;
            } else if (arg == "--privacy") {
                options.privacy = true;
            } else if (arg == "--sas") {
                options.sas = true;
            } else if (arg == "--stream-options") {
                options.stream_options = true;
            } else if (arg == "--send-clipboard" && i + 1 < argc) {
                options.send_clipboard = argv[++i];
                options.clipboard = true;
            } else if (arg == "--input-dry-run") {
                options.input = true;
                options.input_dry_run = true;
            } else if (arg == "--dump-input") {
                options.dump_input = true;
            } else if (arg == "--reannounce") {
                options.reannounce = true;
            } else if (arg == "--input-real") {
                options.input = true;
                options.input_real = true;
            } else if (arg == "--display-mode") {
                options.display_mode = true;
            } else if (arg == "--merged") {
                options.merged = true;
            } else if (arg == "--file-upload") {
                options.file_upload = true;
            } else if (arg == "--file-download") {
                options.file_download = true;
            } else if (arg == "--fingerprint" && i + 1 < argc) {
                options.fingerprint = argv[++i];
            } else if (arg == "--insecure") {
                options.insecure = true;
            } else if (arg == "--token" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value == "auto") {
                    options.token_auto = true;
                } else {
                    options.token = value;
                }
            } else if (arg == "--token-reuse") {
                options.token_reuse = true;
            } else if (arg == "--approve") {
                options.approve = true;
            } else if (arg == "--password" && i + 1 < argc) {
                options.password = argv[++i];
            } else if (arg == "--totp" && i + 1 < argc) {
                options.totp = argv[++i];
            } else if (arg == "--totp-secret" && i + 1 < argc) {
                options.totp_secret = argv[++i];
            } else if (arg == "--auth-mode" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (!chast::parse_auth_mode(value, options.auth_mode)) {
                    std::fprintf(stderr, "--auth-mode wants password, click or both\n");
                    return false;
                }
                options.auth_mode_set = true;
            } else if (arg == "--no-hdr") {
                options.config.want_hdr = false;
            } else if (arg == "--resolution" && i + 1 < argc) {
                const unsigned long value = std::stoul(argv[++i]);
                if (value < 1 || value > 100) {
                    std::fprintf(stderr, "--resolution wants 1..100 percent\n");
                    return false;
                }
                options.resolution = static_cast<uint32_t>(value);
            } else if (arg == "--no-cursor") {
                options.config.capture_cursor = false;
            } else if (arg == "--quiet") {
                options.quiet = true;
            } else {
                std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
                return false;
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "invalid argument value: %s\n", e.what());
        return false;
    }
    // The hardware encoder consumes NV12 and nothing else.
    options.config.nv12.enabled = true;
    options.config.nv12.colorimetry = chast::Nv12Colorimetry::Bt601Limited;
    if (!options.seconds_set) {
        // serve and verify need an end; connect follows the server's FIN.
        options.seconds = options.mode == "connect" ? 0.0 : 5.0;
    }
    if (options.token.size() > chast::kMaxAccessTokenLength) {
        std::fprintf(stderr, "--token is longer than %u bytes\n",
                     static_cast<unsigned>(chast::kMaxAccessTokenLength));
        return false;
    }
    if (options.transport != "udp" && options.mode == "serve") {
        std::fprintf(stderr, "--transport only applies to connect and verify; "
                             "serve always listens on UDP and the carrier\n");
        return false;
    }
    if (!options.carrier.empty()) {
        if (options.transport == "udp") {
            std::fprintf(stderr, "--carrier needs --transport tcp or ws\n");
            return false;
        }
        std::string carrier_host;
        uint16_t carrier_port = 0;
        if (!parse_host_port(options.carrier, carrier_host, carrier_port)) {
            std::fprintf(stderr, "--carrier wants host:port\n");
            return false;
        }
    }
    if (options.transport != "udp" && options.carrier.empty() &&
        (options.port == 0 || options.port == 65535)) {
        std::fprintf(stderr, "--transport %s needs --carrier <host:port> because "
                             "port %u has no port + 1\n",
                     options.transport.c_str(), static_cast<unsigned>(options.port));
        return false;
    }
    if (!options.carrier_host_id.empty() || !options.carrier_token.empty()) {
        if (options.mode == "serve") {
            std::fprintf(stderr, "--carrier-id and --carrier-token select a host on a "
                                 "shared carrier and only apply to connect and verify\n");
            return false;
        }
        if (options.carrier_host_id.empty() || options.carrier_token.empty()) {
            std::fprintf(stderr, "--carrier-id and --carrier-token go together\n");
            return false;
        }
        if (options.transport == "udp") {
            std::fprintf(stderr, "--carrier-id needs --transport tcp or ws\n");
            return false;
        }
        if (options.carrier.empty()) {
            std::fprintf(stderr, "--carrier-id needs --carrier <host:port>: a shared "
                                 "carrier has no fixed port + 1\n");
            return false;
        }
    }
    if ((options.token_reuse || options.approve) && options.mode != "serve") {
        std::fprintf(stderr, "--token-reuse and --approve only apply to serve\n");
        return false;
    }
    if (options.resolution != 0 && options.mode == "connect") {
        std::fprintf(stderr, "--resolution only applies to serve and verify\n");
        return false;
    }
    if (options.encoder_set && options.mode == "connect") {
        std::fprintf(stderr, "--encoder only applies to serve and verify\n");
        return false;
    }
    if (options.file_upload && options.mode != "verify") {
        std::fprintf(stderr, "--file-upload only applies to verify\n");
        return false;
    }
    if (options.file_download && options.mode != "verify") {
        std::fprintf(stderr, "--file-download only applies to verify\n");
        return false;
    }
    if (options.microphone && options.mode != "verify") {
        std::fprintf(stderr, "--microphone only applies to verify\n");
        return false;
    }
    if (options.privacy && options.mode != "verify") {
        std::fprintf(stderr, "--privacy only applies to verify\n");
        return false;
    }
    if (options.sas && options.mode != "verify") {
        std::fprintf(stderr, "--sas only applies to verify\n");
        return false;
    }
    if (options.stream_options && options.mode != "verify") {
        std::fprintf(stderr, "--stream-options only applies to verify\n");
        return false;
    }
    if (options.merged && options.mode != "verify") {
        std::fprintf(stderr, "--merged only applies to verify\n");
        return false;
    }
    if (options.merged && options.display_set) {
        std::fprintf(stderr, "--merged asks for every output; do not also give --display\n");
        return false;
    }
    if (options.merged && options.display_mode) {
        std::fprintf(stderr, "--merged and --display-mode are mutually exclusive\n");
        return false;
    }
    if (!options.send_clipboard.empty()) {
        if (options.mode != "connect") {
            std::fprintf(stderr, "--send-clipboard only applies to connect\n");
            return false;
        }
        if (!chast::valid_clipboard_text(options.send_clipboard)) {
            std::fprintf(stderr, "--send-clipboard wants 1..256 KiB of valid UTF-8\n");
            return false;
        }
    }
    if (!options.password.empty() && options.mode == "verify") {
        std::fprintf(stderr, "--password only applies to serve and connect\n");
        return false;
    }
    if (!options.totp.empty() && options.mode != "connect") {
        std::fprintf(stderr, "--totp only applies to connect\n");
        return false;
    }
    if (!options.totp.empty() && !options.totp_secret.empty()) {
        std::fprintf(stderr, "pass either --totp or --totp-secret, not both\n");
        return false;
    }
    if (!options.totp_secret.empty() && options.mode == "verify") {
        std::fprintf(stderr, "--totp-secret only applies to serve and connect\n");
        return false;
    }
    if (!options.totp_secret.empty()) {
        std::vector<uint8_t> key;
        if (!chast::parse_totp_secret(options.totp_secret, key)) {
            std::fprintf(stderr, "--totp-secret is not base32\n");
            return false;
        }
    }
    if (!options.totp.empty() &&
        (options.totp.size() != 6 ||
         !std::all_of(options.totp.begin(), options.totp.end(),
                      [](char c) { return c >= '0' && c <= '9'; }))) {
        std::fprintf(stderr, "--totp wants the six-digit code\n");
        return false;
    }
    if (options.auth_mode_set && options.mode != "serve") {
        std::fprintf(stderr, "--auth-mode only applies to serve\n");
        return false;
    }
    if (options.auth_mode_set && options.auth_mode != chast::HostAuthMode::Password &&
        !options.approve) {
        std::fprintf(stderr, "--auth-mode %s needs --approve to answer the prompt\n",
                     chast::auth_mode_name(options.auth_mode));
        return false;
    }
    return true;
}

// Eight characters from a 32-symbol alphabet: about 40 bits, which is far
// beyond online guessing for a single one-time token, and short enough to read
// aloud or type on a phone.
std::string generate_token() {
    static constexpr char kAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    std::random_device random;
    std::string token;
    token.reserve(8);
    for (int i = 0; i < 8; ++i) {
        token.push_back(kAlphabet[random() % (sizeof(kAlphabet) - 1)]);
    }
    return token;
}

struct ServerReady {
    bool ok = false;
    std::string fingerprint;
};

int64_t steady_clock_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct SenderStats {
    uint64_t frames = 0;
    uint64_t encoded = 0;
    uint64_t keyframes = 0;
    uint64_t bytes = 0;
    uint64_t pointer_only = 0;
    uint64_t repeats = 0;
    uint64_t send_errors = 0;
    // Encoded picture size from the last announcement, and the captured
    // desktop size it was scaled from.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint32_t resolution_percent = 100;
    uint64_t resolution_changes = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    chast::VideoCodec codec = chast::VideoCodec::H264;
    std::string encoder;
    std::string save_path;
    // Files the host finalized (digest verified, renamed into place), in the
    // order it reported them, and where the last one landed.
    uint64_t files_received = 0;
    uint64_t file_bytes_received = 0;
    std::vector<std::string> received_file_paths;
    // Host-to-viewer probe (verify --file-download): what HostServer counted
    // for the file it offered, and whether the queue accepted it.
    uint64_t files_sent = 0;
    uint64_t file_bytes_sent = 0;
    uint64_t file_send_failures = 0;
    bool download_queued = false;
    std::string error;
    bool client_connected = false;
    bool peer_left = false;
    bool stats_received = false;
    chast::ClientStats peer;
    // Input events the server observed, in order, plus the injector's
    // counters. Empty unless --input was given.
    std::vector<chast::InputEvent> input_events;
    chast::InputStats input_stats;
    uint64_t input_releases = 0;
    bool input_enabled = false;
    // Clipboard texts the server sent and received, plus the gate it
    // announced. The counts are assigned from HostServerStats.
    uint64_t clipboard_sent = 0;
    uint64_t clipboard_received = 0;
    bool clipboard_enabled = false;
    // Chat lines the host sent and received (verify's built-in exchange).
    uint64_t chat_sent = 0;
    uint64_t chat_received = 0;
    bool chat_greeting_queued = false;
    // Audio format the host announced, whether it stayed active, and the
    // HostServer counters for packets it handed to the transport.
    chast::AudioInfo audio_info;
    bool audio_announced = false;
    bool audio_active = false;
    uint64_t audio_packets = 0;
    uint64_t audio_bytes = 0;
    uint64_t audio_dropped = 0;
    uint64_t audio_errors = 0;
    uint64_t audio_capture_recoveries = 0;
    uint64_t audio_gate_transitions = 0;
    bool audio_recovering = false;
    // Viewer microphone (verify --microphone). What the host announced, the
    // CMIC streams it accepted, the packets it decoded, and the PCM its
    // injected player recorded for the tone checks.
    bool microphone_enabled = false;
    bool microphone_active = false;
    uint64_t microphone_streams = 0;
    uint64_t microphone_packets = 0;
    uint64_t microphone_bytes = 0;
    uint64_t microphone_frames = 0;
    uint64_t microphone_errors = 0;
    uint64_t microphone_underruns = 0;
    MicrophoneRecorder::Result microphone_recording;
    // Privacy mode (verify --privacy): changes the host applied and whether
    // the overlay was still up when the session ended. Both requests in the
    // probe are counted, so two changes prove the on and off were applied.
    uint64_t privacy_mode_changes = 0;
    bool privacy_mode_active = false;
    // SAS requests the host handled (verify --sas) and the answers it refused.
    uint64_t sas_requests = 0;
    uint64_t sas_failures = 0;
    // Viewer picture preferences the host handled (verify --stream-options):
    // every request, the ones that changed the running stream, and refusals.
    uint64_t stream_option_requests = 0;
    uint64_t stream_option_changes = 0;
    uint64_t stream_option_failures = 0;
    // CursorUpdates the server sent (shapes carrying a sprite included).
    uint64_t cursor_sent = 0;
    std::vector<chast::DisplayDescription> displays;
    // What Hello asked for (kDisplayAuto when the client left it to the
    // server) and the index the capturer actually used.
    uint16_t requested_display = chast::kDisplayAuto;
    size_t display_index = 0;
    // Display switches the host accepted and refused during the session.
    uint64_t display_switches = 0;
    uint64_t display_switch_failures = 0;
    // True when the running stream is the merged union of the outputs.
    bool display_merged = false;
    // Display mode changes the host applied (accepted requests, including a
    // no-op for the running mode) and refused.
    uint64_t display_mode_changes = 0;
    uint64_t display_mode_failures = 0;
    // StreamInfo messages this server sent.
    uint64_t stream_updates_sent = 0;
    // Adaptive bitrate telemetry: the encoder's current target (the video
    // budget after the audio reserve, or the setting when audio is off), the
    // frame rate the ladder picked, how often the controller changed it, and
    // the transport RTT the controller sampled.
    uint32_t target_bitrate = 0;
    uint32_t framerate = 0;
    uint64_t abr_changes = 0;
    double rtt_ms = 0.0;
    double send_kbps = 0.0;
    // SHA-256 identity the server presents (uppercase hex).
    std::string fingerprint;
    bool ok = false;
};

struct ReceiverStats {
    uint64_t received = 0;
    uint64_t decoded = 0;
    uint64_t keyframes = 0;
    uint64_t bytes = 0;
    uint64_t crc_errors = 0;
    uint64_t decode_errors = 0;
    uint64_t gaps = 0;
    uint64_t protocol_errors = 0;
    uint64_t digest = chast::kFrameDigestSeed;
    // Decoded picture size, plus the source size the last announcement
    // described (the captured desktop, which pointer input uses). The HelloAck
    // picture size is kept separately: a session starts at full resolution,
    // so it must equal the source size.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint32_t hello_width = 0;
    uint32_t hello_height = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    chast::VideoCodec codec = chast::VideoCodec::H264;
    std::string decoder;
    // "udp" for a direct QUIC session, otherwise the carrier that tunneled it.
    std::string transport = "udp";
    std::string error;
    bool ok = false;
    // Events this receiver sent, and whether the HelloAck allowed them.
    std::vector<chast::InputEvent> input_sent;
    bool input_enabled = false;
    uint64_t input_errors = 0;
    // The host's desktop reports: a clean loopback stays on Default.
    uint64_t desktop_states = 0;
    chast::DesktopKind desktop_kind = chast::DesktopKind::Default;
    // Clipboard items this receiver sent and every rich item it received.
    uint64_t clipboard_sent = 0;
    uint64_t clipboard_received = 0;
    uint64_t clipboard_errors = 0;
    bool clipboard_enabled = false;
    // Chat probe: the lines the viewer sent and received, and any transport
    // refusal. The host's side is recorded in ChatProbe.
    bool chat_probe = false;
    uint64_t chat_sent = 0;
    uint64_t chat_received = 0;
    uint64_t chat_errors = 0;
    std::vector<std::string> chat_texts;
    // The verify probe first injects a CCLB blob whose declared size is past
    // the protocol limit, then sends a valid item to prove the session is
    // still alive and old content was kept.
    bool clipboard_over_limit_sent = false;
    bool clipboard_followup_sent = false;
    std::vector<chast::ClipboardContent> clipboard_contents;
    // File upload probe (verify --file-upload): every offer, the host's answer
    // and the final result, in order, plus the bytes streamed and any
    // unexpected protocol state.
    bool file_probe = false;
    uint64_t file_offers_sent = 0;
    uint64_t file_answers = 0;
    std::vector<chast::FileCode> file_answer_codes;
    std::vector<uint64_t> file_answer_offsets;
    std::vector<std::string> file_answer_reasons;
    uint64_t file_bytes_sent = 0;
    uint64_t file_results = 0;
    std::vector<chast::FileCode> file_result_codes;
    std::vector<uint64_t> file_result_received;
    std::vector<std::string> file_result_reasons;
    uint64_t file_errors = 0;
    // Host-to-viewer probe (verify --file-download): the offer the viewer
    // accepted, the bytes it collected and whether they matched the digest.
    bool download_probe = false;
    uint64_t download_offers = 0;
    uint64_t download_bytes = 0;
    bool download_finished = false;
    bool download_ok = false;
    std::string download_error;
    // Audio the viewer received and decoded: announced format, packet and
    // byte counts, sequence/CRC/decode errors, decoded sample count and the
    // RMS of the decoded PCM (the tone-energy check).
    chast::AudioInfo audio_info;
    bool audio_announced = false;
    bool audio_finished = false;
    uint64_t audio_received = 0;
    uint64_t audio_bytes = 0;
    uint64_t audio_gaps = 0;
    uint64_t audio_crc_errors = 0;
    uint64_t audio_decode_errors = 0;
    uint64_t audio_protocol_errors = 0;
    uint64_t audio_samples = 0;
    double audio_energy = 0.0;
    uint64_t audio_energy_values = 0;
    double audio_rms = 0.0;
    double audio_seconds = 0.0;
    double elapsed_seconds = 0.0;
    // Only with --play: what the WASAPI renderer actually did.
    uint64_t audio_played = 0;
    uint64_t audio_underruns = 0;
    uint64_t audio_player_drops = 0;
    uint64_t audio_player_recoveries = 0;
    uint32_t audio_buffer_ms = 0;
    std::string audio_player_error;
    // Microphone uplink probe (verify --microphone): the host grant, the CMIC
    // stream the viewer opened and closed, and the packets it handed to the
    // transport. Send errors are separated from intentional drops.
    bool microphone_announced = false;
    bool microphone_started = false;
    bool microphone_stopped = false;
    chast::AudioInfo microphone_info;
    uint64_t microphone_packets = 0;
    uint64_t microphone_bytes = 0;
    uint64_t microphone_dropped = 0;
    uint64_t microphone_send_errors = 0;
    // Privacy mode probe (verify --privacy): the two answers, the luma of the
    // last frame before the overlay went up and of the frames captured while
    // it was up, and how many frames arrived after it came down.
    uint64_t privacy_updates = 0;
    bool privacy_on_seen = false;
    bool privacy_off_seen = false;
    bool privacy_last_enabled = false;
    chast::PrivacyModeCode privacy_last_code = chast::PrivacyModeCode::Ok;
    std::string privacy_last_reason;
    double privacy_luma_before = -1.0;
    double privacy_luma_last = 0.0;
    uint64_t privacy_frames = 0;
    uint64_t privacy_frames_after_off = 0;
    double last_frame_luma = 0.0;
    bool privacy_probe = false;
    // SAS probe (verify --sas): the answers the host sent back, and whether
    // one of them reported the secure desktop as reached.
    bool sas_probe = false;
    uint64_t sas_updates = 0;
    bool sas_ok_seen = false;
    chast::SasCode sas_last_code = chast::SasCode::Ok;
    std::string sas_last_reason;
    // Stream preference probe (verify --stream-options): every answer, the
    // effective values of the last one, and the StreamInfo the host announced
    // after each applied quality/frame-rate change. The other encoder family
    // either arrived (then the original was restored) or was refused as
    // Unsupported with a reason.
    bool stream_options_probe = false;
    uint64_t stream_option_results = 0;
    chast::StreamOptionsCode stream_option_last_code = chast::StreamOptionsCode::Ok;
    chast::VideoCodec stream_option_last_codec = chast::VideoCodec::H264;
    uint16_t stream_option_last_fps = 0;
    chast::StreamQuality stream_option_last_quality = chast::StreamQuality::High;
    std::string stream_option_last_reason;
    chast::VideoCodec stream_option_base_codec = chast::VideoCodec::H264;
    uint16_t stream_option_change_fps = 0;
    chast::StreamQuality stream_option_change_quality = chast::StreamQuality::High;
    bool stream_option_change_update_seen = false;
    uint32_t stream_option_change_update_fps = 0;
    bool stream_option_restore_update_seen = false;
    uint32_t stream_option_restore_update_fps = 0;
    uint32_t stream_option_restore_update_bitrate = 0;
    // A quality-only step (Balanced with the frame-rate ceiling unchanged):
    // the StreamInfo it announced, whether the bitrate moved, and how many
    // keyframes it cost. oneVPL's H.264 tiers continue the coded sequence, so
    // that count stays zero; libavcodec's QSV encoder resets its MFX session
    // for the change and pays one, as does a reopened HEVC session.
    bool stream_option_rate_update_seen = false;
    uint32_t stream_option_rate_update_bitrate = 0;
    uint64_t stream_option_rate_keyframes = 0;
    std::string stream_option_rate_encoder;
    bool stream_option_alt_seen = false;
    bool stream_option_alt_ok = false;
    bool stream_option_alt_unsupported = false;
    chast::VideoCodec stream_option_alt_codec = chast::VideoCodec::H264;
    std::string stream_option_alt_reason;
    bool stream_option_codec_restored = false;
    std::vector<chast::DisplayDescription> displays;
    // HelloAck said the stream is the merged union of the outputs.
    bool hello_merged = false;
    // StreamInfo messages received, and the description from the last one.
    uint64_t stream_updates = 0;
    chast::ServerInfo last_update;
    // Mid-session display switches this receiver asked for (verify only): the
    // two requested indexes, the answers taken, and the last answer's detail.
    uint16_t switch_ok_index = chast::kDisplayAuto;
    uint16_t switch_bad_index = chast::kDisplayAuto;
    uint64_t display_switch_results = 0;
    uint64_t display_switch_ok = 0;
    uint64_t display_switch_refused = 0;
    uint16_t last_switch_index = 0;
    chast::DisplaySwitchCode last_switch_code = chast::DisplaySwitchCode::Ok;
    std::string last_switch_reason;
    // Display mode probe (verify --display-mode): the modes answer for the
    // streamed output, the requests the probe picked, and every answer.
    bool mode_probe = false;
    uint16_t mode_display = 0;
    uint64_t mode_lists = 0;
    uint64_t mode_results = 0;
    uint64_t mode_ok = 0;
    uint64_t mode_refused = 0;
    std::vector<chast::DisplayModeInfo> modes;
    chast::DisplayModeCode modes_code = chast::DisplayModeCode::Ok;
    std::string modes_error;
    bool mode_plan_ready = false;
    uint16_t mode_original_width = 0;
    uint16_t mode_original_height = 0;
    uint16_t mode_original_refresh = 0;
    uint16_t mode_target_width = 0;
    uint16_t mode_target_height = 0;
    uint16_t mode_target_refresh = 0;
    uint16_t mode_bad_width = 0;
    uint16_t mode_bad_height = 0;
    uint16_t mode_bad_refresh = 0;
    // What the StreamInfo after each real change described: the source size
    // the viewer must have seen, and whether it arrived at all.
    bool mode_change_update_seen = false;
    bool mode_restore_update_seen = false;
    bool mode_leave_update_seen = false;
    uint32_t mode_change_source_width = 0;
    uint32_t mode_change_source_height = 0;
    uint32_t mode_restore_source_width = 0;
    uint32_t mode_restore_source_height = 0;
    uint32_t mode_leave_source_width = 0;
    uint32_t mode_leave_source_height = 0;
    uint16_t last_mode_index = 0;
    uint16_t last_mode_width = 0;
    uint16_t last_mode_height = 0;
    uint16_t last_mode_refresh = 0;
    chast::DisplayModeCode last_mode_code = chast::DisplayModeCode::Ok;
    std::string last_mode_reason;
    // The refusal step's answer, kept apart from the last answer because the
    // probe goes on to leave the display changed.
    bool refused_mode_echoed = false;
    uint16_t refused_mode_index = 0;
    uint16_t refused_mode_width = 0;
    uint16_t refused_mode_height = 0;
    uint16_t refused_mode_refresh = 0;
    std::string refused_mode_reason;
    // Latency probes: the last clock-sync sample plus counters, and the frame
    // age (host capture -> local receipt) of the newest and worst frame seen.
    chast::LatencySample latency;
    uint64_t probes_sent = 0;
    uint64_t probe_errors = 0;
    uint64_t frames_without_capture = 0;
    double last_frame_age_ms = -1.0;
    double max_frame_age_ms = -1.0;
    // Bitrate and frame rate the last StreamInfo announced (the encoder's
    // target and ladder pick after ABR).
    uint32_t target_bitrate = 0;
    uint32_t framerate = 0;
    // Pointer updates received, and how many of them carried a sprite. The
    // last state is reported so a verification run can show the pointer moved.
    uint64_t cursor_updates = 0;
    uint64_t cursor_shapes = 0;
    bool cursor_visible = false;
    int32_t cursor_x = 0;
    int32_t cursor_y = 0;
    // Fingerprint of the certificate the server presented.
    std::string peer_fingerprint;
};

// The input script connect and verify replay. It exercises every action the
// protocol carries: an absolute move, a click, both wheel axes, a relative
// move, and a chord that leaves a modifier down across other events.
std::vector<chast::InputEvent> input_script(int32_t origin_x, int32_t origin_y, bool keys) {
    std::vector<chast::InputEvent> events;
    auto mouse = [&](chast::MouseAction action, uint16_t buttons, int32_t x, int32_t y) {
        chast::InputEvent event;
        event.kind = chast::InputKind::Mouse;
        event.mouse.action = action;
        event.mouse.buttons = buttons;
        event.mouse.x = x;
        event.mouse.y = y;
        events.push_back(event);
    };
    auto key = [&](uint16_t usage, chast::KeyAction action, uint16_t modifiers) {
        chast::InputEvent event;
        event.kind = chast::InputKind::Key;
        event.key.hid_usage = usage;
        event.key.action = action;
        event.modifiers = modifiers;
        events.push_back(event);
    };

    mouse(chast::MouseAction::Move, 0, origin_x + 10, origin_y + 10);
    mouse(chast::MouseAction::ButtonDown, chast::kMouseLeft, 0, 0);
    mouse(chast::MouseAction::ButtonUp, chast::kMouseLeft, 0, 0);
    mouse(chast::MouseAction::Wheel, 0, 0, -WHEEL_DELTA);
    mouse(chast::MouseAction::WheelHorizontal, 0, WHEEL_DELTA, 0);
    mouse(chast::MouseAction::MoveRelative, 0, 5, -3);
    if (keys) {
        key(0xE1, chast::KeyAction::Down, chast::kModifierLeftShift);  // Left Shift
        key(0x04, chast::KeyAction::Down, chast::kModifierLeftShift);  // A
        key(0x04, chast::KeyAction::Up, chast::kModifierLeftShift);
        key(0xE1, chast::KeyAction::Up, 0);
    }
    return events;
}

bool same_event(const chast::InputEvent& a, const chast::InputEvent& b) {
    if (a.kind != b.kind || a.modifiers != b.modifiers) {
        return false;
    }
    if (a.kind == chast::InputKind::Mouse) {
        return a.mouse.action == b.mouse.action && a.mouse.buttons == b.mouse.buttons &&
               a.mouse.x == b.mouse.x && a.mouse.y == b.mouse.y;
    }
    return a.key.action == b.key.action && a.key.hid_usage == b.key.hid_usage;
}

bool same_display(const chast::DisplayDescription& a, const chast::DisplayDescription& b) {
    return a.name == b.name && a.left == b.left && a.top == b.top && a.right == b.right &&
           a.bottom == b.bottom && a.rotation_degrees == b.rotation_degrees &&
           a.primary == b.primary;
}

// One host run over HostServer: the tool supplies the policy (display
// fallback, save sink, approval prompt, run length) and keeps its own stats
// view for the checks.
SenderStats run_sender(const Options& options, const std::string& save_path,
                       std::promise<ServerReady>* listening) {
    SenderStats stats;
    stats.save_path = save_path;
    bool ready_published = false;
    auto publish_ready = [&](bool value) {
        if (listening && !ready_published) {
            ready_published = true;
            ServerReady ready;
            ready.ok = value;
            ready.fingerprint = value ? stats.fingerprint : std::string();
            listening->set_value(std::move(ready));
        }
    };
    auto fail = [&](std::string message) {
        stats.error = std::move(message);
        stats.ok = false;
        publish_ready(false);
        return stats;
    };

    struct FileCloser {
        void operator()(FILE* file) const {
            if (file) {
                std::fclose(file);
            }
        }
    };
    std::unique_ptr<FILE, FileCloser> sink;
    if (!save_path.empty()) {
        FILE* raw = nullptr;
        if (fopen_s(&raw, save_path.c_str(), "wb") != 0 || !raw) {
            return fail("could not open " + save_path);
        }
        sink.reset(raw);
    }

    chast::HostServerConfig config;
    config.settings.bind_address = options.bind;
    config.settings.port = options.port;
    config.settings.allow_input = options.input;
    config.settings.allow_clipboard = options.clipboard;
    config.settings.allow_audio = options.audio;
    config.settings.allow_microphone = options.microphone;
    config.settings.allow_privacy_mode = options.privacy;
    if (options.sas) {
        // The real secure desktop is never involved: the loopback only proves
        // the request reaches the host and the answer reaches the viewer.
        config.send_sas = [](std::string*) { return true; };
    }
    if (options.synthetic_audio) {
        config.audio_capture = [](std::string* capture_error) {
            return make_synthetic_capture(capture_error);
        };
    }
    if (options.microphone && options.microphone_recorder) {
        std::shared_ptr<MicrophoneRecorder> recorder = options.microphone_recorder;
        config.microphone_player = [recorder](std::string*) -> std::unique_ptr<chast::IAudioPlayer> {
            return std::make_unique<RecordingMicrophonePlayer>(recorder);
        };
    }
    config.settings.token_mode = options.token_reuse ? chast::HostTokenMode::Reusable
                                                     : chast::HostTokenMode::OneTime;
    config.settings.access_token = options.token;
    if (!options.password.empty()) {
        config.settings.password_hash = chast::hash_password(options.password);
    }
    config.settings.totp_secret = options.totp_secret;
    // This tool is CLI/automation, so the access token is its credential. With
    // --approve an operator is present and the click flow replaces it.
    if (options.auth_mode_set) {
        config.settings.auth_mode = options.auth_mode;
        config.settings.require_approval =
            options.auth_mode != chast::HostAuthMode::Password;
        config.settings.token_auth = !options.approve;
    } else if (options.approve) {
        config.settings.auth_mode = chast::HostAuthMode::Click;
        config.settings.require_approval = true;
        config.settings.token_auth = false;
    } else if (!options.password.empty()) {
        config.settings.auth_mode = chast::HostAuthMode::Password;
        config.settings.require_approval = false;
        config.settings.token_auth = true;
    } else {
        config.settings.token_auth = true;
    }
    config.settings.codec = options.codec;
    config.settings.bitrate_kbps = options.bitrate_kbps;
    config.settings.framerate = static_cast<uint16_t>(options.framerate);
    config.settings.quality = options.quality;
    if (options.display_set) {
        config.settings.display = static_cast<uint16_t>(options.display);
    }
    config.dry_run_input =
        options.input_dry_run || (options.input && options.mode == "verify" && !options.input_real);
    config.want_hdr = options.config.want_hdr;
    config.capture_cursor = options.config.capture_cursor;
    config.encoder_backend = options.encoder;
    config.serve_once = true;
    config.run_seconds = options.seconds;
    config.reannounce = options.reannounce;
    config.keyframe_interval_ms = options.keyframe_ms;
    config.resolution_probe_percent = static_cast<uint16_t>(options.resolution);
    config.first_client_timeout_ms = static_cast<uint32_t>(options.wait_seconds * 1000.0);
    if (!options.quiet) {
        const auto log_start = std::chrono::steady_clock::now();
        config.log = [log_start](const std::string& message) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - log_start)
                    .count();
            std::printf("quic: [%7.2f] %s\n", elapsed, message.c_str());
        };
    }
    if (options.approve) {
        // Runs on the host thread while wait_for_client() is blocked, so
        // reading stdin here cannot stall the transport's own workers.
        config.approve_client = [](const chast::ClientRequest& request) {
            const std::string display =
                request.display == chast::kDisplayAuto ? "auto"
                                                       : std::to_string(request.display);
            std::printf("quic: %s asks to connect (display %s); allow? [y/N] ",
                        request.peer_address.empty() ? "a client" : request.peer_address.c_str(),
                        display.c_str());
            std::fflush(stdout);
            char line[16]{};
            if (!std::fgets(line, sizeof(line), stdin)) {
                std::printf("\n");
                return chast::ClientApproval{};
            }
            return chast::ClientApproval{line[0] == 'y' || line[0] == 'Y',
                                         request.permissions};
        };
    }
    if (sink) {
        config.packet = [&sink](const chast::FrameHeader&, const uint8_t* data, size_t size) {
            std::fwrite(data, 1, size, sink.get());
        };
    }
    config.input_event = [&stats, &options](const chast::InputEvent& event) {
        stats.input_events.push_back(event);
        if (!options.dump_input) {
            return;
        }
        if (event.kind == chast::InputKind::Mouse) {
            std::printf("input: mouse action=%u buttons=%u x=%d y=%d state=0x%03X\n",
                        static_cast<unsigned>(event.mouse.action), event.mouse.buttons,
                        event.mouse.x, event.mouse.y, event.modifiers);
        } else {
            std::printf("input: key action=%u usage=0x%02X state=0x%03X\n",
                        static_cast<unsigned>(event.key.action), event.key.hid_usage,
                        event.modifiers);
        }
    };
    if (options.fake_clipboard) {
        std::shared_ptr<FakeClipboard> clipboard = options.fake_clipboard;
        config.clipboard_read = [clipboard](chast::ClipboardContent& content) {
            return clipboard->read(content);
        };
        config.clipboard_write = [clipboard](const chast::ClipboardContent& content) {
            return clipboard->write(content);
        };
    }
    config.settings.allow_file_transfer = options.file_upload || options.file_download;
    config.settings.file_receive_directory = options.file_receive_dir;
    config.file_received = [&stats](const std::string& path, uint64_t bytes) {
        ++stats.files_received;
        stats.file_bytes_received += bytes;
        stats.received_file_paths.push_back(path);
    };
    if (options.chat_probe) {
        std::shared_ptr<ChatProbe> chat = options.chat_probe;
        config.chat_message = [chat](const std::string& text) {
            std::lock_guard<std::mutex> lock(chat->mutex);
            chat->host_received.push_back(text);
        };
    }

    std::string error;
    std::unique_ptr<chast::HostServer> server;
    if (options.file_download || options.chat_probe) {
        // Both probes start from the announcement: the stream is live, so a
        // queued line is accepted and the offered file has a viewer to take it.
        std::shared_ptr<ChatProbe> chat = options.chat_probe;
        const bool download = options.file_download;
        config.stream_announced = [&server, &stats, source = options.download_source,
                                   chat, download](const chast::ServerInfo&) {
            if (chat && !stats.chat_greeting_queued) {
                stats.chat_greeting_queued = true;
                if (!server->send_chat(kHostChatLine)) {
                    stats.error = "send_chat: the host refused the greeting";
                }
            }
            if (!download || stats.download_queued) {
                return;
            }
            stats.download_queued = true;
            std::string start_error;
            if (!server->start_file_download(source, &start_error)) {
                stats.error = "start_file_download: " + start_error;
                std::fprintf(stderr, "start_file_download: %s\n", start_error.c_str());
            }
        };
    }
    server = chast::HostServer::create(std::move(config), &error);
    if (!server) {
        return fail("QUIC server: " + error);
    }
    const chast::HostStatus initial = server->status();
    stats.fingerprint = initial.fingerprint;
    publish_ready(true);
    if (!options.quiet) {
        std::printf("quic: identity SHA256 %s\n",
                    chast::format_fingerprint(stats.fingerprint).c_str());
        if (!initial.token.empty()) {
            std::printf("quic: access token %s (%s)\n", initial.token.c_str(),
                        options.token_reuse ? "reusable" : "one-time");
        }
        if (options.approve) {
            std::printf("quic: every viewer will need operator approval\n");
        }
    }

    server->start();
    server->wait();
    const chast::HostStatus status = server->status();

    stats.client_connected = status.stats.client_connected;
    stats.requested_display = status.requested_display;
    stats.display_index = status.display_index;
    stats.display_merged = status.display_merged;
    stats.codec = status.stream.codec;
    stats.width = status.stream.width;
    stats.height = status.stream.height;
    stats.source_width = status.stream.source_width;
    stats.source_height = status.stream.source_height;
    stats.resolution_percent = status.stats.resolution_percent;
    stats.resolution_changes = status.stats.resolution_changes;
    stats.origin_x = status.stream.origin_x;
    stats.origin_y = status.stream.origin_y;
    stats.encoder = status.stream.encoder;
    stats.displays = status.stream.displays;
    stats.input_enabled = options.input;
    stats.input_stats = status.stats.input_stats;
    stats.input_releases = status.stats.input_releases;
    stats.clipboard_enabled = options.clipboard;
    stats.clipboard_sent = status.stats.clipboard_sent;
    stats.clipboard_received = status.stats.clipboard_received;
    stats.audio_info = status.stream.audio;
    stats.audio_announced = status.stream.audio.enabled;
    stats.audio_active = status.stats.audio_active;
    stats.audio_packets = status.stats.audio_packets;
    stats.audio_bytes = status.stats.audio_bytes;
    stats.audio_dropped = status.stats.audio_dropped;
    stats.audio_errors = status.stats.audio_errors;
    stats.audio_capture_recoveries = status.stats.audio_capture_recoveries;
    stats.audio_gate_transitions = status.stats.audio_gate_transitions;
    stats.audio_recovering = status.stats.audio_recovering;
    stats.microphone_enabled = status.stream.microphone_enabled;
    stats.microphone_active = status.stats.microphone_active;
    stats.microphone_streams = status.stats.microphone_streams;
    stats.microphone_packets = status.stats.microphone_packets;
    stats.microphone_bytes = status.stats.microphone_bytes;
    stats.microphone_frames = status.stats.microphone_frames;
    stats.microphone_errors = status.stats.microphone_errors;
    stats.microphone_underruns = status.stats.microphone_underruns;
    stats.privacy_mode_changes = status.stats.privacy_mode_changes;
    stats.privacy_mode_active = status.stats.privacy_mode_active;
    stats.sas_requests = status.stats.sas_requests;
    stats.sas_failures = status.stats.sas_failures;
    stats.chat_sent = status.stats.chat_sent;
    stats.chat_received = status.stats.chat_received;
    stats.stream_option_requests = status.stats.stream_option_requests;
    stats.stream_option_changes = status.stats.stream_option_changes;
    stats.stream_option_failures = status.stats.stream_option_failures;
    if (options.microphone_recorder) {
        stats.microphone_recording = options.microphone_recorder->result();
    }
    stats.stats_received = status.stats.stats_received;
    stats.peer = status.stats.peer;
    stats.peer_left = status.stats.peer_left;
    stats.stream_updates_sent = status.stats.stream_updates;
    stats.display_switches = status.stats.display_switches;
    stats.display_switch_failures = status.stats.display_switch_failures;
    stats.display_mode_changes = status.stats.display_mode_changes;
    stats.display_mode_failures = status.stats.display_mode_failures;
    stats.frames = status.stats.frames;
    stats.encoded = status.stats.encoded;
    stats.keyframes = status.stats.keyframes;
    stats.bytes = status.stats.bytes;
    stats.pointer_only = status.stats.pointer_only;
    stats.repeats = status.stats.repeats;
    stats.cursor_sent = status.stats.cursor_sent;
    stats.send_errors = status.stats.send_errors;
    stats.target_bitrate = status.stats.target_bitrate;
    stats.framerate = status.stats.framerate;
    stats.abr_changes = status.stats.abr_changes;
    stats.rtt_ms = status.stats.rtt_ms;
    stats.send_kbps = status.stats.send_kbps;
    stats.files_sent = status.stats.files_sent;
    stats.file_bytes_sent = status.stats.file_bytes_sent;
    stats.file_send_failures = status.stats.file_send_failures;
    stats.error = status.error;

    if (!options.quiet || !stats.error.empty()) {
        std::printf("sender: frames=%llu encoded=%llu keyframes=%llu pointer-only=%llu "
                    "repeats=%llu bytes=%llu send-errors=%llu stream-updates=%llu\n",
                    static_cast<unsigned long long>(stats.frames),
                    static_cast<unsigned long long>(stats.encoded),
                    static_cast<unsigned long long>(stats.keyframes),
                    static_cast<unsigned long long>(stats.pointer_only),
                    static_cast<unsigned long long>(stats.repeats),
                    static_cast<unsigned long long>(stats.bytes),
                    static_cast<unsigned long long>(stats.send_errors),
                    static_cast<unsigned long long>(stats.stream_updates_sent));
        std::printf("sender: cursor sent=%llu shapes=%llu bytes=%llu dropped=%llu errors=%llu\n",
                    static_cast<unsigned long long>(stats.cursor_sent),
                    static_cast<unsigned long long>(status.stats.cursor_shapes),
                    static_cast<unsigned long long>(status.stats.cursor_bytes),
                    static_cast<unsigned long long>(status.stats.cursor_dropped),
                    static_cast<unsigned long long>(status.stats.cursor_errors));
        std::printf("sender: target-bitrate=%u kbps framerate=%u abr-changes=%llu rtt=%.1f ms "
                    "send-rate=%.1f kbps\n",
                    stats.target_bitrate / 1000u, stats.framerate,
                    static_cast<unsigned long long>(stats.abr_changes), stats.rtt_ms,
                    stats.send_kbps);
        std::printf("sender: picture=%ux%u source=%ux%u resolution=%u%% changes=%llu\n",
                    stats.width, stats.height, stats.source_width, stats.source_height,
                    stats.resolution_percent,
                    static_cast<unsigned long long>(stats.resolution_changes));
        if (stats.stats_received) {
            std::printf("sender: peer received=%llu decoded=%llu digest=%016llX\n",
                        static_cast<unsigned long long>(stats.peer.received),
                        static_cast<unsigned long long>(stats.peer.decoded),
                        static_cast<unsigned long long>(stats.peer.digest));
        }
        if (options.input) {
            std::printf("sender: input received=%llu applied=%llu rejected=%llu "
                        "failed=%llu dropped=%llu releases=%llu\n",
                        static_cast<unsigned long long>(stats.input_events.size()),
                        static_cast<unsigned long long>(stats.input_stats.applied),
                        static_cast<unsigned long long>(stats.input_stats.rejected),
                        static_cast<unsigned long long>(stats.input_stats.failed),
                        static_cast<unsigned long long>(stats.input_stats.dropped),
                        static_cast<unsigned long long>(stats.input_releases));
        }
        if (options.clipboard) {
            std::printf("sender: clipboard sent=%llu received=%llu\n",
                        static_cast<unsigned long long>(stats.clipboard_sent),
                        static_cast<unsigned long long>(stats.clipboard_received));
        }
        if (options.audio) {
            std::printf("sender: audio announced=%d active=%d %s %u Hz %u ch %u kbps "
                        "packets=%llu bytes=%llu dropped=%llu errors=%llu "
                        "recoveries=%llu gates=%llu recovering=%d\n",
                        stats.audio_announced ? 1 : 0, stats.audio_active ? 1 : 0,
                        audio_codec_name(stats.audio_info.codec),
                        stats.audio_info.sample_rate, stats.audio_info.channels,
                        stats.audio_info.bitrate / 1000u,
                        static_cast<unsigned long long>(stats.audio_packets),
                        static_cast<unsigned long long>(stats.audio_bytes),
                        static_cast<unsigned long long>(stats.audio_dropped),
                        static_cast<unsigned long long>(stats.audio_errors),
                        static_cast<unsigned long long>(stats.audio_capture_recoveries),
                        static_cast<unsigned long long>(stats.audio_gate_transitions),
                        stats.audio_recovering ? 1 : 0);
        }
        if (options.microphone) {
            std::printf("sender: microphone announced=%d active=%d streams=%llu "
                        "packets=%llu bytes=%llu frames=%llu errors=%llu underruns=%llu "
                        "recorded=%llu samples rms=%.0f tone=%.0f Hz\n",
                        stats.microphone_enabled ? 1 : 0, stats.microphone_active ? 1 : 0,
                        static_cast<unsigned long long>(stats.microphone_streams),
                        static_cast<unsigned long long>(stats.microphone_packets),
                        static_cast<unsigned long long>(stats.microphone_bytes),
                        static_cast<unsigned long long>(stats.microphone_frames),
                        static_cast<unsigned long long>(stats.microphone_errors),
                        static_cast<unsigned long long>(stats.microphone_underruns),
                        static_cast<unsigned long long>(stats.microphone_recording.samples),
                        stats.microphone_recording.rms,
                        stats.microphone_recording.frequency_hz);
        }
    }
    stats.ok = stats.error.empty();
    return stats;
}

ReceiverStats run_receiver(const Options& options, std::shared_future<ServerReady> server_ready) {
    ReceiverStats stats;
    auto fail = [&](std::string message) {
        stats.error = std::move(message);
        stats.ok = false;
        return stats;
    };

    ServerReady ready;
    if (server_ready.valid()) {
        if (server_ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            return fail("the local server did not start");
        }
        ready = server_ready.get();
        if (!ready.ok) {
            return fail("the local server did not start");
        }
    }

    std::string pinned = chast::normalize_fingerprint(options.fingerprint);
    if (!options.fingerprint.empty() && pinned.empty()) {
        return fail("--fingerprint is not a SHA-256 fingerprint");
    }
    if (pinned.empty()) {
        // verify starts the server itself, so it can take the identity from
        // there instead of asking the caller to copy it.
        pinned = ready.fingerprint;
    }
    if (pinned.empty() && !options.insecure) {
        return fail("no pinned server identity: pass --fingerprint <SHA256> "
                    "(printed by serve), or --insecure for local development");
    }

    // The carrier is transport only: it exposes the remote QUIC endpoint as a
    // loopback UDP socket, so pinning, tokens and every protocol message stay
    // end to end between the same endpoints as a direct session.
    std::unique_ptr<chast::CarrierBridge> carrier_bridge;
    if (options.transport != "udp") {
        std::string carrier_host;
        uint16_t carrier_port = 0;
        if (!options.carrier.empty()) {
            if (!parse_host_port(options.carrier, carrier_host, carrier_port)) {
                return fail("--carrier wants host:port");
            }
        } else {
            if (options.port == 0 || options.port == 65535) {
                return fail("--transport " + options.transport +
                            " needs --carrier <host:port> when the QUIC port is " +
                            std::to_string(static_cast<unsigned>(options.port)));
            }
            carrier_host = options.host;
            carrier_port = static_cast<uint16_t>(options.port + 1);
        }
        chast::CarrierBridgeConfig carrier_config;
        carrier_config.host = carrier_host;
        carrier_config.port = carrier_port;
        carrier_config.transport = options.transport == "ws"
                                       ? chast::CarrierTransport::kWebSocket
                                       : chast::CarrierTransport::kTcp;
        carrier_config.host_id = options.carrier_host_id;
        carrier_config.token = options.carrier_token;
        std::string carrier_error;
        carrier_bridge = chast::CarrierBridge::open(carrier_config, &carrier_error);
        if (!carrier_bridge) {
            return fail(options.transport + " carrier " + carrier_host + ":" +
                        std::to_string(static_cast<unsigned>(carrier_port)) + ": " +
                        carrier_error);
        }
        stats.transport = chast::carrier_transport_name(carrier_bridge->transport());
        if (!options.quiet) {
            const std::string shared_note = options.carrier_host_id.empty()
                                                ? std::string()
                                                : " for host " + options.carrier_host_id;
            std::printf("carrier: %s to %s:%u%s, feeding the QUIC client on 127.0.0.1:%u\n",
                        stats.transport.c_str(), carrier_host.c_str(),
                        static_cast<unsigned>(carrier_port), shared_note.c_str(),
                        static_cast<unsigned>(carrier_bridge->local_port()));
        }
    }

    chast::QuicClientConfig client_config;
    client_config.host = carrier_bridge ? "127.0.0.1" : options.host;
    client_config.port = carrier_bridge ? carrier_bridge->local_port() : options.port;
    client_config.pinned_fingerprint = pinned;
    client_config.allow_any_certificate = pinned.empty() && options.insecure;
    client_config.access_token = options.token;
    client_config.password = options.password;
    client_config.totp = options.totp;
    if (client_config.totp.empty() && !options.totp_secret.empty()) {
        // Computed right before the Hello so the 30 s window is fresh.
        std::vector<uint8_t> key;
        if (!chast::parse_totp_secret(options.totp_secret, key)) {
            return fail("--totp-secret is not base32");
        }
        char code[8]{};
        std::snprintf(code, sizeof(code), "%06u",
                      chast::totp_code(key, static_cast<uint64_t>(std::time(nullptr))));
        client_config.totp = code;
    }
    std::string error;
    std::unique_ptr<chast::QuicSession> session =
        chast::QuicSession::create_client(client_config, &error);
    if (!session) {
        return fail("QUIC client: " + error);
    }
    chast::Error connected = session->connect(std::chrono::seconds(30));
    if (!connected.ok()) {
        return fail("connect: " + connected.to_string());
    }
    stats.peer_fingerprint = session->peer_fingerprint();
    if (!options.quiet) {
        std::printf("quic: server identity SHA256 %s%s\n",
                    chast::format_fingerprint(stats.peer_fingerprint).c_str(),
                    pinned.empty() ? " (unverified)" : " (pinned)");
    }

    chast::ServerInfo info;
    const uint16_t requested_display =
        options.display_set ? static_cast<uint16_t>(options.display) : chast::kDisplayAuto;
    chast::Error handshaken =
        session->handshake(info, std::chrono::seconds(15), requested_display);
    if (!handshaken.ok()) {
        return fail("handshake: " + handshaken.to_string());
    }
    stats.codec = info.codec;
    stats.width = info.width;
    stats.height = info.height;
    stats.source_width = info.source_width;
    stats.source_height = info.source_height;
    stats.hello_width = info.width;
    stats.hello_height = info.height;
    stats.origin_x = info.origin_x;
    stats.origin_y = info.origin_y;
    stats.input_enabled = info.input_enabled;
    stats.clipboard_enabled = info.clipboard_enabled;
    stats.displays = info.displays;
    stats.hello_merged = info.merged;
    stats.target_bitrate = info.bitrate;
    stats.framerate = info.framerate;
    stats.audio_info = info.audio;
    stats.audio_announced = info.audio.enabled;
    stats.microphone_announced = info.microphone_enabled;
    if (!options.quiet) {
        std::printf("quic: connected, server streams %s %ux%u from %ux%u at %u kbps (%s)\n",
                    chast_tools::codec_name(info.codec), info.width, info.height,
                    info.source_width, info.source_height, info.bitrate / 1000u,
                    info.encoder.c_str());
        if (!options.token.empty()) {
            std::printf("quic: access token accepted\n");
        }
        std::printf("quic: server has %zu display%s\n", info.displays.size(),
                    info.displays.size() == 1 ? "" : "s");
        for (size_t i = 0; i < info.displays.size(); ++i) {
            const chast::DisplayDescription& display = info.displays[i];
            std::printf("quic:   [%zu] %s %dx%d at %d,%d rotation %u%s\n", i,
                        display.name.empty() ? "(unnamed)" : display.name.c_str(),
                        display.right - display.left, display.bottom - display.top,
                        display.left, display.top,
                        static_cast<unsigned>(display.rotation_degrees),
                        display.primary ? " primary" : "");
        }
        if (info.merged) {
            std::printf("quic: the stream is the merged layout of %zu display%s\n",
                        info.displays.size(), info.displays.size() == 1 ? "" : "s");
        }
        if (options.display_set && options.display == chast::kDisplayAll) {
            std::printf("quic: asked the server for the merged layout\n");
        } else if (options.display_set) {
            std::printf("quic: asked the server for display %zu\n", options.display);
        } else {
            std::printf("quic: let the server choose the display\n");
        }
        if (options.input) {
            std::printf("quic: server input %s (origin %d,%d)\n",
                        info.input_enabled ? "enabled" : "disabled", info.origin_x,
                        info.origin_y);
        }
        if (options.clipboard) {
            std::printf("quic: server clipboard %s\n",
                        info.clipboard_enabled ? "enabled" : "disabled");
        }
        if (options.audio) {
            std::printf("quic: server audio %s (%s %u Hz %u ch %u kbps)\n",
                        info.audio.enabled ? "enabled" : "disabled",
                        audio_codec_name(info.audio.codec), info.audio.sample_rate,
                        info.audio.channels, info.audio.bitrate / 1000u);
        }
        if (options.microphone) {
            std::printf("quic: server microphone %s\n",
                        info.microphone_enabled ? "granted" : "not granted");
        }
    }
    if (options.input) {
        if (!info.input_enabled) {
            return fail("the server refused input (it was not started with --input)");
        }
        // Sent before the frame loop: the server injects on its own thread, so
        // these land while the stream is already running and the check does
        // not depend on frame timing.
        stats.input_sent = input_script(info.origin_x, info.origin_y, !options.input_real);
        for (const chast::InputEvent& event : stats.input_sent) {
            if (chast::Error sent = session->send_input(event); !sent.ok()) {
                ++stats.input_errors;
                if (stats.input_errors == 1) {
                    std::fprintf(stderr, "send_input: %s\n", sent.to_string().c_str());
                }
            }
        }
        // The viewer does this whenever it gives up control; here it also
        // proves the release message travels and reaches the injector.
        if (chast::Error released = session->release_input(); !released.ok()) {
            ++stats.input_errors;
            std::fprintf(stderr, "release_input: %s\n", released.to_string().c_str());
        }
    }

    if (options.clipboard) {
        if (!info.clipboard_enabled) {
            return fail("the server refused clipboard (it was not started with --clipboard)");
        }
        if (options.mode == "verify") {
            // The fake backend proves the exact four-format item, so verify
            // uploads a fixed one; connect may still upload a text given on
            // the command line through the legacy message.
            chast::ClipboardContent content =
                rich_clipboard_probe("chast clipboard: viewer upload \xE2\x9C\x93");
            if (chast::Error sent = session->send_clipboard_content(std::move(content));
                !sent.ok()) {
                ++stats.clipboard_errors;
                if (stats.clipboard_errors == 1) {
                    std::fprintf(stderr, "send_clipboard_content: %s\n",
                                 sent.to_string().c_str());
                }
            } else {
                ++stats.clipboard_sent;
            }
            // Declare more than the protocol allows without carrying the
            // bytes: the receiver must reject it from the header, drop this
            // stream, keep the item above and stay connected.
            std::vector<uint8_t> over_limit = chast::encode_clipboard_stream(
                rich_clipboard_probe("chast clipboard: over-limit"));
            if (over_limit.size() < chast::kClipboardStreamHeaderSize) {
                return fail("could not encode the over-limit clipboard probe");
            }
            const uint64_t declared =
                static_cast<uint64_t>(chast::kMaxClipboardTotalBytes) + 1;
            for (int i = 0; i < 8; ++i) {
                over_limit[16 + i] = static_cast<uint8_t>(declared >> (8 * i));
            }
            if (chast::Error sent = session->send_clipboard_blob(std::move(over_limit));
                !sent.ok()) {
                ++stats.clipboard_errors;
                std::fprintf(stderr, "send_clipboard_blob: %s\n",
                             sent.to_string().c_str());
            } else {
                stats.clipboard_over_limit_sent = true;
            }
        } else if (!options.send_clipboard.empty()) {
            if (chast::Error sent = session->send_clipboard(1, options.send_clipboard);
                !sent.ok()) {
                ++stats.clipboard_errors;
                std::fprintf(stderr, "send_clipboard: %s\n", sent.to_string().c_str());
            } else {
                ++stats.clipboard_sent;
            }
        }
    }

    // Give the rejected blob time to arrive first, so its handling is what
    // the follow-up send proves survived.
    const auto clipboard_followup_at =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(250);

    // ---- file upload probe (verify --file-upload) ----
    // Five offers in sequence: a complete file, a truncated first attempt that
    // leaves a resumable prefix, the rest of that file resumed at the prefix,
    // a file below a relative directory, and one whose advertised digest
    // cannot match. The host checks the digest before it renames a file into
    // place, so the bytes on disk close the loop.
    const uint32_t kFileProbeId = 0x0F11E001u;
    stats.file_probe = options.file_upload;
    struct FileProbeStage {
        chast::FileOffer offer;
        std::vector<uint8_t> content;
        size_t send_from = 0;
        size_t send_to = 0;
        uint64_t answer_offset = 0;
        chast::FileCode result = chast::FileCode::Ok;
        uint64_t result_received = 0;
        const char* label = "";
    };
    std::vector<FileProbeStage> file_stages;
    size_t file_stage_index = 0;
    enum class FileStep { Offer, WaitAnswer, SendData, WaitResult, Done };
    FileStep file_step = FileStep::Offer;
    size_t file_position = 0;
    if (stats.file_probe) {
        if (!info.file_transfer_enabled) {
            return fail("the server did not announce file transfer");
        }
        const std::vector<uint8_t> payload = file_probe_content(kFileProbeBytes);
        std::array<uint8_t, 32> digest{};
        if (!chast::sha256(payload.data(), payload.size(), digest)) {
            return fail("could not hash the file probe payload");
        }
        auto add_stage = [&](const char* label, const std::string& name, size_t send_from,
                             size_t send_to, bool resume, chast::FileCode result,
                             uint64_t result_received, uint64_t answer_offset) {
            FileProbeStage stage;
            stage.label = label;
            stage.offer.transfer_id =
                kFileProbeId + static_cast<uint32_t>(file_stages.size());
            stage.offer.name = name;
            stage.offer.size = payload.size();
            stage.offer.sha256 = digest;
            stage.offer.resume = resume;
            stage.content = payload;
            stage.send_from = send_from;
            stage.send_to = send_to;
            stage.result = result;
            stage.result_received = result_received;
            stage.answer_offset = answer_offset;
            file_stages.push_back(std::move(stage));
        };
        add_stage("fresh", "chast-verify-upload.bin", 0, payload.size(), false,
                  chast::FileCode::Ok, payload.size(), 0);
        add_stage("truncated", "chast-verify-resume.bin", 0, kFileResumePrefix, true,
                  chast::FileCode::IoError, kFileResumePrefix, 0);
        add_stage("resumed", "chast-verify-resume.bin", kFileResumePrefix, payload.size(),
                  true, chast::FileCode::Ok, payload.size(), kFileResumePrefix);
        // A folder upload: the host must create the relative chain below its
        // receive root and land the leaf there.
        add_stage("nested", "chast-verify-nested.bin", 0, payload.size(), false,
                  chast::FileCode::Ok, payload.size(), 0);
        file_stages.back().offer.relative_path = "chast-verify/nested";
        // The payload is honest; only the advertised digest is wrong. The host
        // can only discover that after the stream ends, so it accepts the
        // offer and refuses the finished file.
        FileProbeStage digest_stage;
        digest_stage.label = "digest";
        digest_stage.offer.transfer_id =
            kFileProbeId + static_cast<uint32_t>(file_stages.size());
        digest_stage.offer.name = "chast-verify-digest.bin";
        digest_stage.content = file_probe_content(kFileDigestBytes);
        digest_stage.offer.size = digest_stage.content.size();
        if (!chast::sha256(digest_stage.content.data(), digest_stage.content.size(),
                           digest_stage.offer.sha256)) {
            return fail("could not hash the digest probe payload");
        }
        digest_stage.offer.sha256[0] ^= 0xFF;
        digest_stage.send_to = digest_stage.content.size();
        digest_stage.result = chast::FileCode::IoError;
        digest_stage.result_received = digest_stage.content.size();
        file_stages.push_back(std::move(digest_stage));
    }
    auto advance_file_upload = [&]() -> bool {
        if (!stats.file_probe || file_step == FileStep::Done) {
            return true;
        }
        if (file_step == FileStep::Offer) {
            const FileProbeStage& stage = file_stages[file_stage_index];
            if (chast::Error offered = session->offer_file(stage.offer); !offered.ok()) {
                ++stats.file_errors;
                stats.error = std::string("offer_file: ") + offered.to_string();
                return false;
            }
            ++stats.file_offers_sent;
            file_step = FileStep::WaitAnswer;
            if (!options.quiet) {
                std::printf("quic: offering %s (%llu bytes%s)\n", stage.offer.name.c_str(),
                            static_cast<unsigned long long>(stage.offer.size),
                            stage.offer.resume ? ", resumable" : "");
            }
            return true;
        }
        const FileProbeStage& stage = file_stages[file_stage_index];
        chast::FileAnswer answer;
        while (session->take_file_answer(answer)) {
            ++stats.file_answers;
            stats.file_answer_codes.push_back(answer.code);
            stats.file_answer_offsets.push_back(answer.resume_offset);
            stats.file_answer_reasons.push_back(answer.reason);
            if (answer.transfer_id != stage.offer.transfer_id) {
                ++stats.file_errors;
                stats.error = "the host answered a transfer the probe did not offer";
                return false;
            }
            if (answer.code != chast::FileCode::Ok) {
                ++stats.file_errors;
                stats.error = std::string("the host refused the ") + stage.label +
                              " file: " + answer.reason;
                return false;
            }
            if (answer.resume_offset != stage.answer_offset) {
                ++stats.file_errors;
                stats.error = std::string("the ") + stage.label + " upload resumed at " +
                              std::to_string(answer.resume_offset) + " instead of " +
                              std::to_string(stage.answer_offset);
                return false;
            }
            file_position = stage.send_from;
            file_step = FileStep::SendData;
        }
        if (file_step == FileStep::SendData) {
            while (file_position < stage.send_to) {
                const size_t chunk =
                    (std::min)(chast::kFileChunkBytes, stage.send_to - file_position);
                chast::Error sent = session->send_file_data(
                    stage.offer.transfer_id, stage.content.data() + file_position, chunk);
                if (!sent.ok()) {
                    ++stats.file_errors;
                    stats.error = "send_file_data: " + sent.to_string();
                    return false;
                }
                file_position += chunk;
                stats.file_bytes_sent += chunk;
            }
            if (chast::Error finished = session->finish_file_upload(stage.offer.transfer_id);
                !finished.ok()) {
                ++stats.file_errors;
                stats.error = "finish_file_upload: " + finished.to_string();
                return false;
            }
            file_step = FileStep::WaitResult;
        }
        chast::FileResult result;
        while (session->take_file_result(result)) {
            ++stats.file_results;
            stats.file_result_codes.push_back(result.code);
            stats.file_result_received.push_back(result.received);
            stats.file_result_reasons.push_back(result.reason);
            if (result.transfer_id != stage.offer.transfer_id) {
                ++stats.file_errors;
                stats.error = "the host reported a result for another transfer";
                return false;
            }
            if (result.code != stage.result || result.received != stage.result_received) {
                ++stats.file_errors;
                stats.error = std::string("the ") + stage.label + " upload ended as " +
                              std::to_string(static_cast<int>(result.code)) + " after " +
                              std::to_string(result.received) + " bytes, expected " +
                              std::to_string(static_cast<int>(stage.result)) + " after " +
                              std::to_string(stage.result_received);
                return false;
            }
            if (!options.quiet) {
                if (result.code == chast::FileCode::Ok) {
                    std::printf("quic: host stored the %s file (%llu bytes)\n",
                                stage.label,
                                static_cast<unsigned long long>(result.received));
                } else {
                    std::printf("quic: host refused the %s upload as planned: %s\n",
                                stage.label, result.reason.c_str());
                }
            }
            ++file_stage_index;
            file_step = file_stage_index < file_stages.size() ? FileStep::Offer
                                                              : FileStep::Done;
        }
        return true;
    };

    // ---- file download probe (verify --file-download) ----
    // The host offers one file through the same channel the upload uses, with
    // the roles reversed. The viewer accepts, collects the bytes in memory,
    // compares them with the offered digest and reports the result.
    stats.download_probe = options.file_download;
    bool download_have_offer = false;
    bool download_done = false;
    uint32_t download_id = 0;
    uint64_t download_size = 0;
    std::array<uint8_t, 32> download_digest{};
    std::vector<uint8_t> download_content;
    auto advance_file_download = [&]() -> bool {
        if (!stats.download_probe || download_done) {
            return true;
        }
        if (!download_have_offer) {
            chast::FileOffer offer;
            while (session->take_file_offer(offer)) {
                if (download_have_offer) {
                    continue;
                }
                if (!info.file_transfer_enabled) {
                    stats.download_error = "the host offered a file transfer it did not announce";
                    return false;
                }
                ++stats.download_offers;
                download_id = offer.transfer_id;
                download_size = offer.size;
                download_digest = offer.sha256;
                download_content.clear();
                download_have_offer = true;
                if (chast::Error answered =
                        session->send_file_answer(download_id, chast::FileCode::Ok);
                    !answered.ok()) {
                    stats.download_error = "send_file_answer: " + answered.to_string();
                    return false;
                }
                if (!options.quiet) {
                    std::printf("quic: accepting host download %s (%llu bytes)\n",
                                offer.name.c_str(),
                                static_cast<unsigned long long>(offer.size));
                }
            }
        }
        if (!download_have_offer) {
            return true;
        }
        while (true) {
            chast::FileChunk chunk;
            chast::Error received =
                session->receive_file_chunk(chunk, std::chrono::milliseconds(0));
            if (received.status() == chast::Status::WaitTimeout ||
                received.status() == chast::Status::Unavailable) {
                break;
            }
            if (!received.ok()) {
                stats.download_error = "receive_file_chunk: " + received.to_string();
                return false;
            }
            if (chunk.transfer_id != download_id) {
                continue;
            }
            if (chunk.offset != download_content.size()) {
                stats.download_error = "the host's download arrived out of order";
                return false;
            }
            download_content.insert(download_content.end(), chunk.data.begin(),
                                    chunk.data.end());
            stats.download_bytes += chunk.data.size();
        }
        chast::FileUploadState state;
        if (!session->file_upload_state(download_id, state)) {
            // The answer is on its way and the host has not opened the CFIL
            // stream yet; the transfer becomes visible when it does.
            return true;
        }
        if (!state.finished) {
            if (!state.error.empty()) {
                stats.download_error = "the download stream broke: " + state.error;
                return false;
            }
            return true;
        }
        bool ok = download_content.size() == download_size;
        std::array<uint8_t, 32> digest{};
        if (ok && !chast::sha256(download_content.data(), download_content.size(), digest)) {
            ok = false;
            stats.download_error = "could not hash the received file";
        } else if (ok && digest != download_digest) {
            ok = false;
            stats.download_error = "the received bytes do not match the offered digest";
        }
        const chast::FileCode code = ok ? chast::FileCode::Ok : chast::FileCode::DigestMismatch;
        if (chast::Error reported =
                session->send_file_result(download_id, code, download_content.size(),
                                          stats.download_error);
            !reported.ok()) {
            stats.download_error = "send_file_result: " + reported.to_string();
            return false;
        }
        stats.download_ok = ok;
        stats.download_finished = true;
        download_done = true;
        if (!options.quiet) {
            std::printf("quic: host download %s (%llu bytes)%s\n",
                        ok ? "verified" : "rejected",
                        static_cast<unsigned long long>(download_content.size()),
                        ok ? "" : (" reason: " + stats.download_error).c_str());
        }
        return ok;
    };

    std::unique_ptr<chast::IAudioDecoder> audio_decoder;
    auto ensure_audio_decoder = [&](const chast::AudioInfo& audio) -> bool {
        if (!options.audio || !audio.enabled) {
            return true;
        }
        if (audio_decoder && audio_decoder->sample_rate() == audio.sample_rate &&
            audio_decoder->channels() == audio.channels) {
            return true;
        }
        audio_decoder.reset();
        std::string audio_error;
        audio_decoder = chast::create_audio_decoder(audio, &audio_error);
        if (!audio_decoder) {
            stats.error = "audio decoder creation failed: " + audio_error;
            return false;
        }
        return true;
    };
    if (options.audio) {
        if (!info.audio.enabled) {
            return fail("the server did not announce audio (serve with --audio)");
        }
        if (!ensure_audio_decoder(info.audio)) {
            return fail(stats.error);
        }
    }
    if (options.microphone && !info.microphone_enabled) {
        return fail("the server did not grant the microphone (serve with --microphone)");
    }
    std::unique_ptr<chast::IAudioPlayer> audio_player;
    if (options.play) {
        chast::AudioPlayerConfig player_config;
        std::string player_error;
        audio_player = chast::create_audio_player(player_config, &player_error);
        if (!audio_player) {
            return fail("audio player creation failed: " + player_error);
        }
        if (chast::Error started = audio_player->start(); !started.ok()) {
            return fail("audio player start failed: " + started.to_string());
        }
    }

    chast::DecoderConfig decoder_config;
    decoder_config.codec = info.codec;
    std::unique_ptr<chast::IVideoDecoder> decoder =
        chast::create_decoder(decoder_config, &error);
    if (!decoder) {
        return fail("decoder creation failed: " + error);
    }
    stats.decoder = decoder->implementation();

    FILE* sink = nullptr;
    if (!options.save.empty() &&
        (fopen_s(&sink, options.save.c_str(), "wb") != 0 || !sink)) {
        return fail("could not open " + options.save);
    }

    const auto start = std::chrono::steady_clock::now();
    // A finite window bounds a stalled peer. The extra 20 s covers the
    // sender's own duration and the closing Stats/Bye exchange, so the normal
    // end is always the server's FIN, not this deadline.
    const double window = options.seconds > 0.0 ? options.seconds + 20.0 : 0.0;
    const auto deadline =
        window > 0.0
            ? start + std::chrono::milliseconds(static_cast<long long>(window * 1000.0))
            : (std::chrono::steady_clock::time_point::max)();
    auto last_frame = start;
    auto last_keyframe_request = start - std::chrono::seconds(10);
    auto next_keyframe_request =
        start + std::chrono::milliseconds(options.keyframe_request_ms);
    uint64_t expected_seq = 0;
    std::vector<chast::DecodedFrame> frames;
    bool saw_video_end = false;
    auto last_probe = start - std::chrono::seconds(1);
    bool clock_synced = false;
    int64_t clock_offset_us = 0;

    // verify covers the mid-session switch: one request for the output that is
    // already streaming (the host answers Ok without rebuilding), then one past
    // the end of the announced list (NotFound). They are spaced wider than the
    // host's 1 s switch rate limit.
    if (options.mode == "verify" && !info.displays.empty()) {
        if (options.merged) {
            // Asking for the merge that is already running must be a no-op.
            stats.switch_ok_index = chast::kDisplayAll;
        } else {
            size_t target = options.display_set ? options.display : 0;
            if (target >= info.displays.size()) {
                target = 0;
            }
            stats.switch_ok_index = static_cast<uint16_t>(target);
        }
        const size_t past = info.displays.size() + 2;
        if (past < chast::kMaxDisplayCount) {
            stats.switch_bad_index = static_cast<uint16_t>(past);
        }
    }
    bool switch_ok_sent = false;
    bool switch_bad_sent = false;
    auto switch_bad_at = start + std::chrono::milliseconds(2400);

    // StreamInfo and the pictures travel on different QUIC streams, so the
    // first picture at a new size can arrive before its announcement. Track
    // what the decoder was built for instead of what was last announced: a
    // frame header that got here first can rebuild it, and the announcement
    // that follows then finds it already current.
    chast::VideoCodec decoder_codec = info.codec;
    uint32_t decoder_width = info.width;
    uint32_t decoder_height = info.height;
    auto rebuild_decoder = [&](chast::VideoCodec codec, uint32_t width,
                               uint32_t height) -> bool {
        if (decoder_codec == codec && decoder_width == width && decoder_height == height) {
            return true;
        }
        decoder.reset();
        chast::DecoderConfig updated_decoder;
        updated_decoder.codec = codec;
        decoder = chast::create_decoder(updated_decoder, &error);
        if (!decoder) {
            stats.error = "decoder recreation failed: " + error;
            return false;
        }
        stats.decoder = decoder->implementation();
        decoder_codec = codec;
        decoder_width = width;
        decoder_height = height;
        last_keyframe_request = start - std::chrono::seconds(10);
        return true;
    };

    // Applies every queued StreamInfo. Returns false with stats.error set when
    // a replacement decoder cannot be created.
    auto drain_stream_updates = [&]() -> bool {
        chast::ServerInfo update;
        while (session->take_stream_update(update)) {
            ++stats.stream_updates;
            const bool reconfigure = update.codec != decoder_codec ||
                                     update.width != decoder_width ||
                                     update.height != decoder_height;
            info = update;
            stats.last_update = update;
            stats.codec = info.codec;
            stats.width = info.width;
            stats.height = info.height;
            stats.source_width = info.source_width;
            stats.source_height = info.source_height;
            stats.origin_x = info.origin_x;
            stats.origin_y = info.origin_y;
            stats.displays = info.displays;
            stats.input_enabled = info.input_enabled;
            stats.clipboard_enabled = info.clipboard_enabled;
            stats.target_bitrate = info.bitrate;
            stats.framerate = info.framerate;
            stats.audio_info = info.audio;
            stats.audio_announced = info.audio.enabled;
            if (!ensure_audio_decoder(info.audio)) {
                return false;
            }
            if (reconfigure && !rebuild_decoder(info.codec, info.width, info.height)) {
                return false;
            }
            if (!options.quiet) {
                std::printf("quic: stream description updated: %s %ux%u from %ux%u at %d,%d\n",
                            chast_tools::codec_name(info.codec), info.width, info.height,
                            info.source_width, info.source_height, info.origin_x,
                            info.origin_y);
            }
        }
        return true;
    };

    auto drain_clipboard = [&] {
        chast::ClipboardContent content;
        while (session->take_clipboard_content(content)) {
            ++stats.clipboard_received;
            const std::string text = content.text();
            stats.clipboard_contents.push_back(content);
            if (!options.quiet) {
                std::printf("clipboard: rich formats=%zu text=%s\n", content.formats.size(),
                            text.c_str());
            }
        }
        chast::ClipboardUpdate update;
        while (session->take_clipboard_update(update)) {
            ++stats.clipboard_received;
            if (!options.quiet) {
                std::printf("clipboard: seq=%llu text=%s\n",
                            static_cast<unsigned long long>(update.seq), update.text.c_str());
            }
        }
    };

    // Only the newest pointer state matters, so take_cursor_update() replaces
    // anything not yet read instead of queueing it.
    auto drain_cursor = [&] {
        chast::CursorUpdate update;
        while (session->take_cursor_update(update)) {
            ++stats.cursor_updates;
            if (!update.sprite.empty()) {
                ++stats.cursor_shapes;
            }
            stats.cursor_visible = update.visible;
            stats.cursor_x = update.x;
            stats.cursor_y = update.y;
        }
    };

    auto drain_desktop_state = [&] {
        chast::DesktopKind kind = chast::DesktopKind::Default;
        while (session->take_desktop_state(kind)) {
            ++stats.desktop_states;
            stats.desktop_kind = kind;
        }
    };

    auto drain_privacy_state = [&] {
        bool enabled = false;
        chast::PrivacyModeCode code = chast::PrivacyModeCode::Ok;
        std::string reason;
        while (session->take_privacy_mode_state(enabled, code, reason)) {
            ++stats.privacy_updates;
            stats.privacy_last_enabled = enabled;
            stats.privacy_last_code = code;
            stats.privacy_last_reason = reason;
            if (enabled && code == chast::PrivacyModeCode::Ok) {
                stats.privacy_on_seen = true;
            } else if (!enabled && code == chast::PrivacyModeCode::Ok) {
                stats.privacy_off_seen = true;
            }
            if (!options.quiet) {
                std::printf("quic: privacy mode %s: code %u%s%s\n",
                            enabled ? "on" : "off", static_cast<unsigned>(code),
                            reason.empty() ? "" : ": ", reason.c_str());
            }
        }
    };

    auto drain_sas_result = [&] {
        chast::SasCode code = chast::SasCode::Ok;
        std::string reason;
        while (session->take_sas_result(code, reason)) {
            ++stats.sas_updates;
            stats.sas_last_code = code;
            stats.sas_last_reason = reason;
            if (code == chast::SasCode::Ok) {
                stats.sas_ok_seen = true;
            }
            if (!options.quiet) {
                std::printf("quic: SAS result: code %u%s%s\n",
                            static_cast<unsigned>(code), reason.empty() ? "" : ": ",
                            reason.c_str());
            }
        }
    };

    auto drain_chat = [&] {
        std::string text;
        while (session->take_chat(text)) {
            ++stats.chat_received;
            stats.chat_texts.push_back(text);
            if (!options.quiet) {
                std::printf("quic: chat from host (%zu bytes): %s\n", text.size(),
                            text.c_str());
            }
        }
    };

    auto drain_display_switch_results = [&] {
        uint16_t index = 0;
        chast::DisplaySwitchCode code = chast::DisplaySwitchCode::Ok;
        std::string reason;
        while (session->take_display_switch_result(index, code, reason)) {
            ++stats.display_switch_results;
            stats.last_switch_index = index;
            stats.last_switch_code = code;
            stats.last_switch_reason = reason;
            if (code == chast::DisplaySwitchCode::Ok) {
                ++stats.display_switch_ok;
            } else {
                ++stats.display_switch_refused;
            }
            if (!options.quiet) {
                std::printf("quic: display switch %u: %s%s%s\n", index,
                            code == chast::DisplaySwitchCode::Ok ? "accepted" : "refused",
                            reason.empty() ? "" : ": ", reason.c_str());
            }
        }
    };

    // verify --display-mode drives four requests in order, spaced past the
    // host's one-second rate limit: a no-op for the running mode (Ok without a
    // StreamInfo), a real change, the restore of the original, and a mode the
    // output does not report (refused). Every answer echoes the request, so
    // the probe also proves the channel correlates them.
    enum class ModeStep {
        WaitList,
        SendNoop,
        WaitNoop,
        SendChange,
        WaitChange,
        WaitChangeUpdate,
        SendRestore,
        WaitRestore,
        WaitRestoreUpdate,
        SendRefused,
        WaitRefused,
        SendLeave,
        WaitLeave,
        WaitLeaveUpdate,
        Done,
    };
    stats.mode_probe = options.mode == "verify" && options.display_mode;
    if (stats.mode_probe) {
        stats.mode_display = static_cast<uint16_t>(options.display);
        for (size_t i = 0; i < info.displays.size(); ++i) {
            const chast::DisplayDescription& display = info.displays[i];
            if (display.left == info.origin_x && display.top == info.origin_y &&
                static_cast<uint32_t>(display.right - display.left) == info.source_width &&
                static_cast<uint32_t>(display.bottom - display.top) == info.source_height) {
                stats.mode_display = static_cast<uint16_t>(i);
                break;
            }
        }
    }
    ModeStep mode_step = ModeStep::WaitList;
    auto mode_next_send = start + std::chrono::milliseconds(3200);
    auto mode_update_deadline = mode_next_send;
    bool mode_list_sent = false;
    uint16_t mode_expected_width = 0;
    uint16_t mode_expected_height = 0;
    uint16_t mode_expected_refresh = 0;
    uint64_t mode_updates_before_change = 0;
    uint64_t mode_updates_before_restore = 0;
    uint64_t mode_updates_before_leave = 0;
    auto mode_name = [](uint16_t width, uint16_t height, uint16_t refresh) {
        return std::to_string(width) + "x" + std::to_string(height) + " at " +
               std::to_string(refresh) + " Hz";
    };
    auto advance_mode_probe = [&]() -> bool {
        if (!stats.mode_probe) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        chast::DisplayModeList list;
        while (session->take_display_modes(list)) {
            ++stats.mode_lists;
            stats.modes = list.modes;
            stats.modes_code = list.code;
            stats.modes_error = list.reason;
            if (!options.quiet) {
                std::printf("quic: display modes for %u: %zu listed%s%s\n", list.display,
                            list.modes.size(), list.reason.empty() ? "" : ", ",
                            list.reason.c_str());
            }
            if (list.display != stats.mode_display) {
                stats.error =
                    "the mode list described display " + std::to_string(list.display) +
                    ", not " + std::to_string(stats.mode_display);
                return false;
            }
            if (mode_step != ModeStep::WaitList) {
                continue;
            }
            if (list.code != chast::DisplayModeCode::Ok || list.modes.empty()) {
                stats.error = "the host could not list the display modes" +
                              (list.reason.empty() ? std::string() : ": " + list.reason);
                return false;
            }
            const chast::DisplayModeInfo* original = nullptr;
            for (const chast::DisplayModeInfo& mode : list.modes) {
                if (mode.current) {
                    original = &mode;
                    break;
                }
            }
            if (original == nullptr) {
                stats.error = "the mode list did not mark the running mode";
                return false;
            }
            stats.mode_original_width = original->width;
            stats.mode_original_height = original->height;
            stats.mode_original_refresh = original->refresh_hz;
            // Prefer another pixel size with the same refresh; a refresh-only
            // mode is the fallback when sizes do not vary.
            const chast::DisplayModeInfo* target = nullptr;
            for (const chast::DisplayModeInfo& mode : list.modes) {
                if ((mode.width != original->width || mode.height != original->height) &&
                    mode.refresh_hz == original->refresh_hz) {
                    target = &mode;
                    break;
                }
            }
            if (target == nullptr) {
                for (const chast::DisplayModeInfo& mode : list.modes) {
                    if (mode.width != original->width || mode.height != original->height) {
                        target = &mode;
                        break;
                    }
                }
            }
            if (target == nullptr) {
                for (const chast::DisplayModeInfo& mode : list.modes) {
                    if (mode.refresh_hz != original->refresh_hz &&
                        mode.width == original->width && mode.height == original->height) {
                        target = &mode;
                        break;
                    }
                }
            }
            if (target == nullptr) {
                stats.error = "the display reports no other mode to switch to";
                return false;
            }
            stats.mode_target_width = target->width;
            stats.mode_target_height = target->height;
            stats.mode_target_refresh = target->refresh_hz;
            // A mode outside the enumerated list must be refused.
            stats.mode_bad_width = 9999;
            stats.mode_bad_height = 9999;
            stats.mode_bad_refresh = 60;
            stats.mode_plan_ready = true;
            if (!options.quiet) {
                std::printf("quic: display mode probe: %s -> %s\n",
                            mode_name(stats.mode_original_width, stats.mode_original_height,
                                      stats.mode_original_refresh).c_str(),
                            mode_name(stats.mode_target_width, stats.mode_target_height,
                                      stats.mode_target_refresh).c_str());
            }
            mode_step = ModeStep::SendNoop;
            mode_next_send = now;
        }

        uint16_t answered_index = 0;
        uint16_t answered_width = 0;
        uint16_t answered_height = 0;
        uint16_t answered_refresh = 0;
        chast::DisplayModeCode code = chast::DisplayModeCode::Ok;
        std::string reason;
        while (session->take_display_mode_result(answered_index, answered_width,
                                                 answered_height, answered_refresh, code,
                                                 reason)) {
            ++stats.mode_results;
            stats.last_mode_index = answered_index;
            stats.last_mode_width = answered_width;
            stats.last_mode_height = answered_height;
            stats.last_mode_refresh = answered_refresh;
            stats.last_mode_code = code;
            stats.last_mode_reason = reason;
            if (code == chast::DisplayModeCode::Ok) {
                ++stats.mode_ok;
            } else {
                ++stats.mode_refused;
            }
            if (!options.quiet) {
                std::printf("quic: display mode %ux%u at %u Hz: %s%s%s\n", answered_width,
                            answered_height, answered_refresh,
                            code == chast::DisplayModeCode::Ok ? "accepted" : "refused",
                            reason.empty() ? "" : ": ", reason.c_str());
            }
            if (answered_index != stats.mode_display ||
                answered_width != mode_expected_width ||
                answered_height != mode_expected_height ||
                answered_refresh != mode_expected_refresh) {
                stats.error = "the display mode answer did not echo the request";
                return false;
            }
            switch (mode_step) {
                case ModeStep::WaitNoop:
                    if (code != chast::DisplayModeCode::Ok) {
                        stats.error = "the request for the running mode was refused: " + reason;
                        return false;
                    }
                    mode_step = ModeStep::SendChange;
                    mode_next_send = now + std::chrono::milliseconds(1200);
                    break;
                case ModeStep::WaitChange:
                    if (code != chast::DisplayModeCode::Ok) {
                        stats.error = "the display mode change was refused: " + reason;
                        return false;
                    }
                    mode_update_deadline = now + std::chrono::seconds(2);
                    mode_step = ModeStep::WaitChangeUpdate;
                    break;
                case ModeStep::WaitRestore:
                    if (code != chast::DisplayModeCode::Ok) {
                        stats.error = "restoring the original display mode was refused: " + reason;
                        return false;
                    }
                    mode_update_deadline = now + std::chrono::seconds(2);
                    mode_step = ModeStep::WaitRestoreUpdate;
                    break;
                case ModeStep::WaitRefused:
                    if (code == chast::DisplayModeCode::Ok) {
                        stats.error =
                            "the host accepted a mode the display does not report";
                        return false;
                    }
                    stats.refused_mode_echoed = true;
                    stats.refused_mode_index = answered_index;
                    stats.refused_mode_width = answered_width;
                    stats.refused_mode_height = answered_height;
                    stats.refused_mode_refresh = answered_refresh;
                    stats.refused_mode_reason = reason;
                    // Leave the output changed so the host's end-of-session
                    // guard is the one that puts the original back.
                    mode_step = ModeStep::SendLeave;
                    mode_next_send = now + std::chrono::milliseconds(1200);
                    break;
                case ModeStep::WaitLeave:
                    if (code != chast::DisplayModeCode::Ok) {
                        stats.error = "leaving the display in the new mode was refused: " +
                                      reason;
                        return false;
                    }
                    mode_update_deadline = now + std::chrono::seconds(2);
                    mode_step = ModeStep::WaitLeaveUpdate;
                    break;
                default:
                    stats.error = "an unexpected display mode answer arrived";
                    return false;
            }
        }

        if (mode_step == ModeStep::WaitChangeUpdate) {
            // A StreamInfo may already have been taken while the frame that
            // announced the new size was being decoded, so compare counters
            // instead of waiting for another one to arrive.
            if (stats.stream_updates > mode_updates_before_change) {
                stats.mode_change_update_seen = true;
                stats.mode_change_source_width = stats.source_width;
                stats.mode_change_source_height = stats.source_height;
                mode_step = ModeStep::SendRestore;
                mode_next_send = now + std::chrono::milliseconds(1200);
            } else if (now >= mode_update_deadline) {
                stats.error = "the host never announced the changed source size";
                return false;
            }
        } else if (mode_step == ModeStep::WaitRestoreUpdate) {
            if (stats.stream_updates > mode_updates_before_restore) {
                stats.mode_restore_update_seen = true;
                stats.mode_restore_source_width = stats.source_width;
                stats.mode_restore_source_height = stats.source_height;
                mode_step = ModeStep::SendRefused;
                mode_next_send = now + std::chrono::milliseconds(1200);
            } else if (now >= mode_update_deadline) {
                stats.error = "the host never announced the restored source size";
                return false;
            }
        } else if (mode_step == ModeStep::WaitLeaveUpdate) {
            if (stats.stream_updates > mode_updates_before_leave) {
                stats.mode_leave_update_seen = true;
                stats.mode_leave_source_width = stats.source_width;
                stats.mode_leave_source_height = stats.source_height;
                mode_step = ModeStep::Done;
            } else if (now >= mode_update_deadline) {
                stats.error = "the host never announced the source size it left behind";
                return false;
            }
        }

        if (mode_step == ModeStep::WaitList) {
            if (!mode_list_sent && now >= mode_next_send) {
                mode_list_sent = true;
                if (chast::Error asked = session->request_display_modes(stats.mode_display);
                    !asked.ok()) {
                    stats.error = "request_display_modes: " + asked.to_string();
                    return false;
                }
                if (!options.quiet) {
                    std::printf("quic: asking display %u for its modes\n",
                                stats.mode_display);
                }
            }
            return true;
        }
        if (mode_step == ModeStep::Done || now < mode_next_send) {
            return true;
        }
        switch (mode_step) {
            case ModeStep::SendNoop:
                mode_expected_width = stats.mode_original_width;
                mode_expected_height = stats.mode_original_height;
                mode_expected_refresh = stats.mode_original_refresh;
                mode_step = ModeStep::WaitNoop;
                break;
            case ModeStep::SendChange:
                mode_expected_width = stats.mode_target_width;
                mode_expected_height = stats.mode_target_height;
                mode_expected_refresh = stats.mode_target_refresh;
                mode_updates_before_change = stats.stream_updates;
                mode_step = ModeStep::WaitChange;
                break;
            case ModeStep::SendRestore:
                mode_expected_width = stats.mode_original_width;
                mode_expected_height = stats.mode_original_height;
                mode_expected_refresh = stats.mode_original_refresh;
                mode_updates_before_restore = stats.stream_updates;
                mode_step = ModeStep::WaitRestore;
                break;
            case ModeStep::SendRefused:
                mode_expected_width = stats.mode_bad_width;
                mode_expected_height = stats.mode_bad_height;
                mode_expected_refresh = stats.mode_bad_refresh;
                mode_step = ModeStep::WaitRefused;
                break;
            case ModeStep::SendLeave:
                mode_expected_width = stats.mode_target_width;
                mode_expected_height = stats.mode_target_height;
                mode_expected_refresh = stats.mode_target_refresh;
                mode_updates_before_leave = stats.stream_updates;
                mode_step = ModeStep::WaitLeave;
                break;
            default:
                return true;
        }
        if (chast::Error sent_mode = session->request_set_display_mode(
                stats.mode_display, mode_expected_width, mode_expected_height,
                mode_expected_refresh);
            !sent_mode.ok()) {
            stats.error = "request_set_display_mode: " + sent_mode.to_string();
            return false;
        }
        return true;
    };

    // The first call may wait for the audio FIN that follows the video's; all
    // later calls take whatever is already queued.
    chast::AudioHeader audio_header;
    std::vector<uint8_t> audio_payload;
    uint64_t expected_audio_seq = 0;
    auto count_audio = [&](const std::vector<chast::DecodedAudio>& blocks) {
        for (const chast::DecodedAudio& block : blocks) {
            const size_t channels = block.channels == 0 ? 1 : block.channels;
            stats.audio_samples += block.samples.size() / channels;
            for (int16_t sample : block.samples) {
                stats.audio_energy += static_cast<double>(sample) * sample;
            }
            stats.audio_energy_values += block.samples.size();
        }
    };
    auto play_audio = [&](const std::vector<chast::DecodedAudio>& blocks) {
        if (!audio_player) {
            return;
        }
        for (const chast::DecodedAudio& block : blocks) {
            if (chast::Error pushed = audio_player->push(block); !pushed.ok()) {
                if (stats.audio_player_error.empty()) {
                    stats.audio_player_error = pushed.to_string();
                }
                return;
            }
        }
    };
    auto drain_audio = [&](std::chrono::milliseconds wait) {
        if (!options.audio) {
            return;
        }
        for (;;) {
            chast::Error next = session->receive_audio(audio_header, audio_payload, wait);
            wait = std::chrono::milliseconds(0);
            if (next.status() == chast::Status::WaitTimeout) {
                break;
            }
            if (next.status() == chast::Status::Unavailable) {
                stats.audio_finished = session->audio_finished();
                break;
            }
            if (!next.ok()) {
                ++stats.audio_protocol_errors;
                if (stats.audio_protocol_errors == 1) {
                    std::fprintf(stderr, "audio receive: %s\n", next.to_string().c_str());
                }
                break;
            }
            ++stats.audio_received;
            stats.audio_bytes += audio_payload.size();
            if (audio_header.seq != expected_audio_seq) {
                ++stats.audio_gaps;
            }
            expected_audio_seq = audio_header.seq + 1;
            if (!audio_decoder) {
                continue;
            }
            if (audio_header.payload_size != audio_payload.size() ||
                chast::crc32(audio_payload.data(), audio_payload.size()) !=
                    audio_header.payload_crc32) {
                ++stats.audio_crc_errors;
                continue;
            }
            std::vector<chast::DecodedAudio> blocks;
            chast::Error decoded_audio = audio_decoder->decode(
                audio_header, audio_payload.data(), audio_payload.size(), blocks);
            if (!decoded_audio.ok()) {
                ++stats.audio_decode_errors;
                if (stats.audio_decode_errors == 1) {
                    std::fprintf(stderr, "audio decode: %s\n",
                                 decoded_audio.to_string().c_str());
                }
                continue;
            }
            count_audio(blocks);
            play_audio(blocks);
        }
    };

    // ---- microphone uplink probe (verify --microphone) ----
    // The synthetic source stands in for a WASAPI capture endpoint: mono Opus
    // at 48 kHz. Opening the stream is the switch; closing it must end the
    // host's CMIC stream without touching video or system sound.
    std::unique_ptr<chast::IMicrophoneCapture> microphone_capture;
    if (options.microphone) {
        std::string microphone_error;
        microphone_capture = make_synthetic_microphone_capture(&microphone_error);
        if (!microphone_capture) {
            return fail("synthetic microphone: " + microphone_error);
        }
        stats.microphone_info = microphone_capture->info();
        if (stats.microphone_info.codec != chast::AudioCodec::Opus ||
            stats.microphone_info.sample_rate != 48000 ||
            stats.microphone_info.channels != 1 ||
            stats.microphone_info.frame_samples != chast::kOpusFrameSamples) {
            return fail("the synthetic microphone format is not 48 kHz mono Opus");
        }
    }
    auto drain_microphone = [&] {
        if (!microphone_capture) {
            return;
        }
        chast::EncodedAudio packet;
        while (microphone_capture->take_packet(packet)) {
            chast::AudioHeader header;
            header.codec = stats.microphone_info.codec;
            header.sample_rate = stats.microphone_info.sample_rate;
            header.channels = stats.microphone_info.channels;
            header.frame_samples = stats.microphone_info.frame_samples;
            header.pts_ms = packet.capture_ms;
            header.capture_ms = packet.capture_ms;
            chast::Error sent = session->send_microphone(
                header, packet.data.data(), packet.data.size());
            if (sent.ok()) {
                ++stats.microphone_packets;
                stats.microphone_bytes += packet.data.size();
            } else if (sent.status() == chast::Status::WaitTimeout) {
                ++stats.microphone_dropped;
            } else {
                ++stats.microphone_send_errors;
                if (stats.microphone_send_errors == 1) {
                    std::fprintf(stderr, "send_microphone: %s\n",
                                 sent.to_string().c_str());
                }
            }
        }
        if (microphone_capture->failed()) {
            ++stats.microphone_send_errors;
            stats.error = "microphone capture: " + microphone_capture->error();
        }
    };
    const auto microphone_start_at = start + std::chrono::milliseconds(800);
    const auto microphone_stop_at = start + std::chrono::milliseconds(2500);
    auto stop_microphone_probe = [&] {
        if (!microphone_capture || !stats.microphone_started || stats.microphone_stopped) {
            return;
        }
        drain_microphone();
        session->stop_microphone();
        microphone_capture->stop();
        stats.microphone_dropped += microphone_capture->stats().dropped;
        stats.microphone_stopped = true;
        if (!options.quiet) {
            std::printf("quic: microphone stream closed after %llu packets\n",
                        static_cast<unsigned long long>(stats.microphone_packets));
        }
    };

    // Privacy mode: turn the host's overlay on, hold it long enough to keep
    // decoding frames under it, then turn it off. The answers arrive on the
    // control stream; only the host knows whether the overlay is real.
    stats.privacy_probe = options.mode == "verify" && options.privacy;
    bool privacy_on_sent = false;
    bool privacy_off_sent = false;
    const auto privacy_on_at = start + std::chrono::milliseconds(800);
    const auto privacy_off_at = start + std::chrono::milliseconds(2600);
    // SAS: ask once the stream is live and let the host's answer come back on
    // the control stream. The loopback host answers through its test callback.
    stats.sas_probe = options.mode == "verify" && options.sas;
    bool sas_sent = false;
    const auto sas_at = start + std::chrono::milliseconds(1200);
    // Chat: the host's greeting was queued at the stream announcement; send
    // the reply once the stream is live and drain whatever arrived each pass.
    stats.chat_probe = options.chat_probe != nullptr;
    bool chat_sent = false;
    const auto chat_at = start + std::chrono::milliseconds(1500);

    // Stream preferences: request the encoder already running, change the
    // quality and frame ceiling, restore them, then change the quality alone
    // (the coded sequence must survive it without an IDR on H.264), restore
    // it, and ask for the other encoder family. Each step waits for its answer
    // and, when the change was applied, for the StreamInfo announcing the new
    // picture, so the host is never sent a request it has not caught up with.
    stats.stream_options_probe = options.mode == "verify" && options.stream_options;
    enum class StreamOptionStep {
        WaitStart,
        WaitSameCodec,
        WaitChange,
        WaitChangeUpdate,
        WaitRestore,
        WaitRestoreUpdate,
        WaitRestoreSettle,
        WaitRateOnly,
        WaitRateOnlyUpdate,
        WaitRateOnlyCheck,
        WaitRateOnlyRestore,
        WaitRateOnlyRestoreUpdate,
        WaitAlt,
        WaitAltUpdate,
        WaitAltRestore,
        WaitAltRestoreAnswer,
        WaitAltRestoreUpdate,
        Done,
    };
    StreamOptionStep stream_step = StreamOptionStep::WaitStart;
    const uint16_t probe_fps = 15;
    // The host encodes at this rate until a viewer caps it; zero in the tool
    // means the encoder's own default, which is what the host would report.
    const uint16_t host_fps =
        static_cast<uint16_t>(options.framerate == 0 ? 30 : options.framerate);
    const uint16_t capped_fps = (std::min)(host_fps, probe_fps);
    const chast::VideoCodec stream_base_codec = info.codec;
    const chast::VideoCodec stream_alt_codec = stream_base_codec == chast::VideoCodec::Hevc
                                                   ? chast::VideoCodec::H264
                                                   : chast::VideoCodec::Hevc;
    stats.stream_option_base_codec = stream_base_codec;
    auto stream_next_send = start + std::chrono::milliseconds(3200);
    uint64_t stream_updates_before = 0;
    // Let the IDR the preceding step forced land before sampling the keyframe
    // count for the rate-only step; frames and time keep a slow host from
    // being sampled too early.
    uint64_t stream_settle_frames = 0;
    auto stream_settle_at = start;
    uint64_t stream_rate_frames = 0;
    auto stream_rate_check_at = start;
    uint64_t stream_rate_keyframes_before = 0;
    auto stream_preference = [](chast::VideoCodec codec) {
        return codec == chast::VideoCodec::Hevc ? chast::StreamCodecPreference::Hevc
                                                : chast::StreamCodecPreference::H264;
    };
    auto send_stream_options = [&](chast::StreamCodecPreference codec, uint16_t max_fps,
                                   chast::StreamQuality quality) -> bool {
        chast::StreamOptions request;
        request.codec = codec;
        request.max_fps = max_fps;
        request.quality = quality;
        if (chast::Error sent = session->request_stream_options(request); !sent.ok()) {
            stats.error = "request_stream_options: " + sent.to_string();
            return false;
        }
        stream_updates_before = stats.stream_updates;
        return true;
    };
    auto stream_answer_matches = [](const chast::StreamOptionsResult& result,
                                    chast::VideoCodec codec, uint16_t fps,
                                    chast::StreamQuality quality) {
        return result.codec == codec && result.fps == fps && result.quality == quality;
    };
    auto stream_answer_detail = [](const chast::StreamOptionsResult& result) {
        std::string detail = std::string(chast_tools::codec_name(result.codec)) + " at " +
                             std::to_string(result.fps) + " fps, quality " +
                             std::to_string(static_cast<unsigned>(result.quality));
        if (!result.reason.empty()) {
            detail += ": " + result.reason;
        }
        return detail;
    };
    auto advance_stream_options = [&]() -> bool {
        if (!stats.stream_options_probe || stream_step == StreamOptionStep::Done) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        chast::StreamOptionsResult result;
        while (session->take_stream_options_result(result)) {
            ++stats.stream_option_results;
            stats.stream_option_last_code = result.code;
            stats.stream_option_last_codec = result.codec;
            stats.stream_option_last_fps = result.fps;
            stats.stream_option_last_quality = result.quality;
            stats.stream_option_last_reason = result.reason;
            if (!options.quiet) {
                std::printf("quic: stream options: %s\n", stream_answer_detail(result).c_str());
            }
            if (result.code != chast::StreamOptionsCode::Ok && result.reason.empty()) {
                stats.error = "a stream-option refusal carried no reason";
                return false;
            }
            switch (stream_step) {
                case StreamOptionStep::WaitSameCodec:
                    if (result.code != chast::StreamOptionsCode::Ok) {
                        stats.error =
                            "the request for the running encoder was refused: " + result.reason;
                        return false;
                    }
                    if (!stream_answer_matches(result, stream_base_codec, host_fps,
                                               chast::StreamQuality::High)) {
                        stats.error =
                            "the request for the running encoder was answered with " +
                            stream_answer_detail(result);
                        return false;
                    }
                    if (!send_stream_options(chast::StreamCodecPreference::Auto, probe_fps,
                                             chast::StreamQuality::Balanced)) {
                        return false;
                    }
                    stream_step = StreamOptionStep::WaitChange;
                    break;
                case StreamOptionStep::WaitChange:
                    if (result.code != chast::StreamOptionsCode::Ok) {
                        stats.error =
                            "the quality/frame-rate change was refused: " + result.reason;
                        return false;
                    }
                    if (!stream_answer_matches(result, stream_base_codec, capped_fps,
                                               chast::StreamQuality::Balanced)) {
                        stats.error = "the quality/frame-rate change was answered with " +
                                      stream_answer_detail(result);
                        return false;
                    }
                    stats.stream_option_change_fps = result.fps;
                    stats.stream_option_change_quality = result.quality;
                    stream_step = StreamOptionStep::WaitChangeUpdate;
                    break;
                case StreamOptionStep::WaitRestore:
                    if (result.code != chast::StreamOptionsCode::Ok) {
                        stats.error = "restoring the quality and frame rate was refused: " +
                                      result.reason;
                        return false;
                    }
                    if (!stream_answer_matches(result, stream_base_codec, host_fps,
                                               chast::StreamQuality::High)) {
                        stats.error = "restoring the quality and frame rate was answered with " +
                                      stream_answer_detail(result);
                        return false;
                    }
                    stream_step = StreamOptionStep::WaitRestoreUpdate;
                    break;
                case StreamOptionStep::WaitRateOnly:
                    if (result.code != chast::StreamOptionsCode::Ok) {
                        stats.error =
                            "the quality-only change was refused: " + result.reason;
                        return false;
                    }
                    if (!stream_answer_matches(result, stream_base_codec, host_fps,
                                               chast::StreamQuality::Balanced)) {
                        stats.error = "the quality-only change was answered with " +
                                      stream_answer_detail(result);
                        return false;
                    }
                    stream_step = StreamOptionStep::WaitRateOnlyUpdate;
                    break;
                case StreamOptionStep::WaitRateOnlyRestore:
                    if (result.code != chast::StreamOptionsCode::Ok) {
                        stats.error = "restoring the quality alone was refused: " + result.reason;
                        return false;
                    }
                    if (!stream_answer_matches(result, stream_base_codec, host_fps,
                                               chast::StreamQuality::High)) {
                        stats.error = "restoring the quality alone was answered with " +
                                      stream_answer_detail(result);
                        return false;
                    }
                    stream_step = StreamOptionStep::WaitRateOnlyRestoreUpdate;
                    break;
                case StreamOptionStep::WaitAlt:
                    stats.stream_option_alt_seen = true;
                    if (result.code == chast::StreamOptionsCode::Ok) {
                        if (!stream_answer_matches(result, stream_alt_codec, host_fps,
                                                   chast::StreamQuality::High)) {
                            stats.error = "the other encoder family was answered with " +
                                          stream_answer_detail(result);
                            return false;
                        }
                        stats.stream_option_alt_ok = true;
                        stats.stream_option_alt_codec = result.codec;
                        stream_step = StreamOptionStep::WaitAltUpdate;
                    } else if (result.code == chast::StreamOptionsCode::Unsupported) {
                        if (result.codec != stream_base_codec) {
                            stats.error = "the Unsupported answer described " +
                                          stream_answer_detail(result);
                            return false;
                        }
                        stats.stream_option_alt_unsupported = true;
                        stats.stream_option_alt_reason = result.reason;
                        stream_step = StreamOptionStep::Done;
                    } else {
                        stats.error = "the request for the other encoder family failed: " +
                                      stream_answer_detail(result);
                        return false;
                    }
                    break;
                case StreamOptionStep::WaitAltRestoreAnswer:
                    if (result.code != chast::StreamOptionsCode::Ok) {
                        stats.error = "restoring the original encoder was refused: " +
                                      result.reason;
                        return false;
                    }
                    if (!stream_answer_matches(result, stream_base_codec, host_fps,
                                               chast::StreamQuality::High)) {
                        stats.error = "restoring the original encoder was answered with " +
                                      stream_answer_detail(result);
                        return false;
                    }
                    stream_step = StreamOptionStep::WaitAltRestoreUpdate;
                    break;
                case StreamOptionStep::WaitStart:
                case StreamOptionStep::WaitChangeUpdate:
                case StreamOptionStep::WaitRestoreUpdate:
                case StreamOptionStep::WaitRestoreSettle:
                case StreamOptionStep::WaitRateOnlyUpdate:
                case StreamOptionStep::WaitRateOnlyCheck:
                case StreamOptionStep::WaitRateOnlyRestoreUpdate:
                case StreamOptionStep::WaitAltUpdate:
                case StreamOptionStep::WaitAltRestore:
                case StreamOptionStep::WaitAltRestoreUpdate:
                case StreamOptionStep::Done:
                    // These steps wait on a StreamInfo or on the pacing timer,
                    // not on an answer.
                    break;
            }
        }
        if (stream_step == StreamOptionStep::WaitStart && now >= stream_next_send) {
            if (!send_stream_options(stream_preference(stream_base_codec), 0,
                                     chast::StreamQuality::High)) {
                return false;
            }
            stream_step = StreamOptionStep::WaitSameCodec;
        } else if (stream_step == StreamOptionStep::WaitChangeUpdate &&
                   stats.stream_updates > stream_updates_before) {
            stats.stream_option_change_update_fps = stats.last_update.framerate;
            if (stats.last_update.codec != stream_base_codec ||
                stats.last_update.framerate != capped_fps) {
                stats.error = std::string("the quality/frame-rate change announced ") +
                              chast_tools::codec_name(stats.last_update.codec) + " at " +
                              std::to_string(stats.last_update.framerate) + " fps";
                return false;
            }
            stats.stream_option_change_update_seen = true;
            if (!send_stream_options(chast::StreamCodecPreference::Auto, 0,
                                     chast::StreamQuality::High)) {
                return false;
            }
            stream_step = StreamOptionStep::WaitRestore;
        } else if (stream_step == StreamOptionStep::WaitRestoreUpdate &&
                   stats.stream_updates > stream_updates_before) {
            stats.stream_option_restore_update_fps = stats.last_update.framerate;
            if (stats.last_update.codec != stream_base_codec ||
                stats.last_update.framerate != host_fps) {
                stats.error = std::string("the restored stream was announced as ") +
                              chast_tools::codec_name(stats.last_update.codec) + " at " +
                              std::to_string(stats.last_update.framerate) + " fps";
                return false;
            }
            stats.stream_option_restore_update_seen = true;
            stats.stream_option_restore_update_bitrate = stats.last_update.bitrate;
            // The restore forced an IDR (the frame rate changed). Let it land
            // before the keyframe baseline, so only pictures caused by the
            // rate-only request can move the count afterwards.
            stream_settle_frames = stats.received + 8;
            stream_settle_at = now + std::chrono::milliseconds(350);
            stream_step = StreamOptionStep::WaitRestoreSettle;
        } else if (stream_step == StreamOptionStep::WaitRestoreSettle &&
                   now >= stream_settle_at && stats.received >= stream_settle_frames) {
            stream_rate_keyframes_before = stats.keyframes;
            if (!send_stream_options(chast::StreamCodecPreference::Auto, 0,
                                     chast::StreamQuality::Balanced)) {
                return false;
            }
            stream_step = StreamOptionStep::WaitRateOnly;
        } else if (stream_step == StreamOptionStep::WaitRateOnlyUpdate &&
                   stats.stream_updates > stream_updates_before) {
            if (stats.last_update.codec != stream_base_codec ||
                stats.last_update.framerate != host_fps) {
                stats.error = std::string("the quality-only change re-announced ") +
                              chast_tools::codec_name(stats.last_update.codec) + " at " +
                              std::to_string(stats.last_update.framerate) + " fps";
                return false;
            }
            if (stats.last_update.bitrate >= stats.stream_option_restore_update_bitrate) {
                stats.error = "the Balanced quality step did not lower the announced bitrate";
                return false;
            }
            stats.stream_option_rate_update_seen = true;
            stats.stream_option_rate_update_bitrate = stats.last_update.bitrate;
            stats.stream_option_rate_encoder = stats.last_update.encoder;
            stream_rate_frames = stats.received + 8;
            stream_rate_check_at = now + std::chrono::milliseconds(400);
            stream_step = StreamOptionStep::WaitRateOnlyCheck;
        } else if (stream_step == StreamOptionStep::WaitRateOnlyCheck &&
                   now >= stream_rate_check_at && stats.received >= stream_rate_frames) {
            stats.stream_option_rate_keyframes =
                stats.keyframes - stream_rate_keyframes_before;
            if (!send_stream_options(chast::StreamCodecPreference::Auto, 0,
                                     chast::StreamQuality::High)) {
                return false;
            }
            stream_step = StreamOptionStep::WaitRateOnlyRestore;
        } else if (stream_step == StreamOptionStep::WaitRateOnlyRestoreUpdate &&
                   stats.stream_updates > stream_updates_before) {
            if (stats.last_update.codec != stream_base_codec ||
                stats.last_update.framerate != host_fps ||
                stats.last_update.bitrate <= stats.stream_option_rate_update_bitrate) {
                stats.error = std::string("restoring the quality alone was announced as ") +
                              chast_tools::codec_name(stats.last_update.codec) + " at " +
                              std::to_string(stats.last_update.framerate) + " fps, " +
                              std::to_string(stats.last_update.bitrate / 1000u) + " kbps";
                return false;
            }
            if (!send_stream_options(stream_preference(stream_alt_codec), 0,
                                     chast::StreamQuality::High)) {
                return false;
            }
            stream_step = StreamOptionStep::WaitAlt;
        } else if (stream_step == StreamOptionStep::WaitAltUpdate &&
                   stats.stream_updates > stream_updates_before) {
            if (stats.last_update.codec != stream_alt_codec) {
                stats.error = std::string("the other encoder family was announced as ") +
                              chast_tools::codec_name(stats.last_update.codec);
                return false;
            }
            // The host refuses a second codec rebuild within one second of the
            // first, so the restore waits that cooldown out.
            stream_next_send = now + std::chrono::milliseconds(1200);
            stream_step = StreamOptionStep::WaitAltRestore;
        } else if (stream_step == StreamOptionStep::WaitAltRestore &&
                   now >= stream_next_send) {
            if (!send_stream_options(chast::StreamCodecPreference::Auto, 0,
                                     chast::StreamQuality::High)) {
                return false;
            }
            stream_step = StreamOptionStep::WaitAltRestoreAnswer;
        } else if (stream_step == StreamOptionStep::WaitAltRestoreUpdate &&
                   stats.stream_updates > stream_updates_before) {
            if (stats.last_update.codec != stream_base_codec) {
                stats.error = std::string("the restored encoder was announced as ") +
                              chast_tools::codec_name(stats.last_update.codec);
                return false;
            }
            stats.stream_option_codec_restored = true;
            stream_step = StreamOptionStep::Done;
        }
        return true;
    };

    while (std::chrono::steady_clock::now() < deadline) {
        // The mode probe's answers arrive on the control stream, so take them
        // before waiting on video: a host that is rebuilding capture sends no
        // frames while a mode change is applied.
        if (!advance_mode_probe()) {
            return fail(stats.error);
        }
        if (!drain_stream_updates()) {
            return fail(stats.error);
        }
        if (stats.clipboard_over_limit_sent && !stats.clipboard_followup_sent &&
            std::chrono::steady_clock::now() >= clipboard_followup_at) {
            chast::ClipboardContent followup =
                rich_clipboard_probe(kClipboardAfterOverflow);
            if (chast::Error sent = session->send_clipboard_content(std::move(followup));
                !sent.ok()) {
                ++stats.clipboard_errors;
                if (stats.clipboard_errors == 1) {
                    std::fprintf(stderr, "send_clipboard_content: %s\n",
                                 sent.to_string().c_str());
                }
            } else {
                ++stats.clipboard_sent;
                stats.clipboard_followup_sent = true;
            }
        }
        drain_clipboard();
        if (!advance_file_upload()) {
            return fail(stats.error);
        }
        if (!advance_file_download()) {
            return fail(stats.download_error.empty() ? stats.error : stats.download_error);
        }
        drain_cursor();
        drain_desktop_state();
        drain_privacy_state();
        drain_sas_result();
        drain_chat();
        if (!advance_stream_options()) {
            return fail(stats.error);
        }
        drain_display_switch_results();
        drain_audio(std::chrono::milliseconds(0));
        if (options.microphone && !stats.microphone_started &&
            std::chrono::steady_clock::now() >= microphone_start_at) {
            if (chast::Error started = session->start_microphone(stats.microphone_info);
                !started.ok()) {
                return fail("start_microphone: " + started.to_string());
            }
            stats.microphone_started = true;
            microphone_capture->start();
        }
        if (options.microphone && stats.microphone_started &&
            std::chrono::steady_clock::now() >= microphone_stop_at) {
            stop_microphone_probe();
        }
        if (stats.privacy_probe) {
            const auto now = std::chrono::steady_clock::now();
            if (!privacy_on_sent && now >= privacy_on_at) {
                stats.privacy_luma_before =
                    stats.decoded > 0 ? stats.last_frame_luma : -1.0;
                if (chast::Error sent = session->set_privacy_mode(true); !sent.ok()) {
                    return fail("set_privacy_mode: " + sent.to_string());
                }
                privacy_on_sent = true;
            } else if (privacy_on_sent && !privacy_off_sent && stats.privacy_on_seen &&
                       now >= privacy_off_at) {
                if (chast::Error sent = session->set_privacy_mode(false); !sent.ok()) {
                    return fail("set_privacy_mode: " + sent.to_string());
                }
                privacy_off_sent = true;
            }
        }
        if (stats.sas_probe && !sas_sent && std::chrono::steady_clock::now() >= sas_at) {
            if (chast::Error requested = session->request_sas(); !requested.ok()) {
                return fail("request_sas: " + requested.to_string());
            }
            sas_sent = true;
        }
        if (stats.chat_probe && !chat_sent &&
            std::chrono::steady_clock::now() >= chat_at) {
            chat_sent = true;
            if (chast::Error sent = session->send_chat(kViewerChatLine); !sent.ok()) {
                ++stats.chat_errors;
                std::fprintf(stderr, "send_chat: %s\n", sent.to_string().c_str());
            } else {
                ++stats.chat_sent;
            }
        }
        drain_microphone();
        {
            const auto probe_now = std::chrono::steady_clock::now();
            if (probe_now - last_probe >= std::chrono::seconds(1)) {
                last_probe = probe_now;
                ++stats.probes_sent;
                if (chast::Error probed = session->send_latency_probe(); !probed.ok()) {
                    ++stats.probe_errors;
                    if (stats.probe_errors == 1) {
                        std::fprintf(stderr, "latency probe: %s\n",
                                     probed.to_string().c_str());
                    }
                } else {
                    stats.latency = session->latency();
                    if (stats.latency.valid) {
                        clock_synced = true;
                        clock_offset_us = stats.latency.offset_us;
                        if (audio_player) {
                            audio_player->set_clock_offset(stats.latency.offset_us / 1000);
                        }
                    }
                }
            }
        }
        chast::FrameHeader header;
        std::vector<uint8_t> payload;
        // Control slots keep only the newest message, so a long wait on video
        // can swallow an announcement that a later one replaces. Wake often
        // enough that each StreamInfo and switch answer is taken on its own.
        chast::Error received =
            session->receive_frame(header, payload, std::chrono::milliseconds(250));
        // Audio is on its own stream and does not wake this call; drain it
        // right after every video wait so it never queues behind a stall.
        drain_audio(std::chrono::milliseconds(0));
        drain_microphone();
        drain_privacy_state();
        drain_sas_result();
        drain_chat();
        if (!advance_stream_options()) {
            return fail(stats.error);
        }
        drain_display_switch_results();
        if (received.status() == chast::Status::WaitTimeout) {
            if (session->async_error().ok() && !session->video_finished() &&
                std::chrono::steady_clock::now() - last_frame < std::chrono::seconds(30)) {
                continue;
            }
            saw_video_end = session->video_finished();
            if (!saw_video_end) {
                const chast::Error async = session->async_error();
                stats.error = async.ok() ? "no frames arrived for 30 s" : async.to_string();
            }
            break;
        }
        if (received.status() == chast::Status::Unavailable) {
            saw_video_end = session->video_finished();
            if (!saw_video_end) {
                const chast::Error async = session->async_error();
                stats.error = async.ok() ? "the connection closed before the video stream "
                                           "ended"
                                         : async.to_string();
            }
            break;
        }
        if (!received.ok()) {
            return fail("receive: " + received.to_string());
        }

        ++stats.received;
        last_frame = std::chrono::steady_clock::now();

        if (header.capture_ms == 0) {
            ++stats.frames_without_capture;
        } else if (clock_synced) {
            const int64_t age_us = steady_clock_us() + clock_offset_us -
                                   static_cast<int64_t>(header.capture_ms) * 1000;
            // A wild value means the offset does not belong to this clock pair
            // (or the frame is a replay); do not fold it into the reported age.
            if (age_us > -100000 && age_us < 10000000) {
                stats.last_frame_age_ms =
                    static_cast<double>(std::max<int64_t>(age_us, 0)) / 1000.0;
                stats.max_frame_age_ms =
                    (std::max)(stats.max_frame_age_ms, stats.last_frame_age_ms);
                if (!options.quiet && stats.last_frame_age_ms > 1000.0) {
                    const double elapsed = std::chrono::duration<double>(
                                               std::chrono::steady_clock::now() - start)
                                               .count();
                    std::fprintf(stderr,
                                 "frame age: %.0f ms at seq %llu (%ux%u) t=%.1f s\n",
                                 stats.last_frame_age_ms,
                                 static_cast<unsigned long long>(header.seq), header.width,
                                 header.height, elapsed);
                }
            }
        }

        if (options.keyframe_request_ms > 0 &&
            last_frame >= next_keyframe_request) {
            session->request_keyframe();
            next_keyframe_request = last_frame +
                std::chrono::milliseconds(options.keyframe_request_ms);
        }
        if (!switch_ok_sent && stats.switch_ok_index != chast::kDisplayAuto &&
            last_frame - start >= std::chrono::milliseconds(1200)) {
            switch_ok_sent = true;
            if (chast::Error sent_switch =
                    session->request_display_switch(stats.switch_ok_index);
                !sent_switch.ok()) {
                return fail("request_display_switch: " + sent_switch.to_string());
            }
            // The refusal has to reach the host a full rate-limit window after
            // the accepted request. Measuring from the send rather than from
            // start keeps them apart when a frame stall delays both.
            switch_bad_at = (std::max)(switch_bad_at,
                                       last_frame + std::chrono::milliseconds(1200));
        }
        if (!switch_bad_sent && stats.switch_bad_index != chast::kDisplayAuto &&
            last_frame >= switch_bad_at) {
            switch_bad_sent = true;
            if (chast::Error sent_switch =
                    session->request_display_switch(stats.switch_bad_index);
                !sent_switch.ok()) {
                return fail("request_display_switch: " + sent_switch.to_string());
            }
        }
        if (header.seq != expected_seq) {
            ++stats.gaps;
            if (stats.gaps == 1 && !options.quiet) {
                std::fprintf(stderr, "sequence gap: expected %llu, got %llu\n",
                             static_cast<unsigned long long>(expected_seq),
                             static_cast<unsigned long long>(header.seq));
            }
        }
        expected_seq = header.seq + 1;

        if (header.codec != info.codec || header.width != info.width ||
            header.height != info.height) {
            // Look for the announcement that belongs to this picture before
            // treating the disagreement as a violation.
            if (!drain_stream_updates()) {
                return fail(stats.error);
            }
        }
        if (header.codec != info.codec || header.width != info.width ||
            header.height != info.height) {
            // The announcement still lost the race. The frame header names the
            // new stream precisely, so rebuild for it and decode this unit; the
            // StreamInfo that follows will find the decoder current.
            if (!rebuild_decoder(header.codec, header.width, header.height)) {
                return fail(stats.error);
            }
            if (!options.quiet) {
                std::printf("quic: stream is %s %ux%u before its announcement\n",
                            chast_tools::codec_name(header.codec), header.width, header.height);
            }
        }
        if (chast::crc32(payload.data(), payload.size()) != header.payload_crc32) {
            ++stats.crc_errors;
            if (stats.crc_errors == 1) {
                std::fprintf(stderr, "CRC mismatch on frame %llu\n",
                             static_cast<unsigned long long>(header.seq));
            }
            continue;
        }
        if (sink) {
            fwrite(payload.data(), 1, payload.size(), sink);
        }
        stats.bytes += payload.size();
        if (header.keyframe) {
            ++stats.keyframes;
        }

        frames.clear();
        chast::Error decoded =
            decoder->decode(payload.data(), payload.size(), header.pts_ms, header.keyframe,
                            frames);
        if (!decoded.ok()) {
            if (!decoder->initialized()) {
                // The stream has not reached its first sequence header. Ask
                // for an IDR instead of treating the delta frames as damage.
                if (std::chrono::steady_clock::now() - last_keyframe_request >=
                    std::chrono::seconds(1)) {
                    last_keyframe_request = std::chrono::steady_clock::now();
                    session->request_keyframe();
                }
                continue;
            }
            ++stats.decode_errors;
            if (stats.decode_errors == 1) {
                std::fprintf(stderr, "decode failed on frame %llu: %s\n",
                             static_cast<unsigned long long>(header.seq),
                             decoded.to_string().c_str());
            }
            if (stats.decode_errors >= 30) {
                return fail("too many decode failures");
            }
            continue;
        }
        for (const chast::DecodedFrame& frame : frames) {
            ++stats.decoded;
            stats.digest = chast::mix_frame_hash(stats.digest, frame.hash);
            stats.width = frame.width;
            stats.height = frame.height;
            if (stats.privacy_probe) {
                stats.last_frame_luma = frame.luma_mean;
                if (privacy_on_sent && stats.privacy_on_seen && !privacy_off_sent) {
                    ++stats.privacy_frames;
                    stats.privacy_luma_last = frame.luma_mean;
                } else if (privacy_off_sent && stats.privacy_off_seen) {
                    ++stats.privacy_frames_after_off;
                }
            }
        }
    }

    std::vector<chast::DecodedFrame> tail;
    drain_clipboard();
    drain_cursor();
    drain_desktop_state();
    drain_privacy_state();
    drain_sas_result();
    drain_chat();
    stop_microphone_probe();
    if (stats.file_probe && file_step != FileStep::Done) {
        // The result normally arrives while video is still streaming; if the
        // stream ended first, give the host time to answer the remaining
        // offers, including the refused ones.
        const auto file_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (file_step != FileStep::Done &&
               std::chrono::steady_clock::now() < file_deadline) {
            if (!advance_file_upload()) {
                break;
            }
            if (file_step != FileStep::Done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (file_step != FileStep::Done && stats.error.empty()) {
            stats.error = "the file upload did not finish";
        }
    }
    if (stats.download_probe && !download_done) {
        // The host hashes, offers and streams from its own frame loop; give it
        // the same grace after video ends that the upload probe gets.
        const auto download_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!download_done && std::chrono::steady_clock::now() < download_deadline) {
            if (!advance_file_download()) {
                break;
            }
            if (!download_done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (!download_done && stats.error.empty() && stats.download_error.empty()) {
            stats.error = "the host download did not finish";
        }
    }
    if (options.audio) {
        // The server FINs the audio stream right after the video's; wait
        // briefly for the tail and its closing packet before finishing.
        const auto audio_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!stats.audio_finished && std::chrono::steady_clock::now() < audio_deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                audio_deadline - std::chrono::steady_clock::now());
            if (left <= std::chrono::milliseconds(0)) {
                break;
            }
            drain_audio((std::min)(left, std::chrono::milliseconds(200)));
        }
        if (audio_decoder) {
            std::vector<chast::DecodedAudio> tail_audio;
            if (audio_decoder->flush(tail_audio).ok()) {
                count_audio(tail_audio);
                play_audio(tail_audio);
            } else {
                ++stats.audio_decode_errors;
            }
        }
    }
    if (audio_player) {
        const chast::AudioPlayerStats played = audio_player->stats();
        stats.audio_played = played.played_frames;
        stats.audio_underruns = played.underruns;
        stats.audio_player_drops = played.dropped;
        stats.audio_player_recoveries = played.recoveries;
        stats.audio_buffer_ms = played.buffer_ms;
        if (stats.audio_player_error.empty() && !audio_player->error().empty()) {
            stats.audio_player_error = audio_player->error();
        }
        audio_player->stop();
    }
    chast::Error flushed = decoder->flush(tail);
    if (flushed.ok()) {
        for (const chast::DecodedFrame& frame : tail) {
            ++stats.decoded;
            stats.digest = chast::mix_frame_hash(stats.digest, frame.hash);
        }
    } else if (decoder->initialized()) {
        ++stats.decode_errors;
    }
    if (sink) {
        fclose(sink);
    }

    const uint64_t elapsed_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
    const uint32_t audio_rate =
        stats.audio_info.sample_rate != 0 ? stats.audio_info.sample_rate : 48000;
    stats.elapsed_seconds = static_cast<double>(elapsed_ms) / 1000.0;
    stats.audio_seconds =
        static_cast<double>(stats.audio_samples) / static_cast<double>(audio_rate);
    if (stats.audio_energy_values != 0) {
        stats.audio_rms = std::sqrt(stats.audio_energy /
                                    static_cast<double>(stats.audio_energy_values));
    }
    // Drain the last reply that may have arrived with the final frames.
    stats.latency = session->latency();
    chast::ClientStats closing;
    closing.received = stats.received;
    closing.decoded = stats.decoded;
    closing.crc_errors = stats.crc_errors;
    closing.decode_errors = stats.decode_errors;
    closing.digest = stats.digest;
    closing.elapsed_ms = elapsed_ms;
    chast::Error finished = session->finish_client(closing, std::chrono::seconds(10));
    if (!finished.ok() && stats.error.empty()) {
        stats.error = "finish: " + finished.to_string();
    }

    if (!options.quiet || !stats.error.empty()) {
        std::printf("receiver: decoder=%s codec=%s transport=%s size=%ux%u source=%ux%u "
                    "received=%llu decoded=%llu keyframes=%llu crc-errors=%llu "
                    "decode-errors=%llu gaps=%llu protocol-errors=%llu stream-updates=%llu "
                    "bytes=%llu digest=%016llX elapsed=%llu ms%s\n",
                    stats.decoder.c_str(), chast_tools::codec_name(stats.codec),
                    stats.transport.c_str(), stats.width, stats.height, stats.source_width,
                    stats.source_height,
                    static_cast<unsigned long long>(stats.received),
                    static_cast<unsigned long long>(stats.decoded),
                    static_cast<unsigned long long>(stats.keyframes),
                    static_cast<unsigned long long>(stats.crc_errors),
                    static_cast<unsigned long long>(stats.decode_errors),
                    static_cast<unsigned long long>(stats.gaps),
                    static_cast<unsigned long long>(stats.protocol_errors),
                    static_cast<unsigned long long>(stats.stream_updates),
                    static_cast<unsigned long long>(stats.bytes),
                    static_cast<unsigned long long>(stats.digest),
                    static_cast<unsigned long long>(elapsed_ms),
                    saw_video_end ? "" : " (stream did not end cleanly)");
        if (!options.quiet) {
            std::printf("receiver: rtt=%.1f ms offset=%.1f ms probes=%llu replies=%llu "
                        "frame-age=%.1f ms max-frame-age=%.1f ms target-bitrate=%u kbps "
                        "uncaptured=%llu\n",
                        stats.latency.valid ? static_cast<double>(stats.latency.rtt_us) / 1000.0
                                            : 0.0,
                        stats.latency.valid
                            ? static_cast<double>(stats.latency.offset_us) / 1000.0
                            : 0.0,
                        static_cast<unsigned long long>(stats.probes_sent),
                        static_cast<unsigned long long>(stats.latency.replies),
                        stats.last_frame_age_ms, stats.max_frame_age_ms,
                        stats.target_bitrate / 1000u,
                        static_cast<unsigned long long>(stats.frames_without_capture));
            if (options.clipboard) {
                std::printf("receiver: clipboard sent=%llu received=%llu errors=%llu\n",
                            static_cast<unsigned long long>(stats.clipboard_sent),
                            static_cast<unsigned long long>(stats.clipboard_received),
                            static_cast<unsigned long long>(stats.clipboard_errors));
            }
            if (stats.chat_probe) {
                std::printf("receiver: chat sent=%llu received=%llu errors=%llu\n",
                            static_cast<unsigned long long>(stats.chat_sent),
                            static_cast<unsigned long long>(stats.chat_received),
                            static_cast<unsigned long long>(stats.chat_errors));
            }
            std::printf("receiver: cursor updates=%llu shapes=%llu visible=%d at=%d,%d\n",
                        static_cast<unsigned long long>(stats.cursor_updates),
                        static_cast<unsigned long long>(stats.cursor_shapes),
                        stats.cursor_visible ? 1 : 0, stats.cursor_x, stats.cursor_y);
            if (options.audio) {
                std::printf("receiver: audio announced=%d finished=%d packets=%llu bytes=%llu "
                            "gaps=%llu crc-errors=%llu decode-errors=%llu samples=%llu "
                            "(%.2f s) rms=%.0f\n",
                            stats.audio_announced ? 1 : 0, stats.audio_finished ? 1 : 0,
                            static_cast<unsigned long long>(stats.audio_received),
                            static_cast<unsigned long long>(stats.audio_bytes),
                            static_cast<unsigned long long>(stats.audio_gaps),
                            static_cast<unsigned long long>(stats.audio_crc_errors),
                            static_cast<unsigned long long>(stats.audio_decode_errors),
                            static_cast<unsigned long long>(stats.audio_samples),
                            stats.audio_seconds, stats.audio_rms);
                if (options.play) {
                    std::printf("receiver: playback played=%llu frames underruns=%llu "
                                "dropped=%llu recoveries=%llu buffer=%u ms%s%s\n",
                                static_cast<unsigned long long>(stats.audio_played),
                                static_cast<unsigned long long>(stats.audio_underruns),
                                static_cast<unsigned long long>(stats.audio_player_drops),
                                static_cast<unsigned long long>(stats.audio_player_recoveries),
                                stats.audio_buffer_ms,
                                stats.audio_player_error.empty() ? "" : " error=",
                                stats.audio_player_error.c_str());
                }
            }
            if (options.microphone) {
                std::printf("receiver: microphone announced=%d started=%d stopped=%d "
                            "packets=%llu bytes=%llu dropped=%llu send-errors=%llu "
                            "format=%s %u Hz %u ch\n",
                            stats.microphone_announced ? 1 : 0,
                            stats.microphone_started ? 1 : 0,
                            stats.microphone_stopped ? 1 : 0,
                            static_cast<unsigned long long>(stats.microphone_packets),
                            static_cast<unsigned long long>(stats.microphone_bytes),
                            static_cast<unsigned long long>(stats.microphone_dropped),
                            static_cast<unsigned long long>(stats.microphone_send_errors),
                            audio_codec_name(stats.microphone_info.codec),
                            stats.microphone_info.sample_rate,
                            stats.microphone_info.channels);
            }
        }
    }
    if (carrier_bridge && !options.quiet) {
        const chast::CarrierBridgeStats carrier_stats = carrier_bridge->stats();
        std::printf("carrier: %s datagrams out=%llu in=%llu bytes out=%llu in=%llu%s%s\n",
                    stats.transport.c_str(),
                    static_cast<unsigned long long>(carrier_stats.to_carrier_datagrams),
                    static_cast<unsigned long long>(carrier_stats.from_carrier_datagrams),
                    static_cast<unsigned long long>(carrier_stats.to_carrier_bytes),
                    static_cast<unsigned long long>(carrier_stats.from_carrier_bytes),
                    carrier_bridge->error().empty() ? "" : " stopped: ",
                    carrier_bridge->error().c_str());
    }
    stats.ok = stats.error.empty() && stats.received > 0 && stats.decoded > 0 &&
               stats.crc_errors == 0 && stats.decode_errors == 0 && stats.gaps == 0 &&
               stats.protocol_errors == 0 && stats.input_errors == 0 &&
               stats.clipboard_errors == 0 && saw_video_end;
    if (options.audio) {
        const double video_seconds = static_cast<double>(elapsed_ms) / 1000.0;
        stats.ok = stats.ok && stats.audio_announced && stats.audio_finished &&
                   stats.audio_received > 0 && stats.audio_crc_errors == 0 &&
                   stats.audio_decode_errors == 0 && stats.audio_gaps == 0 &&
                   stats.audio_protocol_errors == 0 &&
                   stats.audio_seconds > video_seconds - 1.0 &&
                   stats.audio_seconds < video_seconds + 1.0 && stats.audio_rms > 1000.0 &&
                   stats.audio_rms < 15000.0;
    }
    if (options.play) {
        stats.ok = stats.ok && stats.audio_player_error.empty() && stats.audio_played > 0;
    }
    if (options.microphone) {
        stats.ok = stats.ok && stats.microphone_announced && stats.microphone_started &&
                   stats.microphone_stopped && stats.microphone_packets > 0 &&
                   stats.microphone_send_errors == 0;
    }
    return stats;
}

struct StreamDigest {
    uint64_t units = 0;
    uint64_t decoded = 0;
    uint64_t errors = 0;
    uint64_t digest = chast::kFrameDigestSeed;
    UINT width = 0;
    UINT height = 0;
    std::string error;
};

// Replays a saved elementary stream the way chast-decode does: split at AUDs,
// decode each access unit, fold the picture hashes in order.
StreamDigest digest_saved_stream(const std::string& path, chast::VideoCodec codec) {
    StreamDigest result;
    FILE* input = nullptr;
    if (fopen_s(&input, path.c_str(), "rb") != 0 || !input) {
        result.error = "cannot open " + path;
        return result;
    }
    std::vector<uint8_t> data;
    uint8_t chunk[64 * 1024];
    for (;;) {
        const size_t got = fread(chunk, 1, sizeof(chunk), input);
        data.insert(data.end(), chunk, chunk + got);
        if (got != sizeof(chunk)) {
            break;
        }
    }
    fclose(input);

    const std::vector<chast_tools::AccessUnit> units =
        chast_tools::split_access_units(data, codec);
    result.units = units.size();
    if (units.empty()) {
        result.error = "the saved stream has no access units";
        return result;
    }

    chast::DecoderConfig config;
    config.codec = codec;
    std::string error;
    std::unique_ptr<chast::IVideoDecoder> decoder = chast::create_decoder(config, &error);
    if (!decoder) {
        result.error = "decoder creation failed: " + error;
        return result;
    }

    std::vector<chast::DecodedFrame> frames;
    for (size_t i = 0; i < units.size(); ++i) {
        frames.clear();
        chast::Error decoded = decoder->decode(units[i].bytes.data(), units[i].bytes.size(),
                                               static_cast<uint64_t>(i) * 33,
                                               units[i].keyframe, frames);
        if (!decoded.ok() && units[i].keyframe && decoder->initialized()) {
            // A resolution change arrives as a key frame carrying a new
            // sequence header, which the live receiver applies by rebuilding
            // its decoder. This replay has no StreamInfo, so retry the unit on
            // a fresh decoder: the key frame is a complete re-entry point.
            decoder = chast::create_decoder(config, &error);
            if (!decoder) {
                result.error = "decoder recreation failed: " + error;
                return result;
            }
            frames.clear();
            decoded = decoder->decode(units[i].bytes.data(), units[i].bytes.size(),
                                      static_cast<uint64_t>(i) * 33, units[i].keyframe,
                                      frames);
        }
        if (!decoded.ok()) {
            ++result.errors;
            if (!decoder->initialized()) {
                continue;
            }
        }
        for (const chast::DecodedFrame& frame : frames) {
            ++result.decoded;
            result.digest = chast::mix_frame_hash(result.digest, frame.hash);
            result.width = frame.width;
            result.height = frame.height;
        }
    }
    std::vector<chast::DecodedFrame> tail;
    if (decoder->flush(tail).ok()) {
        for (const chast::DecodedFrame& frame : tail) {
            ++result.decoded;
            result.digest = chast::mix_frame_hash(result.digest, frame.hash);
        }
    }
    return result;
}

// Starts a second server and connects to it with a deliberately wrong pin.
// The TLS callback must refuse the certificate before any Hello is exchanged.
bool pinned_client_rejects_other_identity(uint16_t port, std::string* detail) {
    chast::QuicServerConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.port = port;
    server_config.idle_timeout_ms = 3000;
    std::string error;
    std::unique_ptr<chast::QuicSession> server =
        chast::QuicSession::create_server(server_config, &error);
    if (!server) {
        *detail = "server: " + error;
        return false;
    }
    std::thread waiting([&] { server->wait_for_client(std::chrono::seconds(3)); });

    chast::QuicClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = port;
    client_config.idle_timeout_ms = 3000;
    client_config.pinned_fingerprint.assign(64, '0');
    std::unique_ptr<chast::QuicSession> client =
        chast::QuicSession::create_client(client_config, &error);
    bool rejected = false;
    if (!client) {
        *detail = "client: " + error;
    } else {
        const chast::Error connected = client->connect(std::chrono::seconds(5));
        rejected = !connected.ok();
        *detail = connected.to_string();
    }
    waiting.join();
    return rejected;
}

struct ProbeClient {
    bool ok = false;
    chast::Status status = chast::Status::Ok;
    std::string error;
};

// Runs one full connect + Hello against `port`.
ProbeClient probe_handshake(uint16_t port, const std::string& pin, const std::string& token,
                            std::chrono::milliseconds timeout) {
    ProbeClient result;
    chast::QuicClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = port;
    client_config.idle_timeout_ms = 3000;
    client_config.pinned_fingerprint = pin;
    client_config.access_token = token;
    std::string error;
    std::unique_ptr<chast::QuicSession> client =
        chast::QuicSession::create_client(client_config, &error);
    if (!client) {
        result.error = "client: " + error;
        return result;
    }
    const chast::Error connected = client->connect(std::chrono::seconds(5));
    if (!connected.ok()) {
        result.status = connected.status();
        result.error = connected.to_string();
        return result;
    }
    chast::ServerInfo info;
    const chast::Error handshaken = client->handshake(info, timeout, chast::kDisplayAuto);
    result.ok = handshaken.ok();
    result.status = handshaken.status();
    result.error = handshaken.to_string();
    return result;
}

// Token and approval, on their own server so the main loopback stays a plain
// accepted session: a wrong token is refused with a reason, then an operator
// who declines the first viewer and accepts the second gets exactly that.
bool access_control_works(uint16_t port, std::string* detail) {
    {
        chast::QuicServerConfig server_config;
        server_config.bind_address = "127.0.0.1";
        server_config.port = port;
        server_config.idle_timeout_ms = 3000;
        server_config.access_token = "the-right-token";
        std::string error;
        std::unique_ptr<chast::QuicSession> server =
            chast::QuicSession::create_server(server_config, &error);
        if (!server) {
            *detail = "server: " + error;
            return false;
        }
        const std::string pin = server->server_fingerprint();
        std::thread waiting([&] { server->wait_for_client(std::chrono::seconds(2)); });
        const ProbeClient client =
            probe_handshake(port, pin, "the-wrong-token", std::chrono::seconds(3));
        waiting.join();
        if (client.status != chast::Status::PermissionDenied ||
            client.error.find("token") == std::string::npos) {
            *detail = "wrong token was not refused by name: " + client.error;
            return false;
        }
    }

    const uint16_t approval_port = static_cast<uint16_t>(port + 1);
    chast::QuicServerConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.port = approval_port;
    server_config.idle_timeout_ms = 3000;
    server_config.access_token = "approval-token";
    int decisions = 0;
    server_config.approve_client = [&decisions](const chast::ClientRequest& request) {
        return chast::ClientApproval{++decisions > 1, request.permissions};
    };
    std::string error;
    std::unique_ptr<chast::QuicSession> server =
        chast::QuicSession::create_server(server_config, &error);
    if (!server) {
        *detail = "approval server: " + error;
        return false;
    }
    const std::string pin = server->server_fingerprint();
    std::thread serving([&] {
        if (!server->wait_for_client(std::chrono::seconds(15)).ok()) {
            return;
        }
        chast::ServerInfo info;
        info.width = 64;
        info.height = 64;
        info.source_width = 64;
        info.source_height = 64;
        info.framerate = 30;
        info.bitrate = 100000;
        info.encoder = "approval probe";
        server->announce(info);
    });
    const ProbeClient declined =
        probe_handshake(approval_port, pin, "approval-token", std::chrono::seconds(3));
    ProbeClient approved;
    // The refusal closes the old connection and frees the slot on the
    // server's thread; retrying absorbs that hand-off.
    const auto approval_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!approved.ok && std::chrono::steady_clock::now() < approval_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        approved = probe_handshake(approval_port, pin, "approval-token",
                                   std::chrono::seconds(3));
    }
    serving.join();
    if (declined.status != chast::Status::PermissionDenied ||
        declined.error.find("declined") == std::string::npos) {
        *detail = "the declined viewer was not told why: " + declined.error;
        return false;
    }
    if (!approved.ok) {
        *detail = "the approved viewer did not connect: " + approved.error;
        return false;
    }
    if (decisions != 2) {
        *detail = "the approval callback ran " + std::to_string(decisions) + " times";
        return false;
    }

    // A one-time token is spent by the first accepted Hello: the next viewer
    // presenting it is refused by name rather than silently failing.
    const uint16_t spent_port = static_cast<uint16_t>(port + 2);
    {
        chast::QuicServerConfig spent_config;
        spent_config.bind_address = "127.0.0.1";
        spent_config.port = spent_port;
        spent_config.idle_timeout_ms = 3000;
        spent_config.access_token = "one-shot";
        std::string spent_error;
        std::unique_ptr<chast::QuicSession> spent =
            chast::QuicSession::create_server(spent_config, &spent_error);
        if (!spent) {
            *detail = "one-time server: " + spent_error;
            return false;
        }
        const std::string spent_pin = spent->server_fingerprint();
        std::thread spending([&] {
            if (!spent->wait_for_client(std::chrono::seconds(10)).ok()) {
                return;
            }
            chast::ServerInfo info;
            info.width = 64;
            info.height = 64;
            info.source_width = 64;
            info.source_height = 64;
            info.framerate = 30;
            info.bitrate = 100000;
            info.encoder = "one-time probe";
            spent->announce(info);
            // The first viewer leaves on its own; waiting again re-arms the
            // session so the second Hello reaches the token check.
            spent->wait_for_client(std::chrono::seconds(5));
        });
        const ProbeClient first =
            probe_handshake(spent_port, spent_pin, "one-shot", std::chrono::seconds(3));
        ProbeClient second;
        const auto spent_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (second.status != chast::Status::PermissionDenied &&
               std::chrono::steady_clock::now() < spent_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            second = probe_handshake(spent_port, spent_pin, "one-shot",
                                     std::chrono::seconds(3));
        }
        spending.join();
        if (!first.ok) {
            *detail = "the first one-time viewer did not connect: " + first.error;
            return false;
        }
        if (second.status != chast::Status::PermissionDenied ||
            second.error.find("already used") == std::string::npos) {
            *detail = "the spent token was not refused by name: " + second.error;
            return false;
        }
    }
    return true;
}

// Authentication and permissions on their own server: a refused password is
// named, approval can only reduce the mask, and a mid-session revocation
// reaches the viewer and gates the transport on both ends (and can be
// restored, but only up to the grant that was approved).
bool auth_and_permissions_work(uint16_t port, std::string* detail) {
    auto make_client = [](uint16_t target_port, const std::string& pin,
                          const std::string& password) {
        chast::QuicClientConfig config;
        config.host = "127.0.0.1";
        config.port = target_port;
        config.idle_timeout_ms = 3000;
        config.pinned_fingerprint = pin;
        config.password = password;
        return config;
    };

    // 1. The policy hook refuses a wrong password with its own code.
    {
        chast::QuicServerConfig server_config;
        server_config.bind_address = "127.0.0.1";
        server_config.port = port;
        server_config.idle_timeout_ms = 3000;
        server_config.allow_input = true;
        server_config.dry_run_input = true;
        server_config.stop_after_denial = true;
        server_config.authenticate = [](const chast::ClientRequest& request) {
            chast::ClientAuthResult result;
            if (request.hello.password != "open-sesame") {
                result.denial = chast::AccessDenial::WrongPassword;
                result.reason = "the password was wrong";
                return result;
            }
            result.action = chast::ClientAuthAction::Allow;
            result.permissions = chast::kPermissionInput | chast::kPermissionClipboard;
            return result;
        };
        std::string error;
        std::unique_ptr<chast::QuicSession> server =
            chast::QuicSession::create_server(server_config, &error);
        if (!server) {
            *detail = "password server: " + error;
            return false;
        }
        const std::string pin = server->server_fingerprint();
        chast::Status wait_status = chast::Status::Ok;
        std::string wait_error;
        std::thread waiting([&] {
            const chast::Error waited = server->wait_for_client(std::chrono::seconds(10));
            wait_status = waited.status();
            wait_error = waited.message();
        });
        chast::QuicClientConfig client_config = make_client(port, pin, "wrong");
        std::unique_ptr<chast::QuicSession> client =
            chast::QuicSession::create_client(client_config, &error);
        chast::ServerInfo info;
        chast::Error refused =
            client ? client->connect(std::chrono::seconds(5))
                   : chast::Error(chast::Status::InvalidCall, "client: " + error);
        if (refused.ok()) {
            refused = client->handshake(info, std::chrono::seconds(5));
        }
        const std::optional<chast::AccessDenial> code =
            client ? client->denial() : std::nullopt;
        client.reset();
        waiting.join();
        if (refused.status() != chast::Status::PermissionDenied ||
            code != chast::AccessDenial::WrongPassword ||
            refused.message().find("password was wrong") == std::string::npos) {
            *detail = "the wrong password was not named: " + refused.to_string() +
                      (code ? " (code " + std::to_string(static_cast<int>(*code)) + ")"
                            : " (no denial code)") +
                      ", server wait: " + chast::to_string(wait_status) + " " + wait_error;
            return false;
        }
        if (wait_status != chast::Status::PermissionDenied) {
            *detail = "stop_after_denial did not end the wait: " + chast::to_string(wait_status) +
                      " " + wait_error;
            return false;
        }
    }

    // 2. Approval returns a smaller mask than authentication allowed.
    {
        const uint16_t approval_port = static_cast<uint16_t>(port + 1);
        chast::QuicServerConfig server_config;
        server_config.bind_address = "127.0.0.1";
        server_config.port = approval_port;
        server_config.idle_timeout_ms = 3000;
        server_config.allow_input = true;
        server_config.allow_clipboard = true;
        server_config.dry_run_input = true;
        server_config.authenticate = [](const chast::ClientRequest& request) {
            chast::ClientAuthResult result;
            result.action = request.hello.password == "open-sesame"
                                ? chast::ClientAuthAction::RequireApproval
                                : chast::ClientAuthAction::Deny;
            result.permissions = chast::kPermissionInput | chast::kPermissionClipboard;
            return result;
        };
        server_config.approve_client = [](const chast::ClientRequest&) {
            // The operator allows input only.
            return chast::ClientApproval{true, chast::kPermissionInput};
        };
        std::string error;
        std::unique_ptr<chast::QuicSession> server =
            chast::QuicSession::create_server(server_config, &error);
        if (!server) {
            *detail = "approval server: " + error;
            return false;
        }
        const std::string pin = server->server_fingerprint();
        std::thread serving([&] {
            if (!server->wait_for_client(std::chrono::seconds(10)).ok()) {
                return;
            }
            chast::ServerInfo info;
            info.width = 64;
            info.height = 64;
            info.source_width = 64;
            info.source_height = 64;
            info.framerate = 30;
            info.bitrate = 100000;
            info.encoder = "approval mask probe";
            server->announce(info);
        });
        chast::QuicClientConfig client_config = make_client(approval_port, pin, "open-sesame");
        std::unique_ptr<chast::QuicSession> client =
            chast::QuicSession::create_client(client_config, &error);
        chast::ServerInfo info;
        chast::Error handshaken =
            client ? client->connect(std::chrono::seconds(5))
                   : chast::Error(chast::Status::InvalidCall, "client: " + error);
        if (handshaken.ok()) {
            handshaken = client->handshake(info, std::chrono::seconds(5));
        }
        serving.join();
        if (!handshaken.ok()) {
            *detail = "the approval viewer did not connect: " + handshaken.to_string();
            return false;
        }
        if (!info.input_enabled || info.clipboard_enabled) {
            *detail = "the approval mask was not applied to HelloAck";
            return false;
        }
        if (server->permissions() != chast::kPermissionInput) {
            *detail = "the server kept a permission the operator did not allow";
            return false;
        }
        if (client->send_clipboard(1, "not allowed").status() !=
            chast::Status::PermissionDenied) {
            *detail = "the viewer sent clipboard without permission";
            return false;
        }
    }

    // 3. Live revocation and restore inside the approved ceiling.
    {
        const uint16_t live_port = static_cast<uint16_t>(port + 2);
        chast::QuicServerConfig server_config;
        server_config.bind_address = "127.0.0.1";
        server_config.port = live_port;
        server_config.idle_timeout_ms = 3000;
        server_config.allow_input = true;
        server_config.allow_clipboard = true;
        server_config.dry_run_input = true;
        server_config.authenticate = [](const chast::ClientRequest&) {
            chast::ClientAuthResult result;
            result.action = chast::ClientAuthAction::Allow;
            result.permissions = chast::kPermissionInput | chast::kPermissionClipboard;
            return result;
        };
        std::string error;
        std::unique_ptr<chast::QuicSession> server =
            chast::QuicSession::create_server(server_config, &error);
        if (!server) {
            *detail = "permission server: " + error;
            return false;
        }
        const std::string pin = server->server_fingerprint();
        bool live_ok = true;
        std::string live_error;
        std::thread serving([&] {
            if (!server->wait_for_client(std::chrono::seconds(10)).ok()) {
                live_ok = false;
                live_error = "the viewer did not connect";
                return;
            }
            chast::ServerInfo info;
            info.width = 64;
            info.height = 64;
            info.source_width = 64;
            info.source_height = 64;
            info.framerate = 30;
            info.bitrate = 100000;
            info.encoder = "permission probe";
            if (chast::Error announced = server->announce(info); !announced.ok()) {
                live_ok = false;
                live_error = "announce: " + announced.to_string();
                return;
            }
            // Give the client time to leave handshake() before the update.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (chast::Error revoked =
                    server->set_permissions(chast::kPermissionClipboard);
                !revoked.ok()) {
                live_ok = false;
                live_error = "revoke input: " + revoked.to_string();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (chast::Error restored = server->set_permissions(
                    chast::kPermissionInput | chast::kPermissionClipboard);
                !restored.ok()) {
                live_ok = false;
                live_error = "restore input: " + restored.to_string();
                return;
            }
            // A mask above the approved ceiling is clamped, not sent.
            server->set_permissions(chast::kPermissionAll);
            if (server->permissions() !=
                (chast::kPermissionInput | chast::kPermissionClipboard)) {
                live_ok = false;
                live_error = "the ceiling did not clamp an oversized mask";
            }
        });

        chast::QuicClientConfig client_config = make_client(live_port, pin, "open-sesame");
        std::unique_ptr<chast::QuicSession> client =
            chast::QuicSession::create_client(client_config, &error);
        chast::ServerInfo info;
        bool client_ok = client && client->connect(std::chrono::seconds(5)).ok() &&
                         client->handshake(info, std::chrono::seconds(5)).ok();
        auto wait_for_mask = [&](uint16_t expected) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                uint16_t mask = 0;
                if (client && client->take_permission_update(mask) && mask == expected) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return false;
        };
        auto mouse_move = [] {
            chast::InputEvent event;
            event.kind = chast::InputKind::Mouse;
            event.mouse.action = chast::MouseAction::Move;
            event.mouse.x = 10;
            event.mouse.y = 10;
            return event;
        };
        if (!client_ok) {
            *detail = "the live-permission viewer did not connect";
            client.reset();
            serving.join();
            return false;
        }
        if (!client->send_input(mouse_move()).ok()) {
            *detail = "the initial input grant was not usable";
            client.reset();
            serving.join();
            return false;
        }
        if (!wait_for_mask(chast::kPermissionClipboard)) {
            *detail = "the viewer did not see the input revocation";
            client.reset();
            serving.join();
            return false;
        }
        if (client->send_input(mouse_move()).status() != chast::Status::PermissionDenied) {
            *detail = "the viewer sent input after the revocation";
            client.reset();
            serving.join();
            return false;
        }
        if (!wait_for_mask(chast::kPermissionInput | chast::kPermissionClipboard)) {
            *detail = "the viewer did not see the input restored";
            client.reset();
            serving.join();
            return false;
        }
        if (!client->send_input(mouse_move()).ok()) {
            *detail = "input stayed gated after the restore";
            client.reset();
            serving.join();
            return false;
        }
        client.reset();
        serving.join();
        if (!live_ok) {
            *detail = live_error;
            return false;
        }
    }
    return true;
}

// The operator prompt blocks wait_for_client() on the host thread. A viewer
// whose handshake times out disconnects while that prompt is still open; the
// transport must expose that through ClientRequest::peer_alive, refuse the
// dead session even if the prompt answers "allow", and keep the listener ready
// for the next client.
bool approval_liveness_works(uint16_t port, std::string* detail) {
    chast::QuicServerConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.port = port;
    server_config.idle_timeout_ms = 3000;
    server_config.allow_input = true;
    server_config.dry_run_input = true;
    server_config.authenticate = [](const chast::ClientRequest&) {
        chast::ClientAuthResult result;
        result.action = chast::ClientAuthAction::RequireApproval;
        result.permissions = chast::kPermissionInput;
        return result;
    };
    std::atomic<int> approvals{0};
    std::atomic<bool> saw_peer_gone{false};
    server_config.approve_client = [&](const chast::ClientRequest& request) {
        if (approvals.fetch_add(1) == 0) {
            // Stand in for an operator who takes longer than the viewer's
            // handshake timeout: wait until the transport reports the peer
            // gone, then approve anyway. The transport owns the final check.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (request.peer_alive && request.peer_alive() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            saw_peer_gone = !request.peer_alive || !request.peer_alive();
        }
        return chast::ClientApproval{true, request.permissions};
    };
    std::string error;
    std::unique_ptr<chast::QuicSession> server =
        chast::QuicSession::create_server(server_config, &error);
    if (!server) {
        *detail = "approval-liveness server: " + error;
        return false;
    }
    const std::string pin = server->server_fingerprint();
    auto make_client = [&] {
        chast::QuicClientConfig config;
        config.host = "127.0.0.1";
        config.port = port;
        config.idle_timeout_ms = 3000;
        config.pinned_fingerprint = pin;
        return config;
    };

    chast::Error first_wait = chast::Error::success();
    chast::Error second_wait = chast::Error::success();
    std::atomic<bool> second_wait_started{false};
    bool second_announced = false;
    std::thread serving([&] {
        first_wait = server->wait_for_client(std::chrono::seconds(3));
        second_wait_started = true;
        second_wait = server->wait_for_client(std::chrono::seconds(5));
        if (second_wait.ok()) {
            chast::ServerInfo info;
            info.width = 64;
            info.height = 64;
            info.source_width = 64;
            info.source_height = 64;
            info.framerate = 30;
            info.bitrate = 100000;
            info.encoder = "approval liveness probe";
            second_announced = server->announce(info).ok();
        }
    });

    // The first viewer gives up on the handshake and disconnects while the
    // approval callback is still waiting.
    std::unique_ptr<chast::QuicSession> first =
        chast::QuicSession::create_client(make_client(), &error);
    chast::ServerInfo info;
    chast::Error handshaken =
        first ? first->connect(std::chrono::seconds(5))
              : chast::Error(chast::Status::InvalidCall, "client: " + error);
    if (handshaken.ok()) {
        handshaken = first->handshake(info, std::chrono::milliseconds(800));
    }
    first.reset();

    const auto started_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!second_wait_started && std::chrono::steady_clock::now() < started_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!second_wait_started) {
        serving.join();
        *detail = "the listener did not return to waiting after a dead approval";
        return false;
    }

    // A second viewer must still be served: the dead session released its
    // connection slot instead of leaving the listener stuck.
    std::unique_ptr<chast::QuicSession> second =
        chast::QuicSession::create_client(make_client(), &error);
    chast::ServerInfo second_info;
    bool second_ok = second && second->connect(std::chrono::seconds(5)).ok() &&
                     second->handshake(second_info, std::chrono::seconds(5)).ok();
    second.reset();
    serving.join();

    if (handshaken.ok()) {
        *detail = "the first viewer's handshake succeeded while approval was pending";
        return false;
    }
    if (!saw_peer_gone) {
        *detail = "the approval request never saw the viewer leave";
        return false;
    }
    if (approvals.load() != 2) {
        *detail = "the approval callback ran " + std::to_string(approvals.load()) +
                  " times, expected 2";
        return false;
    }
    if (first_wait.status() != chast::Status::WaitTimeout) {
        *detail = "a viewer that left during approval was admitted: " +
                  chast::to_string(first_wait.status()) + " " + first_wait.message();
        return false;
    }
    if (!second_wait.ok() || !second_announced || !second_ok) {
        *detail = "the listener did not serve the next viewer: second wait " +
                  second_wait.to_string() +
                  (second_ok ? "" : ", client handshake failed") +
                  (second_announced ? "" : ", announce failed");
        return false;
    }
    return true;
}

// A server whose clipboard gate is closed must not send text and must drop a
// viewer's updates instead of failing the session.
bool clipboard_gate_works(uint16_t port, std::string* detail) {
    chast::QuicServerConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.port = port;
    server_config.idle_timeout_ms = 3000;
    server_config.allow_clipboard = false;
    std::string error;
    std::unique_ptr<chast::QuicSession> server =
        chast::QuicSession::create_server(server_config, &error);
    if (!server) {
        *detail = "server: " + error;
        return false;
    }
    const std::string pin = server->server_fingerprint();
    std::thread serving([&] {
        if (!server->wait_for_client(std::chrono::seconds(5)).ok()) {
            return;
        }
        chast::ServerInfo info;
        info.width = 64;
        info.height = 64;
        info.source_width = 64;
        info.source_height = 64;
        info.framerate = 30;
        info.bitrate = 100000;
        info.encoder = "clipboard probe";
        server->announce(info);
    });

    chast::QuicClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = port;
    client_config.idle_timeout_ms = 3000;
    client_config.pinned_fingerprint = pin;
    std::unique_ptr<chast::QuicSession> client =
        chast::QuicSession::create_client(client_config, &error);
    if (!client) {
        *detail = "client: " + error;
        serving.join();
        return false;
    }
    if (chast::Error connected = client->connect(std::chrono::seconds(5)); !connected.ok()) {
        *detail = "connect: " + connected.to_string();
        serving.join();
        return false;
    }
    chast::ServerInfo info;
    if (chast::Error handshaken =
            client->handshake(info, std::chrono::seconds(5), chast::kDisplayAuto);
        !handshaken.ok()) {
        *detail = "handshake: " + handshaken.to_string();
        serving.join();
        return false;
    }
    if (info.clipboard_enabled) {
        *detail = "HelloAck announced clipboard despite the closed gate";
        serving.join();
        return false;
    }
    if (server->send_clipboard(1, "server must not send").status() !=
        chast::Status::PermissionDenied) {
        *detail = "the closed server accepted its own clipboard send";
        serving.join();
        return false;
    }
    if (client->send_clipboard(1, "viewer update is dropped").status() !=
        chast::Status::PermissionDenied) {
        *detail = "the viewer sent despite the closed gate";
        serving.join();
        return false;
    }
    // The control stream and the server's handler run on transport threads;
    // give them a moment to deliver and drop the message.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (server->clipboard_updates_received() != 0) {
        *detail = "the closed server queued a viewer update";
        serving.join();
        return false;
    }
    serving.join();
    return true;
}

// A server with the audio gate closed must announce it closed, refuse to send
// audio and never open the stream.
bool audio_gate_works(uint16_t port, std::string* detail) {
    chast::QuicServerConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.port = port;
    server_config.idle_timeout_ms = 3000;
    server_config.allow_audio = false;
    std::string error;
    std::unique_ptr<chast::QuicSession> server =
        chast::QuicSession::create_server(server_config, &error);
    if (!server) {
        *detail = "server: " + error;
        return false;
    }
    const std::string pin = server->server_fingerprint();
    std::thread serving([&] {
        if (!server->wait_for_client(std::chrono::seconds(5)).ok()) {
            return;
        }
        chast::ServerInfo info;
        info.width = 64;
        info.height = 64;
        info.source_width = 64;
        info.source_height = 64;
        info.framerate = 30;
        info.bitrate = 100000;
        info.encoder = "audio probe";
        // The caller asks for audio; the session's gate must still turn it off.
        info.audio.enabled = true;
        info.audio.bitrate = 160000;
        server->announce(info);
    });

    chast::QuicClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = port;
    client_config.idle_timeout_ms = 3000;
    client_config.pinned_fingerprint = pin;
    std::unique_ptr<chast::QuicSession> client =
        chast::QuicSession::create_client(client_config, &error);
    if (!client) {
        *detail = "client: " + error;
        serving.join();
        return false;
    }
    if (chast::Error connected = client->connect(std::chrono::seconds(5)); !connected.ok()) {
        *detail = "connect: " + connected.to_string();
        serving.join();
        return false;
    }
    chast::ServerInfo info;
    if (chast::Error handshaken =
            client->handshake(info, std::chrono::seconds(5), chast::kDisplayAuto);
        !handshaken.ok()) {
        *detail = "handshake: " + handshaken.to_string();
        serving.join();
        return false;
    }
    if (info.audio.enabled) {
        *detail = "HelloAck announced audio despite the closed gate";
        serving.join();
        return false;
    }
    const uint8_t silence[16] = {};
    chast::AudioHeader header;
    header.sample_rate = 48000;
    header.channels = 2;
    header.frame_samples = 1024;
    if (server->send_audio(header, silence, sizeof(silence)).status() !=
        chast::Status::InvalidCall) {
        *detail = "the closed server accepted its own audio send";
        serving.join();
        return false;
    }
    // No audio stream was opened, so the client must time out rather than see
    // data or a FIN.
    chast::AudioHeader received_header;
    std::vector<uint8_t> received_payload;
    if (client->receive_audio(received_header, received_payload,
                              std::chrono::milliseconds(150))
            .status() != chast::Status::WaitTimeout) {
        *detail = "the client saw audio data or a FIN from a closed gate";
        serving.join();
        return false;
    }
    serving.join();
    return true;
}

struct ProbePasswordHandshake {
    bool ok = false;
    chast::Status status = chast::Status::Ok;
    std::optional<chast::AccessDenial> denial;
    std::string error;
};

ProbePasswordHandshake probe_password(uint16_t port, const std::string& pin,
                                      const std::string& password,
                                      std::chrono::milliseconds timeout,
                                      const std::string& totp = {},
                                      const chast::DeviceIdentity* identity = nullptr,
                                      bool trust_device = false) {
    ProbePasswordHandshake result;
    chast::QuicClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = port;
    client_config.idle_timeout_ms = 3000;
    client_config.pinned_fingerprint = pin;
    client_config.password = password;
    client_config.totp = totp;
    client_config.device_identity = identity;
    client_config.trust_device = trust_device;
    std::string error;
    std::unique_ptr<chast::QuicSession> client =
        chast::QuicSession::create_client(client_config, &error);
    if (!client) {
        result.error = "client: " + error;
        return result;
    }
    if (chast::Error connected = client->connect(std::chrono::seconds(5)); !connected.ok()) {
        result.status = connected.status();
        result.error = connected.to_string();
        return result;
    }
    chast::ServerInfo info;
    const chast::Error handshaken = client->handshake(info, timeout, chast::kDisplayAuto);
    result.ok = handshaken.ok();
    result.status = handshaken.status();
    result.denial = client->denial();
    result.error = handshaken.to_string();
    return result;
}

// A transport listener whose Hello gate runs the real HostSecurity policy, so
// the probes below exercise signatures, TOTP and lockout across the wire.
struct PolicyServer {
    std::shared_ptr<chast::HostSecurity> security;
    std::unique_ptr<chast::QuicSession> session;
};

std::unique_ptr<PolicyServer> create_policy_server(uint16_t port,
                                                   chast::HostSecurity::Config policy,
                                                   std::atomic<bool>* trusted_device_used,
                                                   std::string* error) {
    auto server = std::make_unique<PolicyServer>();
    server->security = std::make_shared<chast::HostSecurity>(std::move(policy));
    chast::QuicServerConfig quic;
    quic.bind_address = "127.0.0.1";
    quic.port = port;
    quic.idle_timeout_ms = 3000;
    quic.authenticate = [state = server->security,
                         trusted_device_used](const chast::ClientRequest& request) {
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        const chast::HostSecurity::Result result =
            state->evaluate(request.hello, request.peer_address, now);
        chast::ClientAuthResult auth;
        auth.permissions = result.permissions;
        if (result.decision == chast::HostSecurity::Decision::Allow) {
            state->note_connected(result, now);
            if (trusted_device_used && result.via_trusted_device) {
                trusted_device_used->store(true);
            }
            auth.action = chast::ClientAuthAction::Allow;
        } else {
            auth.action = chast::ClientAuthAction::Deny;
            auth.denial = result.denial;
            auth.reason = result.reason;
        }
        return auth;
    };
    server->session = chast::QuicSession::create_server(quic, error);
    if (!server->session) {
        return nullptr;
    }
    // Signed Hellos bind to the certificate this listener presents.
    chast::HostSecurity::Config bound = server->security->config();
    bound.host_fingerprint = server->session->server_fingerprint();
    server->security->set_config(std::move(bound));
    return server;
}

void announce_probe_stream(chast::QuicSession& session, const char* encoder) {
    chast::ServerInfo info;
    info.width = 64;
    info.height = 64;
    info.source_width = 64;
    info.source_height = 64;
    info.framerate = 30;
    info.bitrate = 100000;
    info.encoder = encoder;
    session.announce(info);
}

std::thread serve_policy_client(PolicyServer& server, const char* encoder,
                                std::chrono::milliseconds timeout,
                                std::atomic<bool>* admitted = nullptr) {
    return std::thread([&server, encoder, timeout, admitted] {
        if (!server.session->wait_for_client(timeout).ok()) {
            return;
        }
        if (admitted) {
            admitted->store(true);
        }
        announce_probe_stream(*server.session, encoder);
    });
}

bool wait_host_listening(chast::HostServer& server, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (server.status().state == chast::HostState::Listening) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::unique_ptr<chast::HostServer> start_probe_host(
    uint16_t port, const std::function<void(chast::HostSettings&)>& setup, std::string* error) {
    chast::HostServerConfig host;
    host.settings.bind_address = "127.0.0.1";
    host.settings.port = port;
    host.settings.auth_mode = chast::HostAuthMode::Password;
    host.settings.password_hash = chast::hash_password("open-sesame");
    host.serve_once = true;
    setup(host.settings);
    std::unique_ptr<chast::HostServer> server =
        chast::HostServer::create(std::move(host), error);
    if (!server) {
        return nullptr;
    }
    server->start();
    if (!wait_host_listening(*server, std::chrono::seconds(5))) {
        *error = "the host did not start listening";
        server->stop();
        return nullptr;
    }
    return server;
}

// A host whose microphone gate is closed must not announce the grant, must
// refuse the viewer's CMIC open with PermissionDenied, and must keep streaming
// video afterwards. Real capture is used for the video half, so a rejected
// microphone cannot be mistaken for a broken stream.
bool microphone_gate_works(uint16_t port, std::string* detail) {
    std::string error;
    std::unique_ptr<chast::HostServer> host = start_probe_host(
        port, [](chast::HostSettings&) {}, &error);
    if (!host) {
        *detail = "microphone-disabled host: " + error;
        return false;
    }
    chast::QuicClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = port;
    client_config.idle_timeout_ms = 5000;
    client_config.pinned_fingerprint = host->status().fingerprint;
    client_config.password = "open-sesame";
    std::unique_ptr<chast::QuicSession> client =
        chast::QuicSession::create_client(client_config, &error);
    if (!client) {
        *detail = "client: " + error;
        host->stop();
        return false;
    }
    chast::ServerInfo info;
    chast::Error connected = client->connect(std::chrono::seconds(10));
    chast::Error handshaken =
        connected.ok() ? client->handshake(info, std::chrono::seconds(10),
                                           chast::kDisplayAuto)
                       : connected;
    if (!handshaken.ok()) {
        *detail = "the viewer did not connect: " + handshaken.to_string();
        client.reset();
        host->stop();
        return false;
    }
    if (info.microphone_enabled) {
        *detail = "HelloAck granted the microphone despite the closed gate";
        client.reset();
        host->stop();
        return false;
    }
    std::unique_ptr<chast::IMicrophoneCapture> capture =
        make_synthetic_microphone_capture(&error);
    if (!capture) {
        *detail = "synthetic microphone: " + error;
        client.reset();
        host->stop();
        return false;
    }
    const chast::Error opened = client->start_microphone(capture->info());
    if (opened.status() != chast::Status::PermissionDenied) {
        *detail = "the viewer opened CMIC without permission: " + opened.to_string();
        client.reset();
        host->stop();
        return false;
    }
    // The refusal must not have disturbed the video stream.
    chast::FrameHeader header;
    std::vector<uint8_t> payload;
    const chast::Error received =
        client->receive_frame(header, payload, std::chrono::seconds(10));
    const bool frame_ok =
        received.ok() && !payload.empty() && payload.size() == header.payload_size &&
        chast::crc32(payload.data(), payload.size()) == header.payload_crc32;
    client.reset();
    host->stop();
    if (!frame_ok) {
        *detail = "video did not survive the refused microphone: " +
                  (received.ok() ? std::string("frame payload did not verify")
                                 : received.to_string());
        return false;
    }
    return true;
}

// A host whose privacy gate is closed must not announce the permission, must
// refuse the viewer's request with PermissionDenied before anything reaches
// the wire, and must keep streaming video afterwards. Real capture is used so
// the closed gate cannot be mistaken for a broken stream.
bool privacy_gate_works(uint16_t port, std::string* detail) {
    std::string error;
    std::unique_ptr<chast::HostServer> host = start_probe_host(
        port, [](chast::HostSettings&) {}, &error);
    if (!host) {
        *detail = "privacy-disabled host: " + error;
        return false;
    }
    chast::QuicClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = port;
    client_config.idle_timeout_ms = 5000;
    client_config.pinned_fingerprint = host->status().fingerprint;
    client_config.password = "open-sesame";
    std::unique_ptr<chast::QuicSession> client =
        chast::QuicSession::create_client(client_config, &error);
    if (!client) {
        *detail = "client: " + error;
        host->stop();
        return false;
    }
    chast::ServerInfo info;
    chast::Error connected = client->connect(std::chrono::seconds(10));
    chast::Error handshaken =
        connected.ok() ? client->handshake(info, std::chrono::seconds(10),
                                           chast::kDisplayAuto)
                       : connected;
    if (!handshaken.ok()) {
        *detail = "the viewer did not connect: " + handshaken.to_string();
        client.reset();
        host->stop();
        return false;
    }
    if (info.privacy_mode_enabled) {
        *detail = "HelloAck granted privacy mode despite the closed gate";
        client.reset();
        host->stop();
        return false;
    }
    const chast::Error requested = client->set_privacy_mode(true);
    if (requested.status() != chast::Status::PermissionDenied) {
        *detail = "the viewer requested privacy mode without permission: " +
                  requested.to_string();
        client.reset();
        host->stop();
        return false;
    }
    // The refusal must not have disturbed the video stream.
    chast::FrameHeader header;
    std::vector<uint8_t> payload;
    const chast::Error received =
        client->receive_frame(header, payload, std::chrono::seconds(10));
    const bool frame_ok =
        received.ok() && !payload.empty() && payload.size() == header.payload_size &&
        chast::crc32(payload.data(), payload.size()) == header.payload_crc32;
    client.reset();
    host->stop();
    if (!frame_ok) {
        *detail = "video did not survive the refused privacy mode: " +
                  (received.ok() ? std::string("frame payload did not verify")
                                 : received.to_string());
        return false;
    }
    return true;
}

// A host without a test callback, running outside service mode, cannot reach
// the secure desktop. It must still answer Unsupported so the viewer gets
// feedback instead of a timeout, and the stream must keep running.
bool sas_unsupported_works(uint16_t port, std::string* detail) {
    std::string error;
    std::unique_ptr<chast::HostServer> host = start_probe_host(
        port, [](chast::HostSettings& settings) { settings.allow_input = true; }, &error);
    if (!host) {
        *detail = "SAS-unsupported host: " + error;
        return false;
    }
    chast::QuicClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = port;
    client_config.idle_timeout_ms = 5000;
    client_config.pinned_fingerprint = host->status().fingerprint;
    client_config.password = "open-sesame";
    std::unique_ptr<chast::QuicSession> client =
        chast::QuicSession::create_client(client_config, &error);
    if (!client) {
        *detail = "client: " + error;
        host->stop();
        return false;
    }
    chast::ServerInfo info;
    chast::Error connected = client->connect(std::chrono::seconds(10));
    chast::Error handshaken =
        connected.ok() ? client->handshake(info, std::chrono::seconds(10),
                                           chast::kDisplayAuto)
                       : connected;
    if (!handshaken.ok()) {
        *detail = "the viewer did not connect: " + handshaken.to_string();
        client.reset();
        host->stop();
        return false;
    }
    if (!info.input_enabled) {
        *detail = "the probe host did not grant input";
        client.reset();
        host->stop();
        return false;
    }
    // Wait for the stream before asking, like a real viewer: the host only
    // drains control requests from its running session loop.
    chast::FrameHeader first_header;
    std::vector<uint8_t> first_payload;
    const chast::Error first_frame =
        client->receive_frame(first_header, first_payload, std::chrono::seconds(10));
    if (!first_frame.ok() || first_payload.empty()) {
        const chast::HostStatus probe_status = host->status();
        *detail = "the probe host did not start streaming: " +
                  (first_frame.ok() ? std::string("empty frame")
                                    : first_frame.to_string()) +
                  " (host state " +
                  std::to_string(static_cast<int>(probe_status.state)) + ", error " +
                  (probe_status.error.empty() ? std::string("none")
                                              : probe_status.error) +
                  ")";
        client.reset();
        host->stop();
        return false;
    }
    const chast::Error requested = client->request_sas();
    if (!requested.ok()) {
        *detail = "the viewer could not ask for SAS: " + requested.to_string();
        client.reset();
        host->stop();
        return false;
    }
    chast::SasCode code = chast::SasCode::Ok;
    std::string reason;
    bool answered = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (client->take_sas_result(code, reason)) {
            answered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // The host publishes its stats on its own cadence, so give the counters a
    // moment to catch up with the answer that already arrived.
    uint64_t failures = host->status().stats.sas_failures;
    const auto count_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (failures != 1 && std::chrono::steady_clock::now() < count_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        failures = host->status().stats.sas_failures;
    }
    if (!answered) {
        const chast::HostStatus probe_status = host->status();
        *detail = "the host never answered the SAS request (requests " +
                  std::to_string(probe_status.stats.sas_requests) + ", failures " +
                  std::to_string(probe_status.stats.sas_failures) + ", error " +
                  (probe_status.error.empty() ? std::string("none")
                                              : probe_status.error) +
                  ")";
        client.reset();
        host->stop();
        return false;
    }
    if (code != chast::SasCode::Unsupported) {
        *detail = "a host without the secure-desktop path answered code " +
                  std::to_string(static_cast<unsigned>(code)) +
                  (reason.empty() ? std::string() : ": " + reason);
        client.reset();
        host->stop();
        return false;
    }
    if (reason.empty()) {
        *detail = "the host did not say why SAS is unsupported";
        client.reset();
        host->stop();
        return false;
    }
    if (failures != 1) {
        *detail = "the host counted " + std::to_string(failures) +
                  " SAS failure(s), want 1";
        client.reset();
        host->stop();
        return false;
    }
    chast::FrameHeader header;
    std::vector<uint8_t> payload;
    const chast::Error received =
        client->receive_frame(header, payload, std::chrono::seconds(10));
    const bool frame_ok =
        received.ok() && !payload.empty() && payload.size() == header.payload_size &&
        chast::crc32(payload.data(), payload.size()) == header.payload_crc32;
    client.reset();
    host->stop();
    if (!frame_ok) {
        *detail = "video did not survive the unsupported SAS answer: " +
                  (received.ok() ? std::string("frame payload did not verify")
                                 : received.to_string());
        return false;
    }
    return true;
}

// Two-factor and trusted devices through the host settings and policy, then
// lockout and the allow-list. The trusted device is remembered by a real
// connection and skips only the second factor: the password is still checked.
bool secure_auth_wiring_works(uint16_t port, std::string* detail) {
    const std::string password = "open-sesame";
    const char* const kTotpSecret = "JBSWY3DPEHPK3PXP";
    std::vector<uint8_t> totp_key;
    if (!chast::parse_totp_secret(kTotpSecret, totp_key)) {
        *detail = "the probe's TOTP secret is not base32";
        return false;
    }
    auto totp_now = [&totp_key] {
        char code[8]{};
        std::snprintf(code, sizeof(code), "%06u",
                      chast::totp_code(totp_key, static_cast<uint64_t>(std::time(nullptr))));
        return std::string(code);
    };
    auto wrong_totp = [&totp_key] {
        char code[8]{};
        std::snprintf(code, sizeof(code), "%06u",
                      chast::totp_code(totp_key, static_cast<uint64_t>(std::time(nullptr))));
        std::string wrong = code;
        wrong.back() = wrong.back() == '0' ? '1' : '0';
        return wrong;
    };

    std::string identity_error;
    std::unique_ptr<chast::DeviceIdentity> identity =
        chast::DeviceIdentity::generate(&identity_error);
    std::unique_ptr<chast::DeviceIdentity> unknown =
        chast::DeviceIdentity::generate(&identity_error);
    if (!identity || !unknown) {
        *detail = "device identity: " + identity_error;
        return false;
    }

    const uint16_t totp_port = static_cast<uint16_t>(port + 1);
    const uint16_t trust_port = static_cast<uint16_t>(port + 2);
    const uint16_t trusted_port = static_cast<uint16_t>(port + 3);
    const uint16_t lockout_port = static_cast<uint16_t>(port + 4);
    const uint16_t allow_port = static_cast<uint16_t>(port + 5);

    // The host's settings -> policy -> Hello path asks for the code.
    {
        std::string error;
        std::unique_ptr<chast::HostServer> server = start_probe_host(
            port, [&](chast::HostSettings& settings) { settings.totp_secret = kTotpSecret; },
            &error);
        if (!server) {
            *detail = "two-factor host: " + error;
            return false;
        }
        const ProbePasswordHandshake refused = probe_password(
            port, server->status().fingerprint, password, std::chrono::seconds(5));
        server->stop();
        if (refused.status != chast::Status::PermissionDenied ||
            refused.denial != chast::AccessDenial::TwoFactorRequired ||
            refused.error.find("two-factor") == std::string::npos) {
            *detail = "the host did not ask for a two-factor code: " + refused.error;
            return false;
        }
    }

    {
        std::string error;
        std::unique_ptr<chast::HostServer> server = start_probe_host(
            totp_port, [&](chast::HostSettings& settings) { settings.totp_secret = kTotpSecret; },
            &error);
        if (!server) {
            *detail = "two-factor host: " + error;
            return false;
        }
        const ProbePasswordHandshake refused =
            probe_password(totp_port, server->status().fingerprint, password,
                           std::chrono::seconds(5), wrong_totp());
        server->stop();
        if (refused.status != chast::Status::PermissionDenied ||
            refused.denial != chast::AccessDenial::WrongTwoFactor) {
            *detail = "a wrong two-factor code was not refused by name: " + refused.error;
            return false;
        }
    }

    // A device proves itself with a signed Hello and asks to be remembered.
    std::vector<chast::TrustedDevice> trusted;
    {
        chast::HostSecurity::Config policy;
        policy.mode = chast::HostAuthMode::Password;
        policy.password_hash = chast::hash_password(password);
        policy.totp_secret = kTotpSecret;
        std::string error;
        std::unique_ptr<PolicyServer> server =
            create_policy_server(trust_port, policy, nullptr, &error);
        if (!server) {
            *detail = "trust server: " + error;
            return false;
        }
        const std::string pin = server->session->server_fingerprint();
        std::atomic<bool> admitted{false};
        std::thread serving =
            serve_policy_client(*server, "trust enrollment", std::chrono::seconds(10), &admitted);
        const ProbePasswordHandshake challenged =
            probe_password(trust_port, pin, password, std::chrono::seconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const ProbePasswordHandshake enrolled =
            probe_password(trust_port, pin, password, std::chrono::seconds(8), totp_now(),
                           identity.get(), true);
        serving.join();
        if (challenged.denial != chast::AccessDenial::TwoFactorRequired) {
            *detail = "the code was not demanded before enrollment: " + challenged.error;
            return false;
        }
        if (!enrolled.ok || !admitted.load()) {
            *detail = "the enrolled device did not connect: " + enrolled.error;
            return false;
        }
        trusted = server->security->config().trusted_devices;
        if (trusted.size() != 1 || trusted.front().public_key != identity->public_key()) {
            *detail = "the host did not remember the enrolled device";
            return false;
        }
    }

    // The remembered device signs with the same key and skips the code. A new
    // device is still challenged, and a bad password is never skipped.
    {
        chast::HostSecurity::Config policy;
        policy.mode = chast::HostAuthMode::Password;
        policy.password_hash = chast::hash_password(password);
        policy.totp_secret = kTotpSecret;
        policy.trusted_devices = trusted;
        std::string error;
        std::atomic<bool> trusted_used{false};
        std::unique_ptr<PolicyServer> server =
            create_policy_server(trusted_port, policy, &trusted_used, &error);
        if (!server) {
            *detail = "trusted server: " + error;
            return false;
        }
        const std::string pin = server->session->server_fingerprint();
        std::atomic<bool> admitted{false};
        std::thread serving = serve_policy_client(
            *server, "trusted device", std::chrono::seconds(12), &admitted);
        const ProbePasswordHandshake wrong_password =
            probe_password(trusted_port, pin, "not-the-password", std::chrono::seconds(5), "",
                           identity.get());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const ProbePasswordHandshake unknown_device =
            probe_password(trusted_port, pin, password, std::chrono::seconds(5), "",
                           unknown.get());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const ProbePasswordHandshake skipped =
            probe_password(trusted_port, pin, password, std::chrono::seconds(8), "",
                           identity.get());
        serving.join();
        if (wrong_password.denial != chast::AccessDenial::WrongPassword) {
            *detail = "a trusted device skipped the password: " + wrong_password.error;
            return false;
        }
        if (unknown_device.denial != chast::AccessDenial::TwoFactorRequired) {
            *detail = "an unknown device was not challenged for a code: " + unknown_device.error;
            return false;
        }
        if (!skipped.ok || !admitted.load() || !trusted_used.load()) {
            *detail = "the trusted device did not skip the code: " + skipped.error;
            return false;
        }
    }

    // Repeated failures lock the address, including a later correct password.
    {
        chast::HostSecurity::Config policy;
        policy.mode = chast::HostAuthMode::Password;
        policy.password_hash = chast::hash_password(password);
        std::string error;
        std::unique_ptr<PolicyServer> server =
            create_policy_server(lockout_port, policy, nullptr, &error);
        if (!server) {
            *detail = "lockout server: " + error;
            return false;
        }
        const std::string pin = server->session->server_fingerprint();
        std::thread serving =
            serve_policy_client(*server, "lockout", std::chrono::seconds(5));
        bool every_failure_named = true;
        for (int attempt = 0; attempt < 6; ++attempt) {
            const ProbePasswordHandshake refused =
                probe_password(lockout_port, pin, "wrong-" + std::to_string(attempt),
                               std::chrono::seconds(3));
            if (refused.denial != chast::AccessDenial::WrongPassword) {
                *detail = "failure " + std::to_string(attempt + 1) +
                          " was not named as a wrong password: " + refused.error;
                every_failure_named = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const ProbePasswordHandshake locked =
            probe_password(lockout_port, pin, password, std::chrono::seconds(3));
        serving.join();
        if (!every_failure_named) {
            return false;
        }
        if (locked.denial != chast::AccessDenial::LockedOut ||
            locked.error.find("too many failed attempts") == std::string::npos) {
            *detail = "the locked address was not refused by name: " + locked.error;
            return false;
        }
    }

    // The allow-list is checked before any credential.
    {
        std::string error;
        std::unique_ptr<chast::HostServer> server = start_probe_host(
            allow_port,
            [](chast::HostSettings& settings) { settings.ip_whitelist = "10.0.0.0/8"; },
            &error);
        if (!server) {
            *detail = "allow-list host: " + error;
            return false;
        }
        const ProbePasswordHandshake refused =
            probe_password(allow_port, server->status().fingerprint, password,
                           std::chrono::seconds(5));
        server->stop();
        if (refused.status != chast::Status::PermissionDenied ||
            refused.denial != chast::AccessDenial::NotWhitelisted) {
            *detail = "an address outside the allow-list was not refused: " + refused.error;
            return false;
        }
    }
    return true;
}

// The HostServer wiring, not just the transport callbacks: a service with no
// permanent password refuses every viewer and names the reason, the
// interactive host exposes a rotatable temporary password, and a wrong
// permanent password is refused through the settings -> policy path.
bool host_security_wiring_works(uint16_t port, std::string* detail) {
    {
        chast::HostServerConfig config;
        config.settings.bind_address = "127.0.0.1";
        config.settings.port = port;
        config.service_mode = true;
        config.serve_once = true;
        std::string error;
        std::unique_ptr<chast::HostServer> server =
            chast::HostServer::create(std::move(config), &error);
        if (!server) {
            *detail = "service server: " + error;
            return false;
        }
        server->start();
        const std::string pin = server->status().fingerprint;
        const ProbePasswordHandshake refused =
            probe_password(port, pin, "", std::chrono::seconds(5));
        server->stop();
        if (refused.status != chast::Status::PermissionDenied ||
            refused.denial != chast::AccessDenial::PasswordRequired ||
            refused.error.find("permanent password") == std::string::npos) {
            *detail = "the service without a password did not refuse by name: " +
                      refused.error + (refused.denial
                                           ? " (code " +
                                                 std::to_string(static_cast<int>(*refused.denial)) +
                                                 ")"
                                           : " (no denial code)");
            return false;
        }
    }

    {
        chast::HostServerConfig config;
        config.settings.bind_address = "127.0.0.1";
        config.settings.port = static_cast<uint16_t>(port + 1);
        config.settings.auth_mode = chast::HostAuthMode::Both;
        std::string error;
        std::unique_ptr<chast::HostServer> server =
            chast::HostServer::create(std::move(config), &error);
        if (!server) {
            *detail = "interactive server: " + error;
            return false;
        }
        const chast::HostStatus first = server->status();
        if (first.temporary_password.size() != 6 ||
            first.temporary_password.find_first_not_of("0123456789") != std::string::npos) {
            *detail = "the interactive host did not publish a six-digit temporary password";
            server->stop();
            return false;
        }
        const std::string refreshed = server->refresh_temporary_password();
        if (refreshed.size() != 6 || refreshed == first.temporary_password ||
            server->status().temporary_password != refreshed) {
            *detail = "refreshing the temporary password did not publish a new one";
            server->stop();
            return false;
        }
        server->stop();
    }

    {
        const uint16_t password_port = static_cast<uint16_t>(port + 2);
        chast::HostServerConfig config;
        config.settings.bind_address = "127.0.0.1";
        config.settings.port = password_port;
        config.settings.auth_mode = chast::HostAuthMode::Password;
        config.settings.password_hash = chast::hash_password("open-sesame");
        config.serve_once = true;
        std::string error;
        std::unique_ptr<chast::HostServer> server =
            chast::HostServer::create(std::move(config), &error);
        if (!server) {
            *detail = "password server: " + error;
            return false;
        }
        server->start();
        const std::string pin = server->status().fingerprint;
        const ProbePasswordHandshake refused =
            probe_password(password_port, pin, "wrong", std::chrono::seconds(5));
        server->stop();
        if (refused.status != chast::Status::PermissionDenied ||
            refused.denial != chast::AccessDenial::WrongPassword) {
            *detail = "the wrong permanent password was not named: " + refused.error;
            return false;
        }
    }
    return true;
}

int run_verify(const Options& options) {
    Options local = options;
    if (local.seconds <= 0.0) {
        local.seconds = 5.0;
    }
    // A fingerprint the caller passed by hand is the one to check against; the
    // normal loopback run pins whatever identity the local server presents.
    local.insecure = false;
    // The loopback always covers the token path. A caller-supplied token is
    // reused so a manual `serve --token`/`connect --token` pair matches.
    if (local.token.empty()) {
        local.token = generate_token();
    }
    local.token_reuse = false;
    // The loopback check always covers the input channel: the server injects
    // in dry-run mode, so the events travel the whole path without touching
    // the real pointer.
    local.input = true;
    // Clipboard text is covered both ways too. The server gets an in-memory
    // backend, so the real clipboard is never read or written.
    local.clipboard = true;
    local.fake_clipboard = std::make_shared<FakeClipboard>();
    // Chat is covered both ways too: the host greets the viewer at the stream
    // announcement, the viewer replies once live, and both sides record what
    // they received for the exact-match checks below.
    local.chat_probe = std::make_shared<ChatProbe>();
    // Audio is covered with a generated tone and the real AAC codec, so the
    // check runs the same path without touching the machine's sound devices.
    local.audio = true;
    local.synthetic_audio = true;
    if (local.microphone) {
        // The host plays the uplink into an in-memory recorder, and the viewer
        // needs about two seconds between opening and closing CMIC.
        local.microphone_recorder = std::make_shared<MicrophoneRecorder>();
        if (local.seconds < 5.0) {
            local.seconds = 5.0;
        }
    }
    if (local.privacy) {
        // The overlay is held for about a second and a half so frames keep
        // arriving while it is up, then released; the session must outlive
        // both answers.
        if (local.seconds < 6.0) {
            local.seconds = 6.0;
        }
    }
    if (local.sas) {
        // The request goes out 1.2 s in; keep the session open long enough for
        // the answer and the frames that follow it.
        if (local.seconds < 4.0) {
            local.seconds = 4.0;
        }
    }
    if (local.stream_options) {
        // Four serialized requests wait for their answers and the StreamInfo
        // that follows each applied change; a codec switch also waits out the
        // host's one-second rebuild cooldown before it is restored.
        if (local.seconds < 10.0) {
            local.seconds = 10.0;
        }
    }
    // Cover the mid-session description path: the sender re-announces the
    // current stream two seconds in.
    local.reannounce = true;
    // Cover the live resize path too: the host scales the published picture to
    // 75% once, 2.5 s in. An explicit --resolution overrides the percentage.
    if (local.resolution == 0) {
        local.resolution = 75;
    }
    if (local.seconds < 3.0) {
        local.seconds = 3.0;
    }
    // The display-mode probe walks four serialized requests, each spaced past
    // the host's one-second rate limit, and every real change waits for
    // capture to come back at the new size. Hold the session open long enough
    // for the whole walk even when --seconds is small.
    if (local.display_mode && local.seconds < 20.0) {
        local.seconds = 20.0;
    }
    if (local.file_upload) {
        // Five serialized offers, two of which wait for the host to reject the
        // finished stream, need more than the default short session.
        if (local.seconds < 6.0) {
            local.seconds = 6.0;
        }
        // Keep the probe out of the operator's Downloads folder: the host gets
        // a fresh temporary directory it can write into without prompting.
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec) / "chast-quic-upload";
        if (!ec) {
            std::filesystem::remove_all(dir, ec);
        }
        if (!ec) {
            std::filesystem::create_directories(dir, ec);
        }
        if (ec) {
            std::fprintf(stderr, "could not prepare the upload directory: %s\n",
                         ec.message().c_str());
            return 1;
        }
        const std::u8string utf8 = dir.u8string();
        local.file_receive_dir.assign(utf8.begin(), utf8.end());
    }
    if (local.file_download) {
        // The host hashes the file before it offers it; hashing 300 KiB takes
        // a moment but the transfer itself needs a few frame-loop passes.
        if (local.seconds < 6.0) {
            local.seconds = 6.0;
        }
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec) / "chast-quic-download";
        if (!ec) {
            std::filesystem::create_directories(dir, ec);
        }
        const std::filesystem::path source = dir / "chast-verify-download.bin";
        if (!ec) {
            const std::vector<uint8_t> payload = file_probe_content(kFileProbeBytes);
            std::ofstream out(source, std::ios::binary | std::ios::trunc);
            if (!out) {
                ec = std::make_error_code(std::errc::io_error);
            } else {
                out.write(reinterpret_cast<const char*>(payload.data()),
                          static_cast<std::streamsize>(payload.size()));
                out.close();
                if (!out) {
                    ec = std::make_error_code(std::errc::io_error);
                }
            }
        }
        if (ec) {
            std::fprintf(stderr, "could not prepare the download source: %s\n",
                         ec.message().c_str());
            return 1;
        }
        const std::u8string utf8 = source.u8string();
        local.download_source.assign(utf8.begin(), utf8.end());
    }
    const bool keep_file = !options.save.empty();
    const std::string path = keep_file ? options.save : ".chast-quic-verify.bin";

    // Both ends must not open the same file: the sender's copy is the one the
    // offline replay checks.
    Options receiver_options = local;
    receiver_options.save.clear();
    // Exercise the wire request instead of leaving the choice to the server.
    if (receiver_options.merged) {
        receiver_options.display_set = true;
        receiver_options.display = chast::kDisplayAll;
    } else if (!receiver_options.display_set) {
        receiver_options.display_set = true;
        receiver_options.display = 0;
    }

    std::promise<ServerReady> listening;
    std::shared_future<ServerReady> server_ready = listening.get_future().share();
    ReceiverStats received;
    std::thread receiver([&] { received = run_receiver(receiver_options, server_ready); });
    SenderStats sent;
    std::thread sender([&] { sent = run_sender(local, path, &listening); });
    // Wait until the viewer's upload arrived, then let the host clipboard
    // change. That ordering is what proves the host's poll sends its own
    // change rather than echoing the upload.
    if (!local.fake_clipboard->wait_for_write(std::chrono::seconds(15))) {
        std::printf("     clipboard: the viewer's item never reached the host\n");
    }
    local.fake_clipboard->enable_update();
    sender.join();
    receiver.join();

    // The probe leaves the output in the other mode, so read the desktop only
    // after the host thread has ended and its guard has restored the original.
    bool session_end_mode_read = false;
    chast::DisplayModeInfo session_end_mode;
    if (local.display_mode) {
        std::string mode_error;
        const std::vector<chast::DisplayInfo> displays = chast::enumerate_displays();
        if (received.mode_display < displays.size()) {
            const std::vector<chast::DisplayMode> modes =
                chast::list_display_modes(displays[received.mode_display], &mode_error);
            for (const chast::DisplayMode& mode : modes) {
                if (mode.current) {
                    session_end_mode.width = mode.width;
                    session_end_mode.height = mode.height;
                    session_end_mode.refresh_hz = mode.refresh_hz;
                    session_end_mode_read = true;
                    break;
                }
            }
        }
        if (!session_end_mode_read) {
            std::printf("     the desktop mode after the session could not be read: %s\n",
                        mode_error.empty() ? "no current mode reported" : mode_error.c_str());
        }
    }

    // The host verifies the offer digest before renaming each file into place,
    // so comparing the stored bytes with the generated payload closes the
    // upload loop without trusting the viewer's own counters.
    bool fresh_file_on_disk_ok = false;
    bool resumed_file_on_disk_ok = false;
    bool nested_file_on_disk_ok = false;
    bool digest_file_absent = false;
    bool no_partial_left = false;
    std::string file_check_detail;
    if (local.file_upload) {
        const std::vector<uint8_t> expected = file_probe_content(kFileProbeBytes);
        auto read_all = [](const std::string& file_path, std::vector<uint8_t>& bytes) {
            std::ifstream stored_file(file_path, std::ios::binary);
            if (!stored_file) {
                return false;
            }
            bytes.assign(std::istreambuf_iterator<char>(stored_file),
                         std::istreambuf_iterator<char>());
            return true;
        };
        // C++20 builds the path from the UTF-8 bytes the options carry.
        auto from_utf8 = [](const std::string& text) {
            return std::filesystem::path(std::u8string(text.begin(), text.end()));
        };
        const std::filesystem::path directory = from_utf8(local.file_receive_dir);
        const std::filesystem::path nested_directory =
            directory / "chast-verify" / "nested";
        std::vector<uint8_t> stored;
        if (sent.received_file_paths.size() == 3) {
            fresh_file_on_disk_ok =
                read_all(sent.received_file_paths[0], stored) && stored == expected;
            stored.clear();
            resumed_file_on_disk_ok =
                read_all(sent.received_file_paths[1], stored) && stored == expected;
            stored.clear();
            const std::filesystem::path nested_path =
                from_utf8(sent.received_file_paths[2]);
            std::error_code nested_ec;
            nested_file_on_disk_ok =
                std::filesystem::equivalent(nested_path.parent_path(), nested_directory,
                                            nested_ec) &&
                !nested_ec && read_all(sent.received_file_paths[2], stored) &&
                stored == expected;
        }
        file_check_detail =
            "the host reported " + std::to_string(sent.received_file_paths.size()) +
            " stored file(s)";
        for (const std::string& stored_path : sent.received_file_paths) {
            file_check_detail += "\n       " + stored_path;
        }
        const std::filesystem::path digest_final = directory / "chast-verify-digest.bin";
        const std::filesystem::path digest_part = directory / "chast-verify-digest.bin.part";
        const std::filesystem::path digest_sidecar =
            directory / "chast-verify-digest.bin.part.chast";
        digest_file_absent = !std::filesystem::exists(digest_final) &&
                             !std::filesystem::exists(digest_part) &&
                             !std::filesystem::exists(digest_sidecar);
        no_partial_left = true;
        std::error_code ec;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(directory, ec)) {
            const std::filesystem::path extension = entry.path().extension();
            if (extension == std::filesystem::path(L".part") ||
                extension == std::filesystem::path(L".chast")) {
                no_partial_left = false;
            }
        }
        if (ec) {
            no_partial_left = false;
        }
    }

    std::printf("---- loopback check ----\n");
    bool failed = false;
    auto check = [&](bool condition, const char* what) {
        std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
        if (!condition) {
            failed = true;
        }
    };

    check(sent.error.empty(), "sender completed");
    if (!sent.error.empty()) {
        std::printf("     sender: %s\n", sent.error.c_str());
    }
    check(received.error.empty(), "receiver completed");
    if (!received.error.empty()) {
        std::printf("     receiver: %s\n", received.error.c_str());
    }
    check(!sent.fingerprint.empty() && received.peer_fingerprint == sent.fingerprint,
          "the viewer connected to the pinned server identity");
    check(received.transport == local.transport,
          "the session moved over the requested transport");
    if (received.transport != local.transport) {
        std::printf("     transport: asked for %s, got %s\n", local.transport.c_str(),
                    received.transport.c_str());
    }
    check(!local.token.empty(), "the loopback ran with an access token");
    // Probes run on neighbouring ports so they cannot disturb the session
    // above.
    const uint16_t probe_base =
        options.port > 65530 ? static_cast<uint16_t>(options.port - 10) : options.port;
    {
        std::string detail;
        const bool rejected =
            pinned_client_rejects_other_identity(static_cast<uint16_t>(probe_base + 1), &detail);
        check(rejected, "a client pinned to another fingerprint rejects the server");
        if (!rejected) {
            std::printf("     mismatch probe: %s\n", detail.c_str());
        }
    }
    {
        std::string detail;
        const bool passed = access_control_works(static_cast<uint16_t>(probe_base + 2), &detail);
        check(passed, "wrong, spent and declined viewers are refused and told why");
        if (!passed) {
            std::printf("     access control probe: %s\n", detail.c_str());
        }
    }
    {
        std::string detail;
        const bool passed =
            auth_and_permissions_work(static_cast<uint16_t>(probe_base + 4), &detail);
        check(passed, "password denial, approval mask and live permissions work");
        if (!passed) {
            std::printf("     auth/permission probe: %s\n", detail.c_str());
        }
    }
    {
        std::string detail;
        const bool passed =
            approval_liveness_works(static_cast<uint16_t>(probe_base + 9), &detail);
        check(passed, "a viewer that leaves during approval is not admitted");
        if (!passed) {
            std::printf("     approval liveness probe: %s\n", detail.c_str());
        }
    }
    {
        std::string detail;
        const bool passed = clipboard_gate_works(static_cast<uint16_t>(probe_base + 3), &detail);
        check(passed, "a clipboard-disabled server refuses to send and drops viewer updates");
        if (!passed) {
            std::printf("     clipboard gate probe: %s\n", detail.c_str());
        }
    }
    {
        std::string detail;
        const bool passed = audio_gate_works(static_cast<uint16_t>(probe_base + 5), &detail);
        check(passed, "an audio-disabled server announces it off and refuses to send");
        if (!passed) {
            std::printf("     audio gate probe: %s\n", detail.c_str());
        }
    }
    if (local.microphone) {
        std::string detail;
        const bool passed =
            microphone_gate_works(static_cast<uint16_t>(probe_base + 18), &detail);
        check(passed, "a microphone-disabled host refuses CMIC and keeps streaming video");
        if (!passed) {
            std::printf("     microphone gate probe: %s\n", detail.c_str());
        }
    }
    if (local.privacy) {
        std::string detail;
        const bool passed =
            privacy_gate_works(static_cast<uint16_t>(probe_base + 19), &detail);
        check(passed, "a privacy-disabled host refuses the request and keeps streaming video");
        if (!passed) {
            std::printf("     privacy gate probe: %s\n", detail.c_str());
        }
    }
    if (local.sas) {
        check(received.sas_updates >= 1, "the host answered the Ctrl+Alt+Del request");
        check(received.sas_ok_seen, "the host reported the SAS as sent");
        check(sent.sas_requests == 1, "the host counted one SAS request");
        check(sent.sas_failures == 0, "the host recorded no SAS failure");
        if (!received.sas_ok_seen) {
            std::printf("     SAS answer: code %u%s%s\n",
                        static_cast<unsigned>(received.sas_last_code),
                        received.sas_last_reason.empty() ? "" : ": ",
                        received.sas_last_reason.c_str());
        }
        std::string detail;
        const bool passed =
            sas_unsupported_works(static_cast<uint16_t>(probe_base + 20), &detail);
        check(passed, "a host without the secure-desktop path reports SAS unsupported");
        if (!passed) {
            std::printf("     SAS gate probe: %s\n", detail.c_str());
        }
    }
    {
        std::string detail;
        const bool passed =
            host_security_wiring_works(static_cast<uint16_t>(probe_base + 6), &detail);
        check(passed, "service mode demands a password and the temporary password rotates");
        if (!passed) {
            std::printf("     host security probe: %s\n", detail.c_str());
        }
    }
    {
        std::string detail;
        const bool passed =
            secure_auth_wiring_works(static_cast<uint16_t>(probe_base + 12), &detail);
        check(passed, "two-factor, trusted devices, lockout and the allow-list work");
        if (!passed) {
            std::printf("     secure auth probe: %s\n", detail.c_str());
        }
    }
    check(sent.encoded > 0, "the sender produced frames");
    check(sent.encoded == received.received, "every sent access unit was received");
    check(received.decoded == received.received,
          "every received access unit decoded to exactly one picture");
    check(received.keyframes > 0 && sent.keyframes == received.keyframes,
          "key-frame counts agree");
    if (options.keyframe_request_ms > 0) {
        check(sent.keyframes >= 2, "a key-frame request produced another IDR");
    }
    check(received.crc_errors == 0, "no payload CRC errors");
    check(received.decode_errors == 0, "no decode errors");
    check(received.gaps == 0, "no sequence gaps");
    check(received.protocol_errors == 0, "no protocol violations");
    check(received.latency.valid && received.latency.replies > 0,
          "the client received a latency reply");
    check(received.latency.valid && received.latency.rtt_us > 0 &&
              received.latency.rtt_us < 1000000,
          "the measured round trip is plausible");
    check(received.frames_without_capture == 0,
          "every received frame carried its capture timestamp");
    check(received.last_frame_age_ms >= -100.0 && received.last_frame_age_ms <= 1000.0,
          "the measured frame age is plausible");
    check(received.desktop_states > 0 && received.desktop_kind == chast::DesktopKind::Default,
          "the host reported the Default desktop to the viewer");
    if (local.privacy) {
        check(received.privacy_updates >= 2, "the host answered both privacy requests");
        check(received.privacy_on_seen, "the host reported the overlay as active");
        check(received.privacy_off_seen && !received.privacy_last_enabled,
              "the host reported the overlay as off again");
        check(sent.privacy_mode_changes == 2,
              "the host applied the on and off requests");
        check(!sent.privacy_mode_active, "the overlay was gone when the session ended");
        check(received.privacy_frames > 0,
              "capture kept running while the overlay was up");
        const bool shown_desktop = received.privacy_luma_before < 0.0
                                       ? received.privacy_luma_last > 5.0
                                       : std::fabs(received.privacy_luma_last -
                                                   received.privacy_luma_before) <= 30.0;
        check(shown_desktop, "the captured picture was not the black overlay");
        check(received.privacy_frames_after_off > 0,
              "video continued after privacy mode ended");
        if (!shown_desktop) {
            std::printf("     privacy frames: %llu under the overlay, luma %.1f before %.1f\n",
                        static_cast<unsigned long long>(received.privacy_frames),
                        received.privacy_luma_last, received.privacy_luma_before);
        }
    }
    if (local.stream_options) {
        const uint16_t host_fps =
            static_cast<uint16_t>(options.framerate == 0 ? 30 : options.framerate);
        const uint16_t capped_fps = (std::min)(host_fps, static_cast<uint16_t>(15));
        check(received.stream_option_results >= 4,
              "the host answered every stream preference request");
        check(received.stream_option_change_update_seen &&
                  received.stream_option_change_fps == capped_fps &&
                  received.stream_option_change_quality == chast::StreamQuality::Balanced,
              "a Balanced request with a 15 fps ceiling was applied and announced");
        check(received.stream_option_restore_update_seen &&
                  received.stream_option_restore_update_fps == host_fps,
              "the restored quality and frame rate were announced");
        check(received.stream_option_rate_update_seen,
              "a quality-only change was applied and announced");
        if (received.stream_option_base_codec == chast::VideoCodec::H264) {
            // libavcodec's QSV encoder flushes and resets the MFX session for
            // a rate change, so the host must force the one IDR the encoder
            // reported as required. oneVPL's H.264 tiers reset continuously
            // and NVENC/AMF update in place, so neither may cost a key frame.
            const bool resets =
                received.stream_option_rate_encoder.find("qsv") != std::string::npos;
            const uint64_t expected = resets ? 1u : 0u;
            check(received.stream_option_rate_keyframes == expected,
                  resets ? "a bitrate-only change cost the QSV session reset (one IDR)"
                         : "a bitrate-only change kept the coded sequence (no IDR)");
            if (received.stream_option_rate_keyframes != expected) {
                std::printf("     rate-only change: %llu keyframe(s) via %s\n",
                            static_cast<unsigned long long>(
                                received.stream_option_rate_keyframes),
                            received.stream_option_rate_encoder.c_str());
            }
        } else {
            // mfx-gen's HEVC encoder cannot reset its session, so the host
            // reopens it and pays the IDR. Other drivers may continue instead.
            std::printf("     rate-only change: %llu keyframe(s) on %s\n",
                        static_cast<unsigned long long>(received.stream_option_rate_keyframes),
                        chast_tools::codec_name(received.stream_option_base_codec));
        }
        check(received.stream_option_alt_seen, "the host answered the other encoder family");
        if (received.stream_option_alt_ok) {
            check(received.stream_option_codec_restored,
                  "the host produced the other encoder family and switched back");
        } else {
            check(received.stream_option_alt_unsupported,
                  "the host reported the other encoder family unsupported");
            if (received.stream_option_alt_unsupported) {
                const chast::VideoCodec alt_codec =
                    received.stream_option_base_codec == chast::VideoCodec::Hevc
                        ? chast::VideoCodec::H264
                        : chast::VideoCodec::Hevc;
                std::printf("     encoder probe: %s is unavailable: %s\n",
                            chast_tools::codec_name(alt_codec),
                            received.stream_option_alt_reason.c_str());
            }
        }
        const uint64_t expected_requests = received.stream_option_alt_ok ? 7u : 6u;
        const uint64_t expected_changes = received.stream_option_alt_ok ? 6u : 4u;
        const uint64_t expected_failures = received.stream_option_alt_unsupported ? 1u : 0u;
        check(sent.stream_option_requests == expected_requests,
              "the host counted every stream preference request");
        check(sent.stream_option_changes == expected_changes,
              "the host applied exactly the requests that changed the stream");
        check(sent.stream_option_failures == expected_failures,
              "the host counted the encoder refusal");
        if (sent.stream_option_requests != expected_requests ||
            sent.stream_option_changes != expected_changes ||
            sent.stream_option_failures != expected_failures) {
            std::printf("     stream options: %llu request(s), %llu change(s), "
                        "%llu failure(s)\n",
                        static_cast<unsigned long long>(sent.stream_option_requests),
                        static_cast<unsigned long long>(sent.stream_option_changes),
                        static_cast<unsigned long long>(sent.stream_option_failures));
        }
    }
    check(received.target_bitrate > 0, "the viewer saw the server's target bitrate");
    check(sent.target_bitrate > 0, "the server exposed adaptive-bitrate telemetry");
    check(sent.rtt_ms > 0.0, "the host sampled transport RTT");
    check(sent.abr_changes == 0, "the clean loopback stayed at the configured bitrate");
    if (local.display_mode) {
        // The probe ends in the other mode, so the first announcement is
        // compared with the mode the session started from.
        check(received.hello_width == received.mode_original_width &&
                  received.hello_height == received.mode_original_height,
              "the first HelloAck published the original mode's source size");
    } else {
        check(received.hello_width == received.source_width &&
                  received.hello_height == received.source_height,
              "the first HelloAck published the source size at full resolution");
    }
    const bool resize_expected = local.resolution < 100;
    if (resize_expected) {
        check(sent.resolution_percent == local.resolution,
              "the probe scaled the published picture");
        check(sent.resolution_changes == 1, "the published resolution changed once");
        check(sent.width < sent.source_width && sent.height < sent.source_height,
              "the announced picture is smaller than the captured source");
    } else {
        check(sent.resolution_changes == 0, "no resolution change was requested");
        check(sent.width == sent.source_width && sent.height == sent.source_height,
              "the picture kept the captured source size");
    }
    check(sent.framerate > 0 && received.framerate == sent.framerate,
          "the viewer saw the frame rate the host captured at");
    check(sent.framerate == options.framerate,
          "a clean path keeps the configured frame rate");
    check(sent.stats_received, "the client's closing statistics reached the server");
    if (sent.stats_received) {
        check(sent.peer.received == received.received, "server-side receive count agrees");
        check(sent.peer.decoded == received.decoded, "server-side decode count agrees");
        check(sent.peer.digest == received.digest, "server-side content digest agrees");
    }
    check(received.width == sent.width && received.height == sent.height,
          "decoded picture size matches the encoded size");
    const uint64_t expected_updates =
        (resize_expected ? 2 : 1) + (local.display_mode ? 3 : 0) +
        (local.stream_options ? (received.stream_option_alt_ok ? 6u : 4u) : 0u);
    check(sent.stream_updates_sent == expected_updates,
          "the server re-announced the stream mid-session");
    check(received.stream_updates == sent.stream_updates_sent,
          "the viewer received every stream update");
    check(received.source_width == sent.source_width &&
              received.source_height == sent.source_height,
          "the announced source size stayed the captured desktop size");
    if (received.stream_updates > 0) {
        const chast::ServerInfo& update = received.last_update;
        check(update.codec == sent.codec && update.width == sent.width &&
                  update.height == sent.height && update.source_width == sent.source_width &&
                  update.source_height == sent.source_height &&
                  update.origin_x == sent.origin_x && update.origin_y == sent.origin_y &&
                  update.displays.size() == sent.displays.size() &&
                  update.audio.enabled == sent.audio_info.enabled &&
                  update.audio.sample_rate == sent.audio_info.sample_rate &&
                  update.audio.channels == sent.audio_info.channels,
              "the stream update described the running stream");
    }

    if (local.merged) {
        check(sent.display_merged, "the host streamed the merged layout");
        check(received.hello_merged, "HelloAck marked the stream as a merged layout");
        check(sent.requested_display == chast::kDisplayAll,
              "the server saw the viewer's merged-layout request");
        check(!received.displays.empty(), "the merged layout listed its member displays");
        bool union_matches = !received.displays.empty();
        int32_t left = 0;
        int32_t top = 0;
        int32_t right = 0;
        int32_t bottom = 0;
        if (union_matches) {
            left = received.displays.front().left;
            top = received.displays.front().top;
            right = received.displays.front().right;
            bottom = received.displays.front().bottom;
            for (const chast::DisplayDescription& display : received.displays) {
                left = (std::min)(left, display.left);
                top = (std::min)(top, display.top);
                right = (std::max)(right, display.right);
                bottom = (std::max)(bottom, display.bottom);
            }
            union_matches =
                left == received.origin_x && top == received.origin_y &&
                static_cast<uint32_t>(right - left) == received.source_width &&
                static_cast<uint32_t>(bottom - top) == received.source_height;
        }
        check(union_matches, "the merged canvas is the union of its member displays");
        bool inside_canvas = union_matches;
        for (const chast::DisplayDescription& display : received.displays) {
            inside_canvas = inside_canvas && display.left >= left && display.top >= top &&
                            display.right <= right && display.bottom <= bottom;
        }
        check(inside_canvas, "every member display lies inside the merged canvas");
        check(received.display_switch_results == 2 && received.display_switch_ok == 1,
              "the host accepted the merge request while the merged stream ran");
        check(received.display_switch_refused == 1 &&
                  received.last_switch_code == chast::DisplaySwitchCode::NotFound,
              "a switch past the merged member list was refused as not found");
        check(sent.display_switches == 1 && sent.display_switch_failures == 1,
              "the host counted the merged no-op and the refusal");
    }

    if (local.input) {
        check(sent.input_enabled, "the server announced input as enabled");
        check(received.input_enabled, "the client saw input enabled in HelloAck");
        check(received.origin_x == sent.origin_x && received.origin_y == sent.origin_y,
              "the viewer saw the captured output's virtual-desktop origin");
        bool displays_same =
            !sent.displays.empty() && sent.displays.size() == received.displays.size();
        for (size_t i = 0; displays_same && i < sent.displays.size(); ++i) {
            displays_same = same_display(sent.displays[i], received.displays[i]);
        }
        check(displays_same, "the display list arrived intact and in order");
        size_t primary_count = 0;
        bool captured_listed = false;
        for (const chast::DisplayDescription& display : received.displays) {
            if (display.primary) {
                ++primary_count;
            }
            if (display.left == sent.origin_x && display.top == sent.origin_y &&
                static_cast<uint32_t>(display.right - display.left) == sent.source_width &&
                static_cast<uint32_t>(display.bottom - display.top) == sent.source_height) {
                captured_listed = true;
            }
        }
        check(primary_count == 1, "exactly one display is marked primary");
        if (!local.merged) {
            check(captured_listed, "the captured output is listed among the displays");
        }
        check(sent.requested_display == static_cast<uint16_t>(receiver_options.display),
              "the server saw the viewer's display choice");
        if (!local.merged) {
            check(sent.display_index == receiver_options.display,
                  "the server captured the display the viewer asked for");
            bool requested_matches = receiver_options.display < received.displays.size();
            if (requested_matches) {
                const chast::DisplayDescription& chosen =
                    received.displays[receiver_options.display];
                requested_matches =
                    chosen.left == sent.origin_x && chosen.top == sent.origin_y &&
                    static_cast<uint32_t>(chosen.right - chosen.left) == sent.source_width &&
                    static_cast<uint32_t>(chosen.bottom - chosen.top) == sent.source_height;
            }
            check(requested_matches,
                  "the stream's origin and size are the requested display's rect");
        }
        check(sent.input_events.size() == received.input_sent.size(),
              "every input event reached the server");
        bool same = sent.input_events.size() == received.input_sent.size();
        for (size_t i = 0; same && i < received.input_sent.size(); ++i) {
            same = same_event(sent.input_events[i], received.input_sent[i]);
        }
        check(same, "input events arrived intact and in order");
        check(sent.input_stats.applied == received.input_sent.size(),
              "every input event was injected");
        check(sent.input_stats.rejected == 0 && sent.input_stats.failed == 0 &&
                  sent.input_stats.dropped == 0,
              "no input event was rejected, failed or dropped");
        check(sent.input_releases == 1, "the viewer's release request reached the server");
        if (!local.merged) {
            check(received.display_switch_results == 2,
                  "both mid-session switch requests were answered");
            check(received.display_switch_ok == 1,
                  "the host accepted a switch to the output already streaming");
            check(received.display_switch_refused == 1 &&
                      received.last_switch_code == chast::DisplaySwitchCode::NotFound,
                  "a switch past the announced list was refused as not found");
            check(received.switch_bad_index != chast::kDisplayAuto &&
                      received.last_switch_index == received.switch_bad_index,
                  "the refusal named the index the viewer asked for");
            check(!received.last_switch_reason.empty(),
                  "the refusal carried the host's reason");
            check(sent.display_switches == 1 && sent.display_switch_failures == 1,
                  "the host counted one accepted and one refused switch");
        }
    }

    if (local.display_mode) {
        check(received.mode_lists == 1, "the viewer read the streamed output's mode list");
        check(received.modes_code == chast::DisplayModeCode::Ok && received.modes.size() >= 2,
              "the host listed more than one mode for the output");
        size_t running_modes = 0;
        bool original_listed = false;
        for (const chast::DisplayModeInfo& mode : received.modes) {
            if (mode.current) {
                ++running_modes;
            }
            if (mode.current && mode.width == received.mode_original_width &&
                mode.height == received.mode_original_height &&
                mode.refresh_hz == received.mode_original_refresh) {
                original_listed = true;
            }
        }
        check(running_modes == 1 && original_listed,
              "the list marked exactly one running mode");
        const bool target_moves =
            received.mode_target_width != received.mode_original_width ||
            received.mode_target_height != received.mode_original_height;
        check(received.mode_plan_ready &&
                  (target_moves ||
                   received.mode_target_refresh != received.mode_original_refresh),
              "the probe picked another mode");
        check(received.mode_results == 5, "all five display mode requests were answered");
        check(received.mode_ok == 4 && received.mode_refused == 1,
              "the no-op, the change, the restore and the leave were accepted; the "
              "unsupported mode was refused");
        check(received.refused_mode_echoed &&
                  received.refused_mode_index == received.mode_display &&
                  received.refused_mode_width == received.mode_bad_width &&
                  received.refused_mode_height == received.mode_bad_height &&
                  received.refused_mode_refresh == received.mode_bad_refresh,
              "the refusal echoed the unsupported mode");
        check(!received.refused_mode_reason.empty(),
              "the refusal carried the host's reason");
        check(received.mode_change_update_seen &&
                  received.mode_change_source_width ==
                      (target_moves ? received.mode_target_width
                                    : received.mode_original_width) &&
                  received.mode_change_source_height ==
                      (target_moves ? received.mode_target_height
                                    : received.mode_original_height),
              "the change announced the new source size");
        check(received.mode_restore_update_seen &&
                  received.mode_restore_source_width == received.mode_original_width &&
                  received.mode_restore_source_height == received.mode_original_height,
              "the restore announced the original source size");
        check(received.mode_leave_update_seen &&
                  received.mode_leave_source_width ==
                      (target_moves ? received.mode_target_width
                                    : received.mode_original_width) &&
                  received.mode_leave_source_height ==
                      (target_moves ? received.mode_target_height
                                    : received.mode_original_height),
              "the session left the display in the new mode");
        check(received.source_width == received.mode_target_width &&
                  received.source_height == received.mode_target_height,
              "the last announcement described the mode the viewer left behind");
        check(session_end_mode_read &&
                  session_end_mode.width == received.mode_original_width &&
                  session_end_mode.height == received.mode_original_height &&
                  session_end_mode.refresh_hz == received.mode_original_refresh,
              "the host restored the desktop mode when the session ended");
        check(sent.display_mode_changes == 4 && sent.display_mode_failures == 1,
              "the host counted four accepted and one refused mode request");
    }

    if (local.clipboard) {
        check(sent.clipboard_enabled, "the server announced clipboard as enabled");        check(received.clipboard_enabled, "the client saw clipboard enabled in HelloAck");
        check(received.clipboard_over_limit_sent && received.clipboard_followup_sent,
              "the viewer injected an over-limit CCLB blob and a follow-up item");
        check(received.clipboard_sent == 2 && received.clipboard_errors == 0,
              "the viewer uploaded two valid rich clipboard items");
        check(sent.clipboard_received == 2,
              "the host accepted both valid items and dropped the over-limit blob");
        const chast::ClipboardContent upload =
            rich_clipboard_probe("chast clipboard: viewer upload \xE2\x9C\x93");
        const chast::ClipboardContent followup = rich_clipboard_probe(kClipboardAfterOverflow);
        check(local.fake_clipboard->writes == 2 &&
                  local.fake_clipboard->written.size() == 2 &&
                  same_clipboard_content(local.fake_clipboard->written[0], upload) &&
                  same_clipboard_content(local.fake_clipboard->written[1], followup),
              "the session survived the over-limit blob and the host wrote only the "
              "valid items");
        check(sent.clipboard_sent == 1, "the host sent exactly one clipboard change");
        check(received.clipboard_received == 1,
              "the viewer received exactly one clipboard item");
        check(received.clipboard_contents.size() == 1 &&
                  same_clipboard_content(received.clipboard_contents.front(),
                                         local.fake_clipboard->updated),
              "the received item carried the host's text, HTML, RTF and DIB");
    }

    if (local.chat_probe) {
        std::vector<std::string> host_lines;
        {
            std::lock_guard<std::mutex> lock(local.chat_probe->mutex);
            host_lines = local.chat_probe->host_received;
        }
        check(received.chat_probe, "the viewer ran the chat probe");
        check(received.chat_sent == 1 && received.chat_errors == 0,
              "the viewer sent exactly one chat line");
        check(received.chat_received == 1 && received.chat_texts.size() == 1 &&
                  received.chat_texts.front() == kHostChatLine,
              "the viewer received the host's chat line verbatim");
        check(host_lines.size() == 1 && host_lines.front() == kViewerChatLine,
              "the host received the viewer's chat line verbatim");
        check(sent.chat_sent == 1 && sent.chat_received == 1,
              "the host counted one chat line in each direction");
        if (received.chat_texts.size() != 1 || host_lines.size() != 1) {
            std::printf("     chat: viewer received %zu, host received %zu, "
                        "viewer errors %llu\n",
                        received.chat_texts.size(), host_lines.size(),
                        static_cast<unsigned long long>(received.chat_errors));
        }
    }

    if (local.file_upload) {
        check(received.file_probe, "the viewer ran the file upload probe");
        check(received.file_offers_sent == 5 && received.file_answers == 5,
              "the viewer made five offers and the host answered each");
        check(received.file_answer_codes.size() == 5 &&
                  std::all_of(received.file_answer_codes.begin(),
                              received.file_answer_codes.end(),
                              [](chast::FileCode code) { return code == chast::FileCode::Ok; }),
              "the host accepted all five offers");
        check(received.file_answer_offsets.size() == 5 &&
                  received.file_answer_offsets[0] == 0 &&
                  received.file_answer_offsets[1] == 0 &&
                  received.file_answer_offsets[2] == kFileResumePrefix &&
                  received.file_answer_offsets[3] == 0 &&
                  received.file_answer_offsets[4] == 0,
              "the retry resumed at the prefix the truncated upload left");
        check(received.file_bytes_sent == 3 * kFileProbeBytes + kFileDigestBytes,
              "the viewer streamed 964 KiB across the five offers");
        check(received.file_results == 5 && received.file_result_codes.size() == 5 &&
                  received.file_result_codes[0] == chast::FileCode::Ok &&
                  received.file_result_codes[1] == chast::FileCode::IoError &&
                  received.file_result_codes[2] == chast::FileCode::Ok &&
                  received.file_result_codes[3] == chast::FileCode::Ok &&
                  received.file_result_codes[4] == chast::FileCode::IoError,
              "the truncated and mis-hashed uploads were refused, the complete ones "
              "stored");
        check(received.file_result_received.size() == 5 &&
                  received.file_result_received[0] == kFileProbeBytes &&
                  received.file_result_received[1] == kFileResumePrefix &&
                  received.file_result_received[2] == kFileProbeBytes &&
                  received.file_result_received[3] == kFileProbeBytes &&
                  received.file_result_received[4] == kFileDigestBytes,
              "every result reported the bytes the host actually holds");
        check(received.file_result_reasons.size() == 5 &&
                  !received.file_result_reasons[1].empty() &&
                  !received.file_result_reasons[4].empty(),
              "both refusals carried the host's reason");
        check(received.file_errors == 0, "the uploader saw no protocol errors");
        check(sent.files_received == 3 && sent.file_bytes_received == 3 * kFileProbeBytes,
              "the host finalized exactly the three complete files");
        check(fresh_file_on_disk_ok, "the fresh upload matches the offered payload");
        check(resumed_file_on_disk_ok, "the resumed upload matches the offered payload");
        check(nested_file_on_disk_ok,
              "the folder upload landed below its relative directory");
        check(digest_file_absent && no_partial_left,
              "the digest refusal left neither the file nor a partial behind");
        if (!fresh_file_on_disk_ok || !resumed_file_on_disk_ok ||
            !nested_file_on_disk_ok || !digest_file_absent || !no_partial_left) {
            std::printf("     %s\n", file_check_detail.c_str());
        }
    }

    if (local.file_download) {
        check(received.download_probe, "the viewer ran the file download probe");
        check(received.download_offers == 1, "the host offered exactly one download");
        check(received.download_bytes == kFileProbeBytes,
              "the viewer received all 300 KiB");
        check(received.download_finished && received.download_ok,
              "the viewer accepted the host's file and its digest");
        check(sent.download_queued, "the host queued the download once the viewer attached");
        check(sent.files_sent == 1 && sent.file_bytes_sent == kFileProbeBytes,
              "the host completed one 300 KiB download");
        check(sent.file_send_failures == 0, "the host's download path saw no failures");
        if (!received.download_ok) {
            std::printf("     download: %s\n", received.download_error.c_str());
        }
    }

    if (local.audio) {
        check(sent.audio_announced, "the server announced the synthetic audio stream");
        check(received.audio_announced, "the client saw audio enabled in HelloAck");
        check(sent.audio_info.codec == chast::AudioCodec::Opus &&
                  sent.audio_info.sample_rate == 48000 && sent.audio_info.channels == 2 &&
                  sent.audio_info.frame_samples == chast::kOpusFrameSamples,
              "the announced audio format is Opus 48 kHz stereo, 960-sample frames");
        check(!sent.audio_info.extradata.empty(), "the server announced the Opus config");
        check(sent.audio_packets > 0 && sent.audio_bytes > 0, "the server sent audio packets");
        if (local.display_mode) {
            // Rebuilding capture and the encoder holds the host loop for a
            // moment; the capture queue is sized for a few hundred
            // milliseconds, so a short gap is expected. It must stay short and
            // must not turn into errors or losses on the wire.
            check(sent.audio_errors == 0 && sent.audio_dropped <= 8,
                  "the mode change cost at most a fraction of a second of audio");
        } else {
            check(sent.audio_errors == 0 && sent.audio_dropped == 0,
                  "the host's audio path reported no errors or drops");
        }
        check(received.audio_received == sent.audio_packets,
              "every sent audio packet was received");
        check(received.audio_crc_errors == 0, "no audio payload CRC errors");
        check(received.audio_decode_errors == 0, "no audio decode errors");
        check(received.audio_gaps == 0, "no audio sequence gaps");
        check(received.audio_protocol_errors == 0, "no audio protocol violations");
        check(received.audio_finished, "the audio stream ended with a FIN");
        check(received.audio_samples > 0 &&
                  std::fabs(received.audio_seconds - received.elapsed_seconds) < 1.0,
              "the decoded audio duration matches the session");
        check(received.audio_rms > 1000.0 && received.audio_rms < 15000.0,
              "the decoded tone carried the expected energy");
    }

    if (local.microphone) {
        check(sent.microphone_enabled, "the server announced the microphone as granted");
        check(received.microphone_announced,
              "the client saw the microphone grant in HelloAck");
        check(received.microphone_started && received.microphone_stopped,
              "the viewer opened and closed the CMIC stream");
        check(received.microphone_packets > 0 && received.microphone_send_errors == 0,
              "the viewer sent every microphone packet it captured");
        check(received.microphone_info.codec == chast::AudioCodec::Opus &&
                  received.microphone_info.sample_rate == 48000 &&
                  received.microphone_info.channels == 1 &&
                  received.microphone_info.frame_samples == chast::kOpusFrameSamples,
              "the CMIC stream carried 48 kHz mono Opus in 960-sample packets");
        check(sent.microphone_streams == 1, "the host accepted exactly one CMIC stream");
        check(sent.microphone_packets == received.microphone_packets,
              "every sent microphone packet reached the host");
        check(sent.microphone_bytes > 0 && sent.microphone_frames > 0,
              "the host decoded microphone frames");
        check(!sent.microphone_active, "closing CMIC ended the host's microphone stream");
        check(sent.microphone_errors == 0 && sent.microphone_underruns == 0,
              "the host's microphone path reported no errors or underruns");
        const MicrophoneRecorder::Result& recording = sent.microphone_recording;
        check(recording.packets > 0 && recording.samples > 0,
              "the injected host player received decoded microphone audio");
        check(recording.frequency_hz > 400.0 && recording.frequency_hz < 480.0,
              "the decoded microphone tone is 440 Hz");
        check(recording.rms > 1000.0 && recording.rms < 15000.0,
              "the decoded microphone tone carried the expected energy");
        if (!(recording.frequency_hz > 400.0 && recording.frequency_hz < 480.0) ||
            !(recording.rms > 1000.0 && recording.rms < 15000.0)) {
            std::printf("     microphone recording: %.0f Hz rms %.0f, %llu blocks, "
                        "%llu samples\n",
                        recording.frequency_hz, recording.rms,
                        static_cast<unsigned long long>(recording.packets),
                        static_cast<unsigned long long>(recording.samples));
        }
    }

    // A run that exercised the encoder preference switched codec families
    // mid-session, so the saved file holds two elementary streams back to back.
    // The single-codec replay below cannot read that by design; the plain
    // loopback (no --stream-options) keeps the check.
    if (!local.stream_options) {
        const StreamDigest replay = digest_saved_stream(path, sent.codec);
        if (!replay.error.empty()) {
            std::printf("     replay: %s\n", replay.error.c_str());
        }
        check(replay.error.empty(), "the sent stream can be replayed offline");
        check(replay.units == sent.encoded,
              "the saved stream splits into the sent access units");
        check(replay.decoded == received.decoded,
              "offline decode produces the same frame count");
        check(replay.digest == received.digest,
              "offline decode produces the same content digest");
    } else {
        std::printf("     replay skipped: the preference probe mixed two codecs in one stream\n");
    }

    if (!keep_file) {
        std::remove(path.c_str());
    } else {
        std::printf("saved the sent stream to %s\n", path.c_str());
    }

    if (failed) {
        std::printf("QUIC loopback FAILED\n");
        return 1;
    }
    std::printf("QUIC loopback PASSED: %llu access units, %llu pictures, digest %016llX\n",
                static_cast<unsigned long long>(sent.encoded),
                static_cast<unsigned long long>(received.decoded),
                static_cast<unsigned long long>(received.digest));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    chast_tools::install_crash_handler();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Options options;
    if (!parse_args(argc, argv, options)) {
        return 2;
    }
    if (options.token_auto) {
        options.token = generate_token();
        options.token_auto = false;
    }

    if (options.mode == "serve") {
        const SenderStats stats = run_sender(options, options.save, nullptr);
        if (!stats.error.empty()) {
            std::fprintf(stderr, "chast-quic serve: %s\n", stats.error.c_str());
        }
        return stats.ok || stats.peer_left ? 0 : 1;
    }
    if (options.mode == "connect") {
        const ReceiverStats stats = run_receiver(options, {});
        if (!stats.error.empty()) {
            std::fprintf(stderr, "chast-quic connect: %s\n", stats.error.c_str());
        }
        return stats.ok ? 0 : 1;
    }
    return run_verify(options);
}
