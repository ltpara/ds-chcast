// Real host + viewer over loopback; every clipboard and input operation is synthetic.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <thread>
#include "chast/host_server.h"
#include "chast/security.h"
#include "viewer_engine.h"
#include "clipboard_stub.h"

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
int failures = 0;
void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", message);
    if (!ok) ++failures;
}
bool wait_for(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 5s) {
    const auto until = Clock::now() + timeout;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    } while (Clock::now() < until);
    return predicate();
}
uint16_t port() {
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(address);
    const bool ok = bind(socket, reinterpret_cast<sockaddr*>(&address), size) == 0 &&
        getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0;
    closesocket(socket);
    return ok ? ntohs(address.sin_port) : 0;
}
class Sink : public chast_viewer::FrameSink {
public:
    bool create() {
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device_, nullptr, nullptr));
    }
    ID3D11Device* device() override { return device_.Get(); }
    bool OnGpuFrame(ID3D11Texture2D*, uint32_t, uint32_t, uint32_t, uint32_t,
                    uint32_t, uint64_t) override { ++frames; return true; }
    std::string error() const override { return {}; }
    std::atomic<unsigned> frames{0};
private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
};

struct Clipboard {
    std::mutex mutex;
    std::condition_variable cv;
    std::string text = "history before connection";
    int fail_reads = 1;
    int fail_writes = 0;
    bool held = false;
    bool reading = false;
    int reads = 0;
    int attempts = 0;
    std::vector<std::string> written;
    bool read(chast::ClipboardContent& content) {
        std::unique_lock lock(mutex);
        ++reads;
        reading = true;
        cv.wait_for(lock, 30s, [&] { return !held; });
        reading = false;
        content = {};
        if (fail_reads > 0) { --fail_reads; return false; }
        if (!text.empty()) content.formats.push_back({chast::ClipboardFormat::Text,
                                                     {text.begin(), text.end()}});
        return true;
    }
    bool write(const chast::ClipboardContent& content) {
        std::lock_guard lock(mutex);
        ++attempts;
        if (reading || fail_writes > 0) {
            if (fail_writes > 0) --fail_writes;
            return false;
        }
        text = content.text();
        written.push_back(text);
        return true;
    }
    void set(std::string value) { std::lock_guard lock(mutex); text = std::move(value); }
    void hold(bool value) { std::lock_guard lock(mutex); held = value; cv.notify_all(); }
    bool blocked() { std::lock_guard lock(mutex); return held && reading; }
    bool busy() { std::lock_guard lock(mutex); return reading; }
    int read_count() { std::lock_guard lock(mutex); return reads; }
    int write_count() { std::lock_guard lock(mutex); return static_cast<int>(written.size()); }
    int attempt_count() { std::lock_guard lock(mutex); return attempts; }
    bool has(const std::string& value) {
        std::lock_guard lock(mutex);
        return !written.empty() && written.back() == value;
    }
    void refuse(int count) { std::lock_guard lock(mutex); fail_writes = count; }
};

void worker_reconnect() {
    auto old = std::make_shared<Clipboard>();
    old->fail_reads = 0;
    old->hold(true);
    chast::ClipboardBackend backend;
    backend.read_files = [](std::vector<std::string>&) { return chast::ClipboardReadState::Empty; };
    backend.read = [old](chast::ClipboardContent& content) {
        old->read(content);
        return chast::ClipboardReadState::Content;
    };
    chast::ClipboardWorker worker(std::move(backend));
    worker.Read(1);
    check(wait_for([&] { return old->blocked(); }), "old backend read is blocked");
    worker.Cancel();
    chast::ClipboardBackend replacement;
    replacement.read_files = [](std::vector<std::string>&) { return chast::ClipboardReadState::Empty; };
    replacement.read = [](chast::ClipboardContent&) { return chast::ClipboardReadState::Empty; };
    worker.SetBackend(std::move(replacement));
    check(!worker.Read(2), "reconnect does not create a competing read");
    old->hold(false);
    check(wait_for([&] { return worker.Read(2); }), "replacement backend starts after old call returns");
    chast::ClipboardWorker::Result result;
    check(wait_for([&] { return worker.Take(result); }) && result.epoch == 2 &&
              result.snapshot.state == chast::ClipboardReadState::Empty,
          "old connection result is discarded and new backend is used");
}

void local_file_lifetime() {
    const auto root = std::filesystem::temp_directory_path() /
        ("chast-clipboard-source-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const auto path = root / "copied.bin", saved = root / "old.bin";
    { std::ofstream output(path, std::ios::binary); output << "old"; }
    std::vector<chast::ClipboardFileEntry> entries;
    std::vector<std::string> paths;
    std::string error;
    auto u8 = path.u8string();
    const bool collected = chast::collect_clipboard_files({std::string(u8.begin(), u8.end())}, entries, paths, &error);
    auto source = collected ? chast::make_local_clipboard_files(entries, paths, &error) : nullptr;
    check(source != nullptr, "copied file identities captured without reading bytes");
    if (source) {
        check(source->Unchanged(0), "a freshly captured file still matches its announced identity");
        std::filesystem::rename(path, saved);
        { std::ofstream output(path, std::ios::binary); output << "new"; }
        std::vector<uint8_t> bytes;
        check(source->Read(0, 0, 3, bytes, &error) && std::string(bytes.begin(), bytes.end()) == "old",
              "replacing the path cannot make an old offer read the new file");
        { std::ofstream output(saved, std::ios::binary); output << "changed"; }
        check(!source->Unchanged(0), "an in-place rewrite invalidates the announced index");
        check(!source->Read(0, 0, 3, bytes, &error) && bytes.empty(), "mutated copied file fails instead of returning mixed content");
        source->Cancel();
        check(!source->Unchanged(0), "a cancelled source never serves a stale index");
        check(!source->Read(0, 0, 1, bytes, &error) && bytes.empty(), "cancelled source returns no bytes");
    }
    source.reset();
    std::filesystem::remove_all(root);
}

void cancelled_virtual_read() {
    chast::ClipboardFileChannel channel;
    auto files = channel.Receive(10, {{"waiting.bin", 12}});
    std::atomic<bool> started{false}, finished{false};
    bool success = true;
    std::thread reader([&] {
        std::vector<uint8_t> bytes;
        started = true;
        success = files->Read(0, 0, 12, bytes, nullptr);
        finished = true;
    });
    wait_for([&] { return started.load(); });
    std::this_thread::sleep_for(30ms);
    channel.CancelRemote();
    check(wait_for([&] { return finished.load(); }, 500ms), "cancel wakes a pending paste read without a network reply");
    reader.join();
    check(!success && !files->Active(), "old virtual offer remains invalid after cancellation");
}
} // namespace

int main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    worker_reconnect();
    local_file_lifetime();
    cancelled_virtual_read();
    Sink sink;
    if (!sink.create()) { std::puts("skip: no D3D desktop"); WSACleanup(); return 77; }
    auto local = std::make_shared<Clipboard>();
    auto remote = std::make_shared<Clipboard>();
    const auto root = std::filesystem::temp_directory_path() /
        ("chast-clipboard-async-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    chast::HostServerConfig config;
    use_empty_clipboard(config);
    config.clipboard_read = [local](chast::ClipboardContent& item) { return local->read(item); };
    config.clipboard_write = [local](const chast::ClipboardContent& item) { return local->write(item); };
    config.settings.bind_address = "127.0.0.1";
    config.settings.port = port();
    const auto host_port = config.settings.port;
    config.settings.carrier = false;
    config.settings.tunnel = false;
    config.settings.max_viewers = 1;
    config.settings.allow_audio = false;
    config.settings.allow_microphone = false;
    config.settings.auth_mode = chast::HostAuthMode::Password;
    config.settings.password_hash = chast::hash_password("clipboard-test");
    config.settings.audit_spool = (root / "audit.jsonl").string();
    config.settings.file_receive_directory = (root / "receive").string();
    config.dry_run_input = true;
    config.first_client_timeout_ms = 0;
    config.keyframe_interval_ms = 500;
    std::string error;
    auto host = chast::HostServer::create(std::move(config), &error);
    check(host != nullptr, "host created");
    if (!host) return 1;
    host->start();
    check(wait_for([&] { return host->status().state == chast::HostState::Listening; }), "host listening");
    auto viewer = std::make_unique<chast_viewer::ViewerEngine>(&sink);
    viewer->SetClipboardSource([remote](chast::ClipboardContent& item) { return remote->read(item); });
    viewer->SetClipboardSink([remote](const chast::ClipboardContent& item) { return remote->write(item); });
    viewer->SetClipboardFileSource([](std::vector<std::string>&) { return false; });
    viewer->SetClipboardFileSink([](const std::vector<std::string>&) { return true; });
    check(viewer->Start("127.0.0.1", host_port, chast::kDisplayAuto, {}, true, {}, "clipboard-test"),
          "viewer started");
    if (!wait_for([&] { return sink.frames.load() > 0; }, 15s)) {
        std::printf("FAIL no stream: %s\n", viewer->Snapshot().error.c_str());
        viewer->Stop(); host->stop(); return 1;
    }
    check(wait_for([&] { return local->read_count() >= 3 && remote->read_count() >= 3; }),
          "both first-read failures recovered");
    check(local->write_count() == 0 && remote->write_count() == 0,
          "neither endpoint sends historical content after baseline failure");

    // A confirmed empty baseline must not swallow the first subsequent copy.
    viewer->SetClipboardSync(false);
    std::this_thread::sleep_for(200ms);
    remote->set("");
    const int reads_before = remote->read_count();
    viewer->SetClipboardSync(true);
    check(wait_for([&] { return remote->read_count() >= reads_before + 2; }), "empty baseline observed");
    remote->set("first copy after empty");
    check(wait_for([&] { return local->has("first copy after empty"); }), "first copy after empty is delivered");

    remote->hold(true);
    check(wait_for([&] { return remote->blocked(); }), "viewer read blocked");
    const auto input_before = host->status().stats.input_events;
    const auto releases_before = host->status().stats.input_releases;
    const auto frames_before = sink.frames.load();
    chast::InputEvent key;
    key.kind = chast::InputKind::Key;
    key.key.hid_usage = 0x04;
    key.key.action = chast::KeyAction::Down;
    check(viewer->SendInput(key), "key down queued during slow clipboard read");
    key.key.action = chast::KeyAction::Up;
    check(viewer->SendInput(key) && viewer->ReleaseInput(), "key up and release queued");
    check(wait_for([&] { const auto stats = host->status().stats;
        return stats.input_events >= input_before + 2 && stats.input_releases > releases_before;
    }, 3s), "input and releases arrive while viewer clipboard remains blocked");
    check(wait_for([&] { return sink.frames.load() > frames_before; }, 3s), "video continues while clipboard is blocked");
    remote->hold(false);

    local->hold(true);
    check(wait_for([&] { return local->blocked(); }), "host read blocked");
    local->refuse(2);
    const int attempts_before = local->attempt_count();
    const auto sent_before = viewer->Snapshot().clipboard_sent;
    remote->set("retry after busy read");
    check(wait_for([&] { return viewer->Snapshot().clipboard_sent > sent_before; }), "copy queued at busy host");
    check(local->attempt_count() == attempts_before, "host does not race its own read");
    local->hold(false);
    check(wait_for([&] { return local->has("retry after busy read"); }), "host retries failed writes until success");
    check(local->attempt_count() == attempts_before + 3, "host performed two failed attempts and one success");

    remote->refuse(2);
    local->set("host copy retried by viewer");
    check(wait_for([&] { return remote->has("host copy retried by viewer"); }), "viewer also retries failed writes");

    local->hold(true);
    check(wait_for([&] { return local->blocked(); }), "host read blocked before replacement");
    auto sent = viewer->Snapshot().clipboard_sent;
    remote->set("obsolete copy");
    check(wait_for([&] { return viewer->Snapshot().clipboard_sent > sent; }), "older copy sent");
    sent = viewer->Snapshot().clipboard_sent;
    remote->set("host copy retried by viewer");
    check(wait_for([&] { return viewer->Snapshot().clipboard_sent > sent; }), "newer copy sent");
    const int writes_before = local->write_count();
    local->hold(false);
    check(wait_for([&] { return local->write_count() > writes_before &&
                               local->has("host copy retried by viewer"); }),
          "newest queued copy wins even when it matches the previous baseline");
    check(local->write_count() == writes_before + 1, "obsolete queued copy was never applied");

    local->hold(true);
    check(wait_for([&] { return local->blocked(); }), "host read blocked before revocation");
    sent = viewer->Snapshot().clipboard_sent;
    remote->set("cancel on revocation");
    check(wait_for([&] { return viewer->Snapshot().clipboard_sent > sent; }), "pending copy sent before revocation");
    host->set_permissions(chast::kPermissionAll & ~chast::kPermissionClipboard);
    check(wait_for([&] { return !viewer->Snapshot().clipboard_enabled; }), "revocation reaches viewer during blocked read");
    const int writes_at_revoke = local->write_count();
    local->hold(false);
    std::this_thread::sleep_for(500ms);
    check(local->write_count() == writes_at_revoke, "revocation cancels pending host write");
    const int reads_at_restore = local->read_count();
    host->set_permissions(chast::kPermissionAll);
    check(wait_for([&] { return viewer->Snapshot().clipboard_enabled; }), "clipboard permission restored");
    check(wait_for([&] { return local->read_count() >= reads_at_restore + 2; }), "host restores its baseline before new copies");

    remote->hold(true);
    check(wait_for([&] { return remote->blocked(); }), "viewer read blocked before local pause");
    const auto host_sent = host->status().stats.clipboard_sent;
    local->set("cancel viewer write on pause");
    check(wait_for([&] { return host->status().stats.clipboard_sent > host_sent; }), "host copy sent to busy viewer");
    viewer->SetClipboardSync(false);
    std::this_thread::sleep_for(200ms);
    const int viewer_writes = remote->write_count();
    remote->hold(false);
    std::this_thread::sleep_for(500ms);
    check(remote->write_count() == viewer_writes, "local pause cancels the queued viewer write");
    const int reads_at_pause = remote->read_count();
    viewer->SetClipboardSync(true);
    check(wait_for([&] { return remote->read_count() >= reads_at_pause + 2; }), "viewer resumes with a fresh baseline");

    local->refuse(100);
    const int cap_start = local->attempt_count();
    remote->set("permanently refused copy");
    check(wait_for([&] { return local->attempt_count() >= cap_start + 10; }, 7s),
          "persistent contention reaches bounded retry limit");
    std::this_thread::sleep_for(600ms);
    check(local->attempt_count() == cap_start + 10, "failed copy is not retried forever");
    local->refuse(0);
    remote->set("copy after retry limit");
    check(wait_for([&] { return local->has("copy after retry limit"); }), "new copies still work after retry exhaustion");

    local->hold(true);
    remote->hold(true);
    check(wait_for([&] { return local->blocked() && remote->blocked(); }), "both reads blocked for shutdown");
    const auto stop_start = Clock::now();
    viewer->Stop();
    viewer.reset();
    host->stop();
    host.reset();
    check(Clock::now() - stop_start < 3s, "stop and destruction do not join blocked clipboard reads");
    local->hold(false);
    remote->hold(false);
    check(wait_for([&] { return !local->busy() && !remote->busy(); }), "detached backend state can finish safely");
    WSACleanup();
    std::printf("clipboard async checks: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
