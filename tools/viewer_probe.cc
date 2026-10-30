// Headless acceptance for the viewer engine's shared-folder transfers and
// remote console.
//
// The Flutter shell only polls ViewerStatus and draws it, so the features
// themselves (listing, transfer-id mapping, local mirroring, cancellation,
// uploads, and the terminal's open/input/resize/close handshake) are exercised
// here without a window: a real HostServer runs in this process, the viewer
// engine connects to it over loopback QUIC with a real certificate and a real
// password, and the transferred files are compared with the seeded tree.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "chast/clipboard.h"
#include "chast/error.h"
#include "chast/file_transfer.h"
#include "chast/host_server.h"
#include "chast/host_settings.h"
#include "chast/protocol.h"
#include "chast/security.h"
#include "viewer_engine.h"

namespace {

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr const char* kPassword = "open-sesame";
constexpr uint64_t kBigFileBytes = 16ull * 1024ull * 1024ull;

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

fs::path from_utf8(const std::string& text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

// A sink that keeps the decoder on the GPU but publishes nothing: the walk
// does not depend on the picture, and a headless run has no window.
class ProbeSink final : public chast_viewer::FrameSink {
public:
    bool create(std::string* error) {
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const HRESULT hr =
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                              D3D11_SDK_VERSION, &device_, nullptr, &context_);
        if (FAILED(hr)) {
            char text[64];
            std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
            *error = std::string("D3D11CreateDevice failed: ") + text;
            return false;
        }
        return true;
    }

    ID3D11Device* device() override { return device_.Get(); }

    bool OnGpuFrame(ID3D11Texture2D*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                    uint64_t) override {
        frames_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::string error() const override { return {}; }

    uint64_t frames() const { return frames_.load(std::memory_order_relaxed); }

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    std::atomic<uint64_t> frames_{0};
};

uint16_t pick_free_port() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return 0;
    }
    SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    uint16_t port = 0;
    int length = sizeof(address);
    if (bind(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
        port = ntohs(address.sin_port);
    }
    closesocket(socket_handle);
    WSACleanup();
    return port;
}

bool write_file(const fs::path& path, const std::string& bytes, std::string* error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        *error = "create_directories(" + utf8(path.parent_path()) + "): " + ec.message();
        return false;
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        *error = "cannot open " + utf8(path);
        return false;
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        *error = "cannot write " + utf8(path);
        return false;
    }
    return true;
}

bool read_file(const fs::path& path, std::string* bytes, std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *error = "cannot open " + utf8(path);
        return false;
    }
    bytes->assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (!stream.good() && !stream.eof()) {
        *error = "cannot read " + utf8(path);
        return false;
    }
    return true;
}

struct ShareSeed {
    std::string tree_name;
    std::string big_name;
    std::string batch_name;
    std::string empties_name;
    std::string loose_name;
};

bool seed_share(const fs::path& share, const ShareSeed& names, std::string* error) {
    std::error_code ec;
    fs::remove_all(share, ec);
    fs::create_directories(share / names.tree_name / "deep" / "nested-empty", ec);
    fs::create_directories(share / names.tree_name / "empty", ec);
    fs::create_directories(share / names.tree_name / "another", ec);
    fs::create_directories(share / names.big_name, ec);
    fs::create_directories(share / names.batch_name / "void", ec);
    fs::create_directories(share / names.empties_name / "hollow", ec);
    if (ec) {
        *error = "cannot seed the shared folder: " + ec.message();
        return false;
    }
    // A file at the top of the share proves the walk stays inside the folder
    // it was asked for.
    if (!write_file(share / "outside.txt", "outside the walk", error) ||
        !write_file(share / names.tree_name / "a.txt", "alpha", error) ||
        !write_file(share / names.tree_name / "deep" / "b.bin",
                    std::string("\x00\x01\x02\x03\xFE\xFF", 6) + "pattern", error) ||
        !write_file(share / names.tree_name / "another" / "c.txt", "charlie", error) ||
        !write_file(share / names.batch_name / "one.txt", "batch one", error) ||
        !write_file(share / names.loose_name, "loose file", error)) {
        return false;
    }
    const fs::path big = share / names.big_name / "Big.bin";
    if (!write_file(big, {}, error)) {
        return false;
    }
    fs::resize_file(big, kBigFileBytes, ec);
    if (ec) {
        *error = "cannot create " + utf8(big) + ": " + ec.message();
        return false;
    }
    return true;
}

// Collects what the viewer would put on the local clipboard. The probe never
// touches the operator's real clipboard, in either direction.
struct ClipboardCapture {
    std::mutex mutex;
    std::condition_variable cv;
    bool landed = false;
    std::vector<std::string> paths;

    bool sink(const std::vector<std::string>& files) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            paths = files;
            landed = true;
        }
        cv.notify_all();
        return true;
    }

    bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return landed; });
    }
};

bool wait_clipboard_received(chast_viewer::ViewerEngine& engine,
                             std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.Snapshot().clipboard_received != 0) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

// What the host clipboard holds in the clipboard scenario: a small text file,
// a quarter-megabyte binary with a non-ASCII name and a copied folder with a
// nested tree, so the viewer must rebuild rel_path subdirectories.
struct ClipboardSeed {
    std::string text_name;
    std::string binary_name;
    std::string folder_name;
    std::string text_bytes;
    std::string binary_bytes;
    std::string inner_name;
    std::string inner_bytes;
    std::string deep_name;
    std::string deep_bytes;
};

std::string clipboard_binary_bytes() {
    std::string bytes;
    bytes.reserve(256u * 1024u);
    for (size_t i = 0; i < 256u * 1024u; ++i) {
        bytes.push_back(static_cast<char>((i * 31u + 7u) & 0xFFu));
    }
    return bytes;
}

bool seed_clipboard(const fs::path& directory, const ClipboardSeed& seed,
                    std::string* error) {
    std::error_code ec;
    fs::remove_all(directory, ec);
    fs::create_directories(directory / from_utf8(seed.folder_name) / "sub", ec);
    if (ec) {
        *error = "cannot seed the clipboard folder: " + ec.message();
        return false;
    }
    return write_file(directory / from_utf8(seed.text_name), seed.text_bytes, error) &&
           write_file(directory / from_utf8(seed.binary_name), seed.binary_bytes, error) &&
           write_file(directory / from_utf8(seed.folder_name) / from_utf8(seed.inner_name),
                      seed.inner_bytes, error) &&
           write_file(
               directory / from_utf8(seed.folder_name) / "sub" / from_utf8(seed.deep_name),
               seed.deep_bytes, error);
}

bool check_clipboard_bytes(const fs::path& path, const std::string& expected,
                           std::string* error) {
    std::string bytes;
    if (!read_file(path, &bytes, error)) {
        return false;
    }
    if (bytes != expected) {
        *error = "the clipboard copy of '" + utf8(path.filename()) + "' is " +
                 std::to_string(bytes.size()) + " bytes, expected " +
                 std::to_string(expected.size());
        return false;
    }
    return true;
}

// The viewer's clipboard must hold the two flat files and the copied folder,
// all inside one staging folder, with the folder tree rebuilt and intact.
bool check_clipboard_files(const std::vector<std::string>& paths,
                           const ClipboardSeed& seed, std::string* staging,
                           std::string* error) {
    if (paths.size() != 3) {
        *error = "the clipboard received " + std::to_string(paths.size()) +
                 " top-level item(s), expected 3";
        return false;
    }
    const fs::path first = from_utf8(paths[0]);
    *staging = utf8(first.parent_path());
    if (utf8(first.parent_path().filename()).rfind("chast-clipboard-", 0) != 0) {
        *error = "the clipboard items landed in '" + utf8(first.parent_path()) +
                 "', not a chast-clipboard staging folder";
        return false;
    }
    fs::path text;
    fs::path binary;
    fs::path folder;
    for (const std::string& item : paths) {
        const fs::path path = from_utf8(item);
        if (path.parent_path() != first.parent_path()) {
            *error = "the clipboard items did not land in one staging folder";
            return false;
        }
        const std::string name = utf8(path.filename());
        if (name == seed.text_name) {
            text = path;
        } else if (name == seed.binary_name) {
            binary = path;
        } else if (name == seed.folder_name) {
            folder = path;
        } else {
            *error = "the clipboard received an unexpected item '" + name + "'";
            return false;
        }
    }
    if (text.empty() || binary.empty() || folder.empty()) {
        *error = "the clipboard lost one of the three seeded top-level items";
        return false;
    }
    std::error_code ec;
    size_t top_level = 0;
    for (fs::directory_iterator it(first.parent_path(), ec), end; it != end; it.increment(ec)) {
        ++top_level;
    }
    if (ec || top_level != 3) {
        *error = "the staging folder holds " + std::to_string(top_level) +
                 " top-level item(s), expected 3";
        return false;
    }
    if (!fs::is_directory(folder, ec) || ec) {
        *error = "the copied folder did not arrive as a directory";
        return false;
    }
    return check_clipboard_bytes(text, seed.text_bytes, error) &&
           check_clipboard_bytes(binary, seed.binary_bytes, error) &&
           check_clipboard_bytes(folder / from_utf8(seed.inner_name), seed.inner_bytes, error) &&
           check_clipboard_bytes(folder / "sub" / from_utf8(seed.deep_name), seed.deep_bytes,
                                 error);
}

// The host's side of the reverse copy (protocol v34): every staged top item
// sits two levels below one session staging folder (".item-N/<name>" for a
// flat file, ".root-N/<folder>" for a copied tree), the folder tree is
// rebuilt and the bytes match.
bool check_host_clipboard_files(const std::vector<std::string>& paths,
                                const ClipboardSeed& seed, const fs::path& share,
                                std::string* staging, std::string* error) {
    if (paths.size() != 3) {
        *error = "the host clipboard received " + std::to_string(paths.size()) +
                 " top-level item(s), expected 3";
        return false;
    }
    const fs::path root = from_utf8(paths[0]).parent_path().parent_path();
    *staging = utf8(root);
    auto under = [](const fs::path& child, const fs::path& parent) {
        const fs::path relative = child.lexically_relative(parent);
        return !relative.empty() && relative != ".." && *relative.begin() != "..";
    };
    std::error_code ec;
    if (!under(root, fs::temp_directory_path(ec)) || ec) {
        *error = "the host staged the copy in '" + *staging +
                 "', not below the temporary directory";
        return false;
    }
    const std::string name = utf8(root.filename());
    if (name.rfind("chast-clipboard-", 0) != 0 || name.size() < 3 ||
        name.compare(name.size() - 3, 3, "-in") != 0) {
        *error = "the host staged the copy in '" + *staging +
                 "', not a chast-clipboard session folder";
        return false;
    }
    if (under(root, share)) {
        *error = "the host staged the copy inside its receive directory";
        return false;
    }
    fs::path text;
    fs::path binary;
    fs::path folder;
    for (const std::string& item : paths) {
        const fs::path path = from_utf8(item);
        if (path.parent_path().parent_path() != root) {
            *error = "the host's clipboard items did not land in one staging folder";
            return false;
        }
        const std::string entry_dir = utf8(path.parent_path().filename());
        if (entry_dir.rfind(".item-", 0) != 0 && entry_dir.rfind(".root-", 0) != 0) {
            *error = "the host's clipboard item '" + item +
                     "' is not a staged top item";
            return false;
        }
        const std::string item_name = utf8(path.filename());
        if (item_name == seed.text_name) {
            text = path;
        } else if (item_name == seed.binary_name) {
            binary = path;
        } else if (item_name == seed.folder_name) {
            folder = path;
        } else {
            *error = "the host clipboard received an unexpected item '" + item_name + "'";
            return false;
        }
    }
    if (text.empty() || binary.empty() || folder.empty()) {
        *error = "the host clipboard lost one of the three copied top-level items";
        return false;
    }
    size_t top_level = 0;
    for (fs::directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        ++top_level;
    }
    if (ec || top_level != 3) {
        *error = "the host staging folder holds " + std::to_string(top_level) +
                 " top-level item(s), expected 3";
        return false;
    }
    if (!fs::is_regular_file(text, ec) || ec) {
        *error = "the flat file did not arrive as a regular file";
        return false;
    }
    if (!fs::is_directory(folder, ec) || ec) {
        *error = "the copied folder did not arrive as a directory";
        return false;
    }
    return check_clipboard_bytes(text, seed.text_bytes, error) &&
           check_clipboard_bytes(binary, seed.binary_bytes, error) &&
           check_clipboard_bytes(folder / from_utf8(seed.inner_name), seed.inner_bytes, error) &&
           check_clipboard_bytes(folder / "sub" / from_utf8(seed.deep_name), seed.deep_bytes,
                                 error);
}

std::unique_ptr<chast::HostServer> start_host(
    const std::string& share_directory, uint16_t port, std::string* error,
    bool allow_file_transfer = true, std::vector<std::string> clipboard_files = {},
    std::shared_ptr<std::atomic<bool>> clipboard_ready = nullptr,
    std::function<bool(const std::vector<std::string>&)> clipboard_write_files = {}) {
    chast::HostServerConfig config;
    config.settings.bind_address = "127.0.0.1";
    config.settings.port = port;
    config.settings.carrier = false;
    config.settings.auth_mode = chast::HostAuthMode::Password;
    config.settings.password_hash = chast::hash_password(kPassword);
    config.settings.allow_file_transfer = allow_file_transfer;
    config.settings.allow_terminal = true;
    config.settings.file_receive_directory = share_directory;
    config.serve_once = true;
    config.keyframe_interval_ms = 500;
    // Loopback acceptance must not read or write the operator's clipboard.
    config.clipboard_read = [](chast::ClipboardContent&) { return false; };
    config.clipboard_write = [](const chast::ClipboardContent&) { return true; };
    // A viewer's clipboard list must never reach the real CF_HDROP here; the
    // scenario captures it instead, and everything else acknowledges silently.
    config.clipboard_write_files = std::move(clipboard_write_files);
    if (!config.clipboard_write_files) {
        config.clipboard_write_files = [](const std::vector<std::string>&) { return true; };
    }
    // Files appear only once the scenario raises the flag, so the session
    // baseline sees an empty clipboard and the copy is announced as a change.
    config.clipboard_read_files = [files = std::move(clipboard_files),
                                   ready = std::move(clipboard_ready)](
                                      std::vector<std::string>& paths) {
        if (files.empty() || (ready && !ready->load())) {
            return false;
        }
        paths = files;
        return true;
    };
    config.log = [](const std::string& message) {
        std::fprintf(stderr, "host: %s\n", message.c_str());
    };

    std::unique_ptr<chast::HostServer> server =
        chast::HostServer::create(std::move(config), error);
    if (!server) {
        return nullptr;
    }
    server->start();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        const chast::HostStatus status = server->status();
        if (status.state == chast::HostState::Listening) {
            return server;
        }
        if (status.state == chast::HostState::Failed) {
            *error = "the host failed: " + status.error;
            server->stop();
            return nullptr;
        }
        std::this_thread::sleep_for(20ms);
    }
    *error = "the host did not start listening";
    server->stop();
    return nullptr;
}

bool wait_streaming(chast_viewer::ViewerEngine& engine, std::chrono::milliseconds timeout,
                    std::string* error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        if (status.state == chast_viewer::ViewerState::kStreaming) {
            return true;
        }
        if (status.state == chast_viewer::ViewerState::kFailed ||
            status.state == chast_viewer::ViewerState::kEnded) {
            *error = "the session ended before streaming: " + status.error;
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = "the session did not reach streaming";
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
}

bool wait_permission(chast_viewer::ViewerEngine& engine, uint16_t mask,
                     std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if ((engine.Snapshot().permissions & mask) != 0) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

bool wait_file_permission(chast_viewer::ViewerEngine& engine,
                          std::chrono::milliseconds timeout) {
    return wait_permission(engine, chast::kPermissionFileTransfer, timeout);
}

// Waits for a refused files-only handshake and hands back the final status so
// the caller can check the denial code. Streaming at any point means the host
// accepted what it was supposed to refuse.
bool wait_refusal(chast_viewer::ViewerEngine& engine, std::chrono::milliseconds timeout,
                  chast_viewer::ViewerStatus* final_status, std::string* error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        if (status.state == chast_viewer::ViewerState::kFailed ||
            status.state == chast_viewer::ViewerState::kEnded) {
            *final_status = status;
            return true;
        }
        if (status.state == chast_viewer::ViewerState::kStreaming) {
            *error = "the host accepted a files-only session without file transfer";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = "the refused files-only session never ended";
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
}

struct WalkOutcome {
    int code = -1;
    std::string destination;
    uint64_t total = 0;
    uint64_t done = 0;
    std::string error;
};

bool await_walk(chast_viewer::ViewerEngine& engine, uint64_t before,
                std::chrono::milliseconds timeout, WalkOutcome* outcome, std::string* error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        if (status.folder_download_answers != before) {
            outcome->code = status.folder_download_code;
            outcome->destination = status.folder_download_destination;
            outcome->total = status.folder_download_files_total;
            outcome->done = status.folder_download_files_done;
            outcome->error = status.folder_download_error;
            return true;
        }
        if (status.state != chast_viewer::ViewerState::kStreaming) {
            *error = "the session ended during the walk";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = "the walk did not finish";
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
}

bool run_walk(chast_viewer::ViewerEngine& engine, const std::string& remote_path,
              std::chrono::milliseconds timeout, WalkOutcome* outcome, std::string* error) {
    const uint64_t before = engine.Snapshot().folder_download_answers;
    if (!engine.RequestDirectoryDownload(remote_path)) {
        *error = "RequestDirectoryDownload(" + remote_path + ") was refused";
        return false;
    }
    return await_walk(engine, before, timeout, outcome, error);
}

// Downloads a mixed selection the way the multi-select bar does: two folders
// (one holding a file and an empty directory, one holding only an empty
// directory) plus a loose file from the shared root.
bool run_batch(chast_viewer::ViewerEngine& engine, const ShareSeed& names,
               std::chrono::milliseconds timeout, WalkOutcome* outcome, std::string* error) {
    std::vector<chast_viewer::ViewerEngine::DownloadRoot> roots;
    if (engine.RequestBatchDownload(roots)) {
        *error = "an empty batch download was accepted";
        return false;
    }
    roots.push_back({"../outside", false});
    if (engine.RequestBatchDownload(roots)) {
        *error = "a batch download outside the share was accepted";
        return false;
    }
    roots.clear();
    roots.push_back({names.batch_name, true});
    roots.push_back({names.empties_name, true});
    roots.push_back({names.loose_name, false});
    const uint64_t before = engine.Snapshot().folder_download_answers;
    if (!engine.RequestBatchDownload(std::move(roots))) {
        *error = "RequestBatchDownload was refused";
        return false;
    }
    return await_walk(engine, before, timeout, outcome, error);
}

// Cancels as soon as the walk asks the host for `file_name`, which puts the
// cancel on the download-answer/offer race the UI hits when the operator
// presses cancel right after the progress line names a file. A file this size
// also guarantees the transfer cannot finish within the polling interval.
bool cancel_walk(chast_viewer::ViewerEngine& engine, const std::string& remote_path,
                 const std::string& file_name, std::chrono::milliseconds timeout,
                 WalkOutcome* outcome, std::string* error) {
    const uint64_t before = engine.Snapshot().folder_download_answers;
    if (!engine.RequestDirectoryDownload(remote_path)) {
        *error = "RequestDirectoryDownload(" + remote_path + ") was refused";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool cancelled = false;
    for (;;) {
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        if (status.folder_download_answers != before) {
            outcome->code = status.folder_download_code;
            outcome->destination = status.folder_download_destination;
            outcome->total = status.folder_download_files_total;
            outcome->done = status.folder_download_files_done;
            outcome->error = status.folder_download_error;
            if (!cancelled) {
                *error = "the walk finished before the cancel was issued (code " +
                         std::to_string(outcome->code) + ")";
                return false;
            }
            return true;
        }
        if (status.folder_download_active && !cancelled &&
            status.folder_download_current.find(file_name) != std::string::npos) {
            engine.CancelDirectoryDownload();
            cancelled = true;
        }
        if (status.state != chast_viewer::ViewerState::kStreaming) {
            *error = "the session ended during the cancelled walk";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = "the cancelled walk of '" + remote_path + "' did not end";
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
}

bool check_tree(const fs::path& destination, const std::string& tree_name,
                const fs::path& downloads, std::string* error) {
    const fs::path expected = (downloads / tree_name).lexically_normal();
    if (destination.lexically_normal() != expected) {
        *error = "destination '" + utf8(destination) + "' is not '" + utf8(expected) + "'";
        return false;
    }
    struct Expectation {
        const char* relative;
        std::string bytes;
    };
    const std::vector<Expectation> files = {
        {"a.txt", "alpha"},
        {"deep/b.bin", std::string("\x00\x01\x02\x03\xFE\xFF", 6) + "pattern"},
        {"another/c.txt", "charlie"},
    };
    for (const Expectation& file : files) {
        const fs::path path = destination / from_utf8(file.relative);
        std::string bytes;
        if (!read_file(path, &bytes, error)) {
            return false;
        }
        if (bytes != file.bytes) {
            *error = "content mismatch for " + utf8(path);
            return false;
        }
    }
    const std::vector<const char*> empty_directories = {"empty", "deep/nested-empty"};
    for (const char* relative : empty_directories) {
        std::error_code ec;
        if (!fs::is_directory(destination / from_utf8(relative), ec)) {
            *error = "empty directory '" + std::string(relative) + "' was not mirrored";
            return false;
        }
    }
    std::error_code ec;
    if (fs::exists(destination / "outside.txt", ec)) {
        *error = "a file outside the requested folder landed in the download";
        return false;
    }
    return true;
}

bool check_batch(const fs::path& downloads, const ShareSeed& names, std::string* error) {
    const fs::path batch = from_utf8(names.batch_name);
    const fs::path empties = from_utf8(names.empties_name);
    const fs::path loose = from_utf8(names.loose_name);
    struct Expectation {
        fs::path relative;
        std::string bytes;
    };
    const std::vector<Expectation> files = {
        {batch / "one.txt", "batch one"},
        {loose, "loose file"},
    };
    for (const Expectation& file : files) {
        const fs::path path = downloads / file.relative;
        std::string bytes;
        if (!read_file(path, &bytes, error)) {
            return false;
        }
        if (bytes != file.bytes) {
            *error = "content mismatch for " + utf8(path);
            return false;
        }
    }
    const std::vector<fs::path> empty_directories = {batch / "void", empties / "hollow"};
    for (const fs::path& relative : empty_directories) {
        std::error_code ec;
        if (!fs::is_directory(downloads / relative, ec)) {
            *error = "empty directory '" + utf8(relative) + "' was not mirrored";
            return false;
        }
    }
    std::error_code ec;
    if (fs::exists(downloads / batch / loose, ec)) {
        *error = "a loose selected file was nested inside a selected folder";
        return false;
    }
    if (fs::exists(downloads / empties / loose, ec)) {
        *error = "a loose selected file was nested inside a selected folder";
        return false;
    }
    return true;
}

// Seeds a browsed folder on the host plus the local files a drop would hand
// to SendFiles: one loose file and one folder whose subdirectories do not
// exist on the host yet.
bool seed_upload(const fs::path& work, const fs::path& share, std::string* error) {
    std::error_code ec;
    fs::create_directories(share / "incoming", ec);
    if (ec) {
        *error = "cannot create the upload folder: " + ec.message();
        return false;
    }
    const fs::path staging = work / "staging";
    return write_file(staging / "one.txt", "upload one", error) &&
           write_file(staging / "drop" / "inside.txt", "inside the dropped folder", error) &&
           write_file(staging / "drop" / "nested" / "deep.bin",
                      std::string("\x10\x20\x30\xFE\xFF", 5) + "deep", error);
}

// Queues the staged file and folder into the browsed folder, the same way a
// drop on the window reaches SendFiles. Waits until all three files finish.
bool run_upload(chast_viewer::ViewerEngine& engine, const fs::path& staging,
                std::chrono::milliseconds timeout, uint64_t* sent, std::string* error) {
    const chast_viewer::ViewerStatus before = engine.Snapshot();
    if (!engine.SendFiles({utf8(staging / "one.txt"), utf8(staging / "drop")}, "incoming")) {
        *error = "SendFiles was refused";
        return false;
    }
    // one.txt plus inside.txt and nested/deep.bin from the folder.
    const uint64_t expected = before.files_sent + 3;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool saw_work = false;
    for (;;) {
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        if (status.files_sent >= expected) {
            *sent = status.files_sent - before.files_sent;
            return true;
        }
        if (status.file_failures != before.file_failures) {
            *error = "the upload failed: " + status.last_file_error;
            return false;
        }
        if (status.file_phase != chast_viewer::FilePhase::kIdle) {
            saw_work = true;
        }
        if (saw_work && status.file_phase == chast_viewer::FilePhase::kFailed) {
            *error = "the upload failed: " + status.last_file_error;
            return false;
        }
        if (status.state != chast_viewer::ViewerState::kStreaming) {
            *error = "the session ended during the upload";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = "the upload did not finish";
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
}

bool check_upload(const fs::path& share, std::string* error) {
    struct Expectation {
        const char* relative;
        std::string bytes;
    };
    const std::vector<Expectation> files = {
        {"incoming/one.txt", "upload one"},
        {"incoming/drop/inside.txt", "inside the dropped folder"},
        {"incoming/drop/nested/deep.bin", std::string("\x10\x20\x30\xFE\xFF", 5) + "deep"},
    };
    for (const Expectation& file : files) {
        const fs::path path = share / from_utf8(file.relative);
        std::string bytes;
        if (!read_file(path, &bytes, error)) {
            return false;
        }
        if (bytes != file.bytes) {
            *error = "content mismatch for " + utf8(path);
            return false;
        }
    }
    return true;
}

const char* terminal_phase_name(chast_viewer::TerminalPhase phase) {
    switch (phase) {
        case chast_viewer::TerminalPhase::kClosed:
            return "closed";
        case chast_viewer::TerminalPhase::kRequesting:
            return "requesting";
        case chast_viewer::TerminalPhase::kOpen:
            return "open";
        case chast_viewer::TerminalPhase::kEnded:
            return "ended";
        case chast_viewer::TerminalPhase::kFailed:
            return "failed";
    }
    return "unknown";
}

std::string terminal_failure(const chast_viewer::ViewerStatus& status) {
    std::string text = std::string(terminal_phase_name(status.terminal_phase)) + " code " +
                       std::to_string(status.terminal_code);
    const std::string& detail =
        status.terminal_reason.empty() ? status.terminal_error : status.terminal_reason;
    if (!detail.empty()) {
        text += ": " + detail;
    }
    return text;
}

// Drives the session's remote console the way the terminal panel does: open at
// 80x24, type a marker, resize, then close and drain until the host reports the
// console over. The marker proves both directions: the shell echoes the typed
// line and prints it again from echo, so it only appears if input reached the
// host and its output reached the viewer.
bool run_terminal(chast_viewer::ViewerEngine& engine, std::chrono::milliseconds timeout,
                  uint64_t* bytes, std::string* error) {
    const chast_viewer::ViewerStatus before = engine.Snapshot();
    if (!before.terminal_supported || (before.permissions & chast::kPermissionTerminal) == 0) {
        *error = "the host did not grant remote terminals";
        return false;
    }
    if (!engine.OpenTerminal(80, 24)) {
        *error = "OpenTerminal(80x24) was refused";
        return false;
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        if (status.terminal_phase == chast_viewer::TerminalPhase::kOpen) {
            if (status.terminal_columns != 80 || status.terminal_rows != 24) {
                *error = "the terminal opened at " + std::to_string(status.terminal_columns) +
                         "x" + std::to_string(status.terminal_rows);
                return false;
            }
            break;
        }
        if (status.terminal_phase != chast_viewer::TerminalPhase::kRequesting) {
            *error = "the terminal did not open: " + terminal_failure(status);
            return false;
        }
        if (status.state != chast_viewer::ViewerState::kStreaming) {
            *error = "the session ended before the terminal opened";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = "the terminal did not open in time";
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }

    if (!engine.SendTerminalInput("echo chast-probe-terminal\r\n")) {
        *error = "SendTerminalInput was refused";
        return false;
    }
    deadline = std::chrono::steady_clock::now() + timeout;
    std::string transcript;
    bool saw_marker = false;
    for (;;) {
        std::vector<uint8_t> data;
        bool ended = false;
        while (engine.TakeTerminalOutput(data, ended)) {
            transcript.append(reinterpret_cast<const char*>(data.data()), data.size());
        }
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        if (!saw_marker && transcript.find("chast-probe-terminal") != std::string::npos) {
            saw_marker = true;
            if (!engine.ResizeTerminal(100, 30)) {
                *error = "ResizeTerminal(100x30) was refused";
                return false;
            }
        }
        if (saw_marker && status.terminal_columns == 100 && status.terminal_rows == 30) {
            break;
        }
        if (status.terminal_phase == chast_viewer::TerminalPhase::kFailed ||
            status.terminal_phase == chast_viewer::TerminalPhase::kEnded) {
            *error = "the terminal ended before the checks finished: " +
                     terminal_failure(status);
            return false;
        }
        if (status.state != chast_viewer::ViewerState::kStreaming) {
            *error = "the session ended during the terminal checks";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = saw_marker ? "the terminal did not apply the resize"
                                : "the terminal never echoed the marker";
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }

    engine.CloseTerminal();
    deadline = std::chrono::steady_clock::now() + timeout;
    bool ended = false;
    for (;;) {
        std::vector<uint8_t> data;
        while (engine.TakeTerminalOutput(data, ended)) {
            transcript.append(reinterpret_cast<const char*>(data.data()), data.size());
        }
        if (ended) {
            break;
        }
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        if (status.terminal_phase == chast_viewer::TerminalPhase::kFailed) {
            *error = "the terminal failed while closing: " + terminal_failure(status);
            return false;
        }
        if (status.state != chast_viewer::ViewerState::kStreaming) {
            *error = "the session ended while the terminal was closing";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = "the terminal did not finish closing";
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    const chast_viewer::ViewerStatus closed = engine.Snapshot();
    if (closed.terminal_phase != chast_viewer::TerminalPhase::kClosed ||
        closed.terminal_code != 0) {
        *error = "the terminal closed as " + terminal_failure(closed);
        return false;
    }
    *bytes = closed.terminal_bytes;
    return true;
}

bool check_no_files(const fs::path& directory, std::string* error) {
    std::error_code ec;
    if (!fs::exists(directory, ec)) {
        return true;
    }
    for (fs::recursive_directory_iterator it(directory, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            *error = "cannot scan " + utf8(directory) + ": " + ec.message();
            return false;
        }
        if (it->is_regular_file(ec)) {
            *error = "the cancelled walk left " + utf8(it->path()) + " behind";
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    bool files_only = false;
    bool file_transfer_denied = false;
    bool clipboard_scenario = false;
    bool clipboard_upload_scenario = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--files-only") {
            files_only = true;
        } else if (arg == "--files-only-denied") {
            files_only = true;
            file_transfer_denied = true;
        } else if (arg == "--clipboard-files") {
            clipboard_scenario = true;
        } else if (arg == "--clipboard-files-up") {
            clipboard_upload_scenario = true;
        } else {
            std::fprintf(stderr,
                         "usage: chast-viewer-probe [--files-only | --files-only-denied | "
                         "--clipboard-files | --clipboard-files-up]\n");
            return 2;
        }
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const uint64_t suffix =
        (static_cast<uint64_t>(GetCurrentProcessId()) << 20) ^ GetTickCount64();
    const ShareSeed names{"probe-" + std::to_string(suffix) + "-tree",
                          "probe-" + std::to_string(suffix) + "-big",
                          "probe-" + std::to_string(suffix) + "-batch",
                          "probe-" + std::to_string(suffix) + "-empties",
                          "probe-" + std::to_string(suffix) + "-loose.txt"};
    const fs::path work_directory =
        fs::temp_directory_path() / ("chast-viewer-probe-" + std::to_string(suffix));
    const fs::path share = work_directory / "share";
    const std::string downloads_text = chast::default_receive_directory();
    if (downloads_text.empty()) {
        std::fprintf(stderr, "viewer-probe: the viewer has no download directory\n");
        return 1;
    }
    const fs::path downloads = from_utf8(downloads_text);

    int exit_code = 1;
    {
        std::string error;
        if (!seed_share(share, names, &error)) {
            std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
        } else if (!seed_upload(work_directory, share, &error)) {
            std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
        } else if (files_only) {
            // Protocol v32: the same file machinery on a session that never
            // carries a screen, plus the refusal a host without file transfer
            // must issue before the viewer sees a stream.
            const uint16_t port = pick_free_port();
            ProbeSink sink;
            std::unique_ptr<chast::HostServer> host;
            if (port == 0) {
                error = "no free loopback port";
            } else if (!sink.create(&error)) {
                // keep the error
            } else {
                host = start_host(utf8(share), port, &error, !file_transfer_denied);
            }
            std::unique_ptr<chast_viewer::ViewerEngine> engine;
            if (host) {
                engine = std::make_unique<chast_viewer::ViewerEngine>(&sink);
                if (!engine->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {},
                                   kPassword, {}, {}, false,
                                   chast_viewer::ViewerTransport::kAuto, {}, 0, {}, {},
                                   true)) {
                    error = "ViewerEngine::Start was refused";
                    engine.reset();
                } else {
                    // The probe never touches the operator's clipboard.
                    engine->SetClipboardFileSource([](std::vector<std::string>&) {
                        return false;
                    });
                }
            }
            if (engine && file_transfer_denied) {
                chast_viewer::ViewerStatus refusal;
                if (!wait_refusal(*engine, 15s, &refusal, &error)) {
                    // error already set
                } else if (refusal.denial !=
                           static_cast<int>(chast::AccessDenial::Declined)) {
                    error = "the files-only refusal was denial " +
                            std::to_string(refusal.denial) + ", expected Declined";
                } else {
                    std::printf(
                        "viewer-probe: files-only refused by a host without file "
                        "transfer (denial Declined): %s\n",
                        refusal.error.c_str());
                    exit_code = 0;
                }
            } else if (engine && wait_streaming(*engine, 20s, &error)) {
                const chast_viewer::ViewerStatus opened = engine->Snapshot();
                if (!opened.files_only) {
                    error = "the host answered a desktop session to a files-only request";
                } else if (opened.width != 0 || opened.height != 0 ||
                           !opened.displays.empty()) {
                    error = "the files-only answer carried a screen description";
                } else if (opened.received != 0) {
                    error = "the files-only answer carried a picture";
                } else if (!wait_file_permission(*engine, 10s)) {
                    error = "the host never granted file transfer";
                } else {
                    WalkOutcome walk;
                    uint64_t uploaded = 0;
                    if (!run_walk(*engine, names.tree_name, 60s, &walk, &error)) {
                        // error already set
                    } else if (walk.code != 0) {
                        error = "the walk failed with code " +
                                std::to_string(walk.code) + ": " + walk.error;
                    } else if (walk.total != 3 || walk.done != 3) {
                        error = "the walk reported " + std::to_string(walk.done) + "/" +
                                std::to_string(walk.total) + " files, expected 3/3";
                    } else if (!check_tree(from_utf8(walk.destination), names.tree_name,
                                           downloads, &error)) {
                        // error already set
                    } else if (!run_upload(*engine, work_directory / "staging", 60s,
                                           &uploaded, &error)) {
                        // error already set
                    } else if (!check_upload(share, &error)) {
                        // error already set
                    } else {
                        // Media would have to arrive continuously; a moment of
                        // silence proves the session never carried a picture.
                        std::this_thread::sleep_for(300ms);
                        const chast_viewer::ViewerStatus after = engine->Snapshot();
                        if (after.received != 0 || sink.frames() != 0) {
                            error = "the files-only session received " +
                                    std::to_string(after.received) + " frame(s) and decoded " +
                                    std::to_string(sink.frames()) + " picture(s)";
                        } else if (after.width != 0 || after.height != 0) {
                            error = "the files-only session reported a picture size";
                        } else {
                            std::printf(
                                "viewer-probe: files-only session -> %llu files walked, "
                                "%llu uploaded, 0 frames, 0 displays, permissions=0x%04X\n",
                                static_cast<unsigned long long>(walk.done),
                                static_cast<unsigned long long>(uploaded),
                                static_cast<unsigned>(after.permissions));
                            exit_code = 0;
                        }
                    }
                }
            }
            if (exit_code != 0 && !error.empty()) {
                std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
            }
            if (engine) {
                engine->Stop();
            }
            if (host) {
                host->stop();
            }
        } else if (clipboard_scenario) {
            // Protocol v33: files and folders copied on the host clipboard
            // reach the viewer as a FileList, are fetched through the ordinary
            // download channel and are handed to the clipboard in one item.
            // The sink captures what CF_HDROP would have received, so the
            // incoming list never overwrites the operator's clipboard.
            const ClipboardSeed clip{"clip-one.txt", "名字 二.bin", "clip-folder",
                                     "clipboard one", clipboard_binary_bytes(), "inner.txt",
                                     "clipboard inner", "deep.bin", "clipboard deep"};
            const fs::path clipboard_source = work_directory / "clipboard";
            auto ready = std::make_shared<std::atomic<bool>>(false);
            const uint16_t port = pick_free_port();
            ProbeSink sink;
            std::unique_ptr<chast::HostServer> host;
            if (!seed_clipboard(clipboard_source, clip, &error)) {
                // keep the error
            } else if (port == 0) {
                error = "no free loopback port";
            } else if (!sink.create(&error)) {
                // keep the error
            } else {
                host = start_host(
                    utf8(share), port, &error, true,
                    {utf8(clipboard_source / from_utf8(clip.text_name)),
                     utf8(clipboard_source / from_utf8(clip.binary_name)),
                     utf8(clipboard_source / from_utf8(clip.folder_name))},
                    ready);
            }
            ClipboardCapture capture;
            std::unique_ptr<chast_viewer::ViewerEngine> engine;
            if (host) {
                engine = std::make_unique<chast_viewer::ViewerEngine>(&sink);
                if (!engine->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {},
                                   kPassword)) {
                    error = "ViewerEngine::Start was refused";
                    engine.reset();
                } else {
                    engine->SetClipboardFileSink(
                        [&capture](const std::vector<std::string>& files) {
                            return capture.sink(files);
                        });
                    // The probe never touches the operator's clipboard.
                    engine->SetClipboardFileSource([](std::vector<std::string>&) {
                        return false;
                    });
                }
            }
            if (engine && wait_streaming(*engine, 20s, &error)) {
                if (!wait_permission(*engine, chast::kPermissionClipboard, 10s)) {
                    error = "the host never granted the clipboard";
                } else {
                    // The copy happens now, after the session's empty baseline.
                    ready->store(true);
                    if (!capture.wait(30s)) {
                        error = "the clipboard files never reached the viewer";
                    } else {
                        std::string staging;
                        if (!check_clipboard_files(capture.paths, clip, &staging, &error)) {
                            // error already set
                        } else if (!wait_clipboard_received(*engine, 5s)) {
                            error = "the viewer did not count the clipboard item";
                        } else {
                            const chast_viewer::ViewerStatus after = engine->Snapshot();
                            const uint32_t file_list_bit =
                                1u << static_cast<uint32_t>(
                                    chast::ClipboardFormat::FileList);
                            if (after.clipboard_failed != 0) {
                                error = "the clipboard transfer reported " +
                                        std::to_string(after.clipboard_failed) +
                                        " failure(s)";
                            } else if ((after.clipboard_received_formats & file_list_bit) ==
                                       0) {
                                error = "the clipboard item was not reported as a file list";
                            } else if (after.clipboard_received != 1) {
                                error = "the viewer counted " +
                                        std::to_string(after.clipboard_received) +
                                        " clipboard item(s), expected 1";
                            } else {
                                std::printf(
                                    "viewer-probe: clipboard files -> 3 top-level items "
                                    "(4 files) staged in %s, formats=0x%X, received=%llu\n",
                                    staging.c_str(),
                                    static_cast<unsigned>(after.clipboard_received_formats),
                                    static_cast<unsigned long long>(after.clipboard_received));
                                exit_code = 0;
                            }
                            // A real viewer keeps the staging folder alive for
                            // the clipboard to point at; the probe is done.
                            std::error_code ec;
                            fs::remove_all(from_utf8(staging), ec);
                        }
                    }
                }
            }
            if (exit_code != 0 && !error.empty()) {
                std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
            }
            if (engine) {
                engine->Stop();
            }
            if (host) {
                host->stop();
            }
        } else if (clipboard_upload_scenario) {
            // Protocol v34: a copy on the viewer's clipboard is announced as a
            // FileList and uploaded entry by entry; the host stages it below a
            // session temp folder and hands the staged top items to CF_HDROP.
            // Neither real clipboard is touched: the source supplies the
            // viewer's list and the host's write is captured.
            const ClipboardSeed clip{"up-one.txt", "向上 二.bin", "up-folder",
                                     "viewer one", clipboard_binary_bytes(), "inner.txt",
                                     "viewer inner", "deep.bin", "viewer deep"};
            const fs::path clipboard_source = work_directory / "clipboard-up";
            auto ready = std::make_shared<std::atomic<bool>>(false);
            const std::vector<std::string> source_paths = {
                utf8(clipboard_source / from_utf8(clip.text_name)),
                utf8(clipboard_source / from_utf8(clip.binary_name)),
                utf8(clipboard_source / from_utf8(clip.folder_name))};
            const uint16_t port = pick_free_port();
            ProbeSink sink;
            ClipboardCapture host_clipboard;
            std::unique_ptr<chast::HostServer> host;
            if (!seed_clipboard(clipboard_source, clip, &error)) {
                // keep the error
            } else if (port == 0) {
                error = "no free loopback port";
            } else if (!sink.create(&error)) {
                // keep the error
            } else {
                host = start_host(
                    utf8(share), port, &error, true, {}, nullptr,
                    [&host_clipboard](const std::vector<std::string>& files) {
                        return host_clipboard.sink(files);
                    });
            }
            ClipboardCapture viewer_clipboard;
            std::unique_ptr<chast_viewer::ViewerEngine> engine;
            if (host) {
                engine = std::make_unique<chast_viewer::ViewerEngine>(&sink);
                if (!engine->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {},
                                   kPassword)) {
                    error = "ViewerEngine::Start was refused";
                    engine.reset();
                } else {
                    engine->SetClipboardFileSource(
                        [source_paths, ready](std::vector<std::string>& paths) {
                            if (!ready->load()) {
                                return false;
                            }
                            paths = source_paths;
                            return true;
                        });
                    engine->SetClipboardFileSink(
                        [&viewer_clipboard](const std::vector<std::string>& files) {
                            return viewer_clipboard.sink(files);
                        });
                }
            }
            std::string staging;
            if (engine && wait_streaming(*engine, 20s, &error)) {
                if (!wait_permission(*engine, chast::kPermissionClipboard, 10s)) {
                    error = "the host never granted the clipboard";
                } else {
                    // The baseline must be the empty clipboard before `ready`;
                    // give the session loop a poll to record it before the
                    // copy appears.
                    std::this_thread::sleep_for(1000ms);
                    ready->store(true);
                    if (!host_clipboard.wait(30s)) {
                        error = "the viewer's clipboard files never reached the host";
                    } else if (!check_host_clipboard_files(host_clipboard.paths, clip, share,
                                                           &staging, &error)) {
                        // error already set
                    } else {
                        // A few poll cycles prove the staged list is not sent
                        // back to the viewer as a new clipboard item.
                        std::this_thread::sleep_for(1500ms);
                        const chast_viewer::ViewerStatus after = engine->Snapshot();
                        if (viewer_clipboard.wait(0ms)) {
                            error = "the host's staged list was echoed back to the viewer";
                        } else if (after.clipboard_received != 0) {
                            error = "the viewer counted " +
                                    std::to_string(after.clipboard_received) +
                                    " incoming clipboard item(s), expected 0";
                        } else if (after.clipboard_failed != 0) {
                            error = "the clipboard transfer reported " +
                                    std::to_string(after.clipboard_failed) +
                                    " failure(s)";
                        } else {
                            std::printf(
                                "viewer-probe: clipboard files up -> 3 top-level items "
                                "(4 files) staged in %s, no echo, failures=0\n",
                                staging.c_str());
                            exit_code = 0;
                        }
                    }
                }
            }
            if (engine) {
                engine->Stop();
            }
            if (host) {
                host->stop();
            }
            // The staging folder outlives the transfer for CF_HDROP but not
            // the session: nothing may be left in the temp directory.
            if (exit_code == 0 && !staging.empty()) {
                std::error_code ec;
                if (fs::exists(from_utf8(staging), ec)) {
                    std::fprintf(stderr,
                                 "viewer-probe: the host staging folder survived the "
                                 "session: %s\n",
                                 staging.c_str());
                    exit_code = 1;
                }
            }
            if (exit_code != 0 && !error.empty()) {
                std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
            }
        } else {
            const uint16_t port = pick_free_port();
            ProbeSink sink;
            std::unique_ptr<chast::HostServer> host;
            if (port == 0) {
                error = "no free loopback port";
            } else if (!sink.create(&error)) {
                // keep the error
            } else {
                host = start_host(utf8(share), port, &error);
            }

            std::unique_ptr<chast_viewer::ViewerEngine> engine;
            if (host) {
                engine = std::make_unique<chast_viewer::ViewerEngine>(&sink);
                if (!engine->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {},
                                   kPassword)) {
                    error = "ViewerEngine::Start was refused";
                    engine.reset();
                } else {
                    // The probe never touches the operator's clipboard.
                    engine->SetClipboardFileSource([](std::vector<std::string>&) {
                        return false;
                    });
                }
            }

            bool ready = false;
            if (engine && wait_streaming(*engine, 20s, &error)) {
                if (!wait_file_permission(*engine, 10s)) {
                    error = "the host never granted file transfer";
                } else {
                    ready = true;
                }
            }
            if (ready) {
                const chast_viewer::ViewerStatus stream = engine->Snapshot();
                std::printf(
                    "viewer-probe: streaming %ux%u codec=%s permissions=0x%04X\n",
                    static_cast<unsigned>(stream.width),
                    static_cast<unsigned>(stream.height),
                    stream.codec == chast::VideoCodec::Hevc ? "hevc" : "h264",
                    static_cast<unsigned>(stream.permissions));

                WalkOutcome walk;
                if (!run_walk(*engine, names.tree_name, 60s, &walk, &error)) {
                    // error already set
                } else if (walk.code != 0) {
                    error = "the walk failed with code " + std::to_string(walk.code) + ": " +
                            walk.error;
                } else if (walk.total != 3 || walk.done != 3) {
                    error = "the walk reported " + std::to_string(walk.done) + "/" +
                            std::to_string(walk.total) + " files, expected 3/3";
                } else if (engine->Snapshot().folder_download_batch) {
                    error = "the folder walk reported itself as a batch";
                } else if (!check_tree(from_utf8(walk.destination), names.tree_name, downloads,
                                       &error)) {
                    // error already set
                } else {
                    std::printf(
                        "viewer-probe: walk %s -> %s (%llu files, empty directories "
                        "mirrored)\n",
                        names.tree_name.c_str(), walk.destination.c_str(),
                        static_cast<unsigned long long>(walk.done));

                    WalkOutcome batch;
                    if (!run_batch(*engine, names, 60s, &batch, &error)) {
                        // error already set
                    } else if (batch.code != 0) {
                        error = "the batch failed with code " +
                                std::to_string(batch.code) + ": " + batch.error;
                    } else if (batch.total != 2 || batch.done != 2) {
                        error = "the batch reported " + std::to_string(batch.done) + "/" +
                                std::to_string(batch.total) + " files, expected 2/2";
                    } else if (engine->Snapshot().folder_download_roots != 3) {
                        error = "the batch reported " +
                                std::to_string(engine->Snapshot().folder_download_roots) +
                                " roots, expected 3";
                    } else if (!engine->Snapshot().folder_download_batch) {
                        error = "the batch did not mark itself as a batch";
                    } else if (batch.destination != downloads_text) {
                        error = "the batch landed in '" + batch.destination + "' instead of '" +
                                downloads_text + "'";
                    } else if (!check_batch(downloads, names, &error)) {
                        // error already set
                    } else {
                        std::printf(
                            "viewer-probe: batch 3 entries -> receive root (folders "
                            "mirrored, loose file at the top)\n");

                        WalkOutcome cancelled;
                        if (cancel_walk(*engine, names.big_name, "Big.bin", 60s, &cancelled,
                                        &error)) {
                            if (cancelled.code != 2) {
                                error = "the cancelled walk ended with code " +
                                        std::to_string(cancelled.code) + ": " +
                                        cancelled.error;
                            } else if (!check_no_files(from_utf8(cancelled.destination), &error)) {
                                // error already set
                            } else {
                                std::printf(
                                    "viewer-probe: cancel %s -> code 2, no partial file "
                                    "behind\n",
                                    names.big_name.c_str());
                                uint64_t uploaded = 0;
                                if (!run_upload(*engine, work_directory / "staging", 60s,
                                                &uploaded, &error)) {
                                    // error already set
                                } else if (!check_upload(share, &error)) {
                                    // error already set
                                } else {
                                    std::printf(
                                        "viewer-probe: upload %llu files -> incoming "
                                        "(folder expanded, nested directories created)\n",
                                        static_cast<unsigned long long>(uploaded));
                                    uint64_t terminal_bytes = 0;
                                    if (!run_terminal(*engine, 15s, &terminal_bytes, &error)) {
                                        // error already set
                                    } else {
                                        std::printf(
                                            "viewer-probe: terminal 80x24 -> marker echoed, "
                                            "resized to 100x30, closed (%llu bytes)\n",
                                            static_cast<unsigned long long>(terminal_bytes));
                                        std::printf("viewer-probe: %llu decoded pictures\n",
                                                    static_cast<unsigned long long>(
                                                        sink.frames()));
                                        exit_code = 0;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (exit_code != 0 && !error.empty()) {
                std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
            }
            if (engine) {
                engine->Stop();
            }
            if (host) {
                host->stop();
            }
        }
    }

    std::error_code ec;
    fs::remove_all(work_directory, ec);
    fs::remove_all(downloads / names.tree_name, ec);
    fs::remove_all(downloads / names.big_name, ec);
    fs::remove_all(downloads / names.batch_name, ec);
    fs::remove_all(downloads / names.empties_name, ec);
    fs::remove(downloads / names.loose_name, ec);
    CoUninitialize();
    return exit_code;
}
