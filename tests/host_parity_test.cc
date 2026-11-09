#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <future>
#include <fstream>
#include <stdexcept>
#include <set>
#include <string>
#include "chast/security.h"
#include "chast/decoder.h"
#include "clipboard_stub.h"
#include "host_media_stub.h"
#include "service_control.h"
#include "elevation_handoff.h"
#include "platform.h"
#include "chast/user_context.h"

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
        config.power_request_factory = [](bool, std::string*) {
            return std::make_unique<ISessionPowerRequest>();
        };
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
            PermissionMask mask = kPermissionAll, std::string ref = {},
            uint16_t display = kDisplayAuto, ServerInfo* description = nullptr,
            VideoCodecMask decoders = kVideoCodecAll,
            std::optional<StreamCodecPreference> preference = {},
            VideoCodecMask decoders_444 = 0,
            VideoChroma prefer_chroma = VideoChroma::Yuv420) {
        QuicClientConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = port;
        cfg.password = "parity-test-password";
        cfg.pinned_fingerprint = host->status().fingerprint;
        cfg.requested_permissions = mask;
        cfg.session_mode = mode;
        cfg.decoding_codecs = decoders;
        cfg.decoding_444 = decoders_444;
        cfg.prefer_chroma = prefer_chroma;
        cfg.codec_preference = preference.value_or(config.settings.codec == VideoCodec::Hevc ?
            StreamCodecPreference::Hevc : config.settings.codec == VideoCodec::Av1 ?
            StreamCodecPreference::Av1 : StreamCodecPreference::H264);
        cfg.audit_ref = std::move(ref);
        std::string error;
        auto viewer = QuicSession::create_client(cfg, &error);
        if (!viewer) throw std::runtime_error("client create: " + error);
        Error result = viewer->connect(6s);
        if (!result.ok()) throw std::runtime_error(result.to_string());
        ServerInfo info;
        result = viewer->handshake(info, 8s, display);
        if (!result.ok()) throw std::runtime_error("handshake: " + result.to_string());
        if (description) *description = info;
        return viewer;
    }
};

struct FakeElevation : IElevationTask {
    std::atomic<ElevationCode> code{ElevationCode::Pending};
    std::atomic<bool> committed{false}, cancelled{false};
    ElevationState poll() override { return {0, code.load(), {}}; }
    void commit() override { committed = true; }
    void cancel() override { if (!committed) cancelled = true; }
};

ElevationState elevation_answer(QuicSession& viewer, uint32_t id, ElevationCode code) {
    ElevationState result;
    require(until([&] {
        ElevationState item;
        while (viewer.take_elevation_state(item)) {
            if (item.request_id == id && item.code == code) { result = item; return true; }
        }
        return false;
    }), "receive correlated elevation state");
    return result;
}

void elevation_permissions() {
    Fixture f;
    auto task = std::make_shared<FakeElevation>();
    std::atomic<unsigned> launches{0};
    f.config.elevation_state = [] { return ElevationState{0, ElevationCode::Available, {}}; };
    f.config.request_elevation = [&] { ++launches; return task; };
    f.start();
    auto viewer = f.connect();
    elevation_answer(*viewer, 0, ElevationCode::Available);
    auto follower = f.connect();
    const auto sent = follower->request_elevation(8);
    if (sent.ok()) elevation_answer(*follower, 8, ElevationCode::Denied);
    require(launches == 0, "view-only follower cannot launch UAC");
    auto files = f.connect(SessionMode::FileTransfer);
    elevation_answer(*files, 0, ElevationCode::Unsupported);
    require(viewer->request_elevation(1).ok(), "controller requests local UAC");
    elevation_answer(*viewer, 1, ElevationCode::Pending);
    require(viewer->request_elevation(2).ok(), "duplicate request reaches host");
    elevation_answer(*viewer, 2, ElevationCode::Busy);
    require(launches == 1 && !task->committed, "one pending launch; no premature takeover");
    for (const auto& connection : f.host->viewers()) {
        if (connection.controls_input) f.host->set_permissions(connection.id, kPermissionAll & ~kPermissionInput);
    }
    elevation_answer(*viewer, 1, ElevationCode::Denied);
    require(task->cancelled && !task->committed, "input revocation cancels pending elevation");
    files.reset(); follower.reset(); viewer.reset();
    f.host->stop(); f.host.reset();

    task = std::make_shared<FakeElevation>(); f.start(); viewer = f.connect();
    require(viewer->request_elevation(3).ok(), "request before disconnect");
    elevation_answer(*viewer, 3, ElevationCode::Pending);
    viewer.reset();
    require(until([&] { return task->cancelled.load(); }), "disconnect cancels uncommitted UAC");

    task = std::make_shared<FakeElevation>(); viewer = f.connect();
    require(viewer->request_elevation(4).ok(), "new controller can retry");
    elevation_answer(*viewer, 4, ElevationCode::Pending);
    task->code = ElevationCode::Restarting;
    elevation_answer(*viewer, 4, ElevationCode::Restarting);
    require(until([&] { return task->committed.load(); }), "host commits after queuing restarting response");
    viewer.reset(); f.host->stop();
    require(!task->cancelled, "ordinary teardown does not cancel committed handoff");
}

int elevation_child(int argc, char** argv) {
    if (argc != 5) return 2;
    std::string error;
    auto child = chast::host::ElevatedChild::receive(chast::host::widen(argv[2]),
        static_cast<DWORD>(std::stoul(argv[3])), std::stoull(argv[4]), &error, false);
    if (!child) { std::fprintf(stderr, "test child: %s\n", error.c_str()); return 3; }
    HostServerConfig config;
    config.settings = child->snapshot.settings;
    config.identity_user_token = child->identity_user_token;
    config.expected_identity_fingerprint = child->snapshot.fingerprint;
    config.serve_once = true;
    config.first_client_timeout_ms = 6000;
    use_empty_clipboard(config);
    auto media = host_test::use_media(config);
    auto server = HostServer::create(config, &error);
    if (!server) { std::fprintf(stderr, "test child create: %s\n", error.c_str()); return 4; }
    server->start();
    if (!child->announce_ready(server->status().fingerprint, &error)) {
        std::fprintf(stderr, "test child ready: %s\n", error.c_str()); return 5;
    }
    server->wait();
    return 0;
}

void elevation_handoff() {
    using namespace chast::host;
    auto denied = ElevationHandoff::start([](const std::wstring&, DWORD& error) -> HANDLE {
        error = ERROR_CANCELLED; return nullptr;
    });
    require(until([&] { return denied->finished(); }) && denied->poll().code == ElevationCode::Cancelled,
            "local UAC cancellation completes without stopping the host");
    std::string error;
    require(!ElevatedChild::receive(std::wstring(32, L'0'), GetCurrentProcessId(), 1, &error, false),
            "child rejects a reused or forged parent creation time before IPC");

    const auto launch = [](const std::wstring& args, DWORD& error) -> HANDLE {
        const auto path = executable_path();
        auto command = L"\"" + path + L"\" " + args;
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &process)) { error = GetLastError(); return nullptr; }
        CloseHandle(process.hThread); return process.hProcess;
    };
    for (const bool wrong_identity : {false, true}) {
        Fixture f; f.config.settings.max_viewers = 1;
        const auto secret = generate_totp_secret();
        if (!wrong_identity) require(protect_totp_secret(secret, SecretScope::CurrentUser,
            f.config.settings.totp_secret, &error), "protect original user's two-factor secret");
        f.start();
        ElevationSnapshot snapshot;
        snapshot.settings = f.config.settings;
        snapshot.config_path = narrow((f.root / L"原主机.ini").wstring());
        snapshot.fingerprint = f.host->status().fingerprint;
        auto task = ElevationHandoff::start(launch, process_is_elevated());
        require(until([&] { return task->poll().code != ElevationCode::Pending; }, 10s) &&
                task->poll().code == ElevationCode::Restarting, "authenticate the actual child and duplicate original token");
        require(f.host->running(), "old listener stays alive throughout preparation");
        task->commit();
        f.host->stop(); f.host.reset();
        if (wrong_identity) snapshot.fingerprint.assign(64, '0');
        require(task->supply(snapshot, &error), "send settings snapshot over authenticated IPC");
        require(until([&] { return task->finished(); }, 15s), "handoff reaches a terminal state");
        if (wrong_identity) {
            require(task->poll().code == ElevationCode::Failed, "identity change aborts the replacement process");
            f.start(); auto restored = f.connect();
            require(restored->peer_alive(), "original host can rebind and authenticate after failed takeover");
        } else {
            require(task->poll().code == ElevationCode::Elevated, "child reports same-identity listener ready");
            QuicClientConfig cfg; cfg.host = "127.0.0.1"; cfg.port = f.port;
            cfg.pinned_fingerprint = snapshot.fingerprint; cfg.password = "parity-test-password";
            std::vector<uint8_t> key;
            require(parse_totp_secret(secret, key), "decode test authenticator secret");
            char code[7]{};
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::snprintf(code, sizeof(code), "%06u", totp_code(key, static_cast<uint64_t>(seconds)));
            cfg.totp = code;
            auto viewer = QuicSession::create_client(cfg, &error);
            require(viewer && viewer->connect(4s).ok(), "SChannel uses original user's cached key after impersonation ends");
            ServerInfo info;
            require(viewer->handshake(info, 4s).ok(), "password plus original user's DPAPI/TOTP authenticate after handoff");
            ClientStats stats;
            require(viewer->finish_client(stats, 3s).ok(), "handed-off session closes cleanly");
            viewer.reset();
        }
    }
    auto cancelled = ElevationHandoff::start(launch, process_is_elevated());
    require(until([&] { return cancelled->poll().code == ElevationCode::Restarting; }, 10s), "child prepared for cancellation");
    cancelled->cancel();
    require(until([&] { return cancelled->finished(); }, 10s) && cancelled->poll().code == ElevationCode::Cancelled,
            "cancellation before commit closes prepared child without binding a port");
}

void clipboard_directions() {
    for (const auto direction : {ClipboardDirection::Both, ClipboardDirection::HostToViewer,
                                 ClipboardDirection::ViewerToHost, ClipboardDirection::Disabled}) {
        Fixture f;
        f.config.settings.clipboard_direction = direction;
        auto revision = std::make_shared<std::atomic<unsigned>>(0);
        auto reads = std::make_shared<std::atomic<unsigned>>(0);
        auto writes = std::make_shared<std::atomic<unsigned>>(0);
        auto file_writes = std::make_shared<std::atomic<unsigned>>(0);
        const auto source = f.root / "copy-source.txt";
        const std::string bytes = "clipboard bytes with browser transfer disabled";
        { std::ofstream out(source); out << bytes; }
        f.config.clipboard_read = [revision, reads](ClipboardContent& item) {
            ++*reads;
            const auto text = "local-" + std::to_string(revision->load());
            item = {}; item.formats.push_back({ClipboardFormat::Text, {text.begin(), text.end()}});
            return true;
        };
        f.config.clipboard_read_files = [revision, source](std::vector<std::string>& paths) {
            if (*revision < 2) return false;
            paths = {source.string()}; return true;
        };
        f.config.clipboard_write = [writes](const ClipboardContent&) { ++*writes; return true; };
        f.config.clipboard_write_files = [file_writes, bytes](const std::vector<std::string>& paths) {
            if (paths.size() != 1) return false;
            std::ifstream in(paths.front(), std::ios::binary);
            if (std::string((std::istreambuf_iterator<char>(in)), {}) != bytes) return false;
            ++*file_writes; return true;
        };
        f.start();
        ServerInfo info;
        auto viewer = f.connect(SessionMode::Desktop, kPermissionAll, {}, kDisplayAuto, &info);
        require(info.clipboard_direction == direction, "host announces its local clipboard direction");
        // A normal browser request still works even with clipboard disabled.
        require(viewer->request_file_list(41, "").ok(), "browser transfer has an independent grant");
        FileListResult listing;
        require(until([&] { return viewer->take_file_list_result(listing); }) && listing.code == FileCode::Ok,
                "clipboard direction does not restrict ordinary directory browsing");
        f.host->set_permissions(kPermissionAll & ~kPermissionFileTransfer);
        require(until([&] { return !(viewer->permissions() & kPermissionFileTransfer); }), "ordinary files revoked");
        require(!viewer->request_file_list(42, "").ok(), "browser stays disabled during clipboard transfer");
        if (clipboard_from_host(direction)) {
            require(until([&] { return *reads >= 2; }), "outbound clipboard establishes a baseline");
            *revision = 1;
            ClipboardContent content;
            require(until([&] { return viewer->take_clipboard_content(content); }) && content.text() == "local-1",
                    "permitted host text arrives after baseline");
            *revision = 2;
            require(until([&] { return viewer->take_clipboard_content(content) && content.find(ClipboardFormat::FileList); }),
                    "host announces copied files with browser permission disabled");
            require(viewer->request_file_download(43, ".chast-clipboard/0").ok(), "clipboard download uses its own direction");
            FileDownloadResult answer;
            require(until([&] { return viewer->take_file_download_result(answer); }) && answer.code == FileCode::Ok,
                    "clipboard download accepted independently of browser transfer");
            FileOffer offer;
            require(until([&] { return viewer->take_file_offer(offer); }) && offer.clipboard,
                    "outgoing clipboard offer retains its permission class");
            require(viewer->send_file_answer(offer.transfer_id, FileCode::Ok, 0).ok(), "accept copied file");
            std::string landed;
            require(until([&] {
                FileChunk chunk;
                if (viewer->receive_file_chunk(chunk, 20ms).ok()) landed.append(reinterpret_cast<char*>(chunk.data.data()), chunk.data.size());
                return landed.size() == bytes.size();
            }) && landed == bytes, "clipboard download bytes arrive intact");
            require(viewer->send_file_result(offer.transfer_id, FileCode::Ok, landed.size()).ok(), "complete copied file");
        } else {
            *revision = 2;
            require(!viewer->request_file_download(43, ".chast-clipboard/0").ok(), "forbidden clipboard download refused");
        }
        const auto text_sent = viewer->send_clipboard(91, "remote text");
        require(text_sent.ok() == clipboard_to_host(direction), "legacy clipboard text obeys direction");
        ClipboardContent content; content.seq = 92;
        content.formats.push_back({ClipboardFormat::Text, {'r','i','c','h'}});
        require(viewer->send_clipboard_content(content).ok() == clipboard_to_host(direction), "rich clipboard obeys direction");
        FileOffer upload; upload.transfer_id = 77; upload.name = "incoming.txt"; upload.clipboard = true;
        upload.size = bytes.size();
        require(sha256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), upload.sha256), "hash copied file");
        if (clipboard_to_host(direction)) {
            require(until([&] { return *writes > 0; }), "allowed incoming text writes the host clipboard");
            content = {}; content.seq = 93;
            content.formats.push_back({ClipboardFormat::FileList, encode_clipboard_file_list({{upload.name, upload.size, {}}})});
            require(viewer->send_clipboard_content(content).ok(), "announce viewer clipboard files");
            // Rich content uses another stream; wait for staging before the offer.
            std::this_thread::sleep_for(300ms);
            require(viewer->offer_file(upload).ok(), "clipboard upload independent of browser permission");
            FileAnswer answer;
            require(until([&] { return viewer->take_file_answer(answer); }) && answer.code == FileCode::Ok,
                    "host accepts announced clipboard upload");
            require(viewer->send_file_data(upload.transfer_id, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()).ok() &&
                    viewer->finish_file_upload(upload.transfer_id).ok(), "clipboard stream sends and finishes without file grant");
            FileResult result;
            require(until([&] { return viewer->take_file_result(result); }) && result.code == FileCode::Ok &&
                    until([&] { return *file_writes == 1; }), "copied file bytes reach the host clipboard sink");
        } else {
            require(!viewer->offer_file(upload).ok(), "forbidden clipboard file upload refused");
        }
        require(clipboard_from_host(direction) || *reads == 0, "receive-only host never reads its local clipboard");
        require(clipboard_to_host(direction) || *writes == 0, "send-only host never writes peer clipboard data");
        if (direction == ClipboardDirection::Both) {
            // Revoke an active clipboard stream while ordinary files remain
            // granted. The marked stream must not borrow their permission.
            f.host->set_permissions(kPermissionAll);
            require(until([&] { return (viewer->permissions() & kPermissionFileTransfer) != 0; }), "restore independent browser grant");
            upload.transfer_id = 78; upload.name = "revoked-copy.txt";
            content = {}; content.seq = 94;
            content.formats.push_back({ClipboardFormat::FileList, encode_clipboard_file_list({{upload.name, upload.size, {}}})});
            require(viewer->send_clipboard_content(content).ok(), "announce clipboard stream for revocation");
            std::this_thread::sleep_for(300ms);
            require(viewer->offer_file(upload).ok(), "offer clipboard stream for revocation");
            FileAnswer answer;
            require(until([&] { return viewer->take_file_answer(answer); }) && answer.code == FileCode::Ok,
                    "clipboard stream accepted before revocation");
            require(viewer->send_file_data(upload.transfer_id, reinterpret_cast<const uint8_t*>(bytes.data()), 4).ok(),
                    "clipboard transfer starts before revocation");
            f.host->set_permissions(kPermissionAll & ~kPermissionClipboard);
            require(until([&] { return !(viewer->permissions() & kPermissionClipboard); }) &&
                    !viewer->send_file_data(upload.transfer_id, reinterpret_cast<const uint8_t*>(bytes.data()), 4).ok() &&
                    !viewer->finish_file_upload(upload.transfer_id).ok(), "active clipboard stream stops despite ordinary file grant");
            require(viewer->request_file_list(44, "").ok() &&
                    until([&] { return viewer->take_file_list_result(listing); }) && listing.code == FileCode::Ok,
                    "clipboard revocation leaves browser transfer available");
        }
        f.host->set_permissions(kPermissionAll & ~(kPermissionClipboard | kPermissionFileTransfer));
        require(until([&] { return !(viewer->permissions() & kPermissionClipboard); }) &&
                !viewer->send_clipboard(94, "revoked").ok() && !viewer->offer_file(upload).ok(),
                "revoking clipboard closes text and copied-file paths");
        viewer.reset();
    }
}

void finish(std::unique_ptr<QuicSession>& viewer) {
    if (viewer) {
        ClientStats stats;
        require(viewer->finish_client(stats, 3s).ok(), "viewer ends cleanly");
        viewer.reset();
    }
}
void session_power_requests() {
    struct Counts { std::atomic<int> system{0}, display{0}, created{0}; };
    struct Request final : ISessionPowerRequest {
        Request(std::shared_ptr<Counts> counts, bool screen) : counts(std::move(counts)), screen(screen) {
            ++this->counts->system; ++this->counts->created;
            if (screen) ++this->counts->display;
        }
        ~Request() override { if (screen) --counts->display; --counts->system; }
        std::shared_ptr<Counts> counts;
        bool screen;
    };
    auto counts = std::make_shared<Counts>();
    const auto factory = [counts](bool screen, std::string*) {
        return std::make_unique<Request>(counts, screen);
    };
    {
        std::atomic<bool> waiting{false}, refuse{false};
        std::atomic<int> approvals{0};
        Fixture f;
        f.config.service_approval = true;
        f.config.settings.auth_mode = HostAuthMode::Click;
        f.config.power_request_factory = factory;
        f.config.approve_client = [&](const ClientRequest& request) {
            if (++approvals == 1) {
                waiting = true;
                until([&] { return refuse.load(); }, 5s);
                return ClientApproval{false, 0};
            }
            return ClientApproval{true, request.permissions};
        };
        f.start();
        require(counts->created == 0, "idle listener holds no power request");
        auto denied = std::async(std::launch::async, [&] {
            try { auto viewer = f.connect(SessionMode::FileTransfer); return false; }
            catch (const std::exception&) { return true; }
        });
        require(until([&] { return waiting.load(); }) && counts->created == 0,
                "connection awaiting approval cannot hold the host awake");
        refuse = true;
        require(denied.get() && counts->created == 0, "refused connection creates no power request");
        auto desktop = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput);
        auto files = f.connect(SessionMode::FileTransfer);
        auto terminal = f.connect(SessionMode::Terminal);
        require(counts->system == 3 && counts->display == 1,
                "view-only desktop keeps display awake; files and terminal keep only system awake");
        finish(desktop);
        require(until([&] { return counts->system == 2 && counts->display == 0; }),
                "desktop exit releases its display request while other sessions stay awake");
        finish(files);
        require(until([&] { return counts->system == 1; }), "one connection cannot release another's power request");
        f.host->stop();
        require(counts->system == 0 && counts->display == 0, "host shutdown releases the last request");
    }
    {
        Fixture f;
        f.config.service_mode = false;
        f.config.settings.max_viewers = 1;
        f.config.power_request_factory = factory;
        f.start();
        for (int i = 0; i < 2; ++i) {
            auto files = f.connect(SessionMode::FileTransfer);
            require(counts->system == 1 && counts->display == 0, "sequential user host acquires per connection");
            finish(files);
            require(until([&] { return counts->system == 0; }), "sequential host releases before next viewer");
        }
    }
    {
        Fixture f;
        f.config.settings.keep_awake = false;
        f.config.power_request_factory = factory;
        const int before = counts->created;
        f.start();
        auto files = f.connect(SessionMode::FileTransfer);
        require(counts->created == before, "disabled local policy creates no power request");
        finish(files);
    }
    {
        Fixture f;
        f.config.power_request_factory = factory;
        f.config.capture_factory = [](const DisplayInfo&, const CaptureConfig&, std::string* error) {
            *error = "power lifetime capture failure";
            return std::unique_ptr<IDxgiCapturer>{};
        };
        f.start();
        const int before = counts->created;
        bool failed = false;
        try { auto viewer = f.connect(); } catch (const std::exception&) { failed = true; }
        require(failed && until([&] { return counts->created > before && counts->system == 0; }),
                "capture failure after admission releases the request");
    }
    {
        std::atomic<int> attempts{0};
        Fixture f;
        f.config.power_request_factory = [&](bool, std::string* error) {
            ++attempts; *error = "power requests unavailable";
            return std::unique_ptr<ISessionPowerRequest>{};
        };
        f.start();
        auto files = f.connect(SessionMode::FileTransfer);
        require(attempts == 1 && files->send_chat("still connected").ok(),
                "power request failure does not abort an authenticated session");
        finish(files);
    }
}
bool keyframe(QuicSession& viewer) {
    return until([&] {
        FrameHeader header;
        std::vector<uint8_t> data;
        return viewer.receive_frame(header, data, 50ms).ok() && header.idr && !data.empty();
    });
}
bool codec_update(QuicSession& viewer, VideoCodec codec) {
    // A real viewer consumes old in-flight video while awaiting StreamInfo.
    // Otherwise v52 correctly pauses its delivery window after 500 ms.
    return until([&] {
        ServerInfo info;
        if (viewer.take_stream_update(info) && info.codec == codec) return true;
        FrameHeader frame;
        std::vector<uint8_t> bytes;
        viewer.receive_frame(frame, bytes, 20ms);
        return false;
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
            follow.code == DisplayFollowCode::Ok, "each subscriber can independently enable display follow");
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
    // RustDesk static_refresh repeats an idle monitor in a bounded burst
    // (at most 30 fps) so rate control can refine the last picture.
    const unsigned refined = f.media->encodes + f.media->repeats - calls;
    require(refined <= 6, "idle desktop refinement is capped at 30 fps");

    f.media->hold_frames = true;
    f.media->reset_requested = true;
    require(until([&] { return f.media->resets > 0; }), "capture reset observed");
    while (publisher->receive_frame(frame, payload, 1ms).ok()) {}
    require(publisher->request_keyframe().ok(), "request IDR during capture reset");
    require(!publisher->receive_frame(frame, payload, 180ms).ok(), "old desktop is not replayed after reset");
    f.media->hold_frames = false;
    require(keyframe(*publisher), "new desktop frame resumes with IDR");
    finish(publisher);
    require(f.media->audio_sources == 1, "shared audio source survives the original viewer leaving");
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

struct LoopbackEcho {
    SOCKET listener = INVALID_SOCKET;
    std::atomic<bool> echoed{false};
    std::atomic<bool> closed{false};
    std::atomic<bool> stopping{false};
    std::thread worker;
    uint16_t port = 0;
    LoopbackEcho() {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(listener != INVALID_SOCKET, "create the loopback echo listener");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        require(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
                "bind the loopback echo listener");
        require(listen(listener, 1) == 0, "listen on the loopback echo port");
        int length = sizeof(addr);
        require(getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &length) == 0,
                "read the loopback echo port");
        port = ntohs(addr.sin_port);
        worker = std::thread([this] {
            SOCKET client = INVALID_SOCKET;
            char buffer[256];
            while (!stopping.load()) {
                fd_set ready;
                FD_ZERO(&ready);
                FD_SET(client == INVALID_SOCKET ? listener : client, &ready);
                timeval wait{0, 100000};
                const int signalled = select(0, &ready, nullptr, nullptr, &wait);
                if (signalled <= 0) {
                    continue;
                }
                if (client == INVALID_SOCKET) {
                    client = accept(listener, nullptr, nullptr);
                    continue;
                }
                const int got = recv(client, buffer, sizeof(buffer), 0);
                if (got <= 0) {
                    closed.store(true);
                    break;
                }
                if (send(client, buffer, got, 0) == got) {
                    echoed.store(true);
                }
            }
            if (client != INVALID_SOCKET) {
                closesocket(client);
            }
        });
    }
    ~LoopbackEcho() {
        stopping.store(true);
        if (worker.joinable()) worker.join();
        if (listener != INVALID_SOCKET) closesocket(listener);
    }
};

void running_services_revocation() {
    LoopbackEcho echo;
    Fixture f;
    f.config.settings.allow_port_forward = true;
    std::mutex log_mutex;
    std::vector<std::string> logs;
    f.config.log = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(log_mutex);
        logs.push_back(line);
    };
    f.start();
    auto viewer = f.connect();
    require((viewer->permissions() & (kPermissionPortForward | kPermissionTerminal)) ==
                (kPermissionPortForward | kPermissionTerminal),
            "desktop session receives forwarding and console grants");

    require(viewer->request_port_forward(1, "127.0.0.1", echo.port).ok(),
            "register a loopback forwarding target");
    PortForwardResult forward_result;
    require(until([&] { return viewer->take_port_forward_result(forward_result); }) &&
            forward_result.code == PortForwardCode::Ok, "the host approves the target");
    PortForwardHandle handle = 0;
    require(viewer->open_port_forward_connection(1, 7, handle).ok(),
            "open one forwarded connection");
    require(viewer->wait_port_forward_connection(handle, 4s).ok(),
            "the host dials the loopback service");
    const char ping[] = "ping";
    require(viewer->send_port_forward_data(handle,
                reinterpret_cast<const uint8_t*>(ping), sizeof(ping) - 1, 1s).ok(),
            "bytes enter the host relay");
    std::vector<uint8_t> chunk;
    require(until([&] {
        const Error received =
            viewer->receive_port_forward_data(handle, chunk, 100ms);
        return received.ok() &&
               std::string(chunk.begin(), chunk.end()) == "ping";
    }, 4s) && echo.echoed.load(), "the loopback service echoes through the host");

    require(viewer->request_terminal(1, 80, 24).ok(), "request a console");
    TerminalResult terminal;
    require(until([&] { return viewer->take_terminal_result(terminal); }) &&
            terminal.code == TerminalCode::Ok, "the host starts the console");
    require(viewer->open_terminal_input(1).ok(), "open the console uplink");
    const std::string command = "echo chast-revocation\r\n";
    require(viewer->send_terminal_input(1,
                reinterpret_cast<const uint8_t*>(command.data()), command.size(), 1s).ok(),
            "type into the console");
    bool printed = false;
    require(until([&] {
        std::vector<uint8_t> output;
        bool ended = false;
        while (viewer->take_terminal_output(1, output, ended)) {
            if (std::string(output.begin(), output.end())
                    .find("chast-revocation") != std::string::npos) {
                printed = true;
            }
        }
        return printed;
    }, 6s), "the console echoes the command");

    f.host->set_permissions(kPermissionAll &
                            ~(kPermissionPortForward | kPermissionTerminal));
    require(until([&] {
        return (viewer->permissions() & (kPermissionPortForward | kPermissionTerminal)) == 0;
    }), "revocation reaches the viewer as a capability update");
    require(until([&] {
        return !viewer->send_port_forward_data(handle,
                   reinterpret_cast<const uint8_t*>(ping), sizeof(ping) - 1, 100ms).ok();
    }), "the revoked forward refuses new bytes");
    require(until([&] {
        return !viewer->send_terminal_input(1,
                   reinterpret_cast<const uint8_t*>(command.data()), command.size(), 100ms).ok();
    }), "the revoked console refuses new keystrokes");
    require(until([&] { return echo.closed.load(); }, 4s),
            "the host relay closes the forwarded TCP socket");
    require(until([&] {
        std::lock_guard<std::mutex> lock(log_mutex);
        for (const std::string& line : logs) {
            if (line.find("terminal access was revoked") != std::string::npos) {
                return true;
            }
        }
        return false;
    }, 4s), "the host relay tears the console and its child down");
    finish(viewer);
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
        // Both the connection and the GPU service attach their own thread.
        // Simulate a switch on the connection's first explicit desktop poll.
        if (switched) *switched = ++follows == 1;
        return true;
    };
    f.start();
    auto viewer = f.connect();
    require(keyframe(*viewer) && f.media->resets > 0,
            "desktop switch during admission waits for rebuilt capture and encoder");
    finish(viewer);
}

struct TwoDisplays {
    Fixture fixture;
    std::shared_ptr<host_test::MediaState> left = fixture.media;
    std::shared_ptr<host_test::MediaState> right = std::make_shared<host_test::MediaState>();
    std::atomic<bool> refuse_right{false};
    std::atomic<bool> reversed{false};
    std::atomic<bool> remove_left{false}, remove_right{false};
    std::atomic<int> right_origin{640};
    std::atomic<unsigned> right_height{480}, right_rotation{0}, right_adapter{0};
    explicit TwoDisplays(VideoCodec codec = VideoCodec::H264, bool adaptive = false) {
        fixture.config.settings.codec = codec;
        fixture.config.adaptive_bitrate = adaptive;
        fixture.config.displays = [this] {
            auto a = host_test::display();
            auto b = a;
            b.device_name = L"test-display-2";
            b.primary = false;
            b.width = 800;
            b.height = right_height;
            b.rotation_degrees = right_rotation;
            b.adapter_luid.LowPart = right_adapter;
            const int origin = right_origin;
            b.rect = {origin, 0, origin + 800, static_cast<LONG>(b.height)};
            left->set_display(a); right->set_display(b);
            if (remove_left && remove_right) return std::vector<DisplayInfo>{};
            if (remove_left) return std::vector<DisplayInfo>{b};
            if (remove_right) return std::vector<DisplayInfo>{a};
            return reversed ? std::vector<DisplayInfo>{b, a} : std::vector<DisplayInfo>{a, b};
        };
        fixture.config.capture_factory = [this](const DisplayInfo& display,
                const CaptureConfig&, std::string* error) -> std::unique_ptr<IDxgiCapturer> {
            if (display.width == 800 && refuse_right) {
                *error = "simulated disconnected output";
                return nullptr;
            }
            return std::make_unique<host_test::Capture>(display.width == 800 ? right : left, display);
        };
        fixture.config.encoder_factory = [this](ID3D11Device*, const EncoderConfig& cfg, std::string*) {
            return std::make_unique<host_test::Encoder>(cfg.width == 800 ? right : left, cfg);
        };
        fixture.start();
    }
    ~TwoDisplays() { fixture.host->stop(); }
};

bool picture(QuicSession& viewer, uint16_t width) {
    return until([&] {
        FrameHeader header;
        std::vector<uint8_t> data;
        return viewer.receive_frame(header, data, 50ms).ok() && header.idr &&
            header.width == width && data.size() == 6 && data.back() == width / 16;
    });
}
void switch_to(QuicSession& viewer, uint16_t target, DisplaySwitchCode expected) {
    require(viewer.request_display_switch(target).ok(), "send independent display selection");
    uint16_t returned = kDisplayAuto;
    DisplaySwitchCode result{};
    std::string reason;
    require(until([&] { return viewer.take_display_switch_result(returned, result, reason); }) &&
            returned == target && result == expected, "display selection returns expected result");
}

CaptureDisplaysResult subscriptions(QuicSession& viewer, CaptureDisplaysRequest request,
                                    DisplaySwitchCode expected = DisplaySwitchCode::Ok) {
    static uint32_t sequence = 0;
    request.request_id = ++sequence;
    require(viewer.request_capture_displays(request).ok(), "send production display subscription request");
    CaptureDisplaysResult result;
    require(until([&] { return viewer.take_capture_displays_result(result) && result.request_id == request.request_id; }) &&
            result.code == expected, "production host answers subscription operation");
    return result;
}
void subscription_pictures(QuicSession& viewer, const CaptureDisplaysResult& result,
                           std::optional<VideoCodec> codec = {}) {
    std::map<uint32_t, uint16_t> waiting;
    for (const auto& stream : result.streams) {
        waiting.emplace(stream.stream_id, stream.info.width);
        require(viewer.request_display_keyframe(stream.stream_id).ok(), "request subscribed display keyframe");
    }
    require(until([&] {
        FrameHeader header;
        std::vector<uint8_t> payload;
        if (viewer.receive_frame(header, payload, 20ms).ok()) {
            auto found = waiting.find(header.stream_id);
            if (found != waiting.end() && header.idr && (!codec || header.codec == *codec) &&
                header.width == found->second && payload.size() == 6 && payload.back() == found->second / 16)
                waiting.erase(found);
        }
        return waiting.empty();
    }), "every subscribed stream sends its own static picture and keyframe");
}
void multiple_displays_one_connection() {
    TwoDisplays f;
    auto viewer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    require(picture(*viewer, 640), "initial desktop stream works before multi-display activation");
    auto both = subscriptions(*viewer, {0, {1, 1, 99}, {}, {}});
    require(both.streams.size() == 2 && both.streams[0].stream_id != both.streams[1].stream_id,
            "one view-only connection subscribes two distinct displays");
    subscription_pictures(*viewer, both);
    require(f.left->capture_created == 1 && f.right->capture_created == 1 &&
            f.left->encoder_created == 1 && f.right->encoder_created == 1,
            "multi-display connection creates exactly one pipeline per display");
    StreamOptions quality;
    quality.codec = StreamCodecPreference::H264;
    quality.max_fps = 10;
    require(viewer->request_stream_options(quality).ok(), "set common FPS for two static display streams");
    StreamOptionsResult quality_result;
    require(until([&] { return viewer->take_stream_options_result(quality_result); }) && quality_result.code == StreamOptionsCode::Ok,
            "multi-display quality preference is accepted");
    std::set<uint32_t> rate_updates;
    require(until([&] {
        DisplayStreamInfo info;
        while (viewer->take_display_stream_update(info))
            if (info.info.framerate == 10) rate_updates.insert(info.stream_id);
        return rate_updates.size() == 2;
    }), "both static displays announce their shared FPS without waiting for desktop motion");
    const auto left_id = both.streams[0].stream_id, right_id = both.streams[1].stream_id;
    auto other = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    require(picture(*other, 640) && f.left->capture_created == 1,
            "another connection shares a multi-stream viewer's display service");
    auto retained = subscriptions(*viewer, {0, {}, {1}, {}});
    require(retained.streams.size() == 1 && retained.streams[0].stream_id == right_id,
            "sub keeps the listed stream without changing its ID");
    subscription_pictures(*viewer, retained);
    require(other->request_keyframe().ok() && picture(*other, 640) && f.left->capture_destroyed == 0,
            "removing one subscription leaves another connection's picture intact");
    both = subscriptions(*viewer, {0, {0}, {}, {}});
    require(both.streams.size() == 2 && both.streams[0].stream_id != left_id &&
            both.streams[1].stream_id != left_id, "re-added display gets a fresh connection stream ID");
    subscription_pictures(*viewer, both);
    f.right->reset_requested = true;
    subscription_pictures(*viewer, both);
    require(f.right->resets == 1 && f.left->capture_created == 1 && f.right->capture_created == 1,
            "one display reset preserves both subscriptions and capture ownership");

    const std::string contents = "file upload survives a connection with zero desktop subscriptions";
    FileOffer offer;
    offer.transfer_id = 93; offer.name = "empty-video.txt"; offer.size = contents.size();
    require(sha256(reinterpret_cast<const uint8_t*>(contents.data()), contents.size(), offer.sha256), "hash empty-video upload");
    require(viewer->offer_file(offer).ok(), "start upload before unsubscribing all video");
    FileAnswer answer;
    require(until([&] { return viewer->take_file_answer(answer); }) && answer.code == FileCode::Ok, "accept empty-video upload");
    const auto split = contents.size() / 2;
    require(viewer->send_file_data(offer.transfer_id, reinterpret_cast<const uint8_t*>(contents.data()), split).ok(),
            "send upload prefix with video active");
    auto empty = subscriptions(*viewer, {});
    require(empty.streams.empty(), "empty set stops every video subscription");
    require(until([&] { return f.right->capture_destroyed == 1; }) && f.left->capture_destroyed == 0,
            "zero subscriptions release unused pipelines while another connection retains its source");
    FrameHeader header; std::vector<uint8_t> payload;
    while (viewer->receive_frame(header, payload, 10ms).ok()) {}
    require(!viewer->receive_frame(header, payload, 100ms).ok() && viewer->peer_alive(),
            "zero subscriptions send no further video and keep the connection open");
    require(audio(*viewer), "system audio continues with no video subscriptions");
    require(viewer->send_file_data(offer.transfer_id, reinterpret_cast<const uint8_t*>(contents.data() + split),
            contents.size() - split).ok() && viewer->finish_file_upload(offer.transfer_id).ok(),
            "complete upload while video is unsubscribed");
    FileResult file;
    require(until([&] { return viewer->take_file_result(file); }) && file.code == FileCode::Ok,
            "upload session and digest survive empty video selection");
    std::ifstream saved(f.fixture.root / offer.name, std::ios::binary);
    const std::string landed((std::istreambuf_iterator<char>(saved)), {});
    require(landed == contents, "uploaded bytes remain correct across empty video selection");
    saved.close();
    StreamOptions options;
    options.codec = StreamCodecPreference::Hevc;
    require(viewer->request_stream_options(options).ok(), "change encoding vote while video is unsubscribed");
    StreamOptionsResult option_result;
    require(until([&] { return viewer->take_stream_options_result(option_result); }) && option_result.code == StreamOptionsCode::Ok,
            "empty video selection still accepts stream preferences");
    both = subscriptions(*viewer, {0, {}, {}, {0, 1}});
    require(both.streams.size() == 2, "set reopens both sources within the same connection");
    subscription_pictures(*viewer, both, VideoCodec::Hevc);
    finish(viewer);
    require(other->request_keyframe().ok() && picture(*other, 640), "multi-stream viewer exit leaves another viewer usable");
    finish(other);
    require(until([&] { return f.left->capture_created == f.left->capture_destroyed &&
                             f.right->capture_created == f.right->capture_destroyed; }) &&
            f.left->wrong_thread == 0 && f.right->wrong_thread == 0,
            "all multi-stream GPU resources release on their owning workers");
}
void multiple_display_topology() {
    TwoDisplays f;
    auto viewer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    auto both = subscriptions(*viewer, {0, {}, {}, {0, 1}});
    subscription_pictures(*viewer, both);
    std::map<uint16_t, uint32_t> ids;
    for (const auto& stream : both.streams) ids[stream.display_index] = stream.stream_id;
    auto updated = [&](auto predicate) {
        CaptureDisplaysResult result;
        require(until([&] {
            while (viewer->take_capture_displays_result(result)) if (predicate(result)) return true;
            FrameHeader h; std::vector<uint8_t> data; viewer->receive_frame(h, data, 10ms);
            return false;
        }, 5s), "host publishes topology subscription update without a selection request");
        return result;
    };
    f.reversed = true;
    auto reversed = updated([&](const auto& result) {
        if (result.streams.size() != 2) return false;
        for (const auto& stream : result.streams)
            if (stream.stream_id == ids[0] && stream.display_index == 1) return true;
        return false;
    });
    subscription_pictures(*viewer, reversed);
    require(f.left->resets == 0 && f.right->resets == 0,
            "index reorder preserves existing capture and encoder resources");
    auto observer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    auto observed = subscriptions(*observer, {0, {}, {}, {0}});
    subscription_pictures(*observer, observed);
    f.right_origin = -800;
    auto moved = updated([&](const auto& result) {
        for (const auto& stream : result.streams)
            if (stream.stream_id == ids[1] && stream.info.origin_x == -800) return true;
        return false;
    });
    subscription_pictures(*viewer, moved);
    require(until([&] {
        DisplayStreamInfo info;
        while (observer->take_display_stream_update(info)) if (info.info.origin_x == -800) return true;
        CaptureDisplaysResult result;
        while (observer->take_capture_displays_result(result))
            for (const auto& stream : result.streams) if (stream.info.origin_x == -800) return true;
        FrameHeader h; std::vector<uint8_t> data; observer->receive_frame(h, data, 10ms);
        return false;
    }), "every observer receives the new origin for its selected display");
    require(f.right->resets == 1 && f.left->resets == 0,
            "layout geometry rebuild is shared once and leaves the other source intact");
    f.right_height = 600;
    f.right_rotation = 90;
    auto resized = updated([&](const auto& result) {
        for (const auto& stream : result.streams)
            if (stream.stream_id == ids[1] && stream.info.displays[0].rotation_degrees == 90) return true;
        return false;
    });
    subscription_pictures(*viewer, resized);
    const auto matches_size = [&](const DisplayStreamInfo& info) {
        return info.stream_id == ids[1] && info.info.height == 600 && info.info.source_height == 600;
    };
    // A complete topology result supersedes any earlier incremental metadata.
    bool resized_info = std::any_of(resized.streams.begin(), resized.streams.end(), matches_size);
    bool resized_frame = false;
    require(viewer->request_display_keyframe(ids[1]).ok(), "request resized display keyframe");
    require(until([&] {
        DisplayStreamInfo info;
        while (viewer->take_display_stream_update(info))
            resized_info = matches_size(info) || resized_info;
        CaptureDisplaysResult result;
        while (viewer->take_capture_displays_result(result))
            resized_info = std::any_of(result.streams.begin(), result.streams.end(), matches_size) || resized_info;
        FrameHeader h; std::vector<uint8_t> data;
        if (viewer->receive_frame(h, data, 10ms).ok() && h.stream_id == ids[1] && h.idr && h.height == 600)
            resized_frame = true;
        return resized_info && resized_frame;
    }), "resize and rotation announce new dimensions on the original stream");
    require(f.right->resets == 2 && f.left->resets == 0,
            "resize and rotation rebuild only the affected shared source");
    f.remove_left = true;
    auto retained = updated([&](const auto& result) {
        return result.streams.size() == 1 && result.streams[0].stream_id == ids[1] &&
            result.streams[0].display_index == 0;
    });
    subscription_pictures(*viewer, retained);
    require(until([&] { return f.left->capture_destroyed == 1; }) && viewer->peer_alive(),
            "unplugged source is retired instead of aliasing the surviving index zero");
    f.remove_right = true;
    updated([](const auto& result) { return result.streams.empty(); });
    require(viewer->peer_alive() && audio(*viewer), "all outputs disappearing keeps session services alive");
    f.remove_right = false;
    auto restored = updated([&](const auto& result) {
        return result.streams.size() == 1 && result.streams[0].display_index == 0 &&
            result.streams[0].stream_id != ids[1];
    });
    subscription_pictures(*viewer, restored);
    require(viewer->peer_alive(), "a surviving primary display resumes after total unplug");
    subscriptions(*viewer, {});
    f.remove_left = false;
    updated([](const auto& result) { return result.streams.empty(); });
    ServerInfo list;
    require(until([&] { return viewer->take_stream_update(list) && list.displays.size() == 2; }),
            "empty subscription still receives newly available displays");
    FrameHeader h; std::vector<uint8_t> bytes;
    while (viewer->receive_frame(h, bytes, 10ms).ok()) {}
    require(!viewer->receive_frame(h, bytes, 100ms).ok(), "explicit empty set does not auto-subscribe after hotplug");
    auto again = subscriptions(*viewer, {0, {}, {}, {0, 1}});
    subscription_pictures(*viewer, again);
    finish(observer);
    std::map<uint16_t, uint32_t> new_ids;
    for (const auto& stream : again.streams) new_ids[stream.display_index] = stream.stream_id;
    f.right_adapter = 1;
    auto replaced = updated([&](const auto& result) {
        return result.streams.size() == 1 && result.streams[0].stream_id == new_ids[1];
    });
    subscription_pictures(*viewer, replaced);
    auto readded = subscriptions(*viewer, {0, {0}, {}, {}});
    require(readded.streams.size() == 2 &&
            std::none_of(readded.streams.begin(), readded.streams.end(), [&](const auto& stream) {
                return stream.stream_id == new_ids[0];
            }), "same display name on a replacement adapter gets a fresh stream identity");
    subscription_pictures(*viewer, readded);
    finish(viewer);
}

void multiple_display_recovery_isolation() {
    TwoDisplays f(VideoCodec::Hevc);
    auto viewer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    auto both = subscriptions(*viewer, {0, {}, {}, {0, 1}});
    subscription_pictures(*viewer, both);
    f.right->reinit_fails = true;
    f.right->reset_requested = true;
    require(until([&] { return f.right->resets > 0; }), "one source attempts its failed reset");
    auto left = both;
    std::erase_if(left.streams, [](const auto& stream) { return stream.display_index != 0; });
    subscription_pictures(*viewer, left);
    require(viewer->peer_alive() && audio(*viewer), "failed reset does not close the other screen or audio");
    const auto selected = subscriptions(*viewer, {0, {}, {}, {0, 1}});
    require(selected.streams.size() == both.streams.size() &&
            selected.streams[0].stream_id == both.streams[0].stream_id &&
            selected.streams[1].stream_id == both.streams[1].stream_id,
            "selection during recovery preserves both stream identities");
    auto observer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0, nullptr,
        kVideoCodecH264, StreamCodecPreference::H264);
    require(picture(*observer, 640), "another viewer can join a healthy screen while a source is unavailable");
    subscription_pictures(*viewer, left, VideoCodec::H264);
    const auto encoders_before_recovery = f.right->encoder_created.load();
    f.right->fail_encode_codecs = kVideoCodecAll;
    f.right->reinit_fails = false;
    require(until([&] {
        FrameHeader header; std::vector<uint8_t> bytes;
        viewer->receive_frame(header, bytes, 10ms);
        return f.right->encoder_created > encoders_before_recovery;
    }),
            "recovered capture attempts to rebuild its unavailable hardware encoder");
    auto during_encoder_failure = subscriptions(*viewer, {0, {}, {}, {0, 1}});
    require(during_encoder_failure.streams.size() == 2 && viewer->peer_alive(),
            "encoder recovery preserves subscriptions and accepts controls");
    subscription_pictures(*viewer, left, VideoCodec::H264);
    auto later = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0, nullptr,
        kVideoCodecH264, StreamCodecPreference::H264);
    require(picture(*later, 640), "unavailable encoder does not block another viewer's healthy display");
    f.right->fail_encode_codecs = 0;
    subscription_pictures(*viewer, both, VideoCodec::H264);
    require(f.left->resets == 0 && f.left->capture_created == 1 && f.right->capture_created == 1,
            "failed source recovers with the current codec without rebuilding neighbours");
    finish(observer);
    finish(later);
    subscription_pictures(*viewer, both, VideoCodec::Hevc);
    finish(viewer);
}

void multiple_display_failures_and_votes() {
    TwoDisplays f;
    auto a = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0, nullptr,
        kVideoCodecAll, StreamCodecPreference::Hevc);
    require(picture(*a, 640), "start multi-display rollback scenario");
    f.refuse_right = true;
    auto refused = subscriptions(*a, {0, {}, {}, {1}}, DisplaySwitchCode::Failed);
    require(refused.streams.size() == 1 && refused.streams[0].display_index == 0,
            "failed replacement retains the original subscription");
    subscription_pictures(*a, refused, VideoCodec::Hevc);
    f.refuse_right = false;
    f.right->fail_encode_codecs = kVideoCodecAll;
    refused = subscriptions(*a, {0, {1}, {}, {}}, DisplaySwitchCode::Failed);
    subscription_pictures(*a, refused, VideoCodec::Hevc);
    require(f.left->capture_created == 1 && f.left->capture_destroyed == 0,
            "failed target encoder does not replace the original source");
    f.right->fail_encode_codecs = 0;
    auto both = subscriptions(*a, {0, {}, {}, {0, 1}});
    subscription_pictures(*a, both, VideoCodec::Hevc);
    const auto invalid = subscriptions(*a, {0, {}, {99}, {}});
    require(invalid.streams.size() == 2, "invalid sub request preserves both active streams");
    auto b = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0, nullptr,
        kVideoCodecAll, StreamCodecPreference::H264);
    auto c = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1, nullptr,
        kVideoCodecAll, StreamCodecPreference::H264);
    subscription_pictures(*a, both, VideoCodec::H264);
    require(picture(*b, 640) && picture(*c, 800), "two H264 voters win over one HEVC viewer with two subscriptions");
    finish(b); finish(c);
    subscription_pictures(*a, both, VideoCodec::Hevc);
    finish(a);
}

void multiple_displays_backpressure() {
    TwoDisplays f;
    f.left->moving = f.right->moving = true;
    auto viewer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    auto both = subscriptions(*viewer, {0, {1}, {}, {}});
    subscription_pictures(*viewer, both);
    auto other = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1);
    const auto until_stalled = std::chrono::steady_clock::now() + 800ms;
    unsigned other_frames = 0;
    while (std::chrono::steady_clock::now() < until_stalled) {
        FrameHeader frame; std::vector<uint8_t> bytes;
        if (other->receive_frame(frame, bytes, 20ms).ok()) ++other_frames;
    }
    require(other_frames > 4, "another viewer continues while the two-stream viewer stops reading");
    const auto changed_at = std::chrono::steady_clock::now();
    const auto retained = subscriptions(*viewer, {0, {}, {1}, {}});
    require(retained.streams.size() == 1 && std::chrono::steady_clock::now() - changed_at < 1s,
            "stalled multi-stream viewer can still change subscriptions promptly");
    subscription_pictures(*viewer, retained);
    both = subscriptions(*viewer, {0, {0}, {}, {}});
    subscription_pictures(*viewer, both);
    std::map<uint32_t, unsigned> counts;
    require(until([&] {
        FrameHeader frame; std::vector<uint8_t> bytes;
        if (viewer->receive_frame(frame, bytes, 20ms).ok()) ++counts[frame.stream_id];
        return counts[both.streams[0].stream_id] >= 5 && counts[both.streams[1].stream_id] >= 5;
    }), "both moving displays make progress after the delivery window recovers");
    require(f.left->capture_created == 2 && f.right->capture_created == 1,
            "resubscribe rebuilds only a released source and preserves the other viewer's source");
    finish(viewer); finish(other);
}

void multiple_display_cursors() {
    TwoDisplays f;
    CursorInfo left, right;
    left.visible = left.position_valid = left.shape_valid = true;
    left.position = {80, 60};
    left.width = left.height = 2; left.content_id = 123;
    left.bitmap.assign(16, 0x80);
    right = left; right.visible = false; right.position.x = -560;
    f.left->set_cursor(left); f.right->set_cursor(right);
    auto viewer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    auto other = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1);
    CursorUpdate legacy;
    require(until([&] { return viewer->take_cursor_update(legacy); }) && legacy.visible &&
            legacy.x == 80 && legacy.sprite == left.bitmap, "legacy viewer receives selected screen cursor");
    auto both = subscriptions(*viewer, {0, {1}, {}, {}});
    auto stream_id = [](const CaptureDisplaysResult& result, uint16_t index) {
        for (const auto& entry : result.streams) if (entry.display_index == index) return entry.stream_id;
        throw std::runtime_error("missing cursor source");
    };
    const auto left_id = stream_id(both, 0), right_id = stream_id(both, 1);
    std::map<uint32_t, CursorUpdate> cursors;
    auto drain = [&] {
        DisplayCursorUpdate update;
        while (viewer->take_display_cursor_update(update))
            coalesce_cursor_update(cursors[update.stream_id], std::move(update.cursor));
    };
    require(until([&] { drain(); return cursors.size() == 2; }) &&
            cursors[left_id].visible && !cursors[right_id].visible && cursors[left_id].x == 80 &&
            cursors[right_id].x == -560 && cursors[left_id].sprite == left.bitmap &&
            cursors[right_id].sprite == right.bitmap,
            "multi-stream activation snapshots each source including an already-delivered static shape");
    require(until([&] { return other->take_cursor_update(legacy); }) && !legacy.visible &&
            legacy.x == -560, "another viewer retains its independently selected source cursor");

    left.visible = false; left.position.x = 672;
    right.visible = true; right.position.x = 32;
    f.left->set_cursor(left); f.right->set_cursor(right);
    require(until([&] { drain(); return !cursors[left_id].visible && cursors[right_id].visible &&
            cursors[left_id].x == 672 && cursors[right_id].x == 32; }),
            "cross-screen motion hides the old screen and uses the new screen's local coordinates");
    require(until([&] { return other->take_cursor_update(legacy) && legacy.visible && legacy.x == 32; }),
            "shared source sends the same new cursor to the other view-only connection");

    f.left->moving = f.right->moving = true;
    std::this_thread::sleep_for(700ms); // Neither viewer consumes video: the v52 window expires.
    left.position.x = 700; right.position.x = 60;
    f.left->set_cursor(left); f.right->set_cursor(right);
    require(until([&] { drain(); return cursors[left_id].x == 700 && cursors[right_id].x == 60; }),
            "current cursor bypasses both streams' stalled video backlog");
    require(until([&] { return other->take_cursor_update(legacy) && legacy.x == 60; }),
            "legacy cursor also keeps moving while its video window is blocked");
    auto kept = subscriptions(*viewer, {0, {}, {1}, {}});
    require(kept.streams.size() == 1 && kept.streams[0].stream_id == right_id,
            "retaining a screen preserves its cursor stream identity");
    cursors.clear();
    left.position.x = 720; right.position.x = 80;
    f.left->set_cursor(left); f.right->set_cursor(right);
    require(until([&] { drain(); return cursors.count(right_id) && cursors[right_id].x == 80; }) &&
            cursors.size() == 1, "unsubscribed screen stops publishing cursor state");
    both = subscriptions(*viewer, {0, {0}, {}, {}});
    const auto new_left_id = stream_id(both, 0);
    require(new_left_id != left_id && until([&] { drain(); return cursors.count(new_left_id) != 0; }) &&
            cursors[new_left_id].sprite == left.bitmap, "re-added screen has a new identity and complete cursor snapshot");
    subscriptions(*viewer, {});
    DisplayCursorUpdate update;
    require(!viewer->take_display_cursor_update(update), "empty set clears queued per-screen cursors");
    right.position.x = 100; f.right->set_cursor(right);
    require(until([&] { return other->take_cursor_update(legacy) && legacy.x == 100; }),
            "one viewer's empty set leaves another viewer's cursor subscription intact");
    finish(viewer); finish(other);
}

void independent_displays() {
    TwoDisplays f;
    ServerInfo a_info, b_info;
    auto a = f.fixture.connect(SessionMode::Desktop, kPermissionAll, {}, 0, &a_info);
    auto b = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1, &b_info);
    require(a_info.width == 640 && a_info.origin_x == 0 &&
            b_info.width == 800 && b_info.origin_x == 640, "Hello selects each connection's own display");
    require(picture(*a, 640) && picture(*b, 800), "two displays produce distinct encoded payloads");
    require(f.left->capture_created == 1 && f.right->capture_created == 1 &&
            f.left->encoder_created == 1 && f.right->encoder_created == 1,
            "one capture and encoder per subscribed display");
    auto c = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    require(picture(*c, 640) && f.left->capture_created == 1 && f.left->encoder_created == 1,
            "late same-display viewer shares existing pipeline and gets static IDR");
    const std::string contents = "file transfer remains open across independent display selection";
    FileOffer offer;
    offer.transfer_id = 91;
    offer.name = "display-selection.txt";
    offer.size = contents.size();
    require(sha256(reinterpret_cast<const uint8_t*>(contents.data()), contents.size(), offer.sha256),
            "hash transfer spanning display switch");
    require(a->offer_file(offer).ok(), "begin file transfer before display switch");
    FileAnswer answer;
    require(until([&] { return a->take_file_answer(answer); }) && answer.code == FileCode::Ok,
            "host accepts transfer before subscription changes");
    const size_t split = contents.size() / 2;
    require(a->send_file_data(offer.transfer_id,
            reinterpret_cast<const uint8_t*>(contents.data()), split).ok(), "send first half before switch");
    const auto c_updates = c->stream_updates();
    switch_to(*a, 1, DisplaySwitchCode::Ok);
    ServerInfo changed;
    require(until([&] { return a->take_stream_update(changed) && changed.width == 800; }) &&
            changed.origin_x == 640 && picture(*a, 800), "first viewer switches its own subscription");
    require(c->request_keyframe().ok() && picture(*c, 640) && c->stream_updates() == c_updates,
            "first viewer switching does not redirect remaining viewers");
    require(f.left->capture_destroyed == 0 && f.left->encoder_destroyed == 0 &&
            f.right->capture_created == 1 && f.right->encoder_created == 1,
            "switch reuses target and preserves source resources still in use");
    require(a->send_file_data(offer.transfer_id,
            reinterpret_cast<const uint8_t*>(contents.data() + split), contents.size() - split).ok() &&
            a->finish_file_upload(offer.transfer_id).ok(), "finish same transfer after switch");
    FileResult uploaded;
    require(until([&] { return a->take_file_result(uploaded); }) && uploaded.code == FileCode::Ok &&
            uploaded.received == contents.size(), "display switch preserves transfer state and digest");
    std::ifstream saved(f.fixture.root / offer.name, std::ios::binary);
    const std::string received((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
    require(received == contents, "transferred file matches across switch");
    saved.close();
    require(f.left->audio_sources == 1, "different displays still share one system audio source");
    finish(a);
    require(b->request_keyframe().ok() && picture(*b, 800) &&
            f.right->capture_created == 1 && f.right->encoder_created == 1,
            "first viewer leaving does not rebuild the remaining pipeline");
    switch_to(*b, 0, DisplaySwitchCode::Ok);
    require(until([&] { return b->take_stream_update(changed) && changed.width == 640; }) && picture(*b, 640),
            "view-only viewer switches without obtaining input control");
    require(!(b->permissions() & kPermissionInput), "display selection does not grant input control");
    require(!b->request_set_display_mode(0, 640, 480, 60).ok(),
            "view-only viewer still cannot change physical display mode");
    require(until([&] { return f.right->capture_destroyed == 1 && f.right->encoder_destroyed == 1; }),
            "last unsubscribe releases unused display pipeline");
    finish(c);
    require(b->request_keyframe().ok() && picture(*b, 640) &&
            f.left->capture_created == 1 && f.left->encoder_created == 1,
            "original display subscriber leaving preserves resources for the new subscriber");
    finish(b);
    require(until([&] { return f.left->capture_destroyed == 1 && f.left->encoder_destroyed == 1; }),
            "last viewer releases capture and encoder");
}

void refused_display_keeps_stream() {
    TwoDisplays f;
    auto a = f.fixture.connect();
    auto b = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput);
    require(picture(*a, 640) && picture(*b, 640), "both viewers start on shared display");
    switch_to(*b, 99, DisplaySwitchCode::NotFound);
    f.refuse_right = true;
    switch_to(*b, 1, DisplaySwitchCode::Failed);
    require(a->request_keyframe().ok() && b->request_keyframe().ok() &&
            picture(*a, 640) && picture(*b, 640), "invalid or unavailable target preserves both subscriptions");
    require(f.left->capture_created == 1 && f.left->encoder_created == 1 &&
            f.left->capture_destroyed == 0, "failed selection does not replace working media resources");
    f.refuse_right = false;
    switch_to(*b, 1, DisplaySwitchCode::Ok);
    require(picture(*b, 800), "same connection can retry after target recovers");
    finish(a);
    finish(b);
}

void concurrent_display_admission() {
    TwoDisplays f;
    std::vector<std::future<std::unique_ptr<QuicSession>>> connecting;
    for (uint16_t index : std::array<uint16_t, 4>{0, 1, 0, 1}) {
        connecting.push_back(std::async(std::launch::async, [&f, index] {
            return f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, index);
        }));
    }
    std::vector<std::unique_ptr<QuicSession>> viewers;
    for (auto& pending : connecting) viewers.push_back(pending.get());
    for (size_t i = 0; i < viewers.size(); ++i)
        require(picture(*viewers[i], i % 2 ? 800 : 640), "concurrent subscription receives its requested screen");
    require(f.left->capture_created == 1 && f.left->encoder_created == 1 &&
            f.right->capture_created == 1 && f.right->encoder_created == 1,
            "simultaneous joins create exactly one pipeline per display");
    f.reversed = true;
    auto renamed = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    require(picture(*renamed, 800) && f.right->capture_created == 1 && f.right->encoder_created == 1,
            "renumbered display list reuses stable device identity");
    for (auto& viewer : viewers) finish(viewer);
    require(renamed->request_keyframe().ok() && picture(*renamed, 800),
            "renumbered subscription survives all earlier viewers leaving");
    finish(renamed);
}

void display_codec_intersection() {
    TwoDisplays f(VideoCodec::Hevc);
    ServerInfo info;
    auto a = f.fixture.connect(SessionMode::Desktop, kPermissionAll, {}, 0, &info, kVideoCodecAll, StreamCodecPreference::Auto);
    require(info.codec == VideoCodec::Hevc && picture(*a, 640), "first display starts in HEVC");
    auto b = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1,
                              &info, kVideoCodecH264);
    require(info.codec == VideoCodec::H264 && picture(*b, 800), "second display admits H264-only viewer");
    auto c = f.fixture.connect(SessionMode::Desktop, kPermissionAll, {}, 1, &info);
    require(info.codec == VideoCodec::H264 && picture(*c, 800), "same display uses common decoder format");
    require(until([&] { return a->take_stream_update(info) && info.codec == VideoCodec::H264; }) && picture(*a, 640),
            "a restricted receiver on another display changes the global codec");
    bool refused = false;
    try { auto d = f.fixture.connect(SessionMode::Desktop, kPermissionAll, {}, 0, &info, kVideoCodecHevc); }
    catch (const std::exception&) { refused = true; }
    require(refused, "a new viewer with no global common decoder is refused");
    switch_to(*b, 0, DisplaySwitchCode::Ok);
    require(until([&] { return b->take_stream_update(info) && info.width == 640 &&
                              info.codec == VideoCodec::H264; }) && picture(*b, 640),
            "switch carries viewer decoder capabilities into target subscription");
    require(c->request_keyframe().ok() && picture(*c, 800), "old display continues while restriction follows the connection");
    finish(b);
    require(codec_update(*a, VideoCodec::Hevc) && picture(*a, 640),
            "restriction removal restores the preferred codec on the first display");
    require(codec_update(*c, VideoCodec::Hevc) && picture(*c, 800),
            "restriction removal restores the preferred codec on the other display");
    require(f.left->capture_created == 1 && f.right->capture_created == 1,
            "negotiation and switching keep each display capture alive");
    finish(a); finish(c);
}

void multiuser_quality() {
    TwoDisplays f;
    ServerInfo info;
    auto a = f.fixture.connect(SessionMode::Desktop, kPermissionAll, {}, 0);
    auto b = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1);
    auto options = [](QuicSession& peer, uint16_t fps, StreamQuality quality) {
        require(peer.request_stream_options({StreamCodecPreference::H264, fps, quality}).ok(), "send quality vote");
        StreamOptionsResult result;
        require(until([&] { return peer.take_stream_options_result(result); }) && result.code == StreamOptionsCode::Ok,
                "host acknowledges quality vote");
    };
    options(*a, 12, StreamQuality::Smooth);
    require(until([&] { return b->take_stream_update(info) && info.framerate == 12; }),
            "FPS vote reaches viewer of another display");
    options(*b, 30, StreamQuality::High);
    require(until([&] { return a->take_stream_update(info) && info.framerate == 12 && info.bitrate > 1000000; }),
            "recent High quality applies while smallest FPS cap remains");
    const auto high_rate = info.bitrate;
    options(*a, 20, StreamQuality::Smooth);
    require(until([&] { return a->take_stream_update(info) && info.framerate == 20 && info.bitrate == high_rate; }),
            "FPS-only change preserves another viewer's newer quality setting");
    options(*a, 20, StreamQuality::Smooth);
    require(until([&] { return a->take_stream_update(info) && info.bitrate < high_rate; }),
            "reselecting previous preset becomes the latest global quality vote");
    options(*b, 30, StreamQuality::High);
    require(until([&] { return a->take_stream_update(info) && info.bitrate == high_rate; }),
            "another viewer can explicitly restore its preferred quality");
    finish(b);
    require(until([&] { return a->take_stream_update(info) && info.framerate == 20 && info.bitrate < high_rate; }),
            "leaving quality owner restores remaining viewer's setting");
    switch_to(*a, 1, DisplaySwitchCode::Ok);
    require(until([&] { return a->take_stream_update(info) && info.width == 800 && info.framerate == 20; }) &&
            picture(*a, 800), "quality participant survives independent display switch");
    finish(a);
}

void global_codec_preferences() {
    TwoDisplays f;
    ServerInfo info;
    const auto open = [&](uint16_t display) {
        return f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, display,
                                 &info, kVideoCodecAll, StreamCodecPreference::Auto);
    };
    auto a = open(0);
    auto b = open(1);
    auto c = open(1);
    require(info.codec == VideoCodec::Hevc && picture(*a, 640) && picture(*b, 800) && picture(*c, 800),
            "Auto starts every display with usable HEVC");
    auto vote = [&](QuicSession& viewer, StreamCodecPreference preference, VideoCodec expected) {
        require(viewer.request_stream_options({preference, 0, StreamQuality::Balanced}).ok(), "send global codec vote");
        StreamOptionsResult answer;
        require(until([&] { return viewer.take_stream_options_result(answer); }) &&
                answer.code == StreamOptionsCode::Ok && answer.codec == expected,
                "vote response reports actual global codec even when preference differs");
    };
    auto changed = [&](QuicSession& viewer, VideoCodec codec) {
        require(codec_update(viewer, codec),
                "global codec announcement reaches another connection");
    };
    vote(*a, StreamCodecPreference::H264, VideoCodec::H264);
    changed(*b, VideoCodec::H264);
    changed(*c, VideoCodec::H264);
    vote(*b, StreamCodecPreference::Hevc, VideoCodec::H264);
    vote(*c, StreamCodecPreference::Hevc, VideoCodec::Hevc);
    changed(*a, VideoCodec::Hevc);
    changed(*b, VideoCodec::Hevc);
    vote(*b, StreamCodecPreference::Auto, VideoCodec::H264);
    changed(*a, VideoCodec::H264);
    changed(*c, VideoCodec::H264);
    switch_to(*a, 1, DisplaySwitchCode::Ok);
    require(picture(*a, 800), "switch preserves the connection's vote and uses the target subscription");
    finish(a);
    changed(*b, VideoCodec::Hevc);
    changed(*c, VideoCodec::Hevc);
    require(picture(*b, 800) && picture(*c, 800), "remaining viewers decode after the winning voter leaves");
    require(f.left->capture_created == 1 && f.right->capture_created == 1,
            "global votes and switch never recreate display capture");
    finish(b); finish(c);
}

void runtime_software_recovery() {
    for (const int scenario : {0, 1, 2}) {
        const auto initial = scenario == 0 ? VideoCodec::H264 : VideoCodec::Hevc;
        TwoDisplays f(initial);
        auto a = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
        auto b = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1,
            nullptr, scenario == 2 ? kVideoCodecHevc : kVideoCodecAll);
        require(picture(*a, 640) && picture(*b, 800), "both hardware displays send initial IDRs");
        f.left->fail_hardware = true;
        require(a->request_keyframe().ok(), "trigger hardware encode failure on a static desktop");
        if (scenario == 2) {
            require(until([&] { return f.left->hardware_errors >= 3; }),
                    "repeat and direct encode failures reach the retry path");
            std::this_thread::sleep_for(500ms);
            require(a->peer_alive() && audio(*a) && f.left->software_created == 0 && f.left->resets == 0,
                    "incompatible decoder vote pauses only video, retaining audio and capture");
            finish(b);
        }
        bool left_announced = false;
        bool right_announced = initial == VideoCodec::H264 || !b;
        require(until([&] {
            ServerInfo info;
            if (a->take_stream_update(info))
                left_announced = info.codec == VideoCodec::H264 &&
                    info.encoder == "deterministic software encoder";
            if (b && b->take_stream_update(info))
                right_announced = info.codec == VideoCodec::H264 &&
                    info.encoder == "deterministic test encoder";
            FrameHeader header;
            std::vector<uint8_t> bytes;
            a->receive_frame(header, bytes, 10ms);
            if (b) b->receive_frame(header, bytes, 10ms);
            return left_announced && right_announced;
        }, 5s), "runtime recovery announces the software backend and compatible codec on both connections");
        require(a->request_keyframe().ok() && picture(*a, 640) &&
                    (!b || (b->request_keyframe().ok() && picture(*b, 800))),
                "remaining connections receive new IDRs after fallback");
        require(a->peer_alive() && audio(*a) && (!b || (b->peer_alive() && audio(*b))),
                "runtime encoder fallback keeps remaining connections and audio alive");
        require(f.left->software_created == 1 && f.right->software_created == 0 &&
                    f.left->resets == 0 && f.right->resets == 0,
                "only failed display switches backend and neither capture is rebuilt");
        finish(a); finish(b);
    }
}

// Reuse the existing deterministic media boundary and real QUIC transport.
// No webcam or desktop access is needed for shared-camera negotiation.
void shared_camera_service() {
    Fixture f;
    auto camera_media = std::make_shared<host_test::MediaState>();
    camera_media->moving = true;
    camera_media->capture_fps = 12;
    f.config.settings.allow_camera = true;
    f.config.settings.codec = VideoCodec::Hevc;
    f.config.cameras = [] { return std::vector<CameraDevice>{{1, "test camera", "test-camera-link", true}}; };
    f.config.camera_capture_factory = [camera_media](const CameraDevice&, LUID, uint32_t, std::string*) {
        auto info = host_test::display();
        info.device_name = L"test-camera-link";
        info.width = 320; info.height = 240; info.rect = {0, 0, 320, 240};
        return std::make_unique<host_test::Capture>(camera_media, info);
    };
    f.config.encoder_factory = [camera_media, desktop = f.media](ID3D11Device*, const EncoderConfig& cfg, std::string*) {
        return std::make_unique<host_test::Encoder>(cfg.width == 320 ? camera_media : desktop, cfg);
    };
    const auto open_camera = [](QuicSession& viewer, CameraCode expected = CameraCode::Ok) {
        require(viewer.request_camera_list().ok(), "request shared camera list");
        CameraList list;
        require(until([&] { return viewer.take_camera_list(list); }) && list.cameras.size() == 1,
                "camera enumeration is announced");
        require(viewer.request_camera_start(list.cameras.front().id).ok(), "request shared camera start");
        CameraResult result;
        require(until([&] { return viewer.take_camera_result(result); }, 5s) && result.code == expected,
                "camera start answers with the expected negotiation result");
    };
    const auto camera_frame = [](QuicSession& viewer, VideoCodec codec, bool first = false) {
        bool seen = false;
        return until([&] {
            FrameHeader header;
            std::vector<uint8_t> bytes;
            if (!viewer.receive_camera_frame(header, bytes, 30ms).ok()) return false;
            if (first && !seen) require(header.idr, "first camera picture is a usable IDR");
            seen = true;
            return header.codec == codec && header.width == 320 && header.height == 240 &&
                bytes.size() == 6 && bytes.back() == 20 && (!first || header.idr);
        });
    };
    f.start();
    auto solo = f.connect(SessionMode::Camera, kPermissionCamera, {}, kDisplayAuto,
        nullptr, kVideoCodecAll, StreamCodecPreference::H264);
    open_camera(*solo);
    require(camera_frame(*solo, VideoCodec::H264, true) && f.media->capture_created == 0,
            "camera-only streaming starts without creating a desktop capture");
    StreamOptions solo_options;
    solo_options.codec = StreamCodecPreference::Hevc;
    solo_options.max_fps = 10;
    require(solo->request_stream_options(solo_options).ok(), "change the only camera's codec vote");
    StreamOptionsResult solo_result;
    require(until([&] { return solo->take_stream_options_result(solo_result); }) &&
                solo_result.code == StreamOptionsCode::Ok && solo_result.codec == VideoCodec::Hevc &&
                solo_result.fps == 10 && camera_frame(*solo, VideoCodec::Hevc),
            "a camera-only session can change its actual encoding format");
    finish(solo);
    require(until([&] { return camera_media->capture_destroyed == 1 &&
        camera_media->encoder_created == camera_media->encoder_destroyed; }),
        "camera-only disconnect releases its capture and encoders");
    auto desktop = f.connect();
    require(keyframe(*desktop), "desktop starts before camera negotiation");
    open_camera(*desktop);
    require(camera_frame(*desktop, VideoCodec::Hevc, true), "attached camera uses the desktop connection's vote");
    auto voter = f.connect(SessionMode::Desktop, kPermissionAll, {}, 0, nullptr,
        kVideoCodecAll, StreamCodecPreference::H264);
    auto majority = f.connect(SessionMode::Desktop, kPermissionAll, {}, 0, nullptr,
        kVideoCodecAll, StreamCodecPreference::H264);
    require(codec_update(*desktop, VideoCodec::H264) && camera_frame(*desktop, VideoCodec::H264),
            "two H264 votes beat one HEVC connection with both desktop and camera");
    finish(voter); finish(majority);
    require(codec_update(*desktop, VideoCodec::Hevc) && camera_frame(*desktop, VideoCodec::Hevc),
            "desktop and attached camera restore the remaining connection's codec");

    auto camera_only = f.connect(SessionMode::Camera, kPermissionCamera, {}, kDisplayAuto,
        nullptr, kVideoCodecH264);
    open_camera(*camera_only);
    require(camera_frame(*camera_only, VideoCodec::H264, true) && codec_update(*desktop, VideoCodec::H264),
            "camera-only decoder capabilities constrain desktop and camera together");
    StreamOptions options;
    options.codec = StreamCodecPreference::Hevc;
    options.max_fps = 10;
    options.quality = StreamQuality::Smooth;
    require(camera_only->request_stream_options(options).ok(), "change camera-only video options");
    StreamOptionsResult applied;
    require(until([&] { return camera_only->take_stream_options_result(applied); }) &&
                applied.code == StreamOptionsCode::Ok && applied.codec == VideoCodec::H264 &&
                applied.fps == 10 && applied.quality == StreamQuality::Smooth,
            "camera-only options report the negotiated codec and shared quality/FPS");
    require(camera_only->request_camera_stop().ok(), "stop camera while retaining its options");
    require(codec_update(*desktop, VideoCodec::Hevc), "stopping camera releases its active policy");
    open_camera(*camera_only);
    require(camera_frame(*camera_only, VideoCodec::H264, true) && codec_update(*desktop, VideoCodec::H264),
            "camera can restart on the same connection with accepted options");
    options.max_fps = 60;
    require(camera_only->request_stream_options(options).ok(), "raise camera requested FPS");
    require(until([&] { return camera_only->take_stream_options_result(applied); }) &&
                applied.code == StreamOptionsCode::Ok && applied.fps == 12,
            "the shared encoder respects the camera's negotiated device rate");
    const auto encoders = camera_media->encoder_created.load();
    auto second = f.connect(SessionMode::Camera, kPermissionCamera, {}, kDisplayAuto,
        nullptr, kVideoCodecH264);
    open_camera(*second);
    require(camera_frame(*second, VideoCodec::H264, true) && camera_media->capture_created == 2 &&
                camera_media->encoder_created == encoders,
            "multiple viewers reuse one camera capture and encoder");
    require(camera_only->request_camera_stop().ok(), "stop one camera subscription");
    finish(camera_only);
    require(camera_frame(*second, VideoCodec::H264) && camera_media->capture_destroyed == 1,
            "another camera viewer continues after one leaves");

    auto incompatible = f.connect(SessionMode::Camera, kPermissionCamera, {}, kDisplayAuto,
        nullptr, kVideoCodecAv1);
    open_camera(*incompatible, CameraCode::Failed);
    require(desktop->request_keyframe().ok() && keyframe(*desktop) &&
                camera_frame(*second, VideoCodec::H264),
            "an incompatible camera viewer does not break working streams");
    finish(incompatible);
    require(second->request_camera_stop().ok(), "stop the last H264-only camera viewer");
    require(codec_update(*desktop, VideoCodec::Hevc) && camera_frame(*desktop, VideoCodec::Hevc),
            "stopping camera releases its decoder vote while its connection remains alive");
    finish(second);

    camera_media->fail_hardware = true;
    require(camera_frame(*desktop, VideoCodec::H264) && codec_update(*desktop, VideoCodec::H264),
            "runtime camera encoder failure falls back and renegotiates the desktop");
    require(camera_media->software_created == 1 && f.media->software_created == 0 &&
                camera_media->resets == 0 && f.media->resets == 0,
            "only the failed camera switches backend without resetting capture");
    f.host->set_permissions(kPermissionAll & ~kPermissionCamera);
    require(until([&] { return camera_media->capture_destroyed == 2; }),
            "revoking camera permission releases the last camera capture");
    require(desktop->request_keyframe().ok() && keyframe(*desktop) && audio(*desktop),
            "desktop and audio remain usable after camera revocation");
    require(camera_media->encoder_created == camera_media->encoder_destroyed && camera_media->wrong_thread == 0,
            "all camera resources are released on their owning service thread");
    finish(desktop);
}

// A camera opens on the desktop connection and shares its codec group. A
// camera that can only supply 4:2:0 must renegotiate the desktop instead of
// leaving one source at 4:4:4; closing the camera restores full color.
void shared_camera_full_color() {
    Fixture f;
    f.media->moving = true;
    f.media->original_bgra = true;
    f.config.settings.allow_camera = true;
    f.config.settings.codec = VideoCodec::Vp9;
    auto camera_media = std::make_shared<host_test::MediaState>();
    camera_media->moving = true;
    camera_media->capture_fps = 12;
    f.config.cameras = [] { return std::vector<CameraDevice>{{1, "test camera", "test-camera-link", true}}; };
    f.config.camera_capture_factory = [camera_media](const CameraDevice&, LUID, uint32_t, std::string*) {
        auto info = host_test::display();
        info.device_name = L"test-camera-link";
        info.width = 320; info.height = 240; info.rect = {0, 0, 320, 240};
        return std::make_unique<host_test::Capture>(camera_media, info);
    };
    f.config.encoder_factory = [camera_media, desktop = f.media](ID3D11Device*, const EncoderConfig& cfg, std::string*) {
        return std::make_unique<host_test::Encoder>(cfg.width == 320 ? camera_media : desktop, cfg);
    };
    const auto desktop_frame = [](QuicSession& viewer, VideoChroma chroma) {
        return until([&] {
            FrameHeader header;
            std::vector<uint8_t> bytes;
            if (!viewer.receive_frame(header, bytes, 50ms).ok()) return false;
            return !bytes.empty() && header.codec == VideoCodec::Vp9 && header.chroma == chroma;
        });
    };
    const auto camera_frame = [](QuicSession& viewer, VideoChroma chroma) {
        return until([&] {
            FrameHeader header;
            std::vector<uint8_t> bytes;
            if (!viewer.receive_camera_frame(header, bytes, 30ms).ok()) return false;
            return !bytes.empty() && header.codec == VideoCodec::Vp9 && header.width == 320 &&
                header.chroma == chroma;
        });
    };
    f.start();
    auto desktop = f.connect(SessionMode::Desktop, kPermissionAll, {}, kDisplayAuto, nullptr,
        kVideoCodecAll, StreamCodecPreference::Vp9, kVideoCodecVp9, VideoChroma::Yuv444);
    require(desktop_frame(*desktop, VideoChroma::Yuv444),
            "a full-color VP9 connection receives 4:4:4");
    require(desktop->request_camera_list().ok(), "request camera list");
    CameraList list;
    require(until([&] { return desktop->take_camera_list(list); }) && list.cameras.size() == 1,
            "camera enumeration is announced");
    require(desktop->request_camera_start(list.cameras.front().id).ok(),
            "start the 4:2:0-only camera");
    CameraResult started;
    require(until([&] { return desktop->take_camera_result(started); }, 5s) &&
                started.code == CameraCode::Ok,
            "camera start renegotiates the shared format");
    require(camera_frame(*desktop, VideoChroma::Yuv420) &&
                desktop_frame(*desktop, VideoChroma::Yuv420),
            "camera and desktop both stream 4:2:0 while the camera is open");
    require(desktop->request_camera_stop().ok(), "stop the 4:2:0-only camera");
    require(desktop_frame(*desktop, VideoChroma::Yuv444),
            "closing the camera restores the desktop's full-color format");
    finish(desktop);
    require(until([&] { return camera_media->capture_destroyed == 1 &&
                camera_media->encoder_created == camera_media->encoder_destroyed; }),
            "the camera pipeline is released on its service thread");
}

void operator_connection_management() {
    Fixture f;
    f.config.settings.max_viewers = 4;
    std::mutex chat_mutex;
    std::map<uint64_t, std::string> received;
    f.config.viewer_chat_message = [&](uint64_t id, const std::string& text) {
        std::lock_guard lock(chat_mutex); received[id] = text;
    };
    f.start();
    auto a = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput);
    auto viewers = f.host->viewers();
    require(viewers.size() == 1 && viewers.front().id && viewers.front().mode == SessionMode::Desktop,
            "admitted desktop has a stable operator ID and session mode");
    const auto aid = viewers.front().id;
    auto b = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput);
    viewers = f.host->viewers();
    require(viewers.size() == 2 && viewers.back().id != aid, "concurrent viewers have distinct IDs");
    const auto bid = viewers.back().id;
    auto files = f.connect(SessionMode::FileTransfer, kPermissionFileTransfer);
    viewers = f.host->viewers();
    require(viewers.size() == 3 && viewers.back().mode == SessionMode::FileTransfer,
            "the operator list includes files-only sessions");
    const auto fid = viewers.back().id;
    require(a->send_chat("from A").ok() && b->send_chat("from B").ok() && files->send_chat("from files").ok(),
            "each session sends a chat message");
    require(until([&] {
        std::lock_guard lock(chat_mutex);
        return received[aid] == "from A" && received[bid] == "from B" && received[fid] == "from files";
    }), "incoming chats identify the actual sender across concurrent session modes");
    const auto chat = [](QuicSession& viewer, const std::string& expected) {
        std::string text;
        return until([&] { return viewer.take_chat(text); }) && text == expected;
    };
    require(f.host->send_chat(aid, "only A") && chat(*a, "only A"),
            "reply to an older viewer despite a different most-recent connection");
    std::string other;
    require(!b->take_chat(other) && !files->take_chat(other), "targeted reply never appears in other conversations");
    require(f.host->send_chat(fid, "only files") && chat(*files, "only files"), "files-only sessions receive operator replies");
    require(f.host->set_permissions(aid, kPermissionAll & ~(kPermissionInput | kPermissionClipboard | kPermissionAudio)),
            "revoke permissions for the selected older connection");
    require(until([&] { return !(a->permissions() & (kPermissionAudio | kPermissionClipboard)); }) &&
                (b->permissions() & (kPermissionAudio | kPermissionClipboard)) == (kPermissionAudio | kPermissionClipboard),
            "selected permission changes leave another viewer's grant unchanged");
    require(f.host->set_permissions(aid, kPermissionAll) &&
                until([&] { return (a->permissions() & kPermissionAudio) != 0; }) && !(a->permissions() & kPermissionInput),
            "restoring selected permissions stays within the original approved ceiling");

    const auto path = f.root / "operator-target.txt";
    const std::string content = "only the selected connection receives these bytes";
    { std::ofstream output(path, std::ios::binary); output << content; }
    std::string error;
    require(f.host->start_file_download(bid, path.string(), &error), "queue a file for a specific desktop viewer");
    FileOffer offer;
    require(until([&] { return b->take_file_offer(offer); }), "only the selected viewer receives the file offer");
    FileOffer unrelated;
    require(!a->take_file_offer(unrelated) && !files->take_file_offer(unrelated), "file offer is not routed to the latest viewer");
    require(b->send_file_answer(offer.transfer_id, FileCode::Ok, 0).ok(), "selected viewer accepts its file");
    std::string transferred;
    require(until([&] {
        FileChunk chunk;
        if (b->receive_file_chunk(chunk, 20ms).ok()) {
            require(chunk.transfer_id == offer.transfer_id && chunk.offset == transferred.size(), "target file offsets are contiguous");
            transferred.append(reinterpret_cast<const char*>(chunk.data.data()), chunk.data.size());
        }
        return transferred.size() == content.size();
    }) && transferred == content, "targeted file content arrives intact");
    require(b->send_file_result(offer.transfer_id, FileCode::Ok, transferred.size()).ok(), "selected viewer completes the download");

    require(f.host->disconnect_viewer(aid), "disconnect one selected viewer without stopping the listener");
    require(until([&] { return !a->peer_alive(); }, 4s), "selected viewer is disconnected");
    a.reset();
    require(b->peer_alive() && files->peer_alive() && f.host->running() &&
                f.host->send_chat(bid, "B stays") && chat(*b, "B stays"),
            "other viewers and the listener remain usable after targeted disconnect");
    auto newcomer = f.connect(SessionMode::Terminal, kPermissionTerminal);
    viewers = f.host->viewers();
    const auto nid = viewers.back().id;
    require(nid != aid && nid != bid && nid != fid && viewers.back().mode == SessionMode::Terminal,
            "a replacement connection gets a new ID and the freed admission slot");
    require(!f.host->send_chat(aid, "stale") && !f.host->set_permissions(aid, 0) &&
                !f.host->start_file_download(aid, path.string(), &error) && !f.host->disconnect_viewer(aid),
            "all stale-ID actions fail without retargeting the replacement viewer");
    require(f.host->send_chat(nid, "terminal target") && chat(*newcomer, "terminal target") &&
                newcomer->send_chat("terminal response").ok(), "control-only sessions support targeted two-way chat");
    require(until([&] { std::lock_guard lock(chat_mutex); return received[nid] == "terminal response"; }),
            "control-only chat callback retains the connection ID");
    finish(newcomer); finish(files); finish(b);

    Fixture single;
    single.config.settings.max_viewers = 1;
    single.start();
    auto old = single.connect(SessionMode::FileTransfer, kPermissionFileTransfer);
    const auto oldid = single.host->viewers().front().id;
    require(single.host->start_file_download(oldid, path.string(), &error),
            "queue a file immediately before ending a sequential session");
    require(single.host->disconnect_viewer(oldid) && until([&] { return !old->peer_alive(); }, 4s),
            "targeted disconnect also works on the sequential listener");
    old.reset();
    require(until([&] { return single.host->status().state == HostState::Listening; }),
            "sequential listener returns to listening after a local disconnect");
    auto next = single.connect(SessionMode::FileTransfer, kPermissionFileTransfer);
    const auto newid = single.host->viewers().front().id;
    require(newid != oldid && newid != aid && !single.host->send_chat(oldid, "stale sequential"),
            "IDs are not reused across sequential sessions or different host instances");
    require(single.host->send_chat(newid, "fresh sequential") && chat(*next, "fresh sequential"),
            "next sequential viewer receives only its own queued messages");
    require(!until([&] { return next->take_file_offer(unrelated); }, 200ms),
            "queued operator files never cross into the next sequential session");
    finish(next);
}

void service_connection_management() {
    host::ServiceControlServer control;
    Fixture f;
    f.config.service_approval = true;
    f.config.settings.auth_mode = HostAuthMode::Click;
    f.config.settings.password_hash.clear();
    f.config.approve_client = [&](const ClientRequest& request) { return control.approve(request); };
    f.config.viewer_chat_message = [&](uint64_t id, const std::string& text) { control.receive_chat(id, text); };
    f.start();
    const auto pipe = L"\\\\.\\pipe\\chast-control-test-" + std::to_wstring(GetCurrentProcessId());
    std::string error;
    require(control.start_for_test(*f.host, pipe, &error, [] {
        return host::ServiceRegistrationInfo{"service-device", true};
    }), "start a logon-restricted local management pipe");
    host::ServiceControlClient client;
    client.start_for_test(pipe, GetCurrentProcessId());
    require(until([&] { return client.connected(); }), "mutual process and logon identity validation connects the manager");
    const auto information = client.information();
    require(information.device_id == "service-device" && information.registration_enabled &&
                !information.fingerprint.empty() && information.fingerprint == f.host->status().fingerprint &&
                information.auth_mode == HostAuthMode::Click && !information.password_configured && !information.two_factor_enabled,
            "manager receives public identity and effective authentication state from the running host");
    auto actions = client.actions();
    auto declined = std::async(std::launch::async, [&] {
        try { auto viewer = f.connect(SessionMode::FileTransfer, kPermissionFileTransfer); return false; }
        catch (const std::exception&) { return true; }
    });
    require(until([&] { return client.approvals().size() == 1; }), "manager receives a request it may decline");
    const auto denied_id = client.approvals().front().id;
    require(client.answer(denied_id, false, 0) && declined.get(), "local refusal rejects the waiting QUIC connection");
    require(until([&] { return client.approvals().empty(); }) && !client.answer(denied_id, true, kPermissionAll),
            "completed approval IDs cannot be reused to admit a different viewer");
    const auto connect_approved = [&] {
        auto waiting = std::async(std::launch::async, [&] { return f.connect(SessionMode::FileTransfer, kPermissionFileTransfer); });
        require(until([&] { return client.approvals().size() == 1; }), "service Click publishes a pending request without sharing credentials");
        const auto request = client.approvals().front();
        require(request.mode == SessionMode::FileTransfer && request.permissions == kPermissionFileTransfer &&
                    client.answer(request.id, true, kPermissionAll), "local approval retains the request's permission ceiling");
        auto viewer = waiting.get();
        require(viewer->permissions() == kPermissionFileTransfer, "approved files-only connection cannot gain desktop permissions");
        return viewer;
    };
    auto a = connect_approved();
    require(until([&] { return actions.viewers().size() == 1; }), "service manager lists admitted connections");
    const auto aid = actions.viewers().front().id;
    auto b = connect_approved();
    require(until([&] { return actions.viewers().size() == 2; }), "service manager lists both clients independently");
    const auto bid = actions.viewers().back().id;
    require(actions.chat(aid, "target A through IPC"), "queue a chat for the older service connection");
    std::string chat;
    require(until([&] { return a->take_chat(chat); }) && chat == "target A through IPC" && !b->take_chat(chat),
            "pipe dispatch routes chat only to the explicit target");
    require(b->send_chat("B replies through IPC").ok(), "viewer sends a reply to the local service manager");
    require(until([&] {
        for (const auto& notice : client.take_notices())
            if (notice.incoming && notice.id == bid && notice.text == "B replies through IPC") return true;
        return false;
    }), "service chat events retain sender identity");
    require(actions.permissions(aid, 0) && until([&] { return a->permissions() == 0; }),
            "local service manager revokes the selected connection's permissions");
    require(b->permissions() == kPermissionFileTransfer, "permission changes preserve the other connection");
    require(actions.permissions(aid, kPermissionAll) && until([&] { return a->permissions() == kPermissionFileTransfer; }),
            "service permission restoration cannot exceed admission");
    const auto path = f.root / "operator-service.txt";
    const std::string content = "file opened under the authenticated operator token";
    { std::ofstream output(path, std::ios::binary); output << content; }
    require(actions.file(bid, path.string(), &error), "queue a user-authorized file through the service pipe");
    FileOffer offer;
    require(until([&] { return b->take_file_offer(offer); }), "service hashes the operator file under the caller token");
    require(b->send_file_answer(offer.transfer_id, FileCode::Ok, 0).ok(), "viewer accepts the service file");
    std::string received;
    require(until([&] {
        FileChunk chunk;
        if (b->receive_file_chunk(chunk, 20ms).ok()) {
            require(chunk.transfer_id == offer.transfer_id && chunk.offset == received.size(), "service file offsets remain contiguous");
            received.append(reinterpret_cast<const char*>(chunk.data.data()), chunk.data.size());
        }
        return received.size() == content.size();
    }) && received == content, "service reopens the file with the operator token and delivers intact bytes");
    require(b->send_file_result(offer.transfer_id, FileCode::Ok, received.size()).ok(), "service file completes");
    FileOffer unrelated;
    require(!a->take_file_offer(unrelated), "service file never reaches the other viewer");
    require(actions.disconnect(aid) && until([&] { return !a->peer_alive(); }), "service manager disconnects the explicit target");
    a.reset();
    require(b->peer_alive() && f.host->running(), "other viewer and host survive operator disconnect");

    control.stop();
    require(until([&] { return !client.connected(); }), "a lost service endpoint disables local actions");
    require(client.information().device_id.empty() && client.information().fingerprint.empty(),
            "endpoint loss clears stale service identity");
    require(!actions.chat(bid, "offline"), "an offline service action is rejected");
    require(control.start_for_test(*f.host, pipe, &error, [] {
        return host::ServiceRegistrationInfo{"restarted-device", false};
    }), "restart the endpoint with a new worker-instance identity");
    require(until([&] { return client.connected() && actions.viewers().size() == 1; }), "manager reconnects after the endpoint restarts");
    require(client.information().device_id == "restarted-device" && !client.information().registration_enabled,
            "reconnected manager replaces the previous registration snapshot");
    const auto fresh = actions.viewers().front().id;
    require(fresh != bid && !actions.chat(bid, "stale after restart") && !actions.disconnect(bid),
            "old UI IDs never target a new worker even when remote numeric IDs are identical");
    require(actions.chat(fresh, "fresh target") && until([&] { return b->take_chat(chat); }) && chat == "fresh target",
            "fresh service selection works after restart");
    finish(b);
    auto abandoned = std::async(std::launch::async, [&] {
        try { auto viewer = f.connect(SessionMode::FileTransfer, kPermissionFileTransfer); return false; }
        catch (const std::exception&) { return true; }
    });
    require(until([&] { return client.approvals().size() == 1; }), "another request waits for its local operator");
    client.stop();
    require(abandoned.get(), "closing the local manager cancels outstanding approval instead of admitting the viewer");
    host::ServiceControlClient untrusted;
    untrusted.start_for_test(pipe, GetCurrentProcessId() + 1);
    require(!until([&] { return untrusted.connected(); }, 500ms), "manager refuses a pipe owned by an unexpected process");
    untrusted.stop();
    host::ServiceControlClient forged_service;
    forged_service.start_for_test(pipe, 0);
    require(!until([&] { return forged_service.connected(); }, 500ms),
            "production identity checks reject a user-owned pipe impersonating the SYSTEM service");
    forged_service.stop();
    control.stop();
    require(control.start_for_test(*f.host, pipe, &error), "service approval starts without a connected operator");
    auto missing = std::async(std::launch::async, [&] {
        try { auto viewer = f.connect(SessionMode::FileTransfer, kPermissionFileTransfer); return false; }
        catch (const std::exception&) { return true; }
    });
    require(missing.get(), "service Click refuses the viewer when no authenticated local operator is present");
    control.stop();
}

void application_latency() {
    Fixture f;
    f.config.settings.max_viewers = 1;
    f.config.adaptive_bitrate = true;
    f.start();
    ServerInfo info;
    auto viewer = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0, &info);
    require(info.framerate == 15 && keyframe(*viewer), "adaptive session begins at RustDesk initial FPS");
    require(viewer->send_latency_probe().ok() && until([&] { return viewer->latency().replies > 0; }),
            "client clock synchronization still works with reverse QoS probes");
    require(until([&] { return f.host->status().stats.qos_probe_replies >= 4; }, 6s),
            "host measures authenticated application round trips");
    require(until([&] { return viewer->take_stream_update(info) && info.framerate > 15; }),
            "good application replies raise shared FPS on a static desktop");
    const auto idle = f.host->status().stats;
    require(idle.repeats > 0 && idle.video_pending_frames == 0,
            "static refinements are best effort and claim no delivery-window credit");
    finish(viewer);
}

void video_backpressure() {
    Fixture f;
    f.config.settings.max_viewers = 1;
    f.config.adaptive_bitrate = true;
    f.media->moving = true;
    f.start();
    auto viewer = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput);
    require(keyframe(*viewer), "moving video begins with an IDR");
    require(until([&] {
        const auto stats = f.host->status().stats;
        return stats.qos_probe_replies >= 4 && stats.video_pending_ms > 2000 && stats.framerate <= 5;
    }, 7s), "unconsumed video throttles shared QoS while control probes still return");
    const auto blocked = f.host->status().stats;
    require(blocked.video_pending_frames > 0 && blocked.video_pending_frames <= 60 && blocked.encoded < 60,
            "a stalled application has a bounded delivery window");
    require(viewer->send_latency_probe().ok() && until([&] { return viewer->latency().replies > 0; }),
            "clock synchronization remains responsive during video backpressure");
    uint64_t expected = 1;
    unsigned pictures = 0;
    bool fresh_idr = false;
    require(until([&] {
        FrameHeader header;
        std::vector<uint8_t> bytes;
        if (viewer->receive_frame(header, bytes, 50ms).ok()) {
            require(header.seq == expected++, "resumed video retains contiguous transport sequences");
            ++pictures;
            fresh_idr |= header.idr;
        }
        const auto stats = f.host->status().stats;
        return fresh_idr && pictures > blocked.video_pending_frames && stats.video_receipts > blocked.video_receipts &&
               stats.video_pending_ms < 500 && stats.framerate > 5;
    }, 12s), "consumption resumes at a fresh IDR and shared FPS recovers");
    require(f.media->capture_created == 1 && f.media->encoder_created == 1,
            "network recovery preserves the shared capture and encoder");
    finish(viewer);
}

void slow_viewer_independent_display() {
    TwoDisplays f;
    f.left->moving = f.right->moving = true;
    auto slow = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    auto fast = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1);
    require(picture(*slow, 640) && picture(*fast, 800), "viewers start on independent moving displays");
    const auto end = std::chrono::steady_clock::now() + 2500ms;
    unsigned received = 0;
    while (std::chrono::steady_clock::now() < end) {
        FrameHeader header;
        std::vector<uint8_t> bytes;
        if (fast->receive_frame(header, bytes, 100ms).ok()) {
            require(header.width == 800, "healthy viewer stays on its selected display");
            ++received;
        }
    }
    require(received >= 20, "stalled viewer does not block another display's video delivery");
    const auto before_switch = std::chrono::steady_clock::now();
    switch_to(*slow, 1, DisplaySwitchCode::Ok);
    require(std::chrono::steady_clock::now() - before_switch < 1s,
            "stalled viewer can switch displays without waiting for its video queue");
    require(picture(*slow, 800) && picture(*fast, 800), "both subscriptions resume on the shared target");
    require(f.right->capture_created == 1 && f.right->encoder_created == 1,
            "slow viewer switch reuses the existing target pipeline");
    finish(slow);
    require(picture(*fast, 800), "healthy viewer continues after slow viewer leaves");
    finish(fast);
}

// Explicit opt-in: captures the local display but injects no input and never
// changes modes, privacy, login state or topology. The normal suite stays
// deterministic and does not need a GPU or interactive desktop.
void hardware_shared_display(bool automatic = false) {
    Fixture f;
    f.config.adaptive_bitrate = true;
    HostServerConfig defaults;
    std::atomic<unsigned> captures{0}, encoders{0};
    f.config.displays = defaults.displays;
    f.config.follow_desktop = defaults.follow_desktop;
    f.config.desktop_status = defaults.desktop_status;
    f.config.capture_factory = [&](const DisplayInfo& display, const CaptureConfig& cfg, std::string* error) {
        ++captures;
        return defaults.capture_factory(display, cfg, error);
    };
    f.config.encoder_factory = [&](ID3D11Device* device, const EncoderConfig& cfg, std::string* error) {
        ++encoders;
        return defaults.encoder_factory(device, cfg, error);
    };
    f.start();
    ComPtr<ID3D11Device> a_device, b_device;
    auto device = [](ComPtr<ID3D11Device>& result) {
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                    result.put(), nullptr, nullptr)), "create viewer GPU device");
    };
    device(a_device); device(b_device);
    const auto a_codecs = supported_decoder_codecs(a_device.get());
    const auto b_codecs = supported_decoder_codecs(b_device.get());
    require(a_codecs != 0 && b_codecs != 0, "probe actual NV12 hardware decoder capabilities");
    ServerInfo a_info, b_info;
    const auto preference = automatic ? StreamCodecPreference::Auto : StreamCodecPreference::H264;
    auto a = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, kDisplayAuto, &a_info, a_codecs, preference);
    auto b = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, kDisplayAuto, &b_info, b_codecs, preference);
    require((a_codecs & video_codec_bit(a_info.codec)) && (b_codecs & video_codec_bit(b_info.codec)),
            "Hello chooses a codec each actual viewer GPU can decode");
    std::string error;
    unsigned a_pictures = 0, b_pictures = 0;
    DecoderConfig cfg;
    cfg.codec = a_info.codec;
    cfg.d3d11_device = a_device.get();
    cfg.gpu_handler = [&](const GpuFrame&) { ++a_pictures; return Error::success(); };
    auto a_decoder = create_decoder(cfg, &error);
    cfg.codec = b_info.codec;
    cfg.d3d11_device = b_device.get();
    cfg.gpu_handler = [&](const GpuFrame&) { ++b_pictures; return Error::success(); };
    auto b_decoder = create_decoder(cfg, &error);
    require(a_decoder && b_decoder, "open two independent hardware-stream decoders");
    auto decode = [&](QuicSession& viewer, IVideoDecoder& decoder, unsigned& pictures, unsigned count,
                      std::optional<uint32_t> expected_stream = {}) {
        const auto target = pictures + count;
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (pictures < target && std::chrono::steady_clock::now() < deadline) {
            FrameHeader header;
            std::vector<uint8_t> data;
            auto received = viewer.receive_frame(header, data, 150ms);
            if (!received.ok()) {
                require(received.status() == Status::WaitTimeout && viewer.request_keyframe().ok(),
                        "request hardware IDR on static desktop");
                continue;
            }
            if (expected_stream && header.stream_id != *expected_stream) continue;
            std::vector<DecodedFrame> output;
            require(decoder.decode(data.data(), data.size(), header.pts_ms, header.keyframe, output).ok(),
                    "shared GPU access unit decodes without reference errors");
            require(output.empty(), "decoder delivers GPU textures without CPU fallback");
        }
        require(pictures >= target, "viewer decodes requested picture count");
    };
    decode(*a, *a_decoder, a_pictures, 8);
    decode(*b, *b_decoder, b_pictures, 8);
    require(captures == 1 && encoders == 1, "two real QUIC viewers share one GPU capture and encoder");
    uint16_t current_display = 0;
    for (size_t index = 0; index < a_info.displays.size(); ++index)
        if (a_info.displays[index].left == a_info.origin_x && a_info.displays[index].top == a_info.origin_y)
            current_display = static_cast<uint16_t>(index);
    auto selected = subscriptions(*a, {0, {}, {}, {current_display}});
    require(selected.streams.size() == 1, "GPU viewer enters subscription-set mode");
    decode(*a, *a_decoder, a_pictures, 4, selected.streams[0].stream_id);
    require(subscriptions(*a, {}).streams.empty(), "GPU viewer can temporarily unsubscribe all video");
    selected = subscriptions(*a, {0, {current_display}, {}, {}});
    require(selected.streams.size() == 1 && selected.streams[0].stream_id != 0,
            "GPU resubscription receives a fresh stream ID");
    require(a->request_display_keyframe(selected.streams[0].stream_id).ok(), "request targeted GPU stream IDR");
    decode(*a, *a_decoder, a_pictures, 4, selected.streams[0].stream_id);
    require(captures == 1 && encoders == 1, "GPU resubscription reuses source retained by the second connection");
    finish(a);
    require(b->request_keyframe().ok(), "remaining viewer requests hardware IDR");
    decode(*b, *b_decoder, b_pictures, 8);
    require(captures == 1 && encoders == 1, "remaining decoder continues without GPU resource replacement");
    finish(b);
}

void negotiated_shared_codec() {
    Fixture f;
    f.config.settings.codec = VideoCodec::Hevc;
    f.start();
    ServerInfo initial;
    auto a = f.connect(SessionMode::Desktop, kPermissionAll, {}, 0, &initial);
    require(initial.codec == VideoCodec::Hevc && keyframe(*a), "unrestricted viewer starts with configured HEVC");
    auto b = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0, &initial, kVideoCodecH264);
    require(initial.codec == VideoCodec::H264 && keyframe(*b), "H264-only viewer receives a compatible Hello and IDR");
    ServerInfo changed;
    require(until([&] { return a->take_stream_update(changed) && changed.codec == VideoCodec::H264; }),
            "existing viewer learns common codec before incompatible payloads arrive");
    require(a->request_stream_options({StreamCodecPreference::Hevc, 0, StreamQuality::High}).ok(),
            "request codec incompatible with another viewer");
    StreamOptionsResult options;
    require(until([&] { return a->take_stream_options_result(options); }) &&
            options.code == StreamOptionsCode::Ok && options.codec == VideoCodec::H264,
            "unusable vote is retained while the result stays compatible with all viewers");
    require(a->request_keyframe().ok() && b->request_keyframe().ok() && keyframe(*a) && keyframe(*b),
            "refused codec does not break either stream");
    finish(b);
    require(until([&] { return a->take_stream_update(changed) && changed.codec == VideoCodec::Hevc; }) && keyframe(*a),
            "last restrictive receiver leaving restores preferred codec");
    require(f.media->capture_created == 1, "codec negotiation never recreates desktop capture");
    finish(a);

    Fixture restricted;
    restricted.config.settings.codec = VideoCodec::Hevc;
    restricted.start();
    auto first = restricted.connect(SessionMode::Desktop, kPermissionAll, {}, 0, &initial, kVideoCodecH264);
    auto later = restricted.connect();
    require(initial.codec == VideoCodec::H264 && keyframe(*first) && keyframe(*later),
            "reverse joining order also selects common decoding format");
    require(restricted.media->encoder_created == 1, "compatible later viewer reuses negotiated encoder");
    finish(first);
    require(until([&] { return later->take_stream_update(changed) && changed.codec == VideoCodec::Hevc; }),
            "first restrictive receiver leaving updates remaining connection");
    finish(later);
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    if (argc >= 2 && std::string(argv[1]) == "--elevated-handoff") return elevation_child(argc, argv);
    const auto run = [](const char* name, auto test) {
        std::printf("\n%s\n", name);
        try { test(); }
        catch (const std::exception& error) { ++failures; std::printf("FAIL %s\n", error.what()); }
    };
    if (argc == 2 && std::string(argv[1]) == "--elevation") {
        run("elevation permission and cancellation", elevation_permissions);
        run("authenticated local elevation handoff", elevation_handoff);
        WSACleanup(); return failures ? 1 : 0;
    }
    if (argc == 2 && (std::string(argv[1]) == "--hardware" || std::string(argv[1]) == "--hardware-auto")) {
        run("hardware shared display", [&] { hardware_shared_display(std::string(argv[1]) == "--hardware-auto"); });
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--topology") {
        run("multiple display topology changes", multiple_display_topology);
        run("multiple display isolated recovery", multiple_display_recovery_isolation);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--camera") {
        run("shared camera service and codec negotiation", shared_camera_service);
        run("camera 4:2:0 fallback on a full-color desktop", shared_camera_full_color);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--connections") {
        run("operator connection management", operator_connection_management);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--service-control") {
        run("authenticated service connection management", service_connection_management);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--power") {
        run("session power request ownership", session_power_requests);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--clipboard-direction") {
        run("clipboard directions and independent file permissions", clipboard_directions);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--revocation") {
        run("running services stop when their permission is revoked",
            running_services_revocation);
        WSACleanup();
        return failures ? 1 : 0;
    }
    run("elevation permission and cancellation", elevation_permissions);
    run("authenticated local elevation handoff", elevation_handoff);
    run("clipboard directions and independent file permissions", clipboard_directions);
    run("running services stop when their permission is revoked",
        running_services_revocation);
    run("session power request ownership", session_power_requests);
    run("static desktop, follower requests and audio", static_desktop_and_followers);
    run("global policy", [] { global_policy(false); });
    run("global policy with frozen UI permissions", [] { global_policy(true); });
    run("policy changed during approval", pending_approval);
    run("publisher-only audio", publisher_only_audio);
    run("audio enabled after admission", late_audio_source);
    run("desktop changed during admission", desktop_switch_during_admission);
    run("independent display subscriptions and resource lifetime", independent_displays);
    run("multiple display streams in one connection", multiple_displays_one_connection);
    run("multiple display rollback and one vote per connection", multiple_display_failures_and_votes);
    run("multiple display backpressure and fair recovery", multiple_displays_backpressure);
    run("multiple display independent cursors", multiple_display_cursors);
    run("multiple display topology changes", multiple_display_topology);
    run("multiple display isolated recovery", multiple_display_recovery_isolation);
    run("failed selection preserves subscriptions", refused_display_keeps_stream);
    run("concurrent admission and display renumbering", concurrent_display_admission);
    run("shared codec negotiation", negotiated_shared_codec);
    run("decoder capabilities follow display subscriptions", display_codec_intersection);
    run("multiuser quality across displays", multiuser_quality);
    run("global codec preference voting", global_codec_preferences);
    run("runtime hardware to software recovery", runtime_software_recovery);
    run("shared camera service and codec negotiation", shared_camera_service);
    run("camera 4:2:0 fallback on a full-color desktop", shared_camera_full_color);
    run("operator connection management", operator_connection_management);
    run("authenticated service connection management", service_connection_management);
    run("host application latency probes", application_latency);
    run("video application backpressure and recovery", video_backpressure);
    run("slow viewer independent display", slow_viewer_independent_display);
    WSACleanup();
    std::printf("host parity: %d failed scenario(s)\n", failures);
    return failures ? 1 : 0;
}
