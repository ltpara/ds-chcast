#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <future>
#include <stdexcept>
#include "chast/security.h"
#include "clipboard_stub.h"
#include "host_media_stub.h"

namespace {
using namespace chast;
using namespace std::chrono_literals;
int failures = 0;

void require(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> bool until(Predicate predicate, std::chrono::milliseconds timeout = 3s) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
uint16_t free_port() {
    const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int bound = bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    int size = sizeof(addr);
    const int read = getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &size);
    closesocket(sock);
    require(bound == 0 && read == 0, "allocate loopback port");
    return ntohs(addr.sin_port);
}

struct Fixture {
    HostServerConfig config;
    std::shared_ptr<host_test::MediaState> media;
    std::unique_ptr<HostServer> host;
    std::filesystem::path root;
    uint16_t port = free_port();
    Fixture() {
        root = std::filesystem::temp_directory_path() /
            ("chast-parity-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(port));
        std::filesystem::create_directories(root);
        config.settings.bind_address = "127.0.0.1";
        config.settings.port = port;
        config.settings.carrier = config.settings.tunnel = false;
        config.settings.max_viewers = 8;
        config.settings.auth_mode = HostAuthMode::Password;
        config.settings.password_hash = hash_password("parity-test-password");
        config.settings.file_receive_directory = root.string();
        config.settings.audit_spool = (root / "audit.jsonl").string();
        config.settings.allow_input = config.settings.allow_audio = true;
        config.settings.allow_file_transfer = config.settings.allow_clipboard = true;
        config.settings.allow_remote_config = config.settings.allow_terminal = true;
        config.settings.allow_session_select = config.settings.allow_os_login = true;
        config.service_mode = true;
        config.first_client_timeout_ms = 0;
        config.log = [](const std::string& s) { std::printf("  host: %s\n", s.c_str()); };
        config.list_sessions = [](std::vector<RemoteSession>& sessions, std::string*) {
            sessions = {{1, "test-user", true, true}, {2, "other-user", true, false}};
            return true;
        };
        config.select_session = [](uint32_t, std::string* error) {
            *error = "test backend refusal";
            return false;
        };
        config.os_login_state = [] { return OsLoginStatus{OsLoginState::Locked, {}}; };
        config.os_login = [](const std::string&, std::string*) { return true; };
        config.host_control = [](HostAction, bool, std::string*) { return true; };
        config.send_sas = [](std::string*) { return true; };
        use_empty_clipboard(config);
        media = host_test::use_media(config);
    }
    ~Fixture() {
        if (host) host->stop();
        host.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void start() {
        std::string error;
        host = HostServer::create(config, &error);
        if (!host) throw std::runtime_error("host create: " + error);
        host->start();
        require(until([&] { return host->status().state == HostState::Listening; }), "host listening");
    }
    std::unique_ptr<QuicSession> connect(SessionMode mode = SessionMode::Desktop,
            PermissionMask mask = kPermissionAll, std::string ref = {}) {
        QuicClientConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = port;
        cfg.password = "parity-test-password";
        cfg.pinned_fingerprint = host->status().fingerprint;
        cfg.requested_permissions = mask;
        cfg.session_mode = mode;
        cfg.audit_ref = std::move(ref);
        std::string error;
        auto viewer = QuicSession::create_client(cfg, &error);
        if (!viewer) throw std::runtime_error("client create: " + error);
        Error result = viewer->connect(6s);
        if (!result.ok()) throw std::runtime_error(result.to_string());
        ServerInfo info;
        result = viewer->handshake(info, 8s);
        if (!result.ok()) throw std::runtime_error("handshake: " + result.to_string());
        return viewer;
    }
};

void finish(std::unique_ptr<QuicSession>& viewer) {
    if (viewer) {
        ClientStats stats;
        require(viewer->finish_client(stats, 3s).ok(), "viewer ends cleanly");
        viewer.reset();
    }
}
bool keyframe(QuicSession& viewer) {
    return until([&] {
        FrameHeader header;
        std::vector<uint8_t> data;
        return viewer.receive_frame(header, data, 50ms).ok() && header.idr && !data.empty();
    });
}
bool audio(QuicSession& viewer, std::chrono::milliseconds timeout = 1s) {
    AudioHeader header;
    std::vector<uint8_t> data;
    return viewer.receive_audio(header, data, timeout).ok() && !data.empty();
}
void drain_audio(QuicSession& viewer) {
    while (audio(viewer, 1ms)) {}
}
void set_policy(QuicSession& viewer, uint32_t keys, uint32_t values) {
    require(viewer.request_remote_config(keys, values).ok(), "send global policy change");
    RemoteConfigState state;
    require(until([&] { return viewer.take_remote_config_state(state); }) &&
            state.code == RemoteConfigCode::Ok && (state.flags & keys) == (values & keys),
            "global policy change acknowledged");
}

void static_desktop_and_followers() {
    Fixture f;
    std::atomic<int> login_calls{0};
    f.config.os_login = [&](const std::string&, std::string*) { ++login_calls; return true; };
    f.start();
    auto publisher = f.connect();
    require(keyframe(*publisher), "publisher receives initial IDR");
    require(audio(*publisher), "publisher receives system audio");
    f.host->set_permissions(kPermissionAll & ~(kPermissionAudio | kPermissionInput));
    require(until([&] { return (publisher->permissions() & (kPermissionAudio | kPermissionInput)) == 0; }),
            "publisher relinquishes audio and control");
    drain_audio(*publisher);
    auto follower = f.connect();
    require(keyframe(*follower), "late follower receives IDR on a completely static desktop");
    require(follower->permissions() & kPermissionInput, "follower acquires available control lease");
    require(audio(*follower), "follower hears audio while publisher is muted");
    require(!audio(*publisher, 120ms), "publisher stays muted");

    require(follower->request_display_follow(kDisplayFollowAll).ok(), "follower requests display follow");
    DisplayFollowState follow;
    require(until([&] { return follower->take_display_follow_state(follow); }) &&
            follow.code == DisplayFollowCode::Unsupported, "follower receives explicit follow refusal");
    require(follower->request_display_follow(0).ok(), "follower disables display follow");
    require(until([&] { return follower->take_display_follow_state(follow); }) &&
            follow.code == DisplayFollowCode::Ok && follow.flags == 0, "disable-follow request completes");
    require(follower->request_os_login("test-only-password").ok(), "follower requests OS login");
    OsLoginCode login;
    std::string reason;
    require(until([&] { return follower->take_os_login_result(login, reason); }) &&
            login == OsLoginCode::Ok && login_calls == 1, "follower OS login reaches fake backend and replies");
    require(follower->request_session_list().ok(), "WTS permission survives stream description");
    SessionListResult list;
    require(until([&] { return follower->take_session_list_result(list); }) &&
            list.code == SessionListCode::Ok && list.sessions.size() == 2, "follower receives session list");
    for (const auto& [id, expected] : std::vector<std::pair<uint32_t, SessionSelectCode>>{
            {1, SessionSelectCode::Ok}, {999, SessionSelectCode::NotFound}, {2, SessionSelectCode::Failed}}) {
        require(follower->request_session_select(id).ok(), "follower requests session selection");
        SessionSelectResult selected;
        require(until([&] { return follower->take_session_select_result(selected); }) &&
                selected.code == expected, "follower receives validated session-selection result");
    }

    f.host->set_permissions(kPermissionAll & ~kPermissionAudio);
    require(until([&] { return !(follower->permissions() & kPermissionAudio); }), "mute follower independently");
    drain_audio(*follower);
    require(!audio(*follower, 120ms) && !audio(*publisher, 120ms), "both muted viewers receive no new audio");
    f.host->set_permissions(kPermissionAll);
    require(until([&] { return (follower->permissions() & kPermissionAudio) != 0; }), "restore follower audio grant");
    require(audio(*follower), "restored follower resumes audio");

    FrameHeader frame;
    std::vector<uint8_t> payload;
    while (publisher->receive_frame(frame, payload, 1ms).ok()) {}
    const unsigned before_request = f.media->encodes + f.media->repeats;
    require(publisher->request_keyframe().ok() &&
            until([&] { return f.media->encodes + f.media->repeats > before_request; }) &&
            keyframe(*publisher), "explicit IDR request works without desktop activity");
    // Empty queued frames so the next IDR proves the encoder fallback ran.
    while (publisher->receive_frame(frame, payload, 1ms).ok()) {}
    const unsigned encoded = f.media->encodes;
    f.media->repeat_supported = false;
    require(publisher->request_keyframe().ok() && keyframe(*publisher) && f.media->encodes > encoded,
            "repeat-unsupported encoder imports retained GPU surface");
    const unsigned calls = f.media->encodes + f.media->repeats;
    std::this_thread::sleep_for(160ms);
    require(f.media->encodes + f.media->repeats == calls, "idle desktop does not encode without a request");

    f.media->hold_frames = true;
    f.media->reset_requested = true;
    require(until([&] { return f.media->resets > 0; }), "capture reset observed");
    while (publisher->receive_frame(frame, payload, 1ms).ok()) {}
    require(publisher->request_keyframe().ok(), "request IDR during capture reset");
    require(!publisher->receive_frame(frame, payload, 180ms).ok(), "old desktop is not replayed after reset");
    f.media->hold_frames = false;
    require(keyframe(*publisher), "new desktop frame resumes with IDR");
    finish(publisher);
    require(until([&] { return f.media->audio_sources >= 2; }), "follower takes over the audio source after publisher leaves");
    drain_audio(*follower);
    require(audio(*follower), "audio continues after publisher departure");
    finish(follower);
}

void global_policy(bool frozen) {
    Fixture f;
    f.config.settings.allow_permission_change = !frozen;
    f.start();
    auto publisher = f.connect();
    auto follower = f.connect();
    auto files = f.connect(SessionMode::FileTransfer);
    auto terminal = f.connect(SessionMode::Terminal);
    for (auto* viewer : {files.get(), terminal.get()}) {
        require(viewer->request_stream_options({}).ok(), "non-desktop session sends stream options");
        StreamOptionsResult options;
        require(until([&] { return viewer->take_stream_options_result(options); }) &&
                options.code == StreamOptionsCode::Unsupported,
                "non-desktop session receives an explicit stream-options result");
    }
    require(audio(*publisher) && audio(*follower), "both desktop sessions initially receive audio");
    require(follower->request_session_select(2).ok(), "view-only follower may request WTS selection");
    SessionSelectResult selected;
    require(until([&] { return follower->take_session_select_result(selected); }) &&
            selected.code == SessionSelectCode::Busy, "follower cannot bypass control lease");
    constexpr uint32_t keys = kRemoteConfigAudio | kRemoteConfigClipboard |
        kRemoteConfigFileTransfer | kRemoteConfigTerminal;
    constexpr PermissionMask revoked = kPermissionAudio | kPermissionClipboard |
        kPermissionFileTransfer | kPermissionTerminal;
    set_policy(*publisher, keys, 0);
    require(until([&] {
        return ((publisher->permissions() | follower->permissions() |
                 files->permissions() | terminal->permissions()) & revoked) == 0;
    }), frozen ? "global revocation reaches every session despite frozen UI permissions"
               : "global revocation reaches desktop, file and control sessions");
    drain_audio(*publisher);
    drain_audio(*follower);
    require(!audio(*publisher, 100ms) && !audio(*follower, 100ms), "global audio disable gates both recipients");
    require(files->request_file_list(1, "").status() == Status::PermissionDenied,
            "revoked file session cannot browse");
    bool refused = false;
    try { auto unavailable = f.connect(SessionMode::FileTransfer); }
    catch (const std::runtime_error& error) {
        refused = std::string(error.what()).find("permission denied") != std::string::npos;
    }
    require(refused, "new file-only connection is refused while globally disabled");
    auto denied = f.connect();
    require((denied->permissions() & revoked) == 0, "new desktop sessions see disabled security defaults");
    set_policy(*publisher, keys, keys);
    std::this_thread::sleep_for(150ms);
    require(((publisher->permissions() | follower->permissions() | files->permissions() |
              terminal->permissions() | denied->permissions()) & revoked) == 0,
            "reopening global policy never silently regrants existing sessions");
    auto newcomer = f.connect();
    require((newcomer->permissions() & revoked) == revoked && audio(*newcomer),
            "new authenticated session receives re-enabled capabilities and audio");
    require(newcomer->request_remote_config(0, 0).ok(), "follower can query global policy");
    RemoteConfigState state;
    require(until([&] { return newcomer->take_remote_config_state(state); }) &&
            (state.flags & keys) == keys, "follower sees current shared settings");
    finish(newcomer);
    finish(denied);
    finish(terminal);
    finish(files);
    finish(follower);
    finish(publisher);
}

void pending_approval() {
    Fixture f;
    f.config.service_mode = false;
    f.config.settings.auth_mode = HostAuthMode::Click;
    std::atomic<bool> waiting{false}, release{false};
    f.config.approve_client = [&](const ClientRequest& request) {
        if (request.hello.audit_ref == "bbbbbbbb") {
            waiting = true;
            until([&] { return release.load(); }, 6s);
        }
        return ClientApproval{true, request.permissions};
    };
    f.start();
    auto publisher = f.connect();
    auto pending = std::async(std::launch::async, [&] {
        return f.connect(SessionMode::Desktop, kPermissionAll, "bbbbbbbb");
    });
    require(until([&] { return waiting.load(); }), "second connection waits for approval");
    set_policy(*publisher, kRemoteConfigClipboard, 0);
    set_policy(*publisher, kRemoteConfigClipboard, kRemoteConfigClipboard);
    release = true;
    auto viewer = pending.get();
    require((viewer->permissions() & kPermissionClipboard) == 0,
            "off/on during approval cannot resurrect the pending permission grant");
    auto fresh = f.connect();
    require(fresh->permissions() & kPermissionClipboard, "fresh approval uses current policy defaults");
    finish(fresh);
    finish(viewer);
    finish(publisher);
}

void publisher_only_audio() {
    Fixture f;
    f.start();
    auto publisher = f.connect();
    auto follower = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionAudio);
    require(audio(*publisher), "publisher audio is independent of a muted follower");
    require(!audio(*follower, 120ms), "follower denied at admission receives no audio");
    finish(follower);
    finish(publisher);
}

void late_audio_source() {
    Fixture f;
    f.config.settings.allow_audio = false;
    f.start();
    auto publisher = f.connect();
    require(f.media->audio_sources == 0, "disabled policy opens no audio source");
    set_policy(*publisher, kRemoteConfigAudio, kRemoteConfigAudio);
    auto follower = f.connect();
    require(until([&] { return (follower->permissions() & kPermissionAudio) != 0; }) && audio(*follower),
            "enabling audio opens source and stream for a new authorized viewer");
    require(!audio(*publisher, 100ms), "enabling source does not grant audio to the old viewer");
    finish(follower);
    finish(publisher);
}

void desktop_switch_during_admission() {
    Fixture f;
    std::atomic<unsigned> follows{0};
    f.config.follow_desktop = [&](std::string*, bool* switched) {
        const auto call = ++follows;
        if (switched) *switched = call == 2;
        return true;
    };
    f.start();
    auto viewer = f.connect();
    require(keyframe(*viewer) && f.media->resets > 0,
            "desktop switch during admission waits for rebuilt capture and encoder");
    finish(viewer);
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    const auto run = [](const char* name, auto test) {
        std::printf("\n%s\n", name);
        try { test(); }
        catch (const std::exception& error) { ++failures; std::printf("FAIL %s\n", error.what()); }
    };
    run("static desktop, follower requests and audio", static_desktop_and_followers);
    run("global policy", [] { global_policy(false); });
    run("global policy with frozen UI permissions", [] { global_policy(true); });
    run("policy changed during approval", pending_approval);
    run("publisher-only audio", publisher_only_audio);
    run("audio enabled after admission", late_audio_source);
    run("desktop changed during admission", desktop_switch_during_admission);
    WSACleanup();
    std::printf("host parity: %d failed scenario(s)\n", failures);
    return failures ? 1 : 0;
}
