// Checks for the host-side voice call (protocol v37): a viewer asks for the
// host microphone, the relay answers with the capture format, forwards Opus
// packets until the viewer stops the call, restarts capture when the operator
// picks another microphone mid-call, and tears the call down when the
// microphone permission is revoked. A synthetic capture stands in for the
// device so the check needs no microphone; it still needs an interactive
// desktop because the call lives inside a desktop session.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chast/audio.h"
#include "chast/host_server.h"
#include "clipboard_stub.h"
#include "chast/security.h"
#include "chast/wallpaper.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

void skip(const char* what) {
    std::printf("skip %s\n", what);
}

const std::string& password() {
    static const std::string value = "voice-call-password";
    return value;
}

uint16_t free_udp_port() {
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    uint16_t port = 0;
    if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        sockaddr_in bound{};
        int length = sizeof(bound);
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
            port = ntohs(bound.sin_port);
        }
    }
    closesocket(socket);
    return port;
}

bool wait_for(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

std::filesystem::path temp_dir() {
    wchar_t buffer[MAX_PATH]{};
    GetTempPathW(MAX_PATH, buffer);
    const std::filesystem::path dir =
        std::filesystem::path(buffer) / L"chast-host-voice-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

struct MicState {
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    std::atomic<int> active{0};
    std::atomic<uint64_t> created{0};
    std::mutex devices_mutex;
    std::vector<std::string> requested_devices;

    std::vector<std::string> devices() {
        std::lock_guard<std::mutex> lock(devices_mutex);
        return requested_devices;
    }
};

struct WallpaperState {
    std::atomic<int> created{0};
    std::atomic<int> restored{0};
};

class FakeWallpaperRemoval : public chast::WallpaperRemoval {
public:
    explicit FakeWallpaperRemoval(std::shared_ptr<WallpaperState> state)
        : state_(std::move(state)) {
        ++state_->created;
    }
    ~FakeWallpaperRemoval() override {
        std::string ignored;
        restore(&ignored);
    }
    bool restore(std::string*) override {
        if (!active_) {
            return true;
        }
        active_ = false;
        ++state_->restored;
        return true;
    }
    bool active() const override { return active_; }

private:
    std::shared_ptr<WallpaperState> state_;
    bool active_ = true;
};

chast::AudioInfo voice_info() {
    chast::AudioInfo info;
    info.enabled = true;
    info.codec = chast::AudioCodec::Opus;
    info.sample_rate = 48000;
    info.channels = 1;
    info.frame_samples = chast::kOpusFrameSamples;
    info.bitrate = 32000;
    info.extradata.assign(19, 0);
    std::memcpy(info.extradata.data(), "OpusHead", 8);
    info.extradata[8] = 1;
    info.extradata[9] = 1;
    info.extradata[10] = 0x38;
    info.extradata[11] = 0x01;
    info.extradata[14] = 0x80;
    info.extradata[15] = 0xBB;
    return info;
}

// Emits one 20 ms Opus-shaped packet whenever the clock has advanced one
// frame, so the relay drains a realistic cadence instead of spinning on an
// always-ready source.
class FakeMicCapture : public chast::IMicrophoneCapture {
public:
    explicit FakeMicCapture(std::shared_ptr<MicState> state) : state_(std::move(state)) {}

    ~FakeMicCapture() override { stop(); }

    void start() override {
        if (!active_) {
            active_ = true;
            ++state_->active;
        }
        state_->started = true;
        state_->stopped = false;
        last_ms_ = 0;
        seq_ = 0;
    }

    void stop() override {
        if (active_) {
            active_ = false;
            if (--state_->active == 0) {
                state_->started = false;
                state_->stopped = true;
            }
        }
    }

    bool take_packet(chast::EncodedAudio& packet) override {
        if (!active_) {
            return false;
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        if (last_ms_ != 0 && now - last_ms_ < 20) {
            return false;
        }
        last_ms_ = now;
        packet.capture_ms = static_cast<uint64_t>(now);
        packet.data.assign(80, static_cast<uint8_t>(seq_++));
        return true;
    }

    bool failed() const override { return false; }
    std::string error() const override { return {}; }
    chast::AudioInfo info() const override { return voice_info(); }
    chast::AudioCaptureStats stats() const override { return {}; }

private:
    std::shared_ptr<MicState> state_;
    bool active_ = false;
    long long last_ms_ = 0;
    uint64_t seq_ = 0;
};

std::unique_ptr<chast::HostServer> start_host(uint16_t port, const std::filesystem::path& dir,
                                              std::shared_ptr<MicState> mic,
                                              std::shared_ptr<WallpaperState> wallpaper,
                                              std::string* error) {
    chast::HostServerConfig config;
    use_empty_clipboard(config);
    config.dry_run_input = true;
    config.settings.bind_address = "127.0.0.1";
    config.settings.port = port;
    config.settings.carrier = false;
    config.settings.tunnel = false;
    config.settings.max_viewers = 1;
    config.settings.allow_input = false;
    config.settings.allow_audio = false;
    config.settings.allow_microphone = true;
    config.settings.voice_microphone = "test-mic-id";
    config.settings.remove_wallpaper = true;
    config.settings.auth_mode = chast::HostAuthMode::Password;
    config.settings.password_hash = chast::hash_password(password());
    config.settings.audit_spool = (dir / L"voice.audit.jsonl").string();
    config.settings.file_receive_directory = (dir / L"receive").string();
    config.first_client_timeout_ms = 0;
    config.serve_once = false;
    config.max_consecutive_failures = 3;
    config.log = [](const std::string& line) { std::printf("     host: %s\n", line.c_str()); };
    config.voice_capture = [mic](const chast::MicrophoneCaptureConfig& microphone,
                                 std::string*) -> std::unique_ptr<chast::IMicrophoneCapture> {
        {
            std::lock_guard<std::mutex> lock(mic->devices_mutex);
            mic->requested_devices.push_back(microphone.device_id);
        }
        ++mic->created;
        // "test-mic-id" answers "unplugged" so the start path exercises the
        // fallback to the default communications device; "second-mic-id" is a
        // working device used to check the mid-call switch.
        if (!microphone.device_id.empty() && microphone.device_id != "second-mic-id") {
            return nullptr;
        }
        return std::make_unique<FakeMicCapture>(mic);
    };
    config.wallpaper_remover =
        [wallpaper](std::string*) -> std::unique_ptr<chast::WallpaperRemoval> {
        return std::make_unique<FakeWallpaperRemoval>(wallpaper);
    };
    std::unique_ptr<chast::HostServer> server =
        chast::HostServer::create(std::move(config), error);
    if (!server) {
        return nullptr;
    }
    server->start();
    if (!wait_for(
            [&server] { return server->status().state == chast::HostState::Listening; },
            std::chrono::seconds(5))) {
        *error = "the host did not start listening";
        server->stop();
        return nullptr;
    }
    return server;
}

std::unique_ptr<chast::QuicSession> connect_viewer(uint16_t port,
                                                   const std::string& fingerprint,
                                                   chast::ServerInfo* info,
                                                   std::string* error) {
    chast::QuicClientConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.idle_timeout_ms = 8000;
    config.pinned_fingerprint = fingerprint;
    config.password = password();
    config.session_mode = chast::SessionMode::Desktop;
    std::unique_ptr<chast::QuicSession> session =
        chast::QuicSession::create_client(config, error);
    if (!session) {
        return nullptr;
    }
    const chast::Error connected = session->connect(std::chrono::seconds(8));
    if (!connected.ok()) {
        *error = "connect: " + connected.to_string();
        return nullptr;
    }
    const chast::Error handshaken = session->handshake(*info, std::chrono::seconds(8));
    if (!handshaken.ok()) {
        *error = "handshake: " + handshaken.to_string();
        return nullptr;
    }
    return session;
}

bool take_accepted(chast::QuicSession& session, chast::VoiceCallResponse& response,
                   std::chrono::milliseconds timeout) {
    return wait_for(
        [&] {
            chast::VoiceCallResponse next;
            if (!session.take_voice_call_response(next)) {
                return false;
            }
            response = std::move(next);
            return true;
        },
        timeout);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("FAIL WSAStartup\n");
        return 1;
    }
    const std::filesystem::path dir = temp_dir();
    const uint16_t port = free_udp_port();
    check(port != 0, "a loopback port is available");
    std::string error;
    auto mic = std::make_shared<MicState>();
    auto wallpaper = std::make_shared<WallpaperState>();
    bool session_streamed = false;
    std::unique_ptr<chast::HostServer> host =
        port == 0 ? nullptr : start_host(port, dir, mic, wallpaper, &error);
    check(host != nullptr, "the voice-call host starts");
    if (host) {
        const std::string fingerprint = host->status().fingerprint;
        chast::ServerInfo info;
        std::unique_ptr<chast::QuicSession> viewer =
            connect_viewer(port, fingerprint, &info, &error);
        check(viewer != nullptr, "the desktop viewer is admitted");
        if (viewer) {
            check(info.microphone_enabled, "the viewer is offered the microphone");
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            if (host->status().state != chast::HostState::Streaming) {
                skip("the voice-call check needs a desktop stream");
            } else {
                session_streamed = true;
                check(viewer->request_voice_call().ok(), "the viewer requests a voice call");
                chast::VoiceCallResponse response;
                check(take_accepted(*viewer, response, std::chrono::seconds(5)),
                      "the host answers the voice call");
                check(response.code == chast::VoiceCallCode::Ok && response.audio.enabled &&
                          response.audio.codec == chast::AudioCodec::Opus &&
                          response.audio.sample_rate == 48000 &&
                          response.audio.channels == 1 &&
                          !response.audio.extradata.empty(),
                      "the answer carries the host microphone format");
                check(wait_for([&host] { return host->status().stats.voice_call_active; },
                               std::chrono::seconds(5)),
                      "the host reports the call as active");
                check(mic->started.load(), "the host started the capture device");
                {
                    const std::vector<std::string> devices = mic->devices();
                    check(devices.size() >= 2 && devices[0] == "test-mic-id" &&
                              devices[1].empty(),
                          "the host asks for the configured microphone, then falls back to the "
                          "default device when it is missing");
                }

                uint64_t packets = 0;
                uint64_t last_seq = 0;
                bool seq_ok = true;
                bool payload_ok = true;
                wait_for(
                    [&] {
                        chast::AudioHeader header;
                        std::vector<uint8_t> payload;
                        while (viewer->take_voice_audio(header, payload)) {
                            seq_ok = seq_ok && (packets == 0 || header.seq > last_seq);
                            last_seq = header.seq;
                            payload_ok = payload_ok && payload.size() == 80;
                            ++packets;
                        }
                        return packets >= 3;
                    },
                    std::chrono::seconds(5));
                check(packets >= 3, "the viewer receives host microphone packets");
                check(seq_ok, "the voice packets carry increasing sequence numbers");
                check(payload_ok, "the voice packets keep their Opus payload");

                // The operator pins another microphone from the tray. The
                // capture restarts on it while the call and the viewer's
                // decoder stay up.
                const uint64_t created_before = mic->created.load();
                host->set_voice_microphone("second-mic-id");
                check(wait_for(
                          [&mic] {
                              const std::vector<std::string> devices = mic->devices();
                              return !devices.empty() && devices.back() == "second-mic-id";
                          },
                          std::chrono::seconds(5)),
                      "the running call restarts capture on the selected microphone");
                check(mic->created.load() > created_before,
                      "the switch opened a new capture device");
                check(host->status().stats.voice_call_active,
                      "the voice call stays active across the microphone switch");
                uint64_t after_switch = 0;
                wait_for(
                    [&] {
                        chast::AudioHeader header;
                        std::vector<uint8_t> payload;
                        while (viewer->take_voice_audio(header, payload)) {
                            ++after_switch;
                        }
                        return after_switch >= 3;
                    },
                    std::chrono::seconds(5));
                check(after_switch >= 3,
                      "the viewer keeps receiving packets after the switch");

                check(viewer->request_voice_call_stop().ok(),
                      "the viewer ends the voice call");
                check(wait_for([&host] { return !host->status().stats.voice_call_active; },
                               std::chrono::seconds(5)),
                      "the host clears the call when the viewer stops it");
                check(mic->stopped.load(), "the host released the capture device");

                // A start cancelled before the host answers must not leave a
                // pending call behind; the next start has to be accepted.
                check(viewer->request_voice_call().ok(),
                      "the viewer starts a call to cancel");
                check(viewer->request_voice_call_stop().ok(),
                      "the viewer cancels the call immediately");
                check(viewer->request_voice_call().ok(), "the viewer can call again");
                chast::VoiceCallResponse second;
                check(take_accepted(*viewer, second, std::chrono::seconds(5)) &&
                          second.code == chast::VoiceCallCode::Ok,
                      "the second call is accepted");
                check(wait_for([&host] { return host->status().stats.voice_call_active; },
                               std::chrono::seconds(5)),
                      "the host reports the second call as active");
                host->set_permissions(chast::kPermissionAll & ~chast::kPermissionMicrophone);
                check(wait_for([&host] { return !host->status().stats.voice_call_active; },
                               std::chrono::seconds(5)),
                      "revoking the microphone ends the running call");
                check(mic->stopped.load(), "the revoked call releases the capture device");
                bool closed = false;
                wait_for(
                    [&] {
                        closed = closed || viewer->take_voice_call_closed();
                        return closed;
                    },
                    std::chrono::seconds(3));
                check(closed, "the viewer is told the call closed");
            }
            chast::ClientStats stats;
            viewer->finish_client(stats, std::chrono::seconds(5));
            viewer.reset();
        }
        host->stop();
        host.reset();
        if (session_streamed) {
            check(wallpaper->created.load() == 1,
                  "the wallpaper is removed once for the desktop session");
            check(wait_for([&wallpaper] { return wallpaper->restored.load() == 1; },
                           std::chrono::seconds(5)),
                  "the wallpaper is restored when the session ends");
        }
    }
    WSACleanup();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (failures != 0) {
        std::printf("host voice-call checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("host voice-call checks passed\n");
    return 0;
}
