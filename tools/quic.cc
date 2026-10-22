// QUIC transport harness: capture -> encode -> MsQuic -> decode.
//
//   chast-quic serve   --port 4433 --codec h264 --seconds 10 [--display N] [--save sent.h264] [--input] [--clipboard] [--audio]
//   chast-quic connect --host 127.0.0.1 --port 4433 [--display N] [--save received.h264] [--input] [--clipboard] [--audio] [--play]
//   chast-quic verify  --port 45433 --seconds 5 [--display N] [--input-real]
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
// announced origin and picture size are that display's rect.
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
// With --clipboard the serve end mirrors text through the real clipboard and
// connect prints what the host sends. verify always covers this path with an
// in-memory clipboard shared by both ends: the viewer uploads one text, the
// host writes it, and only then does the emulated host clipboard change so the
// viewer must receive exactly one update. Exact counts prove neither side
// echoes the other's text back.
//
// With --audio the serve end captures the system sound through WASAPI and
// connect decodes the second uni-stream, printing packet/sample/energy
// counters. With --play connect also renders the decoded blocks through WASAPI
// and prints the player's buffer depth and underrun count, which is the only
// way the tool exercises the viewer's real render path. verify substitutes a
// generated 440 Hz tone (encoded by the real AAC codec) so it can assert packet
// counts, CRCs, decoded duration and energy without a sound card, and probes a
// server whose audio gate is closed.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <functional>
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
#include "chast/decoder.h"
#include "chast/encoder.h"
#include "chast/host_server.h"
#include "chast/host_settings.h"
#include "chast/identity.h"
#include "chast/protocol.h"
#include "chast/quic.h"
#include "chast/security.h"
#include "crash_handler.h"

namespace {

// In-memory clipboard shared by verify's server and its test driver. The
// baseline is only there to be primed; the changed text is released after the
// viewer's upload was written, so both directions have a deterministic order.
struct FakeClipboard {
    bool read(std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        ++reads;
        if (update_enabled && !update_sent) {
            update_sent = true;
            current = updated_text;
        }
        text = current;
        return true;
    }

    bool write(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        ++writes;
        last_write = text;
        current = text;
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
    std::string current = baseline_text;
    std::string last_write;
    uint64_t reads = 0;
    uint64_t writes = 0;
    bool update_enabled = false;
    bool update_sent = false;
};

// verify's stand-in for the WASAPI loopback endpoint: a 440 Hz stereo tone at
// the encoder's rate, paced against the steady clock and encoded with the real
// AAC codec. It makes the loopback check cover encode -> wire -> decode
// without depending on a sound card.
class SyntheticCapture final : public chast::ILoopbackCapture {
public:
    explicit SyntheticCapture(std::unique_ptr<chast::IAudioEncoder> encoder)
        : encoder_(std::move(encoder)) {
        info_.enabled = true;
        info_.codec = chast::AudioCodec::AAC;
        info_.sample_rate = encoder_->sample_rate();
        info_.channels = encoder_->channels();
        info_.bitrate = encoder_->bitrate();
        info_.frame_samples = encoder_->frame_samples();
        info_.extradata = encoder_->extradata();
    }

    ~SyntheticCapture() override { stop(); }

    void start() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_.joinable() || stopping_.load()) {
            return;
        }
        thread_ = std::thread(&SyntheticCapture::run, this);
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

std::unique_ptr<chast::ILoopbackCapture> make_synthetic_capture(std::string* error) {
    chast::AudioEncoderConfig config;
    std::unique_ptr<chast::IAudioEncoder> encoder = chast::create_aac_encoder(config, error);
    if (!encoder) {
        return nullptr;
    }
    return std::make_unique<SyntheticCapture>(std::move(encoder));
}

struct Options {
    std::string mode;
    std::string bind = "127.0.0.1";
    std::string host = "127.0.0.1";
    uint16_t port = 4433;
    // serve: fallback output when the client does not ask for one.
    // connect: the output to ask the server for.
    size_t display = 0;
    bool display_set = false;
    chast::VideoCodec codec = chast::VideoCodec::H264;
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
    // connect only: upload this text once after the handshake, for checking a
    // host's real clipboard write path. Implies --clipboard.
    std::string send_clipboard;
    // verify only: the server's clipboard backend and the test driver's view.
    // When set, the real clipboard is never touched.
    std::shared_ptr<FakeClipboard> fake_clipboard;
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
    if ((options.token_reuse || options.approve) && options.mode != "serve") {
        std::fprintf(stderr, "--token-reuse and --approve only apply to serve\n");
        return false;
    }
    if (options.resolution != 0 && options.mode == "connect") {
        std::fprintf(stderr, "--resolution only applies to serve and verify\n");
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
    // CursorUpdates the server sent (shapes carrying a sprite included).
    uint64_t cursor_sent = 0;
    std::vector<chast::DisplayDescription> displays;
    // What Hello asked for (kDisplayAuto when the client left it to the
    // server) and the index the capturer actually used.
    uint16_t requested_display = chast::kDisplayAuto;
    size_t display_index = 0;
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
    std::string error;
    bool ok = false;
    // Events this receiver sent, and whether the HelloAck allowed them.
    std::vector<chast::InputEvent> input_sent;
    bool input_enabled = false;
    uint64_t input_errors = 0;
    // Clipboard texts this receiver sent and every text it received.
    uint64_t clipboard_sent = 0;
    uint64_t clipboard_received = 0;
    uint64_t clipboard_errors = 0;
    bool clipboard_enabled = false;
    std::vector<std::string> clipboard_texts;
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
    std::vector<chast::DisplayDescription> displays;
    // StreamInfo messages received, and the description from the last one.
    uint64_t stream_updates = 0;
    chast::ServerInfo last_update;
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
    if (options.synthetic_audio) {
        config.audio_capture = [](std::string* capture_error) {
            return make_synthetic_capture(capture_error);
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
        config.clipboard_read = [clipboard](std::string& text) {
            return clipboard->read(text);
        };
        config.clipboard_write = [clipboard](const std::string& text) {
            return clipboard->write(text);
        };
    }

    std::string error;
    std::unique_ptr<chast::HostServer> server =
        chast::HostServer::create(std::move(config), &error);
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
    stats.stats_received = status.stats.stats_received;
    stats.peer = status.stats.peer;
    stats.peer_left = status.stats.peer_left;
    stats.stream_updates_sent = status.stats.stream_updates;
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
                        stats.audio_info.codec == chast::AudioCodec::AAC ? "AAC-LC" : "?",
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

    chast::QuicClientConfig client_config;
    client_config.host = options.host;
    client_config.port = options.port;
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
    stats.target_bitrate = info.bitrate;
    stats.framerate = info.framerate;
    stats.audio_info = info.audio;
    stats.audio_announced = info.audio.enabled;
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
        if (options.display_set) {
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
                        info.audio.codec == chast::AudioCodec::AAC ? "AAC-LC" : "?",
                        info.audio.sample_rate, info.audio.channels,
                        info.audio.bitrate / 1000u);
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
        // verify uploads a fixed text so the fake backend can prove the exact
        // traffic; connect may upload one given on the command line.
        std::string upload;
        if (options.mode == "verify") {
            upload = "chast clipboard: viewer upload \xE2\x9C\x93";
        } else {
            upload = options.send_clipboard;
        }
        if (!upload.empty()) {
            if (chast::Error sent = session->send_clipboard(1, upload); !sent.ok()) {
                ++stats.clipboard_errors;
                std::fprintf(stderr, "send_clipboard: %s\n", sent.to_string().c_str());
            } else {
                ++stats.clipboard_sent;
            }
        }
    }

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
        audio_decoder = chast::create_aac_decoder(audio, &audio_error);
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
        chast::ClipboardUpdate update;
        while (session->take_clipboard_update(update)) {
            ++stats.clipboard_received;
            stats.clipboard_texts.push_back(update.text);
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

    while (std::chrono::steady_clock::now() < deadline) {
        if (!drain_stream_updates()) {
            return fail(stats.error);
        }
        drain_clipboard();
        drain_cursor();
        drain_audio(std::chrono::milliseconds(0));
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
        chast::Error received =
            session->receive_frame(header, payload, std::chrono::milliseconds(2000));
        // Audio is on its own stream and does not wake this call; drain it
        // right after every video wait so it never queues behind a stall.
        drain_audio(std::chrono::milliseconds(0));
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
        }
    }

    std::vector<chast::DecodedFrame> tail;
    drain_clipboard();
    drain_cursor();
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
        std::printf("receiver: decoder=%s codec=%s size=%ux%u source=%ux%u received=%llu "
                    "decoded=%llu keyframes=%llu crc-errors=%llu decode-errors=%llu gaps=%llu "
                    "protocol-errors=%llu stream-updates=%llu bytes=%llu digest=%016llX "
                    "elapsed=%llu ms%s\n",
                    stats.decoder.c_str(), chast_tools::codec_name(stats.codec), stats.width,
                    stats.height, stats.source_width, stats.source_height,
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
        }
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
    // Audio is covered with a generated tone and the real AAC codec, so the
    // check runs the same path without touching the machine's sound devices.
    local.audio = true;
    local.synthetic_audio = true;
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
    const bool keep_file = !options.save.empty();
    const std::string path = keep_file ? options.save : ".chast-quic-verify.bin";

    // Both ends must not open the same file: the sender's copy is the one the
    // offline replay checks.
    Options receiver_options = local;
    receiver_options.save.clear();
    // Exercise the wire request instead of leaving the choice to the server.
    if (!receiver_options.display_set) {
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
        std::printf("     clipboard: the viewer's text never reached the host\n");
    }
    local.fake_clipboard->enable_update();
    sender.join();
    receiver.join();

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
    check(received.target_bitrate > 0, "the viewer saw the server's target bitrate");
    check(sent.target_bitrate > 0, "the server exposed adaptive-bitrate telemetry");
    check(sent.rtt_ms > 0.0, "the host sampled transport RTT");
    check(sent.abr_changes == 0, "the clean loopback stayed at the configured bitrate");
    check(received.hello_width == received.source_width &&
              received.hello_height == received.source_height,
          "the first HelloAck published the source size at full resolution");
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
    const uint64_t expected_updates = resize_expected ? 2 : 1;
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
        check(captured_listed, "the captured output is listed among the displays");
        check(sent.requested_display == static_cast<uint16_t>(receiver_options.display),
              "the server saw the viewer's display choice");
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
    }

    if (local.clipboard) {
        check(sent.clipboard_enabled, "the server announced clipboard as enabled");
        check(received.clipboard_enabled, "the client saw clipboard enabled in HelloAck");
        check(received.clipboard_sent == 1, "the viewer uploaded exactly one clipboard text");
        check(sent.clipboard_received == 1, "the host received exactly one clipboard text");
        check(local.fake_clipboard->writes == 1 &&
                  local.fake_clipboard->last_write ==
                      "chast clipboard: viewer upload \xE2\x9C\x93",
              "the host wrote the viewer's text into its clipboard");
        check(sent.clipboard_sent == 1, "the host sent exactly one clipboard change");
        check(received.clipboard_received == 1,
              "the viewer received exactly one clipboard text");
        check(received.clipboard_texts.size() == 1 &&
                  received.clipboard_texts.front() == local.fake_clipboard->updated_text,
              "the received clipboard text matched the host's change");
    }

    if (local.audio) {
        check(sent.audio_announced, "the server announced the synthetic audio stream");
        check(received.audio_announced, "the client saw audio enabled in HelloAck");
        check(sent.audio_info.codec == chast::AudioCodec::AAC &&
                  sent.audio_info.sample_rate == 48000 && sent.audio_info.channels == 2 &&
                  sent.audio_info.frame_samples == 1024,
              "the announced audio format is AAC-LC 48 kHz stereo, 1024-sample frames");
        check(!sent.audio_info.extradata.empty(), "the server announced the AAC decoder config");
        check(sent.audio_packets > 0 && sent.audio_bytes > 0, "the server sent audio packets");
        check(sent.audio_errors == 0 && sent.audio_dropped == 0,
              "the host's audio path reported no errors or drops");
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

    const StreamDigest replay = digest_saved_stream(path, sent.codec);
    if (!replay.error.empty()) {
        std::printf("     replay: %s\n", replay.error.c_str());
    }
    check(replay.error.empty(), "the sent stream can be replayed offline");
    check(replay.units == sent.encoded, "the saved stream splits into the sent access units");
    check(replay.decoded == received.decoded, "offline decode produces the same frame count");
    check(replay.digest == received.digest, "offline decode produces the same content digest");

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
