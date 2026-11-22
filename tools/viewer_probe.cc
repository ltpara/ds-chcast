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
#include <imm.h>
#include <wrl/client.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "chast/clipboard.h"
#include "chast/clipboard_virtual.h"
#include "chast/audio.h"
#include "chast/error.h"
#include "chast/file_transfer.h"
#include "chast/host_server.h"
#include "chast/host_settings.h"
#include "chast/png.h"
#include "chast/protocol.h"
#include "chast/security.h"
#include "viewer_engine.h"
#include "mp4_recorder.h"

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

fs::path probe_scratch_root() {
    std::error_code ec;
    const fs::path current = fs::current_path(ec);
    if (!ec) {
        return current / "test-temp";
    }
    ec.clear();
    return fs::temp_directory_path(ec) / "chast-probes";
}

// A sink that keeps the decoder on the GPU but publishes nothing: the walk
// does not depend on the picture, and a headless run has no window.
class ProbeSink final : public chast_viewer::FrameSink {
public:
    explicit ProbeSink(std::chrono::milliseconds frame_delay = 0ms)
        : frame_delay_(frame_delay) {}

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

    bool OnGpuFrame(ID3D11Texture2D*, uint32_t, uint32_t width, uint32_t height, uint32_t,
                    uint32_t, uint64_t) override {
        if (frame_delay_ > 0ms) {
            std::this_thread::sleep_for(frame_delay_);
        }
        width_ = width;
        height_ = height;
        frames_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // The probe publishes no real texture, so the screenshot path is fed a
    // deterministic pattern at the streamed size. The engine's PNG write,
    // status reporting and file naming are what this run checks; the GPU
    // readback itself belongs to the plugin-backed sink.
    bool CaptureBgra(std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height,
                     std::string* error) override {
        if (width_ == 0 || height_ == 0) {
            if (error != nullptr) {
                *error = "the probe has no picture yet";
            }
            return false;
        }
        width = width_;
        height = height_;
        bgra.resize(static_cast<size_t>(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t* row = bgra.data() + static_cast<size_t>(y) * width * 4;
            for (uint32_t x = 0; x < width; ++x) {
                row[x * 4] = static_cast<uint8_t>(x & 0xFF);
                row[x * 4 + 1] = static_cast<uint8_t>(y & 0xFF);
                row[x * 4 + 2] = static_cast<uint8_t>((x + y) & 0xFF);
                row[x * 4 + 3] = 0;  // the PNG writer must force this opaque
            }
        }
        return true;
    }

    std::string error() const override { return {}; }

    uint64_t frames() const { return frames_.load(std::memory_order_relaxed); }

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    std::chrono::milliseconds frame_delay_;
    std::atomic<uint64_t> frames_{0};
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

struct SyntheticAudioMetrics {
    std::atomic<uint64_t> capture_instances{0};
    std::atomic<uint64_t> encoded_packets{0};
    std::atomic<uint64_t> dropped_packets{0};
};

class SyntheticSilentLoopback final : public chast::ILoopbackCapture {
public:
    explicit SyntheticSilentLoopback(std::shared_ptr<SyntheticAudioMetrics> metrics)
        : metrics_(std::move(metrics)) {
        chast::AudioEncoderConfig config;
        config.codec = chast::AudioCodec::Opus;
        config.sample_rate = 48000;
        config.channels = 2;
        config.bitrate = 64000;
        encoder_ = chast::create_opus_encoder(config, &error_);
        if (!encoder_) {
            failed_.store(true, std::memory_order_relaxed);
            return;
        }

        info_.enabled = true;
        info_.codec = chast::AudioCodec::Opus;
        info_.sample_rate = encoder_->sample_rate();
        info_.channels = encoder_->channels();
        info_.bitrate = encoder_->bitrate();
        info_.frame_samples = encoder_->frame_samples();
        info_.extradata = encoder_->extradata();
        metrics_->capture_instances.fetch_add(1, std::memory_order_relaxed);
    }

    ~SyntheticSilentLoopback() override { stop(); }

    bool ready() const { return encoder_ != nullptr; }

    void start() override {
        if (!ready() || worker_.joinable()) return;
        worker_ = std::jthread([this](std::stop_token stop) { capture(stop); });
    }

    void stop() override {
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
    }

    bool take_packet(chast::EncodedAudio& packet) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (packets_.empty()) return false;
        packet = std::move(packets_.front());
        packets_.pop_front();
        return true;
    }

    bool failed() const override { return failed_.load(std::memory_order_relaxed); }

    std::string error() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

    chast::AudioInfo info() const override { return info_; }

    chast::AudioCaptureStats stats() const override {
        chast::AudioCaptureStats result;
        result.packets = metrics_->encoded_packets.load(std::memory_order_relaxed);
        result.dropped = metrics_->dropped_packets.load(std::memory_order_relaxed);
        result.capture_errors = failed() ? 1 : 0;
        return result;
    }

private:
    void capture(std::stop_token stop) {
        std::vector<int16_t> silence(chast::kOpusFrameSamples * info_.channels, 0);
        auto deadline = std::chrono::steady_clock::now();
        std::vector<chast::EncodedAudio> encoded;
        while (!stop.stop_requested()) {
            const auto capture_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            encoded.clear();
            chast::Error result = encoder_->encode(silence.data(), chast::kOpusFrameSamples,
                                                   capture_ms, encoded);
            if (!result.ok()) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    error_ = result.to_string();
                }
                failed_.store(true, std::memory_order_relaxed);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (chast::EncodedAudio& packet : encoded) {
                    if (packets_.size() >= kQueueLimit) {
                        packets_.pop_front();
                        metrics_->dropped_packets.fetch_add(1,
                                                            std::memory_order_relaxed);
                    }
                    packets_.push_back(std::move(packet));
                    metrics_->encoded_packets.fetch_add(1, std::memory_order_relaxed);
                }
            }
            deadline += 10ms;
            std::this_thread::sleep_until(deadline);
            if (deadline + 50ms < std::chrono::steady_clock::now()) {
                deadline = std::chrono::steady_clock::now();
            }
        }
    }

    static constexpr size_t kQueueLimit = 256;
    std::shared_ptr<SyntheticAudioMetrics> metrics_;
    std::unique_ptr<chast::IAudioEncoder> encoder_;
    chast::AudioInfo info_;
    mutable std::mutex mutex_;
    std::deque<chast::EncodedAudio> packets_;
    std::string error_;
    std::atomic<bool> failed_{false};
    std::jthread worker_;
};

struct MicrophoneMetrics {
    void push(const chast::DecodedAudio& audio) {
        std::lock_guard<std::mutex> lock(mutex);
        ++packets;
        frames += audio.samples.size() / audio.channels;
        for (const int16_t sample : audio.samples) {
            const int32_t wide = sample;
            sum_squares += static_cast<uint64_t>(wide * wide);
            peak_value = (std::max)(peak_value, static_cast<uint32_t>(std::abs(wide)));
            ++samples;
        }
    }

    chast::AudioPlayerStats stats() const {
        std::lock_guard<std::mutex> lock(mutex);
        chast::AudioPlayerStats result;
        result.packets = packets;
        result.played_frames = frames;
        return result;
    }

    double rms() const {
        std::lock_guard<std::mutex> lock(mutex);
        return samples == 0 ? 0.0 : std::sqrt(static_cast<double>(sum_squares) / samples);
    }

    uint32_t peak() const {
        std::lock_guard<std::mutex> lock(mutex);
        return peak_value;
    }

    mutable std::mutex mutex;
    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t samples = 0;
    uint64_t sum_squares = 0;
    uint32_t peak_value = 0;
};

struct ViewerPlaybackMetrics {
    std::atomic<bool> muted{false};
    std::atomic<uint64_t> pushed_blocks{0};
    std::atomic<uint64_t> muted_blocks{0};
    std::atomic<uint64_t> unmuted_blocks{0};
    std::atomic<uint64_t> unmuted_frames{0};
};

class ViewerMetricsPlayer final : public chast::IAudioPlayer {
public:
    explicit ViewerMetricsPlayer(std::shared_ptr<ViewerPlaybackMetrics> metrics)
        : metrics_(std::move(metrics)) {}

    chast::Error start() override {
        running_.store(true, std::memory_order_relaxed);
        return chast::Error::success();
    }

    void stop() override { running_.store(false, std::memory_order_relaxed); }

    chast::Error push(const chast::DecodedAudio& audio) override {
        if (!running_.load(std::memory_order_relaxed) || audio.channels == 0 ||
            audio.samples.size() % audio.channels != 0) {
            return chast::Error(chast::Status::InvalidCall,
                                "invalid synthetic viewer playback block");
        }
        metrics_->pushed_blocks.fetch_add(1, std::memory_order_relaxed);
        if (muted()) {
            metrics_->muted_blocks.fetch_add(1, std::memory_order_relaxed);
        } else {
            metrics_->unmuted_blocks.fetch_add(1, std::memory_order_relaxed);
            metrics_->unmuted_frames.fetch_add(audio.samples.size() / audio.channels,
                                                std::memory_order_relaxed);
        }
        return chast::Error::success();
    }

    void set_clock_offset(int64_t) override {}
    void set_lead_ms(uint32_t) override {}
    void set_mute(bool mute) override {
        metrics_->muted.store(mute, std::memory_order_relaxed);
    }
    bool muted() const override {
        return metrics_->muted.load(std::memory_order_relaxed);
    }

    chast::AudioPlayerStats stats() const override {
        chast::AudioPlayerStats result;
        result.packets = metrics_->pushed_blocks.load(std::memory_order_relaxed);
        result.played_frames = metrics_->unmuted_frames.load(std::memory_order_relaxed);
        return result;
    }

    std::string error() const override { return {}; }

private:
    std::shared_ptr<ViewerPlaybackMetrics> metrics_;
    std::atomic<bool> running_{false};
};

// Receives the viewer's microphone uplink without playing or retaining PCM.
class MicrophoneMetricsPlayer final : public chast::IAudioPlayer {
public:
    explicit MicrophoneMetricsPlayer(std::shared_ptr<MicrophoneMetrics> metrics)
        : metrics_(std::move(metrics)) {}

    chast::Error start() override { return chast::Error::success(); }
    void stop() override {}

    chast::Error push(const chast::DecodedAudio& audio) override {
        if (audio.channels == 0 || audio.samples.size() % audio.channels != 0) {
            return chast::Error(chast::Status::InvalidCall,
                                "invalid decoded microphone block");
        }
        metrics_->push(audio);
        return chast::Error::success();
    }

    void set_clock_offset(int64_t) override {}
    void set_lead_ms(uint32_t) override {}
    void set_mute(bool mute) override { muted_ = mute; }
    bool muted() const override { return muted_; }
    chast::AudioPlayerStats stats() const override { return metrics_->stats(); }
    std::string error() const override { return {}; }

private:
    std::shared_ptr<MicrophoneMetrics> metrics_;
    std::atomic<bool> muted_{false};
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

bool prepare_receive_directory(const fs::path& directory, std::string* error) {
    const fs::path probe =
        directory / (".chast-probe-" + std::to_string(GetCurrentProcessId()));
    if (!write_file(probe, {}, error)) {
        return false;
    }
    std::error_code ec;
    fs::remove(probe, ec);
    if (ec) {
        *error = "cannot clean the receive probe in " + utf8(directory) + ": " +
                 ec.message();
        return false;
    }
    return true;
}

std::unique_ptr<chast_viewer::ViewerEngine> make_probe_viewer(
    ProbeSink* sink, const fs::path& receive_directory, std::string* error) {
    auto viewer = std::make_unique<chast_viewer::ViewerEngine>(sink);
    if (!viewer->SetFileReceiveDirectoryForTesting(utf8(receive_directory))) {
        *error = "the probe receive directory is not an absolute UTF-8 path";
        return nullptr;
    }
    return viewer;
}

struct ShareSeed {
    std::string tree_name;
    std::string big_name;
    std::string batch_name;
    std::string empties_name;
    std::string loose_name;
    // A large file at the share root whose name is unique per run: a
    // single-file download lands in the receive root, so a shared name such as
    // "Big.bin" would risk touching the operator's own files.
    std::string cancel_name;
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
    const fs::path cancelled = share / names.cancel_name;
    if (!write_file(cancelled, {}, error)) {
        return false;
    }
    fs::resize_file(cancelled, kBigFileBytes, ec);
    if (ec) {
        *error = "cannot create " + utf8(cancelled) + ": " + ec.message();
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
    std::shared_ptr<chast::ClipboardVirtualFiles> virtual_files;

    bool virtual_sink(std::shared_ptr<chast::ClipboardVirtualFiles> files) {
        { std::lock_guard lock(mutex); virtual_files = std::move(files); landed = true; }
        cv.notify_all(); return true;
    }

    bool paste(const fs::path& destination, std::string* error) {
        std::shared_ptr<chast::ClipboardVirtualFiles> files;
        { std::lock_guard lock(mutex); files = virtual_files; }
        auto fail = [&](const char* why) { *error = why; return false; };
        if (!files || !files->Active() || fs::exists(destination)) return fail("virtual clipboard was not published before paste");
        // Inspect the actual Shell object without replacing the user's clipboard.
        ComPtr<IDataObject> object;
        object.Attach(chast::make_virtual_file_data_object(files, error));
        if (!object) return false;
        FORMATETC format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW")),
                         nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if (FAILED(object->GetData(&format, &medium))) return fail("virtual file descriptors unavailable");
        const auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
        if (!group) { ReleaseStgMedium(&medium); return fail("cannot read descriptors"); }
        std::vector<FILEDESCRIPTORW> descriptors(group->fgd, group->fgd + group->cItems);
        GlobalUnlock(medium.hGlobal); ReleaseStgMedium(&medium);
        std::this_thread::sleep_for(350ms);
        if (!paths.empty() || fs::exists(destination)) return fail("file content appeared before paste");
        std::printf("viewer-probe: %zu Shell descriptors ready; paste begins now\n", descriptors.size());
        fs::create_directories(destination);
        std::set<std::string> tops;
        for (size_t i = 0; i < descriptors.size(); ++i) {
            const auto& descriptor = descriptors[i];
            const fs::path relative(descriptor.cFileName);
            const auto target = destination / relative;
            tops.insert(utf8(destination / *relative.begin()));
            if (descriptor.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                fs::create_directories(target); continue;
            }
            format = {static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents")), nullptr,
                      DVASPECT_CONTENT, static_cast<LONG>(i), TYMED_ISTREAM};
            if (FAILED(object->GetData(&format, &medium))) return fail("virtual file stream unavailable");
            ComPtr<IStream> stream;
            stream.Attach(medium.pstm); medium = {};
            STATSTG stat{};
            const uint64_t size = (static_cast<uint64_t>(descriptor.nFileSizeHigh) << 32) | descriptor.nFileSizeLow;
            if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart != size)
                return fail("virtual stream size disagrees with descriptor");
            if (size) {
                LARGE_INTEGER end{}; end.QuadPart = -1;
                ComPtr<IStream> clone;
                uint8_t tail = 0, copied = 0;
                ULONG got = 0;
                if (FAILED(stream->Seek(end, STREAM_SEEK_END, nullptr)) || FAILED(stream->Clone(&clone)) ||
                    stream->Read(&tail, 1, &got) != S_OK || got != 1 ||
                    clone->Read(&copied, 1, &got) != S_OK || got != 1 || tail != copied)
                    return fail("random access or cloned virtual stream failed");
                LARGE_INTEGER begin{};
                if (FAILED(stream->Seek(begin, STREAM_SEEK_SET, nullptr))) return fail("virtual stream could not rewind");
            }
            fs::create_directories(target.parent_path());
            std::ofstream output(target, std::ios::binary);
            std::vector<uint8_t> buffer(chast::kMaxClipboardFileRead);
            uint64_t done = 0;
            while (done < size) {
                const ULONG want = static_cast<ULONG>(std::min<uint64_t>(buffer.size(), size - done));
                ULONG got = 0;
                if (stream->Read(buffer.data(), want, &got) != S_OK || got != want) return fail("paste-time read failed");
                output.write(reinterpret_cast<const char*>(buffer.data()), got); done += got;
            }
            ULONG got = 1;
            if (stream->Read(buffer.data(), 1, &got) != S_FALSE || got != 0) return fail("virtual stream EOF is incorrect");
            if (!output) return fail("could not write pasted file");
        }
        paths.assign(tops.begin(), tops.end());
        return true;
    }

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
    std::string empty_name = "独立空目录";
    bool directories_only = false;
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
    fs::create_directories(directory / from_utf8(seed.empty_name), ec);
    if (ec) { *error = ec.message(); return false; }
    fs::create_directories(directory / from_utf8(seed.folder_name) / "a-empty" / "leaf", ec);
    if (ec) { *error = ec.message(); return false; }
    fs::create_directories(directory / from_utf8(seed.folder_name) / "z-empty", ec);
    if (ec) { *error = ec.message(); return false; }
    if (seed.directories_only) return true;
    return write_file(directory / from_utf8(seed.folder_name) / "zero.bin", "", error) &&
           write_file(directory / from_utf8(seed.text_name), seed.text_bytes, error) &&
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

std::vector<std::string> clipboard_sources(const fs::path& root, const ClipboardSeed& seed) {
    std::vector<std::string> paths{utf8(root / from_utf8(seed.folder_name)), utf8(root / from_utf8(seed.empty_name))};
    if (!seed.directories_only) {
        paths.push_back(utf8(root / from_utf8(seed.text_name)));
        paths.push_back(utf8(root / from_utf8(seed.binary_name)));
    }
    return paths;
}

bool check_clipboard_identity(const std::vector<std::string>& original,
                              const std::vector<std::string>& staged, std::string* error) {
    std::vector<chast::ClipboardFileEntry> before, after;
    std::vector<std::string> paths;
    if (!chast::collect_clipboard_files(original, before, paths, error) ||
        !chast::collect_clipboard_files(staged, after, paths, error)) return false;
    if (chast::clipboard_file_identity_hash(before) != chast::clipboard_file_identity_hash(after)) {
        *error = "staged clipboard tree changed its echo-suppression identity";
        return false;
    }
    return true;
}

bool check_clipboard_directories(const fs::path& folder, const fs::path& empty,
                                  const ClipboardSeed& seed, std::string* error) {
    std::error_code ec;
    for (const auto& path : {empty, folder / "a-empty" / "leaf", folder / "z-empty"}) {
        if (!fs::is_directory(path, ec) || ec || !fs::is_empty(path, ec) || ec) {
            *error = "empty clipboard directory was lost or populated: " + utf8(path);
            return false;
        }
    }
    if (seed.directories_only) {
        size_t files = 0;
        for (const auto& entry : fs::recursive_directory_iterator(folder))
            if (entry.is_regular_file()) ++files;
        if (files != 0) { *error = "directory-only clipboard created file data"; return false; }
        return true;
    }
    return check_clipboard_bytes(folder / "zero.bin", "", error);
}

// The viewer's clipboard must hold the two flat files and the copied folder,
// all inside one staging folder, with the folder tree rebuilt and intact.
bool check_clipboard_files(const std::vector<std::string>& paths,
                           const ClipboardSeed& seed, std::string* staging,
                           std::string* error) {
    const size_t expected = seed.directories_only ? 2 : 4;
    if (paths.size() != expected) {
        *error = "the clipboard received " + std::to_string(paths.size()) +
                 " top-level item(s), expected " + std::to_string(expected);
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
    fs::path empty;
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
        } else if (name == seed.empty_name) {
            empty = path;
        } else if (name == seed.folder_name) {
            folder = path;
        } else {
            *error = "the clipboard received an unexpected item '" + name + "'";
            return false;
        }
    }
    if (empty.empty() || folder.empty() || (!seed.directories_only && (text.empty() || binary.empty()))) {
        *error = "the clipboard lost a seeded top-level item";
        return false;
    }
    std::error_code ec;
    size_t top_level = 0;
    for (fs::directory_iterator it(first.parent_path(), ec), end; it != end; it.increment(ec)) {
        // The receiver keeps a reserved directory for resumable partial
        // downloads next to the delivered items; it is not a clipboard item.
        if (utf8(it->path().filename()) == ".chast-partial") {
            continue;
        }
        ++top_level;
    }
    if (ec || top_level != expected) {
        *error = "the staging folder holds " + std::to_string(top_level) +
                 " top-level item(s), expected " + std::to_string(expected);
        return false;
    }
    if (!fs::is_directory(folder, ec) || ec) {
        *error = "the copied folder did not arrive as a directory";
        return false;
    }
    if (!check_clipboard_directories(folder, empty, seed, error)) return false;
    if (seed.directories_only) return true;
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
    const size_t expected = seed.directories_only ? 2 : 4;
    if (paths.size() != expected) {
        *error = "the host clipboard received " + std::to_string(paths.size()) +
                 " top-level item(s), expected " + std::to_string(expected);
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
    fs::path empty;
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
        } else if (item_name == seed.empty_name) {
            empty = path;
        } else if (item_name == seed.folder_name) {
            folder = path;
        } else {
            *error = "the host clipboard received an unexpected item '" + item_name + "'";
            return false;
        }
    }
    if (empty.empty() || folder.empty() || (!seed.directories_only && (text.empty() || binary.empty()))) {
        *error = "the host clipboard lost a copied top-level item";
        return false;
    }
    size_t top_level = 0;
    for (fs::directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (utf8(it->path().filename()) == ".chast-partial") {
            continue;
        }
        ++top_level;
    }
    if (ec || top_level != expected) {
        *error = "the host staging folder holds " + std::to_string(top_level) +
                 " top-level item(s), expected " + std::to_string(expected);
        return false;
    }
    if (!seed.directories_only && (!fs::is_regular_file(text, ec) || ec)) {
        *error = "the flat file did not arrive as a regular file";
        return false;
    }
    if (!fs::is_directory(folder, ec) || ec) {
        *error = "the copied folder did not arrive as a directory";
        return false;
    }
    if (!check_clipboard_directories(folder, empty, seed, error)) return false;
    if (seed.directories_only) return true;
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
    std::function<bool(const std::vector<std::string>&)> clipboard_write_files = {},
    chast::ClipboardDirection clipboard_direction = chast::ClipboardDirection::Both,
    std::function<bool(std::shared_ptr<chast::ClipboardVirtualFiles>)> virtual_sink = {},
    const std::string& record_directory = {},
    std::function<void(chast::HostServerConfig&)> configure = {}) {
    chast::HostServerConfig config;
    config.settings.bind_address = "127.0.0.1";
    config.settings.port = port;
    config.settings.carrier = false;
    config.settings.auth_mode = chast::HostAuthMode::Password;
    config.settings.password_hash = chast::hash_password(kPassword);
    config.settings.allow_file_transfer = allow_file_transfer;
    config.settings.clipboard_direction = clipboard_direction;
    config.settings.allow_terminal = true;
    config.settings.file_receive_directory = share_directory;
    config.serve_once = true;
    config.keyframe_interval_ms = 500;
    if (!record_directory.empty()) {
        config.serve_once = false;
        config.settings.record_incoming = true;
        config.settings.record_directory = record_directory;
        config.settings.allow_record = false; // Independent of viewer recording consent.
        config.settings.allow_clipboard = false;
        config.settings.allow_audio = false;
        config.settings.allow_input = false;
        config.settings.allow_terminal = false;
        config.settings.max_viewers = 2;
        config.settings.codec = chast::VideoCodec::Vp9;
        config.encoder_backend = chast::EncoderBackend::Software;
    }
    // Loopback acceptance must not read or write the operator's clipboard.
    config.clipboard_read = [](chast::ClipboardContent&) { return true; };
    config.clipboard_write = [](const chast::ClipboardContent&) { return true; };
    // A viewer's clipboard list must never reach the real CF_HDROP here; the
    // scenario captures it instead, and everything else acknowledges silently.
    config.clipboard_write_files = std::move(clipboard_write_files);
    config.clipboard_write_virtual_files = std::move(virtual_sink);
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
    if (configure) configure(config);

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

bool run_process(const std::wstring& command, DWORD* exit_code, std::string* error) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        *error = "CreateProcessW failed: " +
                 std::to_string(static_cast<unsigned long>(GetLastError()));
        return false;
    }
    WaitForSingleObject(process.hProcess, 10000);
    DWORD code = ERROR_TIMEOUT;
    if (GetExitCodeProcess(process.hProcess, &code) && code == STILL_ACTIVE) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, INFINITE);
        code = ERROR_TIMEOUT;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    *exit_code = code;
    return code != ERROR_TIMEOUT;
}

// A junction is the Windows reparse point that often looks like an ordinary
// directory to std::filesystem. Prefer it for the acceptance test and fall
// back to a symlink when the volume cannot host a mount point.
bool create_directory_reparse(const fs::path& link, const fs::path& target,
                              std::string* error) {
    std::wstring command = L"cmd.exe /d /s /c mklink /J \"" + link.wstring() +
                           L"\" \"" + target.wstring() + L"\"";
    DWORD exit_code = 0;
    std::string process_error;
    if (run_process(command, &exit_code, &process_error) && exit_code == 0) {
        return true;
    }
    constexpr DWORD kAllowUnprivilegedCreate = 0x2;
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(),
                            SYMBOLIC_LINK_FLAG_DIRECTORY |
                                kAllowUnprivilegedCreate)) {
        return true;
    }
    *error = "cannot create a directory reparse point (junction exit " +
             std::to_string(static_cast<unsigned long>(exit_code)) + ", link error " +
             std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
    if (!process_error.empty()) *error += ": " + process_error;
    return false;
}

void remove_directory_reparse(const fs::path& path) {
    std::error_code ignored;
    fs::remove(path, ignored);
}

bool wait_upload_settled(chast_viewer::ViewerEngine& engine, uint64_t expected_sent,
                         uint64_t expected_failures,
                         const std::vector<fs::path>& expected_directories,
                         std::chrono::milliseconds timeout, std::string* error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::optional<std::chrono::steady_clock::time_point> settled_since;
    for (;;) {
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        std::error_code ec;
        bool directories_ready = true;
        for (const fs::path& directory : expected_directories) {
            if (!fs::is_directory(directory, ec) || ec) {
                directories_ready = false;
                ec.clear();
                break;
            }
        }
        if (status.files_sent == expected_sent &&
            status.file_failures == expected_failures && status.file_queue == 0 &&
            directories_ready) {
            // A second poll catches the short window where the file worker has
            // not yet published a completed directory into the queue.
            if (!settled_since) settled_since = std::chrono::steady_clock::now();
            if (std::chrono::steady_clock::now() - *settled_since >= 50ms) return true;
        } else {
            settled_since.reset();
        }
        if (status.state != chast_viewer::ViewerState::kStreaming) {
            *error = "the session ended during the directory upload";
            return false;
        }
        if (status.files_sent > expected_sent || status.file_failures > expected_failures) {
            *error = "the directory upload counted " + std::to_string(status.files_sent) +
                     " files sent and " + std::to_string(status.file_failures) +
                     " failures, expected " + std::to_string(expected_sent) + " and " +
                     std::to_string(expected_failures) + " (" + status.last_file_error + ")";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = "the directory upload did not settle: queue=" +
                     std::to_string(status.file_queue) + ", files=" +
                     std::to_string(status.files_sent) + ", failures=" +
                     std::to_string(status.file_failures) + " (" + status.file_error + ")";
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
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

// Queues one large upload and cancels it while it is still hashing, before any
// byte can reach the host. The cancel must count apart from failures and must
// not be reported as a sent file.
bool run_upload_cancel(chast_viewer::ViewerEngine& engine, const fs::path& work,
                       std::chrono::milliseconds timeout, std::string* error) {
    const fs::path big = work / "cancel-me.bin";
    std::string bytes(64u * 1024u * 1024u, '\x5A');
    if (!write_file(big, bytes, error)) {
        return false;
    }
    const chast_viewer::ViewerStatus before = engine.Snapshot();
    if (!engine.SendFiles({utf8(big)}, "incoming")) {
        *error = "SendFiles was refused";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool hashing = false;
    bool cancelled = false;
    for (;;) {
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        if (status.file_failures != before.file_failures) {
            *error = "the cancel was reported as a failure: " + status.last_file_error;
            return false;
        }
        if (status.files_sent != before.files_sent) {
            *error = "the upload finished before it could be cancelled";
            return false;
        }
        if (!cancelled && status.file_phase == chast_viewer::FilePhase::kHashing) {
            hashing = true;
            engine.CancelUploads();
            cancelled = true;
        }
        if (cancelled && status.file_cancels > before.file_cancels) {
            if (!hashing) {
                *error = "the cancel was counted without an upload";
                return false;
            }
            return true;
        }
        if (status.state != chast_viewer::ViewerState::kStreaming) {
            *error = "the session ended during the cancel";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = "the cancel was not reported";
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
}

// A cancelled upload may leave a partial file, but it must never reach the
// host in full.
bool check_upload_cancelled(const fs::path& share, std::string* error) {
    std::error_code ec;
    const fs::path path = share / "incoming" / "cancel-me.bin";
    if (!fs::exists(path, ec)) {
        return true;
    }
    const uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        *error = "cannot size " + utf8(path) + ": " + ec.message();
        return false;
    }
    if (size >= 64u * 1024u * 1024u) {
        *error = "the cancelled upload reached the host in full";
        return false;
    }
    return true;
}

// Requests the unique large file and cancels while bytes are still arriving.
// A viewer cancel is not a receive failure, must not count as a received file,
// and must delete the partial file instead of keeping it for a resume.
bool run_download_cancel(chast_viewer::ViewerEngine& engine, chast::HostServer& host,
                         const std::string& path, std::chrono::milliseconds timeout,
                         std::string* error) {
    const chast_viewer::ViewerStatus before = engine.Snapshot();
    const chast::HostServerStats host_before = host.status().stats;
    if (!engine.RequestFileDownload(path)) {
        *error = "RequestFileDownload(" + path + ") was refused";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool started = false;
    bool cancelled = false;
    for (;;) {
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        if (status.file_receive_failures != before.file_receive_failures) {
            *error = "the download failed before it could be cancelled: " +
                     status.download_error;
            return false;
        }
        if (status.files_received != before.files_received) {
            *error = "the download finished before it could be cancelled";
            return false;
        }
        if (!started && status.file_download_answers != before.file_download_answers &&
            status.file_download_code != 0) {
            *error = "the download request was answered with code " +
                     std::to_string(status.file_download_code) + ": " +
                     status.file_download_error;
            return false;
        }
        if (!cancelled && status.download_phase == chast_viewer::FilePhase::kSending &&
            status.download_received >= 1024u * 1024u) {
            if (status.download_size != kBigFileBytes) {
                *error = "the download offered " + std::to_string(status.download_size) +
                         " bytes, expected " + std::to_string(kBigFileBytes);
                return false;
            }
            if (!status.download_cancellable) {
                *error = "the single-file download was not marked cancellable";
                return false;
            }
            started = true;
            engine.CancelDownload();
            cancelled = true;
        }
        if (cancelled && status.download_cancels > before.download_cancels) {
            if (!started) {
                *error = "the cancel was counted without a download";
                return false;
            }
            if (status.download_phase != chast_viewer::FilePhase::kFailed) {
                *error = "the cancelled download ended in phase " +
                         std::to_string(static_cast<int>(status.download_phase));
                return false;
            }
            if (status.download_error.find("cancelled") == std::string::npos) {
                *error = "the cancelled download reported '" + status.download_error + "'";
                return false;
            }
            if (!status.download_path.empty()) {
                *error = "the cancelled download kept a final path";
                return false;
            }
            // The host drains the viewer's FileResult asynchronously and must
            // book it as a cancel, not as a send failure.
            for (;;) {
                const chast::HostServerStats now = host.status().stats;
                if (now.file_send_cancels > host_before.file_send_cancels) {
                    if (now.file_send_cancels != host_before.file_send_cancels + 1) {
                        *error = "the host counted " +
                                 std::to_string(now.file_send_cancels -
                                                host_before.file_send_cancels) +
                                 " download cancels, expected one";
                        return false;
                    }
                    if (now.file_send_failures != host_before.file_send_failures) {
                        *error = "the viewer's cancel was booked as a host send failure";
                        return false;
                    }
                    return true;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    *error = "the host did not count the viewer's download cancel";
                    return false;
                }
                std::this_thread::sleep_for(1ms);
            }
        }
        if (status.state != chast_viewer::ViewerState::kStreaming) {
            *error = "the session ended during the download cancel";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = "the download cancel was not reported";
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
}

// A cancelled download deletes its partial file, so neither the final name nor
// the .part pieces may remain in the receive root.
bool check_download_cancelled(const fs::path& downloads, const std::string& name,
                              std::string* error) {
    std::error_code ec;
    const std::vector<std::string> leftovers = {name, name + ".part",
                                                name + ".part.chast"};
    for (const std::string& leftover : leftovers) {
        const fs::path path = downloads / from_utf8(leftover);
        if (fs::exists(path, ec)) {
            *error = "the cancelled download left " + utf8(path) + " behind";
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

std::string terminal_failure(const chast_viewer::ViewerTerminalStatus& status) {
    std::string text = std::string(terminal_phase_name(status.phase)) + " code " +
                       std::to_string(status.code);
    const std::string& detail = status.reason.empty() ? status.error : status.reason;
    if (!detail.empty()) {
        text += ": " + detail;
    }
    return text;
}

std::string terminal_failure(const chast_viewer::ViewerStatus& status) {
    chast_viewer::ViewerTerminalStatus first;
    first.phase = status.terminal_phase;
    first.code = status.terminal_code;
    first.reason = status.terminal_reason;
    first.error = status.terminal_error;
    return terminal_failure(first);
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

    // A second console opens beside the first. The host keeps both shells
    // alive and each slot must be able to echo, resize and close on its own
    // while the other stays untouched; chunk merges that serialize the slots
    // or mix their output queues get caught here.
    if (!engine.OpenTerminal(90, 28, 2)) {
        *error = "OpenTerminal(90x28, slot 2) was refused";
        return false;
    }
    deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        const chast_viewer::ViewerTerminalStatus& first = status.terminals[0];
        const chast_viewer::ViewerTerminalStatus& second = status.terminals[1];
        if (first.phase != chast_viewer::TerminalPhase::kOpen || first.columns != 100 ||
            first.rows != 30) {
            *error = "the first terminal changed while the second opened: " +
                     terminal_failure(first);
            return false;
        }
        if (second.phase == chast_viewer::TerminalPhase::kOpen) {
            if (second.columns != 90 || second.rows != 28) {
                *error = "the second terminal opened at " +
                         std::to_string(second.columns) + "x" +
                         std::to_string(second.rows);
                return false;
            }
            break;
        }
        if (second.phase != chast_viewer::TerminalPhase::kRequesting) {
            *error = "the second terminal did not open: " + terminal_failure(second);
            return false;
        }
        if (status.state != chast_viewer::ViewerState::kStreaming) {
            *error = "the session ended before the second terminal opened";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = "the second terminal did not open in time";
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }

    if (!engine.SendTerminalInput("echo chast-probe-second\r\n", 2)) {
        *error = "SendTerminalInput(slot 2) was refused";
        return false;
    }
    deadline = std::chrono::steady_clock::now() + timeout;
    std::string second_transcript;
    bool saw_second_marker = false;
    for (;;) {
        std::vector<uint8_t> data;
        bool second_ended = false;
        while (engine.TakeTerminalOutput(data, second_ended, 2)) {
            second_transcript.append(reinterpret_cast<const char*>(data.data()),
                                     data.size());
        }
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        const chast_viewer::ViewerTerminalStatus& first = status.terminals[0];
        const chast_viewer::ViewerTerminalStatus& second = status.terminals[1];
        if (!saw_second_marker &&
            second_transcript.find("chast-probe-second") != std::string::npos) {
            saw_second_marker = true;
            if (!engine.ResizeTerminal(110, 32, 2)) {
                *error = "ResizeTerminal(110x32, slot 2) was refused";
                return false;
            }
        }
        if (first.phase != chast_viewer::TerminalPhase::kOpen || first.columns != 100 ||
            first.rows != 30) {
            *error = "the first terminal changed during the second console's checks: " +
                     terminal_failure(first);
            return false;
        }
        if (saw_second_marker && second.columns == 110 && second.rows == 32) {
            break;
        }
        if (second.phase == chast_viewer::TerminalPhase::kFailed ||
            second.phase == chast_viewer::TerminalPhase::kEnded) {
            *error = "the second terminal ended before the checks finished: " +
                     terminal_failure(second);
            return false;
        }
        if (status.state != chast_viewer::ViewerState::kStreaming) {
            *error = "the session ended during the second terminal's checks";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = saw_second_marker
                         ? "the second terminal did not apply the resize"
                         : "the second terminal never echoed the marker";
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }

    engine.CloseTerminal(2);
    deadline = std::chrono::steady_clock::now() + timeout;
    bool second_ended = false;
    for (;;) {
        std::vector<uint8_t> data;
        while (engine.TakeTerminalOutput(data, second_ended, 2)) {
            second_transcript.append(reinterpret_cast<const char*>(data.data()),
                                     data.size());
        }
        if (second_ended) {
            break;
        }
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        if (status.terminals[1].phase == chast_viewer::TerminalPhase::kFailed) {
            *error = "the second terminal failed while closing: " +
                     terminal_failure(status.terminals[1]);
            return false;
        }
        if (status.state != chast_viewer::ViewerState::kStreaming) {
            *error = "the session ended while the second terminal was closing";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = "the second terminal did not finish closing";
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    {
        const chast_viewer::ViewerStatus status = engine.Snapshot();
        if (status.terminals[1].phase != chast_viewer::TerminalPhase::kClosed ||
            status.terminals[1].code != 0) {
            *error = "the second terminal closed as " +
                     terminal_failure(status.terminals[1]);
            return false;
        }
        if (status.terminals[0].phase != chast_viewer::TerminalPhase::kOpen ||
            status.terminals[0].columns != 100 || status.terminals[0].rows != 30) {
            *error = "the first terminal changed when the second closed: " +
                     terminal_failure(status.terminals[0]);
            return false;
        }
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
    *bytes = closed.terminals[0].bytes + closed.terminals[1].bytes;
    return true;
}

// Parses the signature and the IHDR every PNG begins with, so a screenshot is
// checked for being a real picture of the streamed size and not just a file
// with a plausible name.
bool read_png_header(const fs::path& path, uint32_t* width, uint32_t* height,
                     std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        *error = "cannot open " + utf8(path);
        return false;
    }
    unsigned char header[24] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (file.gcount() != static_cast<std::streamsize>(sizeof(header))) {
        *error = utf8(path) + " is too short to be a PNG";
        return false;
    }
    static const unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (std::memcmp(header, kSignature, sizeof(kSignature)) != 0) {
        *error = utf8(path) + " does not start with the PNG signature";
        return false;
    }
    if (header[12] != 'I' || header[13] != 'H' || header[14] != 'D' || header[15] != 'R') {
        *error = utf8(path) + " has no IHDR chunk";
        return false;
    }
    *width = (static_cast<uint32_t>(header[16]) << 24) |
             (static_cast<uint32_t>(header[17]) << 16) |
             (static_cast<uint32_t>(header[18]) << 8) | static_cast<uint32_t>(header[19]);
    *height = (static_cast<uint32_t>(header[20]) << 24) |
              (static_cast<uint32_t>(header[21]) << 16) |
              (static_cast<uint32_t>(header[22]) << 8) | static_cast<uint32_t>(header[23]);
    return true;
}

bool check_screenshot(const std::string& path, const fs::path& directory, uint32_t width,
                      uint32_t height, std::string* error) {
    const fs::path file = from_utf8(path);
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec || size < 24) {
        *error = "the screenshot " + path + " is missing or too short";
        return false;
    }
    if (!fs::equivalent(file.parent_path(), directory, ec) || ec) {
        *error = "the screenshot landed in " + utf8(file.parent_path()) + " instead of " +
                 utf8(directory);
        return false;
    }
    uint32_t png_width = 0;
    uint32_t png_height = 0;
    if (!read_png_header(file, &png_width, &png_height, error)) {
        return false;
    }
    if (png_width != width || png_height != height) {
        *error = "the screenshot is " + std::to_string(png_width) + "x" +
                 std::to_string(png_height) + ", expected " + std::to_string(width) + "x" +
                 std::to_string(height);
        return false;
    }
    return true;
}

// Drives SaveScreenshot the way the toolbar does: queue a request, then poll
// the status until the counter moves. Both requests happen in the same second,
// so the second one also proves the collision suffix keeps both files.
bool run_screenshots(chast_viewer::ViewerEngine& engine, const fs::path& directory,
                     std::chrono::milliseconds timeout, std::string* first_path,
                     std::string* second_path, std::string* error) {
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        *error = "cannot create " + utf8(directory) + ": " + ec.message();
        return false;
    }
    const chast_viewer::ViewerStatus before = engine.Snapshot();
    auto wait_for_update = [&](uint64_t previous, chast_viewer::ViewerStatus* status) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            *status = engine.Snapshot();
            if (status->screenshot_updates > previous) {
                return true;
            }
            if (status->state != chast_viewer::ViewerState::kStreaming) {
                *error = "the session ended while a screenshot was pending";
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                *error = "the screenshot was not written in time";
                return false;
            }
            std::this_thread::sleep_for(5ms);
        }
    };

    if (!engine.SaveScreenshot(utf8(directory))) {
        *error = "SaveScreenshot was refused";
        return false;
    }
    chast_viewer::ViewerStatus status = before;
    if (!wait_for_update(before.screenshot_updates, &status)) {
        return false;
    }
    if (!status.screenshot_error.empty()) {
        *error = "the first screenshot failed: " + status.screenshot_error;
        return false;
    }
    if (status.screenshot_path.empty()) {
        *error = "the first screenshot reported no path";
        return false;
    }
    *first_path = status.screenshot_path;
    if (!check_screenshot(*first_path, directory, status.width, status.height, error)) {
        return false;
    }

    if (!engine.SaveScreenshot(utf8(directory))) {
        *error = "the second SaveScreenshot was refused";
        return false;
    }
    if (!wait_for_update(status.screenshot_updates, &status)) {
        return false;
    }
    if (!status.screenshot_error.empty()) {
        *error = "the second screenshot failed: " + status.screenshot_error;
        return false;
    }
    *second_path = status.screenshot_path;
    if (*second_path == *first_path) {
        *error = "both screenshots landed at " + *second_path;
        return false;
    }
    if (!check_screenshot(*second_path, directory, status.width, status.height, error)) {
        return false;
    }
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

bool seed_file_pages(const fs::path& tree, std::string* error) {
    for (size_t i = 0; i < chast::kMaxFileListEntries + 1; ++i) {
        std::error_code ec;
        fs::create_directories(tree / ("page-empty-" + std::to_string(i)), ec);
        if (ec) { *error = "cannot seed directory pages: " + ec.message(); return false; }
    }
    return write_file(tree / "zz-after-page.txt", "data from the last directory page", error);
}

bool verify_file_pages(chast_viewer::ViewerEngine& engine, const ShareSeed& names,
                       const fs::path& downloads, std::string* error) {
    std::set<std::string> seen;
    std::string cursor;
    int pages = 0;
    // A superseded browse reply must not replace the most recent folder.
    if (!engine.RequestFileList("")) { *error = "cannot queue root listing"; return false; }
    do {
        if (!engine.RequestFileList(names.tree_name, cursor)) { *error = "cannot queue directory page"; return false; }
        chast_viewer::ViewerEngine::FileListAnswer page;
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (engine.TakeFileList(page)) { ready = true; break; }
            std::this_thread::sleep_for(5ms);
        }
        if (!ready || page.directory != names.tree_name || page.cursor != cursor || page.result.code != chast::FileCode::Ok) {
            *error = "directory page missing or replaced by a stale reply"; return false;
        }
        for (const auto& entry : page.result.entries) {
            if (!seen.insert(entry.name).second) { *error = "duplicate directory entry across pages"; return false; }
        }
        cursor = page.result.next_cursor;
        ++pages;
    } while (!cursor.empty() && pages < 4);
    if (!cursor.empty() || pages != 2 || seen.size() != chast::kMaxFileListEntries + 6 || !seen.contains("zz-after-page.txt")) {
        *error = "paged listing did not include every folder and the final file"; return false;
    }
    WalkOutcome walk;
    if (!run_walk(engine, names.tree_name, 120s, &walk, error)) return false;
    if (walk.code != 0 || walk.done != 4 || walk.total != 4) {
        *error = "paged folder download failed: " + walk.error; return false;
    }
    const auto destination = from_utf8(walk.destination);
    if (!check_tree(destination, names.tree_name, downloads, error)) return false;
    for (size_t i = 0; i < chast::kMaxFileListEntries + 1; ++i) {
        std::error_code ec;
        if (!fs::is_directory(destination / ("page-empty-" + std::to_string(i)), ec)) {
            *error = "paged empty folder was not mirrored"; return false;
        }
    }
    std::string bytes;
    if (!read_file(destination / "zz-after-page.txt", &bytes, error) || bytes != "data from the last directory page") {
        *error = "final-page file contents differ"; return false;
    }
    std::printf("viewer-probe: 2 listing pages, 1030 entries, 1025 paged empty folders and 4 files mirrored over QUIC\n");
    return true;
}

struct RawClipboardFormat {
    UINT format = 0;
    std::vector<uint8_t> bytes;
};

struct ClipboardHandleInfo {
    HGLOBAL handle = nullptr;
    SIZE_T bytes = 0;
};

bool open_clipboard_with_retry() {
    for (int attempt = 0; attempt != 50; ++attempt) {
        if (OpenClipboard(nullptr)) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

bool publish_raw_clipboard(const std::vector<RawClipboardFormat>& formats,
                           std::string* error) {
    std::vector<HGLOBAL> handles;
    handles.reserve(formats.size());
    for (const RawClipboardFormat& format : formats) {
        if (format.format == 0 || format.bytes.empty()) {
            *error = "raw clipboard format has no data";
            return false;
        }
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, format.bytes.size());
        if (handle == nullptr) {
            for (HGLOBAL staged : handles) GlobalFree(staged);
            *error = "cannot allocate a raw clipboard handle";
            return false;
        }
        void* target = GlobalLock(handle);
        if (target == nullptr) {
            GlobalFree(handle);
            for (HGLOBAL staged : handles) GlobalFree(staged);
            *error = "cannot lock a raw clipboard handle";
            return false;
        }
        std::memcpy(target, format.bytes.data(), format.bytes.size());
        GlobalUnlock(handle);
        handles.push_back(handle);
    }

    if (!open_clipboard_with_retry()) {
        for (HGLOBAL handle : handles) GlobalFree(handle);
        *error = "cannot open the private clipboard";
        return false;
    }
    if (!EmptyClipboard()) {
        CloseClipboard();
        for (HGLOBAL handle : handles) GlobalFree(handle);
        *error = "cannot clear the private clipboard";
        return false;
    }
    size_t published = 0;
    for (size_t i = 0; i < formats.size(); ++i) {
        if (SetClipboardData(formats[i].format, handles[i]) != nullptr) {
            handles[i] = nullptr;  // ownership moved to the clipboard
            ++published;
        }
    }
    CloseClipboard();
    for (HGLOBAL handle : handles) {
        if (handle != nullptr) GlobalFree(handle);
    }
    if (published != formats.size()) {
        *error = "the private clipboard refused a raw format";
        return false;
    }
    return true;
}

bool inspect_raw_clipboard(const std::vector<RawClipboardFormat>& formats,
                           std::vector<ClipboardHandleInfo>* handles,
                           std::string* error) {
    handles->clear();
    if (!open_clipboard_with_retry()) {
        *error = "cannot inspect the private clipboard";
        return false;
    }
    struct Closer {
        ~Closer() { CloseClipboard(); }
    } closer;
    for (const RawClipboardFormat& expected : formats) {
        HGLOBAL handle =
            static_cast<HGLOBAL>(GetClipboardData(expected.format));
        if (handle == nullptr) {
            *error = "a published raw clipboard format is missing";
            return false;
        }
        const SIZE_T size = GlobalSize(handle);
        const void* data = GlobalLock(handle);
        if (data == nullptr || size != expected.bytes.size() ||
            std::memcmp(data, expected.bytes.data(), size) != 0) {
            if (data != nullptr) GlobalUnlock(handle);
            *error = "a published raw clipboard format has the wrong bytes";
            return false;
        }
        GlobalUnlock(handle);
        handles->push_back({handle, size});
    }
    return true;
}

std::vector<uint8_t> make_clipboard_dib(uint32_t width, uint32_t height) {
    const uint64_t pixel_bytes = uint64_t{width} * height * 4;
    std::vector<uint8_t> dib(40 + static_cast<size_t>(pixel_bytes), 0);
    std::fill(dib.begin() + 40, dib.end(), static_cast<uint8_t>(0x80));
    const auto put32 = [&dib](size_t offset, uint32_t value) {
        for (int i = 0; i != 4; ++i) {
            dib[offset + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    };
    put32(0, 40);
    put32(4, width);
    // A negative height makes the rows top-down, matching the stored BGRA.
    put32(8, static_cast<uint32_t>(-static_cast<int32_t>(height)));
    dib[12] = 1;  // planes
    dib[14] = 32; // bits per pixel
    put32(20, static_cast<uint32_t>(pixel_bytes));
    return dib;
}

int native_clipboard_probe() {
    // A private window station has its own clipboard. No interactive user's
    // clipboard is saved, cleared or replaced by this integration check.
    const std::wstring station_name = L"ChastClipboardProbe-" + std::to_wstring(GetCurrentProcessId());
    HWINSTA station = CreateWindowStationW(station_name.c_str(), 0, WINSTA_ALL_ACCESS, nullptr);
    if (!station) {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED) {
            std::fprintf(stderr,
                         "skipped: the process token cannot create a private clipboard station (%lu)\n",
                         static_cast<unsigned long>(error));
            return 77;
        }
        std::fprintf(stderr, "viewer-probe: cannot create private clipboard station (%lu)\n",
                     static_cast<unsigned long>(error));
        return 1;
    }
    if (!SetProcessWindowStation(station)) {
        const DWORD error = GetLastError();
        CloseWindowStation(station);
        if (error == ERROR_ACCESS_DENIED) {
            std::fprintf(stderr,
                         "skipped: the process cannot select its private clipboard station (%lu)\n",
                         static_cast<unsigned long>(error));
            return 77;
        }
        std::fprintf(stderr, "viewer-probe: cannot select private clipboard station (%lu)\n",
                     static_cast<unsigned long>(error));
        return 1;
    }
    HDESK desktop = CreateDesktopW(L"ChastProbe", nullptr, nullptr, 0,
                                  DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
    if (!desktop || !SetThreadDesktop(desktop) || FAILED(OleInitialize(nullptr))) {
        std::fprintf(stderr, "viewer-probe: cannot initialize private OLE desktop (%lu)\n", GetLastError()); return 1;
    }
    struct Source final : chast::ClipboardVirtualFiles {
        std::vector<chast::ClipboardFileEntry> list{{"native.txt", 5}};
        std::atomic<bool> active{true};
        std::atomic<unsigned> reads{0};
        const std::vector<chast::ClipboardFileEntry>& entries() const override { return list; }
        bool Active() const override { return active.load(); }
        void Cancel() override { active = false; }
        bool Read(size_t index, uint64_t offset, uint32_t length, std::vector<uint8_t>& bytes, std::string*) override {
            if (!Active() || index || offset > 5 || length > 5 - offset) return false;
            ++reads; const std::string text = "hello";
            bytes.assign(text.begin() + static_cast<size_t>(offset), text.begin() + static_cast<size_t>(offset + length)); return true;
        }
    };
    auto source = std::make_shared<Source>();
    int result = 1;
    {
        const UINT png_format = RegisterClipboardFormatW(L"PNG");
        const UINT png_mime_format = RegisterClipboardFormatW(L"image/png");
        const UINT svg_format = RegisterClipboardFormatW(L"image/svg+xml");
        const uint8_t png_pixels[16] = {
            0, 0, 255, 255, 0, 255, 0, 255,
            255, 0, 0, 255, 255, 255, 255, 0};
        std::vector<uint8_t> png;
        std::string error;
        std::string failed_stage;
        const auto stage = [&failed_stage](bool ok, const char* name) {
            if (!ok && failed_stage.empty()) failed_stage = name;
            return ok;
        };
        bool raw_ok = stage(png_format != 0 && png_mime_format != 0 &&
                                chast::encode_png_bgra(png_pixels, 2, 2, 8, png, &error),
                            "encode source PNG");
        chast::ClipboardContent png_content;
        chast::ClipboardFormatData png_entry;
        png_entry.format = chast::ClipboardFormat::Png;
        png_entry.data = png;
        png_content.formats.push_back(std::move(png_entry));
        if (raw_ok) raw_ok = stage(chast::write_clipboard(png_content, &error),
                                   "write PNG through clipboard API");

        const std::vector<RawClipboardFormat> png_formats{{png_format, png},
                                                          {png_mime_format, png}};
        std::vector<ClipboardHandleInfo> png_handles;
        if (raw_ok) {
            raw_ok = stage(inspect_raw_clipboard(png_formats, &png_handles, &error) &&
                               png_handles.size() == 2 &&
                               png_handles[0].handle != png_handles[1].handle,
                           "inspect independent PNG handles");
        }
        chast::ClipboardContent png_readback;
        if (raw_ok) {
            raw_ok = stage(chast::read_clipboard(png_readback, &error) &&
                               png_readback.formats.size() == 1 &&
                               png_readback.find(chast::ClipboardFormat::Png) != nullptr &&
                               png_readback.find(chast::ClipboardFormat::Png)->data == png,
                           "read back PNG through clipboard API");
        }

        const std::string svg_markup =
            "<svg xmlns=\"http://www.w3.org/2000/svg\"><circle cx=\"5\" cy=\"5\" "
            "r=\"4\" fill=\"#36c\"/></svg>";
        chast::ClipboardContent svg_content;
        chast::ClipboardFormatData svg_entry;
        svg_entry.format = chast::ClipboardFormat::Svg;
        svg_entry.data.assign(svg_markup.begin(), svg_markup.end());
        svg_content.formats.push_back(std::move(svg_entry));
        if (raw_ok) {
            raw_ok = stage(svg_format != 0 && chast::write_clipboard(svg_content, &error),
                           "write SVG through clipboard API");
        }
        chast::ClipboardContent svg_readback;
        if (raw_ok) {
            raw_ok = stage(chast::read_clipboard(svg_readback, &error) &&
                               svg_readback.formats.size() == 1 &&
                               svg_readback.find(chast::ClipboardFormat::Svg) != nullptr &&
                               svg_readback.find(chast::ClipboardFormat::Svg)->data ==
                                   svg_content.formats.front().data,
                           "read back SVG through clipboard API");
        }
        const std::string active_svg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\"><script>alert(1)</script></svg>";
        const std::vector<uint8_t> hostile_svg(active_svg.begin(), active_svg.end());
        if (raw_ok) {
            raw_ok = stage(publish_raw_clipboard({
                               {svg_format, hostile_svg}}, &error),
                           "publish active SVG");
        }
        chast::ClipboardContent hostile_readback;
        if (raw_ok) {
            raw_ok = stage(chast::read_clipboard_state(hostile_readback, &error) ==
                                   chast::ClipboardReadState::Failed &&
                               hostile_readback.formats.empty(),
                           "reject active SVG from native clipboard");
        }

        const wchar_t fallback_text[] = L"fallback";
        const auto* text_bytes = reinterpret_cast<const uint8_t*>(fallback_text);
        const std::vector<uint8_t> raw_text(text_bytes, text_bytes + sizeof(fallback_text));
        const std::vector<uint8_t> expected_text{'f', 'a', 'l', 'l', 'b', 'a', 'c', 'k'};
        if (raw_ok) {
            raw_ok = stage(publish_raw_clipboard({{svg_format, hostile_svg},
                                                  {CF_UNICODETEXT, raw_text},
                                                  {png_format, png}},
                                                 &error),
                           "publish active SVG with text and PNG");
        }
        chast::ClipboardContent mixed_readback;
        if (raw_ok) {
            raw_ok = stage(chast::read_clipboard_state(mixed_readback, &error) ==
                                   chast::ClipboardReadState::Content &&
                               mixed_readback.formats.size() == 2 &&
                               mixed_readback.formats[0].format == chast::ClipboardFormat::Text &&
                               mixed_readback.formats[0].data == expected_text &&
                               mixed_readback.formats[1].format == chast::ClipboardFormat::Png &&
                               mixed_readback.formats[1].data == png,
                           "retain text and PNG when SVG is rejected");
        }

        constexpr uint32_t kLargeWidth = 4096;
        constexpr uint32_t kLargeHeight = 2048;
        const std::vector<uint8_t> large_dib =
            make_clipboard_dib(kLargeWidth, kLargeHeight);
        if (raw_ok) {
            raw_ok = stage(large_dib.size() > chast::kMaxClipboardImageBytes &&
                               large_dib.size() < chast::kMaxClipboardDibSourceBytes &&
                               publish_raw_clipboard({{CF_DIB, large_dib}}, &error),
                           "publish oversized DIB");
        }
        chast::ClipboardContent large_readback;
        if (raw_ok) {
            raw_ok = stage(chast::read_clipboard_state(large_readback, &error) ==
                                   chast::ClipboardReadState::Content &&
                               large_readback.formats.size() == 1 &&
                               large_readback.find(chast::ClipboardFormat::Dib) == nullptr &&
                               large_readback.find(chast::ClipboardFormat::Png) != nullptr,
                           "convert oversized DIB to PNG");
        }

        std::vector<uint8_t> bad_png = png;
        bad_png[1] ^= 0xFF;
        const std::vector<uint8_t> small_dib = make_clipboard_dib(2, 2);
        if (raw_ok) {
            raw_ok = stage(publish_raw_clipboard({{png_format, bad_png},
                                                  {png_mime_format, bad_png},
                                                  {CF_DIB, small_dib}},
                                                 &error),
                           "publish broken PNG with DIB");
        }
        chast::ClipboardContent fallback_readback;
        if (raw_ok) {
            raw_ok = stage(chast::read_clipboard_state(fallback_readback, &error) ==
                                   chast::ClipboardReadState::Content &&
                               fallback_readback.find(chast::ClipboardFormat::Dib) != nullptr &&
                               fallback_readback.find(chast::ClipboardFormat::Png) != nullptr,
                           "fall back from broken PNG to DIB");
        }
        if (!raw_ok) {
            std::fprintf(stderr,
                         "viewer-probe: native image clipboard contract failed at %s: %s\n",
                         failed_stage.c_str(), error.c_str());
        } else {
            std::puts("viewer-probe: private native clipboard: PNG aliases, safe SVG "
                      "round-trip, active SVG rejection and mixed-format fallback, "
                      "oversized DIB conversion, and broken-PNG DIB fallback passed");
        }

        chast::VirtualClipboard owner;
        error.clear();
        ComPtr<IDataObject> object;
        if (!raw_ok) {
            result = 1;
        } else if (!OpenClipboard(nullptr)) {
            std::fprintf(stderr, "viewer-probe: cannot reserve the private clipboard\n");
            result = 1;
        } else {
        const bool published_while_busy = owner.Publish(source, &error);
        CloseClipboard();
        if (published_while_busy || !source->Active() || source->reads != 0) {
            std::fprintf(stderr, "viewer-probe: busy clipboard did not preserve the offer for retry\n");
            result = 1;
        } else if (!owner.Publish(source, &error) || OleGetClipboard(&object) != S_OK || !object) {
            std::fprintf(stderr, "viewer-probe: native publication failed: %s\n", error.c_str());
        } else {
            FORMATETC format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW")),
                             nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium{};
            bool ok = source->reads == 0 && object->GetData(&format, &medium) == S_OK;
            if (ok) {
                const auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
                ok = group && group->cItems == 1 && std::wstring(group->fgd[0].cFileName) == L"native.txt";
                if (group) GlobalUnlock(medium.hGlobal);
                ReleaseStgMedium(&medium); medium = {};
            }
            format = {static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents")), nullptr,
                      DVASPECT_CONTENT, -1, TYMED_ISTREAM};
            ok = ok && object->QueryGetData(&format) == S_OK;
            format.lindex = 0;
            ok = ok && source->reads == 0 && object->GetData(&format, &medium) == S_OK && source->reads == 0;
            if (ok) {
                char data[5]{}; ULONG got = 0;
                ok = medium.pstm->Read(data, 5, &got) == S_OK && got == 5 && std::string(data, 5) == "hello" && source->reads == 1;
                // Replacing the clipboard must invalidate retained old streams.
                OleSetClipboard(nullptr);
                const auto until = std::chrono::steady_clock::now() + 2s;
                while (source->Active() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(10ms);
                LARGE_INTEGER begin{};
                ok = ok && !source->Active() && FAILED(medium.pstm->Seek(begin, STREAM_SEEK_SET, nullptr));
            }
            if (medium.tymed != TYMED_NULL) ReleaseStgMedium(&medium);
            if (ok) { std::puts("viewer-probe: private OLE clipboard: metadata-only publish, marshalled IStream read and ownership cancellation passed"); result = 0; }
            else std::fprintf(stderr, "viewer-probe: native clipboard contract failed (reads=%u)\n", source->reads.load());
        }
        }
    }
    OleUninitialize();
    // The OS closes private station/desktop handles when this dedicated probe exits.
    return result;
}

int host_recording_probe(bool viewer_session_isolation = false,
                         bool recording_revocation = false) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::string probe_name =
        (viewer_session_isolation ? "chast-viewer-session-isolation-probe-"
                                  : "chast-host-recording-probe-") +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    const fs::path root = probe_scratch_root() / probe_name;
    const auto recordings = root / L"录像 archive";
    const auto first_viewer_recordings = root / L"viewer-a";
    const auto second_viewer_recordings = root / L"viewer-b";
    std::string error;
    ProbeSink first_sink, second_sink;
    std::unique_ptr<chast::HostServer> host;
    std::unique_ptr<chast_viewer::ViewerEngine> first, second;
    bool ok = false;
    try {
        const auto require = [](bool condition, const std::string& message) {
            if (!condition) throw std::runtime_error(message);
        };
        require(first_sink.create(&error) && second_sink.create(&error), error);
        const auto port = pick_free_port();
        require(port != 0, "no loopback port");
        std::function<void(chast::HostServerConfig&)> configure;
        if (viewer_session_isolation) {
            configure = [](chast::HostServerConfig& config) {
                config.settings.allow_record = true;
            };
        }
        host = start_host(utf8(root / "share"), port, &error, false, {}, {}, {},
                         chast::ClipboardDirection::Disabled, {}, utf8(recordings),
                         std::move(configure));
        require(host != nullptr, error);
        const auto connect = [&](ProbeSink& sink) {
            auto viewer = std::make_unique<chast_viewer::ViewerEngine>(&sink);
            viewer->SetClipboardSync(false);
            require(viewer->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {}, kPassword),
                    "start recording viewer");
            require(wait_streaming(*viewer, 20s, &error), error);
            return viewer;
        };
        first = connect(first_sink);
        const auto first_host_viewers = host->viewers();
        require(first_host_viewers.size() == 1, "first viewer has a unique host connection");
        const auto first_host_viewer = first_host_viewers.front();
        second = connect(second_sink);
        if (viewer_session_isolation) {
            require(wait_permission(*first, chast::kPermissionRecord, 5s) &&
                        wait_permission(*second, chast::kPermissionRecord, 5s),
                    "both viewers receive local-recording consent");
        }
        const auto wait_frames = [&](auto& viewer, uint64_t previous) {
            const auto until = std::chrono::steady_clock::now() + 10s;
            while (std::chrono::steady_clock::now() < until) {
                const auto status = viewer.Snapshot();
                if (status.received > previous) return true;
                std::this_thread::sleep_for(20ms);
            }
            return false;
        };
        require(wait_frames(*first, 0) && wait_frames(*second, 0), "both viewers receive encoded video");
        const chast::VideoCodec recording_codec = first->Snapshot().codec;
        require(second->Snapshot().codec == recording_codec,
                "viewers of one source negotiate the same recording codec");
        if (viewer_session_isolation) {
            require(first->SetAudioProcessing(false, true) &&
                        second->SetAudioProcessing(true, false),
                    "each viewer accepts its own audio-processing choices");
            require(first->Snapshot().audio_aec == false &&
                        first->Snapshot().audio_noise_suppression == true &&
                        second->Snapshot().audio_aec == true &&
                        second->Snapshot().audio_noise_suppression == false,
                    "audio-processing state is isolated by viewer engine");

            require(first->SetRecording(true, utf8(first_viewer_recordings)) &&
                        second->SetRecording(true, utf8(second_viewer_recordings)),
                    "each viewer starts its own local recorder");
            const auto recording_deadline = std::chrono::steady_clock::now() + 10s;
            while (std::chrono::steady_clock::now() < recording_deadline &&
                   (!first->Snapshot().recording || !second->Snapshot().recording)) {
                std::this_thread::sleep_for(20ms);
            }
            require(first->Snapshot().recording && second->Snapshot().recording,
                    "both session-local recorders become active");
            std::this_thread::sleep_for(1200ms);
            if (recording_revocation) {
                require(host->set_permissions(first_host_viewer.id,
                            first_host_viewer.permissions & ~chast::kPermissionRecord),
                        "host revokes recording only for the first viewer");
            } else {
                require(first->SetRecording(false), "the first viewer stops only its recorder");
            }
            const auto stop_deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < stop_deadline &&
                   first->Snapshot().recording) {
                std::this_thread::sleep_for(20ms);
            }
            require(!first->Snapshot().recording && second->Snapshot().recording &&
                        second->Snapshot().state == chast_viewer::ViewerState::kStreaming,
                    recording_revocation
                        ? "revocation must stop the first recorder and preserve the second"
                        : "stopping one viewer leaves the other stream and recorder active");
            if (recording_revocation) {
                require((first->Snapshot().permissions & chast::kPermissionRecord) == 0 &&
                            !first->SetRecording(true),
                        "revoked viewer cannot restart recording");
                const auto first_before = first->Snapshot().received;
                const auto second_before = second->Snapshot().received;
                require(wait_frames(*first, first_before) && wait_frames(*second, second_before),
                        "both videos continue after recording revocation");
                require(host->set_permissions(first_host_viewer.id, first_host_viewer.permissions) &&
                            wait_permission(*first, chast::kPermissionRecord, 5s),
                        "recording permission can be restored");
                require(wait_frames(*first, first->Snapshot().received) &&
                            !first->Snapshot().recording && second->Snapshot().recording,
                        "restoring permission cannot resume recording automatically");
            }
        } else {
            require(!first->Snapshot().recording && !second->Snapshot().recording,
                    "viewer-side recorders remain off");
        }
        first->Stop(); first.reset();
        const auto before = second->Snapshot().received;
        require(wait_frames(*second, before), "remaining viewer survives the first viewer leaving");
        std::this_thread::sleep_for(1100ms);
        second->Stop(); second.reset();
        host->stop(); host.reset();
        size_t directories = 0, files = 0;
        for (const auto& entry : fs::directory_iterator(recordings)) if (entry.is_directory()) ++directories;
        require(directories == 1, "two viewers of one source must create exactly one recording directory");
        for (const auto& entry : fs::recursive_directory_iterator(recordings)) {
            if (entry.path().extension() != ".mp4") continue;
            uint64_t samples = 0, keys = 0;
            const bool valid = chast_tools::Mp4Recorder::verify(
                utf8(entry.path()), recording_codec, &samples, &keys, &error);
            require(valid && keys > 0, "cannot reopen host recording: " + error);
            std::printf("host-recording-probe: %s samples=%llu keys=%llu\n", utf8(entry.path()).c_str(),
                static_cast<unsigned long long>(samples), static_cast<unsigned long long>(keys));
            ++files;
        }
        require(files == 1, "stable shared stream should produce exactly one finished MP4");
        if (viewer_session_isolation) {
            const auto verify_viewer_recording = [&](const fs::path& directory,
                                                      const char* label) {
                size_t viewer_files = 0;
                for (const auto& entry : fs::recursive_directory_iterator(directory)) {
                    if (entry.path().extension() != ".mp4") continue;
                    uint64_t samples = 0, keys = 0;
                    const bool valid = chast_tools::Mp4Recorder::verify(
                        utf8(entry.path()), recording_codec, &samples, &keys, &error);
                    require(valid && samples > 0 && keys > 0,
                            std::string(label) + " recording is not a playable MP4: " + error);
                    std::printf("viewer-session-isolation: %s %s samples=%llu keys=%llu\n",
                                label, utf8(entry.path()).c_str(),
                                static_cast<unsigned long long>(samples),
                                static_cast<unsigned long long>(keys));
                    ++viewer_files;
                }
                require(viewer_files == 1,
                        std::string(label) + " must own exactly one recording file");
            };
            verify_viewer_recording(first_viewer_recordings, "viewer-a");
            verify_viewer_recording(second_viewer_recordings, "viewer-b");
            std::puts("viewer-session-isolation: PASS two loopback sessions kept audio preferences and local recordings separate");
            if (recording_revocation)
                std::puts("viewer-recording-revocation: PASS stop, reject restart, preserve video and other recorder, no auto-resume, playable MP4");
        } else {
            std::puts("host-recording-probe: PASS real QUIC + Auto encoder, two viewers, publisher departure, final save and Unicode folder");
        }
        ok = true;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "host-recording-probe: FAIL %s\n", exception.what());
    }
    if (first) first->Stop();
    if (second) second->Stop();
    if (host) host->stop();
    CoUninitialize();
    return ok ? 0 : 1;
}

int real_microphone_probe() {
    HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        std::fprintf(stderr, "real-microphone-probe: COM initialization failed\n");
        return 1;
    }

    std::string error;
    auto microphones = chast::list_microphones(&error);
    if (microphones.empty()) {
        std::fprintf(stderr, "real-microphone-probe: no active WASAPI microphone: %s\n",
                     error.c_str());
        if (uninitialize_com) CoUninitialize();
        return 1;
    }
    const auto selected = std::find_if(microphones.begin(), microphones.end(),
                                       [](const chast::MicrophoneDevice& device) {
                                           return device.is_default;
                                       });
    const chast::MicrophoneDevice& device =
        selected == microphones.end() ? microphones.front() : *selected;
    std::printf("real-microphone-probe: endpoint=%s default=%s; audio remains in memory\n",
                device.name.c_str(), device.is_default ? "yes" : "no");

    ProbeSink sink;
    std::unique_ptr<chast::HostServer> host;
    std::unique_ptr<chast_viewer::ViewerEngine> viewer;
    auto player = std::make_shared<MicrophoneMetrics>();
    int result = 1;
    try {
        const auto require = [](bool condition, const std::string& message) {
            if (!condition) throw std::runtime_error(message);
        };
        require(sink.create(&error), error);
        const uint16_t port = pick_free_port();
        require(port != 0, "no loopback port");
        const std::string identity = chast::find_stored_identity_fingerprint(
            chast::IdentityScope::CurrentUser, &error);
        require(!identity.empty(),
                "probe requires an existing current-user host identity: " + error);
        auto configure = [player, identity](chast::HostServerConfig& config) {
            config.settings.allow_microphone = true;
            config.settings.allow_audio = false;
            config.settings.allow_input = false;
            config.settings.allow_clipboard = false;
            config.settings.allow_terminal = false;
            config.settings.allow_record = false;
            config.expected_identity_fingerprint = identity;
            config.microphone_player = [player](const chast::AudioPlayerConfig&,
                                                std::string*) {
                return std::make_unique<MicrophoneMetricsPlayer>(player);
            };
        };
        host = start_host(utf8(fs::temp_directory_path()), port, &error, false, {}, {}, {},
                          chast::ClipboardDirection::Disabled, {}, {}, std::move(configure));
        require(host != nullptr, error);
        viewer = std::make_unique<chast_viewer::ViewerEngine>(&sink);
        viewer->SetClipboardSync(false);
        require(viewer->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {}, kPassword),
                "start microphone viewer");
        require(wait_streaming(*viewer, 20s, &error), error);
        require(wait_permission(*viewer, chast::kPermissionMicrophone, 5s),
                "the host did not grant microphone permission");
        require(viewer->SelectMicrophone(device.id), "select the enumerated microphone");
        require(viewer->SetAudioProcessing(false, false),
                "disable DSP for the initial capture sample");
        require(viewer->SetMicrophone(true), "start WASAPI microphone capture");

        const auto wait_for = [&](auto predicate, std::chrono::milliseconds timeout) {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto status = viewer->Snapshot();
                if (predicate(status)) return true;
                if (status.state != chast_viewer::ViewerState::kStreaming) return false;
                std::this_thread::sleep_for(20ms);
            }
            return predicate(viewer->Snapshot());
        };
        require(wait_for([](const chast_viewer::ViewerStatus& status) {
                            return status.microphone_enabled &&
                                   status.microphone_packets >= 10;
                        }, 8s),
                "WASAPI did not produce microphone packets");
        std::this_thread::sleep_for(800ms);

        const uint64_t before_noise = viewer->Snapshot().microphone_ns_frames;
        require(viewer->SetAudioProcessing(false, true),
                "enable noise suppression for the live microphone");
        require(wait_for([&](const chast_viewer::ViewerStatus& status) {
                            return status.microphone_ns_frames > before_noise;
                        }, 5s),
                "noise suppression did not process captured microphone frames");

        const uint64_t before_aec = viewer->Snapshot().microphone_aec_frames;
        const uint64_t before_aec_ns = viewer->Snapshot().microphone_ns_frames;
        require(viewer->SetAudioProcessing(true, true),
                "enable echo cancellation and noise suppression");
        require(wait_for([&](const chast_viewer::ViewerStatus& status) {
                            return status.microphone_aec_frames > before_aec &&
                                   status.microphone_ns_frames > before_aec_ns;
                        }, 5s),
                "AEC/noise suppression did not process captured microphone frames");
        std::this_thread::sleep_for(800ms);

        auto captured = viewer->Snapshot();
        const auto host_captured = host->status().stats;
        require(captured.microphone_enabled && captured.microphone_errors == 0 &&
                    captured.microphone_dropped == 0,
                "microphone capture or uplink reported errors or dropped packets");
        require(host_captured.microphone_packets > 0 &&
                    host_captured.microphone_frames > 0 &&
                    host_captured.microphone_errors == 0,
                "the host did not decode the real microphone stream cleanly");
        require(player->stats().played_frames > 0,
                "the host's silent sink did not receive decoded microphone samples");

        const auto viewers = host->viewers();
        require(viewers.size() == 1, "expected one authenticated microphone viewer");
        const uint64_t connection_id = viewers.front().id;
        const chast::PermissionMask revoked_permissions =
            viewers.front().permissions & ~chast::kPermissionMicrophone;
        require(host->set_permissions(connection_id, revoked_permissions),
                "revoke microphone permission");
        require(wait_for([](const chast_viewer::ViewerStatus& status) {
                            return !status.microphone_authorized &&
                                   !status.microphone_enabled &&
                                   (status.permissions & chast::kPermissionMicrophone) == 0;
                        }, 5s),
                "the viewer did not stop microphone capture after permission revocation");
        require(!viewer->SetMicrophone(true),
                "the viewer accepted a microphone start after permission revocation");
        const uint64_t frames_before_revocation = viewer->Snapshot().received;
        const uint64_t packets_after_revocation = host->status().stats.microphone_packets;
        std::this_thread::sleep_for(500ms);
        captured = viewer->Snapshot();
        const auto host_after_revocation = host->status().stats;
        require(captured.state == chast_viewer::ViewerState::kStreaming &&
                    captured.received > frames_before_revocation,
                "video did not continue after microphone permission revocation");
        require(!host_after_revocation.microphone_active &&
                    host_after_revocation.microphone_packets <= packets_after_revocation + 2,
                "microphone packets continued after permission revocation");

        std::printf("real-microphone-probe: sent=%llu decoded=%llu samples=%llu rms=%.1f "
                    "peak=%u dsp-aec=%llu dsp-ns=%llu video-after-revoke=%llu\n",
                    static_cast<unsigned long long>(captured.microphone_packets),
                    static_cast<unsigned long long>(host_captured.microphone_packets),
                    static_cast<unsigned long long>(player->stats().played_frames),
                    player->rms(), player->peak(),
                    static_cast<unsigned long long>(captured.microphone_aec_frames),
                    static_cast<unsigned long long>(captured.microphone_ns_frames),
                    static_cast<unsigned long long>(captured.received - frames_before_revocation));
        std::puts("real-microphone-probe: PASS WASAPI capture, loopback uplink, DSP frame counters, "
                  "permission revocation and continuing video; reused existing identity; "
                  "no audio file created");
        result = 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "real-microphone-probe: FAIL %s\n", exception.what());
    }
    if (viewer) viewer->Stop();
    if (host) host->stop();
    if (uninitialize_com) CoUninitialize();
    return result;
}

// Long-lived, loopback-only fixture for exercising the actual Flutter windows.
// Only synthetic silence is captured. This test fixture approves local viewers
// without an interactive password flow; it must never bind beyond loopback.
class AliasedDisplayCapturer final : public chast::IDxgiCapturer {
public:
    AliasedDisplayCapturer(std::unique_ptr<chast::IDxgiCapturer> capture,
                           chast::DisplayInfo display)
        : capture_(std::move(capture)), display_(std::move(display)) {}

    chast::CaptureResult capture(std::chrono::milliseconds timeout) override {
        return capture_->capture(timeout);
    }
    const chast::FrameInfo& frame() const override { return capture_->frame(); }
    const chast::CursorInfo& cursor() const override { return capture_->cursor(); }
    const chast::DisplayInfo& display() const override { return display_; }
    chast::AdapterDevice adapter_device() const override {
        return capture_->adapter_device();
    }
    UINT max_framerate() const override { return capture_->max_framerate(); }
    chast::Error reinitialize() override { return capture_->reinitialize(); }
    chast::Error set_nv12_scale(UINT percent) override {
        return capture_->set_nv12_scale(percent);
    }
    bool merged() const override { return capture_->merged(); }
    size_t merged_display_count() const override {
        return capture_->merged_display_count();
    }
    UINT nv12_scale_percent() const override {
        return capture_->nv12_scale_percent();
    }
    const chast::Error& last_error() const override {
        return capture_->last_error();
    }

private:
    std::unique_ptr<chast::IDxgiCapturer> capture_;
    chast::DisplayInfo display_;
};

int media_window_host(const fs::path& directory, bool multi_display = false) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);
    std::unique_ptr<chast::HostServer> host;
    int result = 1;
    try {
        const auto require = [](bool condition, const std::string& message) {
            if (!condition) throw std::runtime_error(message);
        };
        require(SUCCEEDED(com_result) || com_result == RPC_E_CHANGED_MODE,
                "COM initialization failed");
        require(directory.is_absolute() && fs::create_directory(directory),
                "media fixture requires a new absolute scratch directory");
        std::string error;
        const std::string identity = chast::find_stored_identity_fingerprint(
            chast::IdentityScope::CurrentUser, &error);
        require(!identity.empty(), "an existing current-user host identity is required: " + error);
        const uint16_t port = pick_free_port();
        require(port != 0, "no loopback port");
        auto metrics = std::make_shared<SyntheticAudioMetrics>();
        std::vector<chast::DisplayInfo> logical_displays;
        chast::DisplayInfo physical_display;
        if (multi_display) {
            logical_displays = chast::enumerate_displays();
            require(!logical_displays.empty(),
                    "the synthetic display fixture needs one attached physical display");
            const auto primary = std::find_if(logical_displays.begin(),
                logical_displays.end(), [](const chast::DisplayInfo& item) {
                    return item.primary;
                });
            physical_display = primary == logical_displays.end()
                ? logical_displays.front() : *primary;
            logical_displays = {physical_display};
            auto second = physical_display;
            second.device_name = L"CHAST_TEST_DISPLAY_2";
            second.primary = false;
            second.rect.left = physical_display.rect.right;
            second.rect.top = physical_display.rect.top;
            second.rect.right = second.rect.left + static_cast<LONG>(second.width);
            second.rect.bottom = second.rect.top + static_cast<LONG>(second.height);
            logical_displays.push_back(std::move(second));
        }
        auto configure = [metrics, identity, multi_display, logical_displays,
                          physical_display](chast::HostServerConfig& config) {
            config.settings.bind_address = "127.0.0.1";
            config.serve_once = false;
            config.first_client_timeout_ms = 600000;
            config.settings.max_viewers = 2;
            config.settings.auth_mode = chast::HostAuthMode::Click;
            config.settings.token_auth = false;
            config.settings.password_hash.clear();
            config.approve_client = [](const chast::ClientRequest& request) {
                return chast::ClientApproval{true, request.permissions};
            };
            config.settings.allow_audio = !multi_display;
            config.settings.allow_microphone = false;
            config.settings.allow_input = false;
            config.settings.allow_clipboard = false;
            config.settings.allow_terminal = false;
            config.settings.allow_file_transfer = false;
            config.settings.allow_record = !multi_display;
            config.settings.record_incoming = false;
            config.expected_identity_fingerprint = identity;
            config.log = {}; // No connection credentials in the fixture log.
            if (multi_display) {
                config.displays = [logical_displays] { return logical_displays; };
                config.capture_factory = [physical_display](
                    const chast::DisplayInfo& logical,
                    const chast::CaptureConfig& capture_config,
                    std::string* failure) -> std::unique_ptr<chast::IDxgiCapturer> {
                    auto capture = chast::create_shared_capturer(
                        physical_display, capture_config, failure);
                    if (!capture) return nullptr;
                    return std::make_unique<AliasedDisplayCapturer>(
                        std::move(capture), logical);
                };
            } else {
                config.audio_capture = [metrics](std::string* failure)
                    -> std::unique_ptr<chast::ILoopbackCapture> {
                    auto capture = std::make_unique<SyntheticSilentLoopback>(metrics);
                    if (!capture->ready()) {
                        if (failure) *failure = capture->error();
                        return nullptr;
                    }
                    return capture;
                };
            }
        };
        host = start_host(utf8(directory), port, &error, false, {}, {}, {},
                          chast::ClipboardDirection::Disabled, {}, {}, std::move(configure));
        require(host != nullptr, error);
        require(host->status().fingerprint == identity, "fixture host identity changed");
        const auto temporary_endpoint = directory / "endpoint.pending";
        std::ofstream endpoint(temporary_endpoint);
        endpoint << "{\"port\":" << port << ",\"fingerprint\":\"" << identity
                 << "\"}\n";
        endpoint.close();
        require(endpoint.good(), "cannot write fixture endpoint");
        fs::rename(temporary_endpoint, directory / "endpoint.json");
        std::puts(multi_display
            ? "media-window-host: ready; loopback approval; two logical displays alias one local capture; input/audio/files disabled"
            : "media-window-host: ready; loopback test approval; synthetic silence; input/microphone/files disabled");
        std::fflush(stdout);
        bool record_revoked = false;
        const auto deadline = std::chrono::steady_clock::now() + 10min;
        while (host->running() && std::chrono::steady_clock::now() < deadline &&
               !fs::exists(directory / "stop")) {
            const auto disconnect_marker = directory / "disconnect-one";
            const auto disconnect_first_marker = directory / "disconnect-first";
            if (fs::exists(disconnect_marker) ||
                fs::exists(disconnect_first_marker)) {
                const auto viewers = host->viewers();
                if (!viewers.empty() && host->disconnect_viewer(viewers.front().id)) {
                    std::error_code ignored;
                    fs::remove(disconnect_marker, ignored);
                    fs::remove(disconnect_first_marker, ignored);
                    std::puts("media-window-host: disconnected the oldest viewer by test command");
                    std::fflush(stdout);
                }
            }
            const bool revoke = fs::exists(directory / "revoke-recording");
            // Reconcile every live viewer, including connections admitted after
            // the marker changed. A disconnect may race this best-effort update.
            for (const auto& viewer : host->viewers()) {
                const auto permissions = static_cast<chast::PermissionMask>(revoke
                    ? viewer.permissions & ~chast::kPermissionRecord
                    : viewer.permissions | chast::kPermissionRecord);
                if (permissions != viewer.permissions)
                    host->set_permissions(viewer.id, permissions);
            }
            if (revoke != record_revoked) {
                record_revoked = revoke;
                std::printf("media-window-host: recording permission %s\n", revoke ? "revoked" : "restored");
                std::fflush(stdout);
            }
            std::this_thread::sleep_for(100ms);
        }
        require(host->status().state != chast::HostState::Failed, host->status().error);
        std::puts("media-window-host: stopped; UI checks must be recorded separately");
        result = 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "media-window-host: FAIL %s\n", exception.what());
    }
    if (host) host->stop();
    if (uninitialize_com) CoUninitialize();
    return result;
}

int audio_video_isolation_probe() {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        std::fprintf(stderr, "audio-video-isolation: COM initialization failed\n");
        return 1;
    }

    ProbeSink first_sink(150ms), second_sink(150ms);
    std::unique_ptr<chast::HostServer> host;
    std::unique_ptr<chast_viewer::ViewerEngine> first, second;
    auto metrics = std::make_shared<SyntheticAudioMetrics>();
    auto first_playback = std::make_shared<ViewerPlaybackMetrics>();
    auto second_playback = std::make_shared<ViewerPlaybackMetrics>();
    int result = 1;
    std::string error;
    try {
        const auto require = [](bool condition, const std::string& message) {
            if (!condition) throw std::runtime_error(message);
        };
        require(first_sink.create(&error) && second_sink.create(&error), error);
        const uint16_t port = pick_free_port();
        require(port != 0, "no loopback port");

        auto configure = [metrics](chast::HostServerConfig& config) {
            config.serve_once = false;
            config.settings.allow_audio = true;
            config.settings.allow_input = false;
            config.settings.allow_clipboard = false;
            config.settings.allow_terminal = false;
            config.settings.allow_record = false;
            config.settings.framerate = 5;
            config.adaptive_fps = false;
            config.audio_capture = [metrics](std::string* failure)
                -> std::unique_ptr<chast::ILoopbackCapture> {
                auto capture = std::make_unique<SyntheticSilentLoopback>(metrics);
                if (!capture->ready()) {
                    if (failure != nullptr) *failure = capture->error();
                    return nullptr;
                }
                return capture;
            };
        };
        host = start_host(utf8(fs::temp_directory_path()), port, &error, false, {}, {}, {},
                          chast::ClipboardDirection::Disabled, {}, {},
                          std::move(configure));
        require(host != nullptr, error);

        const auto connect = [&](ProbeSink& sink,
                                 std::shared_ptr<ViewerPlaybackMetrics> playback) {
            chast_viewer::ViewerEngine::AudioPlayerFactory player_factory =
                [playback = std::move(playback)](const chast::AudioPlayerConfig&,
                                                  std::string*) {
                    return std::make_unique<ViewerMetricsPlayer>(playback);
                };
            auto next = std::make_unique<chast_viewer::ViewerEngine>(
                &sink, nullptr, chast::create_decoder, std::move(player_factory));
            next->SetClipboardSync(false);
            require(next->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {},
                                kPassword),
                    "start synthetic-audio viewer session");
            require(wait_streaming(*next, 20s, &error), error);
            return next;
        };
        const auto wait_audio = [&](chast_viewer::ViewerEngine& current,
                                    uint64_t minimum_packets,
                                    std::chrono::milliseconds timeout) {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                const chast_viewer::ViewerStatus status = current.Snapshot();
                if (status.state != chast_viewer::ViewerState::kStreaming) return false;
                if (status.audio_enabled && status.audio_received >= minimum_packets &&
                    status.audio_decoded >= minimum_packets) {
                    return true;
                }
                std::this_thread::sleep_for(10ms);
            }
            const chast_viewer::ViewerStatus status = current.Snapshot();
            return status.audio_enabled && status.audio_received >= minimum_packets &&
                   status.audio_decoded >= minimum_packets;
        };
        const auto wait_mute = [&](chast_viewer::ViewerEngine& current,
                                   const std::shared_ptr<ViewerPlaybackMetrics>& playback,
                                   bool expected) {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < deadline) {
                if (current.Snapshot().audio_muted == expected &&
                    playback->muted.load(std::memory_order_relaxed) == expected) {
                    return true;
                }
                std::this_thread::sleep_for(10ms);
            }
            return current.Snapshot().audio_muted == expected &&
                   playback->muted.load(std::memory_order_relaxed) == expected;
        };

        first = connect(first_sink, first_playback);
        second = connect(second_sink, second_playback);
        require(wait_audio(*first, 30, 10s) && wait_audio(*second, 30, 10s),
                "both viewers must receive and decode the synthetic audio stream");
        require(!first->Snapshot().audio_muted && !second->Snapshot().audio_muted,
                "each synthetic playback session must start unmuted");
        require(first->SetAudioMuted(true), "mute synthetic playback session A");
        require(wait_mute(*first, first_playback, true) &&
                    wait_mute(*second, second_playback, false),
                "muting session A must not mute session B");

        const chast_viewer::ViewerStatus first_before = first->Snapshot();
        const chast_viewer::ViewerStatus second_before = second->Snapshot();
        const uint64_t first_muted_before =
            first_playback->muted_blocks.load(std::memory_order_relaxed);
        const uint64_t first_unmuted_before =
            first_playback->unmuted_blocks.load(std::memory_order_relaxed);
        const uint64_t second_muted_before =
            second_playback->muted_blocks.load(std::memory_order_relaxed);
        const uint64_t second_unmuted_before =
            second_playback->unmuted_blocks.load(std::memory_order_relaxed);
        const uint64_t first_video_before = first_sink.frames();
        const uint64_t second_video_before = second_sink.frames();
        std::this_thread::sleep_for(2200ms);
        const chast_viewer::ViewerStatus first_after = first->Snapshot();
        const chast_viewer::ViewerStatus second_after = second->Snapshot();
        const uint64_t first_video_after = first_sink.frames();
        const uint64_t second_video_after = second_sink.frames();
        const chast::HostServerStats host_stats = host->status().stats;
        require(first_after.state == chast_viewer::ViewerState::kStreaming &&
                    second_after.state == chast_viewer::ViewerState::kStreaming,
                "one of the viewer sessions stopped during slow video delivery");
        require(first_after.audio_errors == 0 && second_after.audio_errors == 0 &&
                    first_after.audio_received > first_before.audio_received &&
                    first_after.audio_decoded > first_before.audio_decoded &&
                    second_after.audio_received > second_before.audio_received &&
                    second_after.audio_decoded > second_before.audio_decoded,
                "either session's audio receive/decode stopped or reported an error");
        require(first_after.audio_decoded - first_before.audio_decoded >= 120 &&
                    second_after.audio_decoded - second_before.audio_decoded >= 120,
                "a session's audio decode throughput fell behind during the 5 FPS video probe");
        require(first_video_after >= first_video_before + 5 &&
                    second_video_after >= second_video_before + 5,
                "5 FPS video did not continue for both sessions while audio was active");
        require(first_after.framerate > 0 && first_after.framerate <= 5 &&
                    second_after.framerate > 0 && second_after.framerate <= 5,
                "the host did not retain the configured 5 FPS video rate for both sessions");
        require(first_after.audio_muted && !second_after.audio_muted &&
                    first_playback->muted_blocks.load(std::memory_order_relaxed) -
                            first_muted_before >= 120 &&
                    first_playback->unmuted_blocks.load(std::memory_order_relaxed) ==
                            first_unmuted_before &&
                    second_playback->unmuted_blocks.load(std::memory_order_relaxed) -
                            second_unmuted_before >= 120 &&
                    second_playback->muted_blocks.load(std::memory_order_relaxed) ==
                            second_muted_before,
                "session A mute changed session B's playback or failed to silence A");
        require(host_stats.audio_packets >= 150 && host_stats.audio_errors == 0 &&
                    host_stats.audio_dropped == 0,
                "the synthetic host source or send path reported errors or packet drops");
        require(metrics->encoded_packets.load(std::memory_order_relaxed) >= 150 &&
                    metrics->dropped_packets.load(std::memory_order_relaxed) == 0,
                "the synthetic source failed or overflowed its bounded queue");

        require(first->SetAudioMuted(false) && second->SetAudioMuted(true),
                "reverse the per-session mute states");
        require(wait_mute(*first, first_playback, false) &&
                    wait_mute(*second, second_playback, true),
                "unmuting A and muting B must remain session-scoped");
        const uint64_t first_unmuted_reverse_before =
            first_playback->unmuted_blocks.load(std::memory_order_relaxed);
        const uint64_t first_muted_reverse_before =
            first_playback->muted_blocks.load(std::memory_order_relaxed);
        const uint64_t second_unmuted_reverse_before =
            second_playback->unmuted_blocks.load(std::memory_order_relaxed);
        const uint64_t second_muted_reverse_before =
            second_playback->muted_blocks.load(std::memory_order_relaxed);
        const uint64_t first_decoded_reverse_before = first->Snapshot().audio_decoded;
        const uint64_t second_decoded_reverse_before = second->Snapshot().audio_decoded;
        std::this_thread::sleep_for(1100ms);
        const chast_viewer::ViewerStatus first_reversed = first->Snapshot();
        const chast_viewer::ViewerStatus second_reversed = second->Snapshot();
        require(!first_reversed.audio_muted && second_reversed.audio_muted &&
                    first_reversed.audio_decoded - first_decoded_reverse_before >= 50 &&
                    second_reversed.audio_decoded - second_decoded_reverse_before >= 50 &&
                    first_playback->unmuted_blocks.load(std::memory_order_relaxed) -
                            first_unmuted_reverse_before >= 50 &&
                    first_playback->muted_blocks.load(std::memory_order_relaxed) ==
                            first_muted_reverse_before &&
                    second_playback->muted_blocks.load(std::memory_order_relaxed) -
                            second_muted_reverse_before >= 50 &&
                    second_playback->unmuted_blocks.load(std::memory_order_relaxed) ==
                            second_unmuted_reverse_before,
                "reversing mute states did not independently restore A and silence B");

        first->Stop();
        first.reset();
        const auto sibling_deadline = std::chrono::steady_clock::now() + 10s;
        while (host->viewers().size() != 1 &&
               std::chrono::steady_clock::now() < sibling_deadline) {
            std::this_thread::sleep_for(10ms);
        }
        require(host->viewers().size() == 1,
                "the host did not release session A while preserving session B");
        const uint64_t sibling_decoded_before = second->Snapshot().audio_decoded;
        require(wait_audio(*second, sibling_decoded_before + 30, 5s) &&
                    second->Snapshot().audio_muted,
                "session B audio did not continue with its mute state after A disconnected");
        second->Stop();
        second.reset();
        const auto disconnect_deadline = std::chrono::steady_clock::now() + 10s;
        while (!host->viewers().empty() &&
               std::chrono::steady_clock::now() < disconnect_deadline) {
            std::this_thread::sleep_for(10ms);
        }
        require(host->viewers().empty(), "the host did not release both viewer sessions");

        const uint64_t packets_before_reconnect =
            metrics->encoded_packets.load(std::memory_order_relaxed);
        auto reconnect_playback = std::make_shared<ViewerPlaybackMetrics>();
        first = connect(first_sink, reconnect_playback);
        require(wait_audio(*first, 30, 10s),
                "audio did not restart after the viewer reconnected");
        require(first->SetAudioMuted(true) && wait_mute(*first, reconnect_playback, true),
                "the reconnected synthetic session could not mute its playback");
        std::this_thread::sleep_for(700ms);
        const chast_viewer::ViewerStatus reconnected = first->Snapshot();
        require(reconnected.state == chast_viewer::ViewerState::kStreaming &&
                    reconnected.audio_errors == 0 && reconnected.audio_received >= 60 &&
                    reconnected.audio_decoded >= 60,
                "the reconnected audio/video session did not remain healthy");
        require(metrics->capture_instances.load(std::memory_order_relaxed) >= 2 &&
                    metrics->encoded_packets.load(std::memory_order_relaxed) >
                        packets_before_reconnect,
                "a fresh audio source was not started after reconnect");

        std::printf("audio-video-isolation: two sessions, synthetic mute A/B then B/A, "
                    "audio decode A=%llu B=%llu packets, reconnect=%llu/%llu, "
                    "video A/B=%llu/%llu, errors=%llu\n",
                    static_cast<unsigned long long>(
                        first_after.audio_decoded - first_before.audio_decoded),
                    static_cast<unsigned long long>(
                        second_after.audio_decoded - second_before.audio_decoded),
                    static_cast<unsigned long long>(reconnected.audio_received),
                    static_cast<unsigned long long>(reconnected.audio_decoded),
                    static_cast<unsigned long long>(first_video_after - first_video_before),
                    static_cast<unsigned long long>(second_video_after - second_video_before),
                    static_cast<unsigned long long>(first_after.audio_errors +
                                                    second_after.audio_errors +
                                                    reconnected.audio_errors));
        std::puts("audio-video-isolation: PASS two isolated silent Opus playback states, "
                  "decode while muted, 5 FPS video, sibling survival and reconnect; "
                  "no physical audio output opened");
        result = 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "audio-video-isolation: FAIL %s\n", exception.what());
    }
    if (first) first->Stop();
    if (second) second->Stop();
    if (host) host->stop();
    if (uninitialize_com) CoUninitialize();
    return result;
}

int elevation_probe() {
    using namespace chast;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct Task final : IElevationTask {
        std::atomic<ElevationCode> code{ElevationCode::Pending};
        std::atomic<bool> committed{false};
        ElevationState poll() override { return {0, code.load(), {}}; }
        void commit() override { committed = true; }
        void cancel() override {}
    };
    auto task = std::make_shared<Task>();
    auto paused = std::make_shared<std::atomic<bool>>(false);
    ProbeSink sink;
    std::unique_ptr<HostServer> host;
    std::unique_ptr<chast_viewer::ViewerEngine> viewer;
    std::string error;
    bool success = false;
    const auto require = [](bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); };
    const auto wait = [](auto predicate, std::chrono::milliseconds duration = 10s) {
        const auto until = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < until) {
            if (predicate()) return true;
            std::this_thread::sleep_for(20ms);
        }
        return predicate();
    };
    try {
        require(sink.create(&error), error);
        const auto port = pick_free_port();
        host = start_host({}, port, &error, false, {}, {}, {}, ClipboardDirection::Disabled, {}, {},
            [=](HostServerConfig& config) {
                config.settings.max_viewers = 1;
                config.settings.allow_input = true;
                config.settings.allow_audio = config.settings.allow_clipboard = false;
                config.settings.allow_terminal = false;
                config.settings.codec = VideoCodec::Vp9;
                config.encoder_backend = EncoderBackend::Software;
                config.elevation_state = [] { return ElevationState{0, ElevationCode::Available, {}}; };
                config.request_elevation = [task] { return task; };
                config.desktop_status = [paused] {
                    return *paused ? InputDesktopStatus{InputDesktopKind::Unavailable, false, {}, "simulated local UAC"}
                                   : input_desktop_status();
                };
                config.follow_desktop = [paused](std::string* reason, bool* switched) {
                    if (*paused) { *reason = "simulated local UAC"; *switched = false; return false; }
                    return follow_input_desktop(reason, switched);
                };
            });
        require(host != nullptr, error);
        viewer = std::make_unique<chast_viewer::ViewerEngine>(&sink);
        viewer->SetClipboardSync(false);
        require(viewer->Start("127.0.0.1", port, kDisplayAuto, host->status().fingerprint, false, {}, kPassword), "start elevation viewer");
        require(wait_streaming(*viewer, 20s, &error) && wait([&] { return viewer->Snapshot().elevation.code == ElevationCode::Available; }), error);
        require(viewer->RequestElevation(), "queue first elevation request");
        require(wait([&] { return viewer->Snapshot().elevation.code == ElevationCode::Pending; }), "pending response");
        const auto first_id = viewer->Snapshot().elevation.request_id;
        require(first_id && !viewer->RequestElevation(), "pending request cannot be duplicated");
        *paused = true;
        require(wait([&] { return viewer->Snapshot().desktop == DesktopKind::Unavailable; }), "viewer receives UAC capture pause");
        std::this_thread::sleep_for(500ms);
        const auto before = viewer->Snapshot().received;
        std::this_thread::sleep_for(22000ms);
        require(viewer->Snapshot().state == chast_viewer::ViewerState::kStreaming && viewer->Snapshot().received == before,
                "announced UAC pause survives the ordinary 20-second video stall deadline");
        task->code = ElevationCode::Cancelled; *paused = false;
        require(wait([&] { return viewer->Snapshot().elevation.code == ElevationCode::Cancelled && viewer->Snapshot().received > before; }),
                "cancelled UAC resumes real captured video");
        task->code = ElevationCode::Pending;
        require(viewer->RequestElevation(), "retry elevation after cancellation");
        require(wait([&] { return viewer->Snapshot().elevation.code == ElevationCode::Pending; }), "second pending response");
        require(viewer->Snapshot().elevation.request_id != first_id, "retry has a new correlation id");
        task->code = ElevationCode::Restarting;
        require(wait([&] { return task->committed && viewer->Snapshot().elevation.code == ElevationCode::Restarting; }),
                "native viewer receives restart state before handoff");
        std::puts("elevation-probe: PASS real DXGI/Auto encoder/QUIC/ViewerEngine, 22-second UAC pause, cancel/resume, retry and restart state");
        success = true;
    } catch (const std::exception& e) { std::fprintf(stderr, "elevation-probe: FAIL %s\n", e.what()); }
    if (viewer) viewer->Stop();
    if (host) host->stop();
    CoUninitialize();
    return success ? 0 : 1;
}

// Protocol-level acceptance for configured extra share roots: the receive
// folder and two configured folders appear as sorted top-level aliases, while
// browse, walk-download, upload and management operations all map through an
// alias. Unknown aliases, traversals and attempts to rename or remove a share
// root itself are refused.
int multi_share_probe() {
    const uint64_t suffix =
        (static_cast<uint64_t>(GetCurrentProcessId()) << 20) ^ GetTickCount64();
    const fs::path work = probe_scratch_root() /
                          ("chast-multi-share-probe-" + std::to_string(suffix));
    const fs::path receive = work / "receive";
    const fs::path share_a = work / "share-a";
    const fs::path share_b = work / "share-b";
    const fs::path staging = work / "staging";
    const std::string tree = "tree-" + std::to_string(suffix);
    const fs::path downloads = work / "downloads";
    const std::string downloads_text = utf8(downloads);

    std::string error;
    bool success = false;
    if (!prepare_receive_directory(downloads, &error) ||
        !write_file(receive / "receive-only.txt", "receive", &error) ||
        !write_file(share_a / tree / "a.txt", "alpha", &error) ||
        !write_file(share_a / tree / "sub" / "inner.txt", "inner", &error) ||
        !write_file(share_b / "b.txt", "bravo", &error) ||
        !write_file(staging / "upload.txt", "upload", &error)) {
        std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
        std::error_code ignored;
        fs::remove_all(work, ignored);
        return 1;
    }

    const uint16_t port = pick_free_port();
    ProbeSink sink;
    std::unique_ptr<chast::HostServer> host;
    if (port == 0) {
        error = "no free loopback port";
    } else if (!sink.create(&error)) {
        // keep the error
    } else {
        host = start_host(utf8(receive), port, &error, true, {}, {}, {},
                          chast::ClipboardDirection::Both, {}, {},
                          [&](chast::HostServerConfig& config) {
                              config.settings.file_share_roots = {utf8(share_a),
                                                                  utf8(share_b)};
                          });
    }
    std::unique_ptr<chast_viewer::ViewerEngine> engine;
    if (host) {
        engine = make_probe_viewer(&sink, downloads, &error);
        engine->SetClipboardSource([](chast::ClipboardContent&) { return true; });
        engine->SetClipboardFileSource([](std::vector<std::string>&) { return false; });
        if (!engine->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {},
                           kPassword, {}, {}, false,
                           chast_viewer::ViewerTransport::kAuto, {}, 0, {}, {},
                           true)) {
            error = "ViewerEngine::Start was refused";
            engine.reset();
        }
    }
    if (engine && wait_streaming(*engine, 20s, &error) &&
        wait_file_permission(*engine, 10s)) {
        chast_viewer::ViewerEngine& viewer = *engine;
        auto take_list = [&](chast::FileListResult* out) {
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            for (;;) {
                chast_viewer::ViewerEngine::FileListAnswer answer;
                if (viewer.TakeFileList(answer)) {
                    *out = std::move(answer.result);
                    return true;
                }
                if (viewer.Snapshot().state != chast_viewer::ViewerState::kStreaming) {
                    error = "the session ended while a listing was pending";
                    return false;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    error = "a listing answer did not arrive";
                    return false;
                }
                std::this_thread::sleep_for(5ms);
            }
        };
        auto list = [&](const std::string& directory, chast::FileListResult* out) {
            if (!viewer.RequestFileList(directory)) {
                error = "RequestFileList(" + directory + ") was refused locally";
                return false;
            }
            return take_list(out);
        };
        auto has_entry = [](const chast::FileListResult& result, const std::string& name,
                            bool directory) {
            for (const chast::FileListEntry& entry : result.entries) {
                if (entry.name == name && entry.directory == directory) {
                    return true;
                }
            }
            return false;
        };
        auto wait_upload = [&](uint64_t sent_before, uint64_t failures_before) {
            // 1 sent, 0 failed, -1 session/timeout.
            const auto deadline = std::chrono::steady_clock::now() + 60s;
            for (;;) {
                const chast_viewer::ViewerStatus status = viewer.Snapshot();
                if (status.files_sent > sent_before) {
                    return 1;
                }
                if (status.file_failures > failures_before) {
                    error = status.last_file_error;
                    return 0;
                }
                if (status.state != chast_viewer::ViewerState::kStreaming) {
                    error = "the session ended during an upload";
                    return -1;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    error = "the upload did not finish";
                    return -1;
                }
                std::this_thread::sleep_for(5ms);
            }
        };
        auto operation = [&](chast::FileOperation op, const std::string& path,
                             const std::string& new_name, int* code) {
            const uint64_t before = viewer.Snapshot().file_op_answers;
            if (!viewer.RequestFileOperation(op, path, new_name)) {
                error = "RequestFileOperation(" + path + ") was refused locally";
                return false;
            }
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            for (;;) {
                const chast_viewer::ViewerStatus status = viewer.Snapshot();
                if (status.file_op_answers > before) {
                    *code = status.file_op_code;
                    error = status.file_op_error;
                    return true;
                }
                if (status.state != chast_viewer::ViewerState::kStreaming) {
                    error = "the session ended while an operation was pending";
                    return false;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    error = "a file operation answer did not arrive";
                    return false;
                }
                std::this_thread::sleep_for(5ms);
            }
        };

        chast::FileListResult listing;
        WalkOutcome walk;
        std::string bytes;
        int code = -1;
        const int refused = static_cast<int>(chast::FileCode::Refused);
        const chast_viewer::ViewerStatus upload_before = viewer.Snapshot();
        std::error_code ignored;
        if (!list("", &listing)) {
            // error set
        } else if (listing.code != chast::FileCode::Ok || listing.entries.size() != 3 ||
                   listing.entries[0].name != "receive" ||
                   listing.entries[1].name != "share-a" ||
                   listing.entries[2].name != "share-b" || !listing.entries[0].directory ||
                   !listing.entries[1].directory || !listing.entries[2].directory) {
            error = "the top-level listing is not the three sorted share aliases";
        } else if (!list("share-a", &listing) || listing.code != chast::FileCode::Ok ||
                   listing.entries.size() != 1 || !has_entry(listing, tree, true)) {
            error = "share-a does not list its seeded folder";
        } else if (!list("share-a/" + tree, &listing) ||
                   listing.code != chast::FileCode::Ok || listing.entries.size() != 2 ||
                   !has_entry(listing, "sub", true) || !has_entry(listing, "a.txt", false)) {
            error = "a nested folder inside share-a does not list its entries";
        } else if (!list("share-a/" + tree + "/sub", &listing) ||
                   listing.code != chast::FileCode::Ok ||
                   !has_entry(listing, "inner.txt", false)) {
            error = "a two-level folder inside share-a does not list its file";
        } else if (!list("share-b", &listing) || listing.code != chast::FileCode::Ok ||
                   !has_entry(listing, "b.txt", false)) {
            error = "share-b does not list its seeded file";
        } else if (!list("missing-alias", &listing) ||
                   listing.code != chast::FileCode::NotFound) {
            error = "an unknown alias did not report NotFound";
        } else if (viewer.RequestFileList("../outside")) {
            error = "a traversal was accepted for listing";
        } else if (!run_walk(viewer, "share-a/" + tree, 60s, &walk, &error)) {
            // error set
        } else if (walk.code != 0 || walk.total != 2 || walk.done != 2) {
            error = "the share-a walk reported code " + std::to_string(walk.code) + " with " +
                    std::to_string(walk.done) + "/" + std::to_string(walk.total) + " files";
        } else if (!read_file(downloads / tree / "a.txt", &bytes, &error) ||
                   bytes != "alpha" ||
                   !read_file(downloads / tree / "sub" / "inner.txt", &bytes, &error) ||
                   bytes != "inner") {
            error = "the share-a walk did not mirror the seeded tree";
        } else if (!read_file(receive / "receive-only.txt", &bytes, &error) ||
                   bytes != "receive") {
            error = "the receive folder changed during the share walk";
        } else if (!viewer.SendFiles({utf8(staging / "upload.txt")}, "share-b/incoming")) {
            error = "SendFiles into share-b was refused locally";
        } else if (wait_upload(upload_before.files_sent, upload_before.file_failures) != 1) {
            // error set by wait_upload
        } else if (!read_file(share_b / "incoming" / "upload.txt", &bytes, &error) ||
                   bytes != "upload") {
            error = "the upload did not land below share-b/incoming";
        } else {
            const chast_viewer::ViewerStatus refused_before = viewer.Snapshot();
            if (!viewer.SendFiles({utf8(staging / "upload.txt")}, "missing-alias")) {
                error = "an upload to an unknown alias was refused locally";
            } else if (wait_upload(refused_before.files_sent, refused_before.file_failures) != 0) {
                error = "an upload to an unknown alias was not refused";
            } else if (fs::exists(work / "missing-alias", ignored) ||
                       fs::exists(receive / "missing-alias", ignored) ||
                       fs::exists(share_a / "missing-alias", ignored) ||
                       fs::exists(share_b / "missing-alias", ignored)) {
                error = "a refused upload created files";
            } else if (!operation(chast::FileOperation::MakeDirectory, "share-b/made", "",
                                  &code) ||
                       code != 0 || !fs::is_directory(share_b / "made", ignored)) {
                error = "creating a folder inside share-b failed: " + error;
            } else if (!operation(chast::FileOperation::Rename, "share-b/made", "renamed",
                                  &code) ||
                       code != 0 || !fs::is_directory(share_b / "renamed", ignored) ||
                       fs::exists(share_b / "made", ignored)) {
                error = "renaming a folder inside share-b failed: " + error;
            } else if (!operation(chast::FileOperation::Remove, "share-b/renamed", "", &code) ||
                       code != 0 || fs::exists(share_b / "renamed", ignored)) {
                error = "removing a folder inside share-b failed: " + error;
            } else if (!operation(chast::FileOperation::Rename, "share-b", "renamed-root",
                                  &code) ||
                       code != refused || !fs::is_directory(share_b, ignored)) {
                error = "a share root itself accepted a rename (code " +
                        std::to_string(code) + ")";
            } else if (!operation(chast::FileOperation::RemoveTree, "share-b", "", &code) ||
                       code != refused || !fs::is_directory(share_b, ignored) ||
                       !fs::exists(share_b / "b.txt", ignored)) {
                error = "a share root itself accepted a recursive removal (code " +
                        std::to_string(code) + ")";
            } else {
                std::printf(
                    "viewer-probe: multi-share session -> 3 sorted aliases, nested listing "
                    "in share-a, 2 files walked from share-a, upload landed in "
                    "share-b/incoming, unknown alias and root rename/remove refused\n");
                success = true;
            }
        }
        if (!success && !error.empty()) {
            std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
        }
        std::filesystem::remove_all(downloads / tree, ignored);
    } else if (!error.empty()) {
        std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
    }
    if (engine) {
        engine->Stop();
    }
    if (host) {
        host->stop();
    }
    std::error_code ignored;
    fs::remove_all(work, ignored);
    return success ? 0 : 1;
}

// Protocol v69: a destination that already holds the offered name is never
// renamed silently. The viewer prompts, Skip keeps the existing file without
// counting a failure, Overwrite replaces it, and Cancel drops the queue; a
// remembered answer then covers both directions for the rest of the session.
int file_conflict_probe() {
    const uint64_t suffix =
        (static_cast<uint64_t>(GetCurrentProcessId()) << 20) ^ GetTickCount64();
    const fs::path work = probe_scratch_root() /
                          ("chast-file-conflict-probe-" + std::to_string(suffix));
    const fs::path receive = work / "receive";
    const fs::path share = work / "share";
    const fs::path staging = work / "staging";
    const std::string upload_name = "clash-" + std::to_string(suffix) + ".txt";
    const std::string download_name = "remote-" + std::to_string(suffix) + ".txt";
    const fs::path downloads = work / "downloads";

    std::string error;
    if (!prepare_receive_directory(downloads, &error) ||
        !write_file(receive / "incoming" / upload_name, "host-old", &error) ||
        !write_file(share / download_name, "host-remote", &error) ||
        !write_file(downloads / download_name, "local-old", &error) ||
        !write_file(staging / upload_name, "viewer-new", &error)) {
        std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
        std::error_code ignored;
        fs::remove_all(work, ignored);
        return 1;
    }

    const uint16_t port = pick_free_port();
    ProbeSink sink;
    std::unique_ptr<chast::HostServer> host;
    if (port == 0) {
        error = "no free loopback port";
    } else if (!sink.create(&error)) {
        // keep the error
    } else {
        host = start_host(utf8(receive), port, &error, true, {}, {}, {},
                          chast::ClipboardDirection::Both, {}, {},
                          [&](chast::HostServerConfig& config) {
                              config.settings.file_share_roots = {utf8(share)};
                          });
    }
    std::unique_ptr<chast_viewer::ViewerEngine> engine;
    if (host) {
        engine = make_probe_viewer(&sink, downloads, &error);
        engine->SetClipboardSource([](chast::ClipboardContent&) { return true; });
        engine->SetClipboardFileSource([](std::vector<std::string>&) { return false; });
        if (!engine->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {},
                           kPassword, {}, {}, false,
                           chast_viewer::ViewerTransport::kAuto, {}, 0, {}, {},
                           true)) {
            error = "ViewerEngine::Start was refused";
            engine.reset();
        }
    }

    bool success = false;
    if (engine && wait_streaming(*engine, 20s, &error) &&
        wait_file_permission(*engine, 10s)) {
        chast_viewer::ViewerEngine& viewer = *engine;
        auto fail = [&](std::string message) {
            error = std::move(message);
            return false;
        };
        auto wait_conflict = [&](bool download, const std::string& name,
                                 uint64_t existing_size, bool identical,
                                 uint32_t* transfer_id) {
            const auto deadline = std::chrono::steady_clock::now() + 20s;
            for (;;) {
                const chast_viewer::ViewerStatus status = viewer.Snapshot();
                if (status.file_conflict_pending) {
                    if (status.file_conflict_download != download ||
                        status.file_conflict_name != name) {
                        return fail("the prompt named '" + status.file_conflict_name +
                                    "' (download " +
                                    std::to_string(status.file_conflict_download) +
                                    "), expected the " +
                                    (download ? "download" : "upload") + " of " + name);
                    }
                    if (status.file_conflict_existing_size != existing_size) {
                        return fail("the prompt reported an existing size of " +
                                    std::to_string(status.file_conflict_existing_size) +
                                    ", expected " + std::to_string(existing_size));
                    }
                    if (status.file_conflict_identical != identical) {
                        return fail("the prompt reported identical=" +
                                    std::to_string(status.file_conflict_identical));
                    }
                    if (status.file_conflict_transfer_id == 0) {
                        return fail("the prompt carried no transfer id");
                    }
                    *transfer_id = status.file_conflict_transfer_id;
                    return true;
                }
                if (status.state != chast_viewer::ViewerState::kStreaming) {
                    return fail("the session ended while a conflict was expected");
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    return fail("the conflict prompt for " + name + " did not appear");
                }
                std::this_thread::sleep_for(5ms);
            }
        };
        // Waits for a success counter to advance; a failure counter moving
        // instead is an error, and `prompted` records a prompt that should not
        // have appeared under a remembered answer.
        auto wait_progress = [&](uint64_t chast_viewer::ViewerStatus::*counter,
                                 uint64_t before,
                                 uint64_t chast_viewer::ViewerStatus::*failures,
                                 const char* what, bool* prompted) {
            const uint64_t failures_before = viewer.Snapshot().*failures;
            const auto deadline = std::chrono::steady_clock::now() + 30s;
            for (;;) {
                const chast_viewer::ViewerStatus status = viewer.Snapshot();
                if (status.*counter > before) {
                    return true;
                }
                if (prompted && status.file_conflict_pending) {
                    *prompted = true;
                }
                if (status.*failures != failures_before) {
                    return fail(std::string(what) + " was counted as a failure");
                }
                if (status.state != chast_viewer::ViewerState::kStreaming) {
                    return fail(std::string(what) + ": the session ended");
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    return fail(std::string(what) + " did not finish");
                }
                std::this_thread::sleep_for(5ms);
            }
        };

        bool ok = true;
        std::string bytes;
        uint32_t transfer_id = 0;
        chast_viewer::ViewerStatus before = viewer.Snapshot();
        do {
            // 1. A host download whose name already exists locally prompts;
            // Overwrite replaces the local file in place.
            if (!viewer.RequestFileDownload("share/" + download_name)) {
                ok = fail("RequestFileDownload(" + download_name + ") was refused");
                break;
            }
            if (!wait_conflict(true, download_name, 9, false, &transfer_id)) {
                ok = false;
                break;
            }
            if (!viewer.AnswerFileConflict(chast::FileConflictDecision::Overwrite)) {
                ok = fail("AnswerFileConflict(Overwrite) was refused");
                break;
            }
            if (!wait_progress(&chast_viewer::ViewerStatus::files_received,
                               before.files_received,
                               &chast_viewer::ViewerStatus::file_receive_failures,
                               "the overwritten download", nullptr)) {
                ok = false;
                break;
            }
            if (!read_file(downloads / download_name, &bytes, &error)) {
                ok = false;
                break;
            }
            if (bytes != "host-remote") {
                ok = fail("the overwritten download did not replace the local file");
                break;
            }

            // 2. Cancel releases the held offer and keeps the host's file.
            before = viewer.Snapshot();
            if (!viewer.SendFiles({utf8(staging / upload_name)}, "receive/incoming")) {
                ok = fail("SendFiles(" + upload_name + ") was refused");
                break;
            }
            if (!wait_conflict(false, upload_name, 8, false, &transfer_id)) {
                ok = false;
                break;
            }
            if (!viewer.AnswerFileConflict(chast::FileConflictDecision::Cancel)) {
                ok = fail("AnswerFileConflict(Cancel) was refused");
                break;
            }
            if (!wait_progress(&chast_viewer::ViewerStatus::file_cancels,
                               before.file_cancels,
                               &chast_viewer::ViewerStatus::file_failures,
                               "the cancelled upload", nullptr)) {
                ok = false;
                break;
            }
            if (!read_file(receive / "incoming" / upload_name, &bytes, &error)) {
                ok = false;
                break;
            }
            if (bytes != "host-old") {
                ok = fail("a cancelled upload changed the host's file");
                break;
            }

            // 3. Skip keeps the host's file and is counted apart from failures.
            before = viewer.Snapshot();
            if (!viewer.SendFiles({utf8(staging / upload_name)}, "receive/incoming")) {
                ok = fail("SendFiles(" + upload_name + ") was refused");
                break;
            }
            if (!wait_conflict(false, upload_name, 8, false, &transfer_id)) {
                ok = false;
                break;
            }
            if (!viewer.AnswerFileConflict(chast::FileConflictDecision::Skip)) {
                ok = fail("AnswerFileConflict(Skip) was refused");
                break;
            }
            if (!wait_progress(&chast_viewer::ViewerStatus::files_skipped,
                               before.files_skipped,
                               &chast_viewer::ViewerStatus::file_failures,
                               "the skipped upload", nullptr)) {
                ok = false;
                break;
            }
            if (!read_file(receive / "incoming" / upload_name, &bytes, &error)) {
                ok = false;
                break;
            }
            if (bytes != "host-old") {
                ok = fail("a skipped upload changed the host's file");
                break;
            }
            if (viewer.Snapshot().files_sent != before.files_sent) {
                ok = fail("a skipped upload was counted as sent");
                break;
            }

            // 4. Overwrite together with "remember" replaces the host's file
            // and stores the policy on the viewer.
            before = viewer.Snapshot();
            if (!viewer.SendFiles({utf8(staging / upload_name)}, "receive/incoming")) {
                ok = fail("SendFiles(" + upload_name + ") was refused");
                break;
            }
            if (!wait_conflict(false, upload_name, 8, false, &transfer_id)) {
                ok = false;
                break;
            }
            if (!viewer.AnswerFileConflict(chast::FileConflictDecision::Overwrite, true)) {
                ok = fail("AnswerFileConflict(Overwrite, remember) was refused");
                break;
            }
            if (!wait_progress(&chast_viewer::ViewerStatus::files_sent, before.files_sent,
                               &chast_viewer::ViewerStatus::file_failures,
                               "the overwriting upload", nullptr)) {
                ok = false;
                break;
            }
            if (!read_file(receive / "incoming" / upload_name, &bytes, &error)) {
                ok = false;
                break;
            }
            if (bytes != "viewer-new") {
                ok = fail("the overwriting upload did not replace the host's file");
                break;
            }
            if (viewer.Snapshot().file_conflict_policy != 1) {
                ok = fail("the remembered overwrite was not stored");
                break;
            }

            // 5. The remembered answer covers the rest of the queue: the same
            // name again finishes without a prompt.
            before = viewer.Snapshot();
            bool prompted = false;
            if (!viewer.SendFiles({utf8(staging / upload_name)}, "receive/incoming")) {
                ok = fail("SendFiles(" + upload_name + ") was refused");
                break;
            }
            if (!wait_progress(&chast_viewer::ViewerStatus::files_sent, before.files_sent,
                               &chast_viewer::ViewerStatus::file_failures,
                               "the remembered overwrite upload", &prompted)) {
                ok = false;
                break;
            }
            if (prompted) {
                ok = fail("a remembered answer still prompted for the next upload");
                break;
            }
            if (!read_file(receive / "incoming" / upload_name, &bytes, &error)) {
                ok = false;
                break;
            }
            if (bytes != "viewer-new") {
                ok = fail("the remembered upload changed the host's file");
                break;
            }

            // 6. The same remembered answer applies to a download.
            before = viewer.Snapshot();
            prompted = false;
            if (!viewer.RequestFileDownload("share/" + download_name)) {
                ok = fail("the remembered RequestFileDownload was refused");
                break;
            }
            if (!wait_progress(&chast_viewer::ViewerStatus::files_received,
                               before.files_received,
                               &chast_viewer::ViewerStatus::file_receive_failures,
                               "the remembered overwrite download", &prompted)) {
                ok = false;
                break;
            }
            if (prompted) {
                ok = fail("a remembered answer still prompted for a download");
                break;
            }
            if (!read_file(downloads / download_name, &bytes, &error)) {
                ok = false;
                break;
            }
            if (bytes != "host-remote") {
                ok = fail("the remembered download changed the local file");
                break;
            }
        } while (false);

        if (ok && error.empty()) {
            std::printf(
                "viewer-probe: file conflicts -> download prompt/overwrite, upload "
                "cancel/skip/overwrite, remembered answer covers uploads and downloads\n");
            success = true;
        } else if (!error.empty()) {
            std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
        }
    } else if (!error.empty()) {
        std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
    }
    if (engine) {
        engine->Stop();
    }
    if (host) {
        host->stop();
    }
    std::error_code ignored;
    fs::remove_all(work, ignored);
    fs::remove(downloads / download_name, ignored);
    return success ? 0 : 1;
}

// Protocol v74: selected folders travel as directory offers. The receiver must
// create even an empty tree, while only regular files count as sent files or
// produce file-audit entries. A Windows junction is never a folder to follow:
// as a selected root it fails, and inside a tree it is skipped.
int directory_upload_probe() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const uint64_t suffix =
        (static_cast<uint64_t>(GetCurrentProcessId()) << 20) ^ GetTickCount64();
    const std::string work_name =
        "chast-directory-upload-probe-" + std::to_string(suffix);
    fs::path work = probe_scratch_root() / work_name;
    std::error_code prepare;
    fs::create_directories(work, prepare);
    if (prepare) {
        // Some Windows test hosts run downloaded/build binaries with a token
        // that cannot write the user temp directory. Keep the fallback inside
        // the current build tree instead of turning a host-policy restriction
        // into a directory-upload failure.
        work = fs::current_path() / "test-temp" / work_name;
        prepare.clear();
        fs::create_directories(work, prepare);
    }
    if (prepare) {
        std::fprintf(stderr, "viewer-probe: cannot create the test work directory: %s\n",
                     prepare.message().c_str());
        return 1;
    }
    const fs::path receive = work / "receive";
    const fs::path staging = work / "staging";
    const fs::path outside = work / "outside";
    const fs::path root_junction = staging / "root-junction";
    const fs::path tree_junction = staging / "junction-tree" / "jump";
    const std::string payload = std::string("\x00payload\x01", 9);
    const std::string nested_payload = "nested payload";
    const std::string marker_payload = "must not escape the junction";

    std::string error;
    if (!write_file(staging / "mixed-tree" / "payload.bin", payload, &error) ||
        !write_file(staging / "mixed-tree" / "nested-empty" / "a" / "nested.txt",
                    nested_payload, &error) ||
        !write_file(staging / "zero.txt", {}, &error) ||
        !write_file(staging / "junction-tree" / "before.txt", "before", &error) ||
        !write_file(staging / "junction-tree" / "after" / "after.txt", "after",
                    &error) ||
        !write_file(outside / "marker.txt", marker_payload, &error)) {
        std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
        std::error_code ignored;
        fs::remove_all(work, ignored);
        return 1;
    }
    std::error_code ec;
    fs::create_directories(staging / "empty-root", ec);
    if (!ec) fs::create_directories(staging / "nested-empty" / "level-one" / "level-two", ec);
    if (!ec) {
        fs::create_directories(staging / "mixed-tree" / "empty-child", ec);
    }
    if (!ec) {
        fs::create_directories(staging / "mixed-tree" / "nested-empty" / "a" / "b", ec);
    }
    if (!ec) fs::create_directories(receive / "incoming", ec);
    if (ec) {
        std::fprintf(stderr, "viewer-probe: cannot seed directory upload: %s\n",
                     ec.message().c_str());
        fs::remove_all(work, ec);
        return 1;
    }
    if (!create_directory_reparse(root_junction, outside, &error) ||
        !create_directory_reparse(tree_junction, outside, &error)) {
        std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
        remove_directory_reparse(tree_junction);
        remove_directory_reparse(root_junction);
        std::error_code ignored;
        fs::remove_all(work, ignored);
        return 1;
    }

    const uint16_t port = pick_free_port();
    ProbeSink sink;
    std::unique_ptr<chast::HostServer> host;
    if (port == 0) {
        error = "no free loopback port";
    } else if (!sink.create(&error)) {
        // keep the error
    } else {
        host = start_host(utf8(receive), port, &error);
    }
    std::unique_ptr<chast_viewer::ViewerEngine> engine;
    if (host) {
        engine = std::make_unique<chast_viewer::ViewerEngine>(&sink);
        engine->SetClipboardSource([](chast::ClipboardContent&) { return true; });
        engine->SetClipboardFileSource([](std::vector<std::string>&) { return false; });
        if (!engine->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {},
                           kPassword, {}, {}, false,
                           chast_viewer::ViewerTransport::kAuto, {}, 0, {}, {},
                           true)) {
            error = "ViewerEngine::Start was refused";
            engine.reset();
        }
    }

    bool success = false;
    if (engine && wait_streaming(*engine, 20s, &error) &&
        wait_file_permission(*engine, 10s)) {
        chast_viewer::ViewerEngine& viewer = *engine;
        auto fail = [&](std::string message) {
            error = std::move(message);
            return false;
        };
        auto check_bytes = [&](const fs::path& path, const std::string& expected) {
            std::string actual;
            return read_file(path, &actual, &error) && actual == expected;
        };
        do {
            const chast_viewer::ViewerStatus before = viewer.Snapshot();
            const std::vector<std::string> roots = {
                utf8(staging / "empty-root"), utf8(staging / "nested-empty"),
                utf8(staging / "mixed-tree"), utf8(staging / "zero.txt")};
            if (!viewer.SendFiles(roots, "incoming")) {
                fail("SendFiles for directories and files was refused");
                break;
            }
            const std::vector<fs::path> directories = {
                receive / "incoming" / "empty-root",
                receive / "incoming" / "nested-empty",
                receive / "incoming" / "nested-empty" / "level-one",
                receive / "incoming" / "nested-empty" / "level-one" / "level-two",
                receive / "incoming" / "mixed-tree",
                receive / "incoming" / "mixed-tree" / "empty-child",
                receive / "incoming" / "mixed-tree" / "nested-empty",
                receive / "incoming" / "mixed-tree" / "nested-empty" / "a",
                receive / "incoming" / "mixed-tree" / "nested-empty" / "a" / "b"};
            if (!wait_upload_settled(viewer, before.files_sent + 3,
                                     before.file_failures, directories, 30s, &error)) {
                break;
            }
            const chast_viewer::ViewerStatus after = viewer.Snapshot();
            if (after.file_failures != before.file_failures ||
                after.files_skipped != before.files_skipped) {
                fail("successful directory offers affected the upload failure counters");
                break;
            }
            if (!check_bytes(receive / "incoming" / "mixed-tree" / "payload.bin",
                             payload) ||
                !check_bytes(receive / "incoming" / "mixed-tree" / "nested-empty" /
                                 "a" / "nested.txt", nested_payload)) {
                fail("uploaded file bytes do not match their sources");
                break;
            }
            const fs::path zero = receive / "incoming" / "zero.txt";
            if (!fs::is_regular_file(zero, ec) || ec ||
                fs::file_size(zero, ec) != 0 || ec) {
                fail("a zero-byte regular file did not arrive as a file");
                break;
            }
            std::vector<chast_viewer::ViewerEngine::CompletedFile> completed;
            if (!viewer.TakeCompletedFiles(completed) || completed.size() != 3) {
                fail("directory upload produced " + std::to_string(completed.size()) +
                     " audit entries for 3 regular files");
                break;
            }
            bool saw_payload = false;
            bool saw_nested = false;
            bool saw_zero = false;
            bool audit_ok = true;
            for (const auto& file : completed) {
                audit_ok = audit_ok && file.upload && file.success;
                saw_payload = saw_payload || (file.name == "payload.bin" &&
                                               file.size == payload.size());
                saw_nested = saw_nested || (file.name == "nested.txt" &&
                                             file.size == nested_payload.size());
                saw_zero = saw_zero || (file.name == "zero.txt" && file.size == 0);
            }
            if (!audit_ok || !saw_payload || !saw_nested || !saw_zero) {
                fail("the file audit did not contain exactly the three regular files");
                break;
            }

            // A junction selected as an upload root is a failed folder, not a
            // file or an empty directory, and it must not enter the file audit.
            const chast_viewer::ViewerStatus root_before = viewer.Snapshot();
            if (!viewer.SendFiles({utf8(root_junction)}, "incoming")) {
                fail("SendFiles for a junction root was refused locally");
                break;
            }
            if (!wait_upload_settled(viewer, root_before.files_sent,
                                     root_before.file_failures + 1, {}, 15s, &error)) {
                break;
            }
            const chast_viewer::ViewerStatus root_failed = viewer.Snapshot();
            if (root_failed.last_file_name != "root-junction" ||
                root_failed.last_file_error.find("reparse points") == std::string::npos) {
                fail("the junction root did not report a reparse-point failure");
                break;
            }
            if (fs::exists(receive / "incoming" / "root-junction", ec) || ec) {
                fail("the failed junction root created a destination entry");
                break;
            }
            std::vector<chast_viewer::ViewerEngine::CompletedFile> failed_audit;
            if (viewer.TakeCompletedFiles(failed_audit)) {
                fail("a failed directory offer entered the file audit");
                break;
            }

            // Inside an ordinary tree the junction (and its outside target) is
            // skipped without becoming a failure or an empty destination folder.
            const chast_viewer::ViewerStatus tree_before = viewer.Snapshot();
            if (!viewer.SendFiles({utf8(staging / "junction-tree")}, "incoming")) {
                fail("SendFiles for a tree containing a junction was refused");
                break;
            }
            const std::vector<fs::path> tree_directories = {
                receive / "incoming" / "junction-tree",
                receive / "incoming" / "junction-tree" / "after"};
            if (!wait_upload_settled(viewer, tree_before.files_sent + 2,
                                     tree_before.file_failures, tree_directories,
                                     15s, &error)) {
                break;
            }
            if (!check_bytes(receive / "incoming" / "junction-tree" / "before.txt",
                             "before") ||
                !check_bytes(receive / "incoming" / "junction-tree" / "after" /
                                 "after.txt", "after")) {
                fail("the ordinary files around a junction were not uploaded");
                break;
            }
            if (fs::exists(receive / "incoming" / "junction-tree" / "jump", ec) || ec) {
                fail("a junction inside the tree created a destination folder");
                break;
            }
            std::vector<chast_viewer::ViewerEngine::CompletedFile> tree_audit;
            if (!viewer.TakeCompletedFiles(tree_audit) || tree_audit.size() != 2 ||
                tree_audit[0].name == "jump" || tree_audit[1].name == "jump") {
                fail("the tree audit did not contain only its two regular files");
                break;
            }
            fs::recursive_directory_iterator entry(receive, ec);
            const fs::recursive_directory_iterator end;
            bool marker_uploaded = false;
            while (!ec && entry != end) {
                if (entry->is_regular_file(ec) && entry->path().filename() == "marker.txt") {
                    marker_uploaded = true;
                    break;
                }
                entry.increment(ec);
            }
            if (ec || marker_uploaded) {
                fail("a junction target file escaped into the receive tree");
                break;
            }
            if (!check_bytes(outside / "marker.txt", marker_payload)) {
                fail("the junction target was modified");
                break;
            }
            success = true;
        } while (false);
        if (!success && !error.empty()) {
            std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
        }
    } else if (!error.empty()) {
        std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
    }
    if (engine) engine->Stop();
    if (host) host->stop();
    remove_directory_reparse(tree_junction);
    remove_directory_reparse(root_junction);
    std::error_code ignored;
    fs::remove_all(work, ignored);
    if (success) {
        std::printf("viewer-probe: directory upload -> 9 folders (including empty trees), "
                    "3 regular files, zero-byte file, junction root refused, nested "
                    "junction skipped, no directory audit entries\n");
    }
    return success ? 0 : 1;
}

// Keyboard acceptance against the real Windows input stack. The viewer-side
// hook mapping is covered by keyboard_test; this probe covers the half that
// needs a desktop: keys injected by the host session land in a real edit
// control, and the host layout, the host IME or a Unicode commit decides the
// text. The window belongs to this process and stays foreground for the run,
// so no other application receives the injected keys.
constexpr wchar_t kKeyboardClass[] = L"chast-keyboard-acceptance";

struct KeyboardWindow {
    HWND window = nullptr;
    HWND edit = nullptr;
};

LRESULT CALLBACK keyboard_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void pump_messages(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    for (;;) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(2ms);
    }
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring edit_text(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    if (length <= 0) return {};
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, buffer.data(), length + 1);
    return std::wstring(buffer.data(), static_cast<size_t>(length));
}

bool create_keyboard_window(KeyboardWindow& target, std::string* error) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = keyboard_window_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    cls.lpszClassName = kKeyboardClass;
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        *error = "RegisterClassEx failed with " + std::to_string(GetLastError());
        return false;
    }
    target.window = CreateWindowExW(
        WS_EX_TOPMOST, kKeyboardClass, L"chast keyboard acceptance", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 260, nullptr, nullptr, instance, nullptr);
    if (!target.window) {
        *error = "CreateWindowEx failed with " + std::to_string(GetLastError());
        return false;
    }
    target.edit = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                               WS_VSCROLL | ES_LEFT | WS_TABSTOP,
        0, 0, 0, 0, target.window, nullptr, instance, nullptr);
    if (!target.edit) {
        *error = "creating the edit control failed with " +
                 std::to_string(GetLastError());
        return false;
    }
    RECT client{};
    GetClientRect(target.window, &client);
    MoveWindow(target.edit, 0, 0, client.right, client.bottom, TRUE);
    ShowWindow(target.window, SW_SHOW);
    UpdateWindow(target.window);
    SetFocus(target.edit);
    pump_messages(120ms);
    return true;
}

void destroy_keyboard_window(KeyboardWindow& target) {
    if (target.window) DestroyWindow(target.window);
    target = KeyboardWindow{};
    pump_messages(50ms);
}

// SetForegroundWindow is refused when the caller is not the foreground
// process; attaching to the current foreground thread is the standard escape.
// The window stays topmost for the run so nothing can cover it, and every key
// is sent only after this check passes.
bool ensure_keyboard_foreground(const KeyboardWindow& target) {
    if (GetForegroundWindow() != target.window) {
        const HWND foreground = GetForegroundWindow();
        const DWORD foreground_thread =
            foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        const DWORD own_thread = GetCurrentThreadId();
        const bool attached =
            foreground_thread != 0 && foreground_thread != own_thread;
        if (attached) AttachThreadInput(own_thread, foreground_thread, TRUE);
        BringWindowToTop(target.window);
        SetForegroundWindow(target.window);
        SetFocus(target.edit);
        if (attached) AttachThreadInput(own_thread, foreground_thread, FALSE);
        pump_messages(40ms);
    }
    return GetForegroundWindow() == target.window && GetFocus() == target.edit;
}

// The injector compares this bitmap with the host's own lock state and taps a
// lock key when they disagree, so reporting the real state keeps the operator's
// Caps/Num/Scroll untouched during the run.
uint16_t current_locks() {
    uint16_t bits = 0;
    if ((GetKeyState(VK_CAPITAL) & 1) != 0) bits |= chast::kLockCaps;
    if ((GetKeyState(VK_NUMLOCK) & 1) != 0) bits |= chast::kLockNum;
    if ((GetKeyState(VK_SCROLL) & 1) != 0) bits |= chast::kLockScroll;
    return bits;
}

bool send_probe_key(chast_viewer::ViewerEngine& viewer, uint16_t usage, bool down,
                    uint16_t modifiers) {
    chast::InputEvent event;
    event.kind = chast::InputKind::Key;
    event.key.action = down ? chast::KeyAction::Down : chast::KeyAction::Up;
    event.key.hid_usage = usage;
    event.modifiers = modifiers;
    return viewer.SendInput(event);
}

bool send_legacy_key(chast_viewer::ViewerEngine& viewer, uint16_t virtual_key,
                     bool down, uint16_t modifiers) {
    chast::InputEvent event;
    event.kind = chast::InputKind::LegacyKey;
    event.legacy_key.action = down ? chast::KeyAction::Down : chast::KeyAction::Up;
    event.legacy_key.virtual_key = virtual_key;
    event.modifiers = modifiers;
    return viewer.SendInput(event);
}

bool send_local_probe_key(uint16_t usage, bool down) {
    INPUT input{};
    return chast::make_scancode_input(usage, down, input) &&
           SendInput(1, &input, sizeof(input)) == 1;
}

bool pump_until_text(HWND edit, const std::wstring& expected,
                     std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        pump_messages(10ms);
        if (edit_text(edit) == expected) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
    }
}

bool type_usage(chast_viewer::ViewerEngine& viewer, uint16_t usage,
                uint16_t held, uint16_t locks) {
    if (!send_probe_key(viewer, usage, true, held | locks)) return false;
    pump_messages(75ms);
    if (!send_probe_key(viewer, usage, false, held | locks)) return false;
    pump_messages(75ms);
    return true;
}

bool type_shifted(chast_viewer::ViewerEngine& viewer, uint16_t usage,
                  uint16_t locks) {
    const uint16_t held = chast::kModifierLeftShift;
    if (!send_probe_key(viewer, 0xE1, true, held | locks)) return false;
    pump_messages(75ms);
    if (!send_probe_key(viewer, usage, true, held | locks)) return false;
    pump_messages(75ms);
    if (!send_probe_key(viewer, usage, false, held | locks)) return false;
    pump_messages(75ms);
    if (!send_probe_key(viewer, 0xE1, false, locks)) return false;
    pump_messages(75ms);
    return true;
}

bool type_altgr(chast_viewer::ViewerEngine& viewer, uint16_t usage,
                uint16_t locks) {
    const uint16_t held = chast::kModifierRightAlt;
    if (!send_probe_key(viewer, 0xE6, true, held | locks)) return false;
    pump_messages(75ms);
    if (!send_probe_key(viewer, usage, true, held | locks)) return false;
    pump_messages(75ms);
    if (!send_probe_key(viewer, usage, false, held | locks)) return false;
    pump_messages(75ms);
    if (!send_probe_key(viewer, 0xE6, false, locks)) return false;
    pump_messages(75ms);
    return true;
}

bool activate_keyboard_layout(HKL layout, std::string* error) {
    if (layout == nullptr ||
        ActivateKeyboardLayout(layout, KLF_SETFORPROCESS) == nullptr) {
        *error = "ActivateKeyboardLayout failed with " +
                 std::to_string(GetLastError());
        return false;
    }
    pump_messages(500ms);
    wchar_t active[KL_NAMELENGTH]{};
    if (GetKeyboardLayoutNameW(active)) {
        std::printf("viewer-probe: active keyboard layout %ls\n", active);
    }
    return true;
}

// Chooses an installed Chinese IME layout and opens it in native (Chinese)
// mode. Returns the layout, or null with `error` set when none is installed.
HKL prepare_host_ime(KeyboardWindow& target, std::string* error) {
    HKL ime = nullptr;
    for (const wchar_t* id : {L"00000804", L"E0080804", L"E0200804"}) {
        const HKL candidate = LoadKeyboardLayoutW(id, 0);
        if (candidate != nullptr && ImmIsIME(candidate)) {
            ime = candidate;
            break;
        }
    }
    if (ime == nullptr) {
        *error = "no Chinese IME layout is installed";
        return nullptr;
    }
    if (!activate_keyboard_layout(ime, error)) return nullptr;
    ImmAssociateContextEx(target.edit, nullptr, IACE_DEFAULT);
    const HIMC context = ImmGetContext(target.edit);
    if (context == nullptr) {
        *error = "the edit control has no IME context";
        return nullptr;
    }
    ImmSetOpenStatus(context, TRUE);
    ImmSetConversionStatus(context, IME_CMODE_NATIVE, IME_SMODE_NONE);
    ImmReleaseContext(target.edit, context);
    pump_messages(120ms);
    return ime;
}

int keyboard_probe() {
    // main() returns before its own CoInitializeEx for the single-purpose
    // probes; the host and the IME can touch COM, so initialize here too.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    KeyboardWindow window;
    std::string error;
    std::mutex input_log_mutex;
    std::vector<std::string> input_log;
    if (!create_keyboard_window(window, &error)) {
        std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
        return 1;
    }
    const uint16_t port = pick_free_port();
    ProbeSink sink;
    std::unique_ptr<chast::HostServer> host;
    if (port == 0) {
        error = "no free loopback port";
    } else if (!sink.create(&error)) {
        // keep the error
    } else {
        // A real desktop session with input: the host injects into the
        // foreground edit control below. No shared folder is involved.
        host = start_host({}, port, &error, true, {}, {}, {},
                          chast::ClipboardDirection::Both, {}, {},
                          [&](chast::HostServerConfig& config) {
                              config.settings.allow_input = true;
                              config.input_event = [&](const chast::InputEvent& event) {
                                  char line[128]{};
                                  if (event.kind == chast::InputKind::Key) {
                                      std::snprintf(line, sizeof(line),
                                                    "hid=%02X %s mods=%04X",
                                                    event.key.hid_usage,
                                                    event.key.action == chast::KeyAction::Down
                                                        ? "down" : "up",
                                                    event.modifiers);
                                  } else if (event.kind == chast::InputKind::LegacyKey) {
                                      std::snprintf(line, sizeof(line),
                                                    "vk=%02X %s mods=%04X",
                                                    event.legacy_key.virtual_key,
                                                    event.legacy_key.action == chast::KeyAction::Down
                                                        ? "down" : "up",
                                                    event.modifiers);
                                  } else {
                                      std::snprintf(line, sizeof(line), "kind=%u",
                                                    static_cast<unsigned>(event.kind));
                                  }
                                  std::lock_guard<std::mutex> lock(input_log_mutex);
                                  input_log.emplace_back(line);
                              };
                          });
    }
    std::unique_ptr<chast_viewer::ViewerEngine> engine;
    if (host) {
        engine = std::make_unique<chast_viewer::ViewerEngine>(&sink);
        engine->SetClipboardSource([](chast::ClipboardContent&) { return true; });
        engine->SetClipboardFileSource([](std::vector<std::string>&) { return false; });
        if (!engine->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {},
                           kPassword, {}, {}, false,
                           chast_viewer::ViewerTransport::kAuto, {}, 0, {}, {},
                           false)) {
            error = "ViewerEngine::Start was refused";
            engine.reset();
        }
    }

    bool success = false;
    bool ime_tested = false;
    if (engine && wait_streaming(*engine, 20s, &error) &&
        wait_permission(*engine, chast::kPermissionInput, 10s)) {
        chast_viewer::ViewerEngine& viewer = *engine;
        auto fail = [&](std::string message) {
            error = std::move(message);
            return false;
        };
        auto type_text = [&](const std::wstring& expected, const char* what,
                             const std::function<bool()>& type) {
            SetWindowTextW(window.edit, L"");
            pump_messages(30ms);
            if (!ensure_keyboard_foreground(window)) {
                return fail(std::string("the acceptance window could not take the foreground for ") +
                            what);
            }
            if (!type()) return fail(std::string("an input event was refused for ") + what);
            if (!pump_until_text(window.edit, expected, 3s)) {
                return fail(std::string(what) + " produced '" +
                            to_utf8(edit_text(window.edit)) + "', expected '" +
                            to_utf8(expected) + "'");
            }
            return true;
        };

        do {
            // 1. US layout: physical positions produce the US text.
            HKL us = LoadKeyboardLayoutW(L"00000409", 0);
            if (us == nullptr || !activate_keyboard_layout(us, &error)) {
                fail("the US layout could not be activated");
                break;
            }
            if (!type_text(L"Chast1", "US layout text", [&] {
                    return type_shifted(viewer, 0x06, current_locks()) &&  // C
                           type_usage(viewer, 0x0B, 0, current_locks()) &&  // H
                           type_usage(viewer, 0x04, 0, current_locks()) &&  // A
                           type_usage(viewer, 0x16, 0, current_locks()) &&  // S
                           type_usage(viewer, 0x17, 0, current_locks()) &&  // T
                           type_usage(viewer, 0x1E, 0, current_locks());    // 1
                })) {
                break;
            }

            // 2. German layout: the same physical positions now follow the
            // host layout (QWERTZ), Shift+7 becomes '/', AltGr+Q becomes '@'.
            HKL german = LoadKeyboardLayoutW(L"00000407", 0);
            if (german == nullptr || !activate_keyboard_layout(german, &error)) {
                fail("the German layout could not be activated");
                break;
            }
            if (!type_text(L"zy/@", "German layout text", [&] {
                    return type_usage(viewer, 0x1C, 0, current_locks()) &&  // Y position -> z
                           type_usage(viewer, 0x1D, 0, current_locks()) &&  // Z position -> y
                           type_shifted(viewer, 0x24, current_locks()) &&   // Shift+7 -> /
                           type_altgr(viewer, 0x14, current_locks());       // AltGr+Q -> @
                })) {
                break;
            }

            // 3. Legacy mode sends the named key: VK_Y is the letter Y even
            // though the physical US-Y position is Z on this layout.
            if (!type_text(L"y", "legacy virtual-key text", [&] {
                    return send_legacy_key(viewer, 'Y', true, current_locks()) &&
                           (pump_messages(25ms), true) &&
                           send_legacy_key(viewer, 'Y', false, current_locks()) &&
                           (pump_messages(25ms), true);
                })) {
                break;
            }

            // 4. Unicode commit (the local-IME mode's wire message) does not
            // depend on the host layout at all.
            if (!type_text(L"直接提交文字", "Unicode text commit", [&] {
                    return viewer.SendText("直接提交文字");
                })) {
                break;
            }

            // 5. Host IME: physical keys feed Microsoft Pinyin and space
            // commits the first candidate.
            HKL ime = prepare_host_ime(window, &error);
            if (ime != nullptr) {
                ime_tested = true;
                if (!type_text(L"你好", "host IME composition", [&] {
                        return type_usage(viewer, 0x11, 0, current_locks()) &&  // N
                               type_usage(viewer, 0x0C, 0, current_locks()) &&  // I
                               type_usage(viewer, 0x0B, 0, current_locks()) &&  // H
                               type_usage(viewer, 0x04, 0, current_locks()) &&  // A
                               type_usage(viewer, 0x12, 0, current_locks()) &&  // O
                               type_usage(viewer, 0x2C, 0, current_locks());    // Space
                    })) {
                    SetWindowTextW(window.edit, L"");
                    if (ensure_keyboard_foreground(window)) {
                        constexpr uint16_t raw_ime_usages[] = {
                            0x11, 0x0C, 0x0B, 0x04, 0x12, 0x2C};
                        for (const uint16_t usage : raw_ime_usages) {
                            send_local_probe_key(usage, true);
                            pump_messages(75ms);
                            send_local_probe_key(usage, false);
                            pump_messages(75ms);
                        }
                        std::fprintf(stderr,
                                     "viewer-probe: raw scan-code IME control produced '%s'\n",
                                     to_utf8(edit_text(window.edit)).c_str());
                    }
                    break;
                }
            } else {
                std::fprintf(stderr, "viewer-probe: IME check skipped: %s\n",
                             error.c_str());
                error.clear();
            }

            std::printf(
                "viewer-probe: keyboard -> US layout text, German layout text "
                "(physical positions, Shift and AltGr), legacy VK_Y, Unicode "
                "commit%s\n",
                ime_tested ? ", host Pinyin composition" : " (IME unavailable)");
            success = true;
        } while (false);
        if (!success && !error.empty()) {
            std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
            if (host) {
                const chast::HostStatus status = host->status();
                const chast::InputStats& stats = status.stats.input_stats;
                std::fprintf(stderr,
                             "viewer-probe: input events=%llu applied=%llu rejected=%llu "
                             "failed=%llu dropped=%llu lock-toggles=%llu\n",
                             static_cast<unsigned long long>(status.stats.input_events),
                             static_cast<unsigned long long>(stats.applied),
                             static_cast<unsigned long long>(stats.rejected),
                             static_cast<unsigned long long>(stats.failed),
                             static_cast<unsigned long long>(stats.dropped),
                             static_cast<unsigned long long>(stats.lock_toggles));
                std::lock_guard<std::mutex> lock(input_log_mutex);
                for (size_t i = 0; i < input_log.size(); ++i) {
                    std::fprintf(stderr, "viewer-probe: input[%zu] %s\n", i,
                                 input_log[i].c_str());
                }
            }
        }
    } else if (!error.empty()) {
        std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
    }
    if (engine) engine->Stop();
    if (host) host->stop();
    destroy_keyboard_window(window);
    return success ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--media-window-host")
        return media_window_host(from_utf8(argv[2]));
    if (argc == 3 && std::string(argv[1]) == "--multi-display-window-host")
        return media_window_host(from_utf8(argv[2]), true);
    if (argc == 2 && std::string(argv[1]) == "--elevation") return elevation_probe();
    if (argc == 2 && std::string(argv[1]) == "--clipboard-native") return native_clipboard_probe();
    if (argc == 2 && std::string(argv[1]) == "--host-recording") return host_recording_probe();
    if (argc == 2 && std::string(argv[1]) == "--real-microphone")
        return real_microphone_probe();
    if (argc == 2 && std::string(argv[1]) == "--audio-video-isolation")
        return audio_video_isolation_probe();
    if (argc == 2 && std::string(argv[1]) == "--viewer-session-isolation")
        return host_recording_probe(true);
    if (argc == 2 && std::string(argv[1]) == "--viewer-recording-revocation")
        return host_recording_probe(true, true);
    if (argc == 2 && std::string(argv[1]) == "--multi-shares") return multi_share_probe();
    if (argc == 2 && std::string(argv[1]) == "--file-conflict") return file_conflict_probe();
    if (argc == 2 && std::string(argv[1]) == "--directory-upload") return directory_upload_probe();
    if (argc == 2 && std::string(argv[1]) == "--keyboard") return keyboard_probe();
    bool files_only = false;
    bool file_pages = false;
    bool file_transfer_denied = false;
    bool clipboard_scenario = false;
    bool clipboard_empty = false;
    bool clipboard_upload_scenario = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--files-only") {
            files_only = true;
        } else if (arg == "--file-pages") {
            file_pages = files_only = true;
        } else if (arg == "--files-only-denied") {
            files_only = true;
            file_transfer_denied = true;
        } else if (arg == "--clipboard-empty") {
            clipboard_empty = true;
        } else if (arg == "--clipboard-files") {
            clipboard_scenario = true;
        } else if (arg == "--clipboard-files-up") {
            clipboard_upload_scenario = true;
        } else {
            std::fprintf(stderr,
                         "usage: chast-viewer-probe [--files-only | --file-pages | --files-only-denied | "
                         "--clipboard-files | --clipboard-files-up | --file-conflict | "
                         "--directory-upload | --keyboard] "
                         "[--clipboard-empty]\n");
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
                          "probe-" + std::to_string(suffix) + "-loose.txt",
                          "probe-" + std::to_string(suffix) + "-cancel.bin"};
    const fs::path work_directory =
        probe_scratch_root() / ("chast-viewer-probe-" + std::to_string(suffix));
    const fs::path share = work_directory / "share";
    const fs::path downloads = work_directory / "downloads";
    const std::string downloads_text = utf8(downloads);

    int exit_code = 1;
    {
        std::string error;
        if (!prepare_receive_directory(downloads, &error)) {
            std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
        } else if (!seed_share(share, names, &error)) {
            std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
        } else if (!seed_upload(work_directory, share, &error)) {
            std::fprintf(stderr, "viewer-probe: %s\n", error.c_str());
        } else if (file_pages && !seed_file_pages(share / names.tree_name, &error)) {
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
                engine = make_probe_viewer(&sink, downloads, &error);
                engine->SetClipboardSource([](chast::ClipboardContent&) { return true; });
                engine->SetClipboardFileSource([](std::vector<std::string>&) { return false; });
                if (!engine->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {},
                                   kPassword, {}, {}, false,
                                   chast_viewer::ViewerTransport::kAuto, {}, 0, {}, {},
                                   true)) {
                    error = "ViewerEngine::Start was refused";
                    engine.reset();
                } else {

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
                } else if (file_pages) {
                    if (verify_file_pages(*engine, names, downloads, &error)) exit_code = 0;
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
                    } else if (!run_upload_cancel(*engine, work_directory, 10s,
                                                  &error)) {
                        // error already set
                    } else if (!check_upload_cancelled(share, &error)) {
                        // error already set
                    } else if (!run_download_cancel(*engine, *host, names.cancel_name,
                                                    30s, &error)) {
                        // error already set
                    } else if (!check_download_cancelled(downloads, names.cancel_name,
                                                         &error)) {
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
                                "%llu uploaded, 1 upload cancelled, 1 download cancelled "
                                "(host counted it), 0 frames, 0 displays, "
                                "permissions=0x%04X\n",
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
            ClipboardSeed clip{"clip-one.txt", "名字 二.bin", "clip-folder",
                                     "clipboard one", clipboard_binary_bytes(), "inner.txt",
                                     "clipboard inner", "deep.bin", "clipboard deep"};
            clip.directories_only = clipboard_empty;
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
                    utf8(share), port, &error, false,
                    clipboard_sources(clipboard_source, clip),
                    ready, {}, chast::ClipboardDirection::HostToViewer);
            }
            auto capture = std::make_shared<ClipboardCapture>();
            auto reads = std::make_shared<std::atomic<unsigned>>(0);
            std::unique_ptr<chast_viewer::ViewerEngine> engine;
            if (host) {
                engine = make_probe_viewer(&sink, downloads, &error);
                engine->SetClipboardSource([reads](chast::ClipboardContent&) { ++*reads; return true; });
                engine->SetClipboardFileSource([reads](std::vector<std::string>&) { ++*reads; return false; });
                engine->SetClipboardVirtualSink(
                    [capture](std::shared_ptr<chast::ClipboardVirtualFiles> files) {
                        return capture->virtual_sink(std::move(files));
                    });
                if (!engine->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {},
                                   kPassword)) {
                    error = "ViewerEngine::Start was refused";
                    engine.reset();
                }
            }
            if (engine && wait_streaming(*engine, 20s, &error)) {
                if (engine->Snapshot().clipboard_direction != chast::ClipboardDirection::HostToViewer ||
                    (engine->Snapshot().permissions & chast::kPermissionFileTransfer) != 0) {
                    error = "viewer did not adopt clipboard-only host-to-viewer policy";
                } else if (!wait_permission(*engine, chast::kPermissionClipboard, 10s)) {
                    error = "the host never granted the clipboard";
                } else {
                    // The copy happens now, after the session's empty baseline.
                    ready->store(true);
                    if (!capture->wait(30s)) {
                        error = "the clipboard files never reached the viewer";
                    } else {
                        std::string staging;
                        if (!capture->paste(work_directory / "chast-clipboard-paste-down", &error) ||
                            !check_clipboard_files(capture->paths, clip, &staging, &error) ||
                            !check_clipboard_identity(clipboard_sources(clipboard_source, clip), capture->paths, &error)) {
                            // error already set
                        } else if (!wait_clipboard_received(*engine, 5s)) {
                            error = "the viewer did not count the clipboard item";
                        } else {
                            const chast_viewer::ViewerStatus after = engine->Snapshot();
                            const uint32_t file_list_bit =
                                1u << static_cast<uint32_t>(
                                    chast::ClipboardFormat::FileList);
                            if (clip.directories_only && after.files_received != 0) {
                                error = "directory-only clipboard unexpectedly downloaded file data";
                            } else if (after.clipboard_failed != 0) {
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
                                    "viewer-probe: clipboard files -> %zu top-level items "
                                    "(empty directories preserved) staged in %s, formats=0x%X, received=%llu\n",
                                    capture->paths.size(), staging.c_str(),
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
            if (*reads != 0) { error = "receive-only viewer read its local clipboard"; exit_code = 1; }
            if (exit_code == 0 && capture->virtual_files) {
                engine->SetClipboardSync(false);
                const auto until = std::chrono::steady_clock::now() + 2s;
                while (capture->virtual_files->Active() && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(10ms);
                if (capture->virtual_files->Active()) { std::fprintf(stderr, "viewer-probe: pausing sync retained a readable offer\n"); exit_code = 1; }
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
            ClipboardSeed clip{"up-one.txt", "向上 二.bin", "up-folder",
                                     "viewer one", clipboard_binary_bytes(), "inner.txt",
                                     "viewer inner", "deep.bin", "viewer deep"};
            clip.directories_only = clipboard_empty;
            const fs::path clipboard_source = work_directory / "clipboard-up";
            auto ready = std::make_shared<std::atomic<bool>>(false);
            const auto source_paths = clipboard_sources(clipboard_source, clip);
            const uint16_t port = pick_free_port();
            ProbeSink sink;
            auto host_clipboard = std::make_shared<ClipboardCapture>();
            std::unique_ptr<chast::HostServer> host;
            if (!seed_clipboard(clipboard_source, clip, &error)) {
                // keep the error
            } else if (port == 0) {
                error = "no free loopback port";
            } else if (!sink.create(&error)) {
                // keep the error
            } else {
                host = start_host(
                    utf8(share), port, &error, false, {}, nullptr,
                    [host_clipboard](const std::vector<std::string>& files) {
                        return host_clipboard->sink(files);
                    }, chast::ClipboardDirection::ViewerToHost,
                    [host_clipboard](std::shared_ptr<chast::ClipboardVirtualFiles> files) {
                        return host_clipboard->virtual_sink(std::move(files));
                    });
            }
            auto viewer_clipboard = std::make_shared<ClipboardCapture>();
            std::unique_ptr<chast_viewer::ViewerEngine> engine;
            if (host) {
                engine = make_probe_viewer(&sink, downloads, &error);
                engine->SetClipboardSource([](chast::ClipboardContent&) { return true; });
                engine->SetClipboardFileSource([](std::vector<std::string>&) { return false; });
                engine->SetClipboardFileSource(
                    [source_paths, ready](std::vector<std::string>& paths) {
                        if (!ready->load()) {
                            return false;
                        }
                        paths = source_paths;
                        return true;
                    });
                engine->SetClipboardFileSink(
                    [viewer_clipboard](const std::vector<std::string>& files) {
                        return viewer_clipboard->sink(files);
                    });
                if (!engine->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {},
                                   kPassword)) {
                    error = "ViewerEngine::Start was refused";
                    engine.reset();
                }
            }
            std::string staging;
            if (engine && wait_streaming(*engine, 20s, &error)) {
                if (engine->Snapshot().clipboard_direction != chast::ClipboardDirection::ViewerToHost ||
                    (engine->Snapshot().permissions & chast::kPermissionFileTransfer) != 0) {
                    error = "viewer did not adopt clipboard-only viewer-to-host policy";
                } else if (!wait_permission(*engine, chast::kPermissionClipboard, 10s)) {
                    error = "the host never granted the clipboard";
                } else {
                    // The baseline must be the empty clipboard before `ready`;
                    // give the session loop a poll to record it before the
                    // copy appears.
                    std::this_thread::sleep_for(1000ms);
                    ready->store(true);
                    if (!host_clipboard->wait(30s)) {
                        error = "the viewer's clipboard files never reached the host";
                    } else if (!host_clipboard->paste(work_directory / "chast-clipboard-paste-up", &error) ||
                               !check_clipboard_files(host_clipboard->paths, clip, &staging, &error) ||
                               !check_clipboard_identity(source_paths, host_clipboard->paths, &error)) {
                        // error already set
                    } else {
                        // A few poll cycles prove the staged list is not sent
                        // back to the viewer as a new clipboard item.
                        std::this_thread::sleep_for(1500ms);
                        const chast_viewer::ViewerStatus after = engine->Snapshot();
                        if (clip.directories_only && after.files_sent != 0) {
                            error = "directory-only clipboard unexpectedly uploaded file data";
                        } else if (viewer_clipboard->wait(0ms)) {
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
                                "viewer-probe: clipboard files up -> %zu top-level items "
                                "(empty directories preserved) staged in %s, no echo, failures=0\n",
                                host_clipboard->paths.size(), staging.c_str());
                            exit_code = 0;
                        }
                    }
                }
            }
            if (exit_code == 0 && host_clipboard->virtual_files) {
                host->set_permissions(static_cast<chast::PermissionMask>(engine->Snapshot().permissions & ~chast::kPermissionClipboard));
                const auto until = std::chrono::steady_clock::now() + 2s;
                while (host_clipboard->virtual_files->Active() && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(10ms);
                if (host_clipboard->virtual_files->Active()) { std::fprintf(stderr, "viewer-probe: revocation retained a readable offer\n"); exit_code = 1; }
            }
            if (engine) engine->Stop();
            if (host) host->stop();
            // The paste target owns the completed files, including after the
            // remote connection is closed. The probe removes only its own copy.
            if (exit_code == 0 && !staging.empty()) {
                std::error_code ec;
                if (!fs::exists(from_utf8(staging), ec) || !host_clipboard->virtual_files ||
                    host_clipboard->virtual_files->Active()) {
                    std::fprintf(stderr, "viewer-probe: paste lifetime or remote cancellation failed\n");
                    exit_code = 1;
                }
                fs::remove_all(from_utf8(staging), ec);
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
                engine = make_probe_viewer(&sink, downloads, &error);
                engine->SetClipboardSource([](chast::ClipboardContent&) { return true; });
                engine->SetClipboardFileSource([](std::vector<std::string>&) { return false; });
                if (!engine->Start("127.0.0.1", port, chast::kDisplayAuto, {}, true, {},
                                   kPassword)) {
                    error = "ViewerEngine::Start was refused";
                    engine.reset();
                } else {

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
                    stream.codec == chast::VideoCodec::Hevc
                        ? "hevc"
                        : (stream.codec == chast::VideoCodec::Av1 ? "av1" : "h264"),
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
                                            "resized to 100x30; second console 90x28 -> "
                                            "marker echoed, resized to 110x32 and closed "
                                            "independently (%llu bytes total)\n",
                                            static_cast<unsigned long long>(terminal_bytes));
                                        std::string first_shot;
                                        std::string second_shot;
                                        if (!run_screenshots(*engine,
                                                             work_directory / "screenshots",
                                                             15s, &first_shot, &second_shot,
                                                             &error)) {
                                            // error already set
                                        } else {
                                            const chast_viewer::ViewerStatus shot =
                                                engine->Snapshot();
                                            std::printf(
                                                "viewer-probe: screenshots %s and %s (%ux%u "
                                                "PNG, collision suffix)\n",
                                                first_shot.c_str(), second_shot.c_str(),
                                                static_cast<unsigned>(shot.width),
                                                static_cast<unsigned>(shot.height));
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
    for (const std::string& leftover :
         {names.cancel_name, names.cancel_name + ".part",
          names.cancel_name + ".part.chast"}) {
        fs::remove(downloads / from_utf8(leftover), ec);
    }
    CoUninitialize();
    return exit_code;
}
