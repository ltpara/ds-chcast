// Winsock 2 must precede windows.h, which chast/quic.h pulls in through
// chast/error.h; otherwise the SDK sees winsock 1 first and ws2tcpip.h fails.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "chast/camera_relay.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "chast/com_ptr.h"

namespace chast {

namespace {

using Clock = std::chrono::steady_clock;

// Hard ceiling on encoded packets waiting for the session thread. The worker
// already stops encoding once two are queued, so reaching four means a burst
// (an IDR plus deltas) that still has to be bounded.
constexpr size_t kMaxQueuedPackets = 4;
constexpr size_t kMaxQueuedBytes = 4u * 1024u * 1024u;
// Encoded frames queued before the worker skips capture frames entirely: a
// skipped source frame costs nothing, while an encoded frame that is dropped
// later breaks the reference chain.
constexpr size_t kBacklogSkip = 2;
// A healthy camera delivers every few dozen milliseconds; the timeout bounds
// both a stalled device and how long teardown waits for the worker.
constexpr auto kReadTimeout = std::chrono::milliseconds(200);

// Intel's video-memory frames must be 16-aligned; the desktop pipeline pads
// for the same reason. Padding stays black.
uint32_t align16(uint32_t value) { return (value + 15u) & ~15u; }

const char* codec_name(VideoCodec codec) {
    return codec == VideoCodec::Hevc ? "hevc" : "h264";
}

// Same clock and encoding as the desktop stream (FrameHeader::capture_ms).
uint64_t capture_clock_ms() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now().time_since_epoch())
                        .count();
    return static_cast<uint64_t>(ms) & 0xFFFFFFFFFFFFull;
}

bool create_device(LUID luid, ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context,
                   std::string* error) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(factory.put()));
    if (FAILED(hr)) {
        *error = "CreateDXGIFactory1 failed";
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;; ++index) {
        if (factory->EnumAdapters1(index, adapter.put()) == DXGI_ERROR_NOT_FOUND) {
            *error = "the capture adapter is gone";
            return false;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == luid.LowPart &&
            desc.AdapterLuid.HighPart == luid.HighPart) {
            break;
        }
    }
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                           ARRAYSIZE(levels), D3D11_SDK_VERSION, device.put(), &level,
                           context.put());
    if (hr == E_INVALIDARG) {
        // Pre-Windows 8 runtimes reject 11_1 in the feature-level list.
        hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &levels[1],
                               1, D3D11_SDK_VERSION, device.put(), &level, context.put());
    }
    if (FAILED(hr)) {
        *error = "D3D11CreateDevice on the capture adapter failed";
        return false;
    }
    return true;
}

}  // namespace

struct CameraRelay::Worker {
    struct Packet {
        FrameHeader header;
        std::vector<uint8_t> payload;
    };

    uint16_t camera_id = 0;
    std::string name;
    std::thread thread;
    std::mutex mutex;
    std::deque<Packet> frames;
    size_t frame_bytes = 0;
    bool resolved = false;
    bool ready = false;
    bool answered = false;
    bool failed = false;
    bool ended = false;
    std::string failure;
    std::string detail;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t dropped = 0;
    uint64_t skipped = 0;
    std::atomic<bool> stop{false};
    // Set whenever an encoded frame was lost (queue overflow or a transport
    // drop). The next encoded frame must be an IDR or the viewer cannot
    // decode anything that follows.
    std::atomic<bool> force_keyframe{false};

    void run(LUID adapter, CameraRelayConfig config, CameraDevice camera);
    void resolve_failed(const std::string& reason);
    void mark_ended(const std::string& reason, bool bad);
    void publish(Packet packet);
};

void CameraRelay::Worker::resolve_failed(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex);
    resolved = true;
    ready = false;
    failed = true;
    ended = true;
    failure = reason;
}

void CameraRelay::Worker::mark_ended(const std::string& reason, bool bad) {
    std::lock_guard<std::mutex> lock(mutex);
    ended = true;
    failed = bad;
    failure = reason;
}

void CameraRelay::Worker::publish(Packet packet) {
    std::lock_guard<std::mutex> lock(mutex);
    if (frames.size() >= kMaxQueuedPackets ||
        frame_bytes + packet.payload.size() > kMaxQueuedBytes) {
        ++dropped;
        force_keyframe = true;
        return;
    }
    frame_bytes += packet.payload.size();
    frames.push_back(std::move(packet));
}

void CameraRelay::Worker::run(LUID adapter, CameraRelayConfig config, CameraDevice camera) {
    struct ComScope {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() {
            if (SUCCEEDED(result)) {
                CoUninitialize();
            }
        }
    } com;
    if (FAILED(com.result)) {
        resolve_failed("camera capture needs a thread initialized as MTA");
        return;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    {
        std::string reason;
        if (!create_device(adapter, device, context, &reason)) {
            resolve_failed(reason);
            return;
        }
    }
    std::string reason;
    std::unique_ptr<ICameraCapture> capture = open_camera(camera, config.framerate, &reason);
    if (!capture) {
        resolve_failed(reason);
        return;
    }
    const uint32_t source_width = capture->width();
    const uint32_t source_height = capture->height();
    if (stop.load()) {
        resolve_failed("stopped before streaming");
        return;
    }

    const UINT surface_width = align16(source_width);
    const UINT surface_height = align16(source_height);
    EncoderConfig encoder_config;
    encoder_config.codec = config.codec;
    encoder_config.width = surface_width;
    encoder_config.height = surface_height;
    encoder_config.framerate = capture->framerate() > 0 ? capture->framerate() : config.framerate;
    encoder_config.gop_size = 0;
    encoder_config.quality_ratio = config.quality_ratio;
    encoder_config.backend = config.backend;
    encoder_config.bitrate = hardware_bitrate(surface_width, surface_height,
                                              config.quality_ratio,
                                              config.codec == VideoCodec::H264);
    std::unique_ptr<IVideoEncoder> encoder = create_encoder(device.get(), encoder_config, &reason);
    if (!encoder) {
        resolve_failed("encoder: " + reason);
        return;
    }

    // The texture keeps the desktop NV12 flags: oneVPL mirrors the descriptor
    // of the surface it imports and refuses a texture without the codec
    // bindings, and the shared flag is what lets the runtime import it at all.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = surface_width;
    desc.Height = surface_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER |
                     D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.put()))) {
        resolve_failed("CreateTexture2D(NV12) failed on the capture adapter");
        return;
    }

    // Padded NV12 upload buffer. The visible area is overwritten per frame;
    // the padding rows keep the black written here.
    std::vector<uint8_t> upload(static_cast<size_t>(surface_width) * surface_height * 3 / 2, 0);
    const size_t luma_bytes = static_cast<size_t>(surface_width) * surface_height;
    std::fill(upload.begin(), upload.begin() + luma_bytes, 16);
    std::fill(upload.begin() + luma_bytes, upload.end(), 128);

    {
        std::lock_guard<std::mutex> lock(mutex);
        resolved = true;
        ready = true;
        width = surface_width;
        height = surface_height;
        detail = std::to_string(source_width) + "x" + std::to_string(source_height) +
                 " encoded at " + std::to_string(surface_width) + "x" +
                 std::to_string(surface_height) + ", " + codec_name(encoder->codec()) + ", " +
                 encoder->implementation() + ", " +
                 std::to_string(encoder->bitrate() / 1000u) + " kbps";
    }

    D3D11_BOX box{};
    box.right = surface_width;
    box.bottom = surface_height;
    box.back = 1;
    std::vector<EncodedPacket> packets;
    bool first = true;
    uint64_t first_pts = 0;
    while (!stop.load()) {
        CameraFrame frame;
        const Error read = capture->read(kReadTimeout, [&](CameraFrame&& got) {
            frame = std::move(got);
        });
        if (!read.ok()) {
            if (stop.load()) {
                break;
            }
            if (read.status() == Status::WaitTimeout) {
                continue;
            }
            mark_ended("capture: " + read.to_string(), true);
            return;
        }
        if (frame.width != source_width || frame.height != source_height) {
            mark_ended("the camera changed its frame size", true);
            return;
        }
        if (first) {
            first_pts = frame.timestamp_ms;
        }
        const uint64_t pts_ms = frame.timestamp_ms - first_pts;
        const bool force = first || force_keyframe.exchange(false);
        if (!force) {
            std::lock_guard<std::mutex> lock(mutex);
            if (frames.size() >= kBacklogSkip) {
                // The session thread is behind; this frame would only be
                // dropped after encoding. Skipping it loses no encoded frame,
                // so no IDR is needed.
                ++skipped;
                continue;
            }
        }

        for (uint32_t row = 0; row < source_height; ++row) {
            std::memcpy(upload.data() + static_cast<size_t>(row) * surface_width,
                        frame.data.data() + static_cast<size_t>(row) * source_width,
                        source_width);
        }
        const uint8_t* source_chroma =
            frame.data.data() + static_cast<size_t>(source_width) * source_height;
        uint8_t* target_chroma = upload.data() + luma_bytes;
        for (uint32_t row = 0; row < source_height / 2; ++row) {
            std::memcpy(target_chroma + static_cast<size_t>(row) * surface_width,
                        source_chroma + static_cast<size_t>(row) * source_width, source_width);
        }
        // One call fills the whole NV12 surface: the runtime takes the chroma
        // plane from the second half of the source buffer.
        context->UpdateSubresource(texture.get(), 0, &box, upload.data(), surface_width, 0);

        packets.clear();
        const Error encoded = encoder->encode(texture.get(), pts_ms, force, packets);
        if (!encoded.ok()) {
            mark_ended("encode: " + encoded.to_string(), true);
            return;
        }
        for (EncodedPacket& packet : packets) {
            if (packet.data.empty()) {
                continue;
            }
            FrameHeader header;
            header.pts_ms = packet.pts_ms;
            header.width = encoder->width();
            header.height = encoder->height();
            header.codec = encoder->codec();
            header.keyframe = packet.keyframe;
            header.idr = packet.idr;
            header.capture_ms = capture_clock_ms();
            publish(Packet{header, std::move(packet.data)});
        }
        first = false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        ended = true;
    }
}

CameraRelay::CameraRelay(QuicSession& session, AdapterProvider adapter,
                         const CameraRelayConfig& config)
    : session_(session), adapter_(std::move(adapter)), config_(config) {}

CameraRelay::~CameraRelay() {
    if (!worker_) {
        return;
    }
    worker_->stop = true;
    if (worker_->thread.joinable()) {
        worker_->thread.join();
    }
    worker_.reset();
}

bool CameraRelay::streaming() const {
    return worker_ != nullptr && !worker_->stop.load();
}

void CameraRelay::send_list() {
    CameraList list;
    if ((session_.permissions() & kPermissionCamera) == 0) {
        list.code = CameraCode::Refused;
        list.reason = "camera access is not permitted by the host";
    } else {
        listed_ = enumerate_cameras();
        for (const CameraDevice& device : listed_) {
            list.cameras.push_back(CameraDescription{device.id, device.name, device.is_default});
        }
    }
    session_.send_camera_list(list);
}

void CameraRelay::start_camera(uint16_t camera_id, std::vector<std::string>& events) {
    if ((session_.permissions() & kPermissionCamera) == 0) {
        session_.send_camera_result(
            CameraResult{camera_id, CameraCode::Refused, "camera access is not permitted"});
        return;
    }
    const CameraDevice* chosen = nullptr;
    for (const CameraDevice& device : listed_) {
        if (device.id == camera_id) {
            chosen = &device;
            break;
        }
    }
    if (!chosen) {
        ++failures_;
        session_.send_camera_result(
            CameraResult{camera_id, CameraCode::NotFound, "the camera is not in the host's list"});
        events.push_back("camera " + std::to_string(camera_id) +
                         ": start refused, it is not in the announced list");
        return;
    }
    if (worker_) {
        // The transport refuses a second camera; this is a request that
        // crossed the previous camera's teardown.
        session_.send_camera_result(
            CameraResult{camera_id, CameraCode::Busy, "another camera is still closing"});
        return;
    }
    worker_ = std::make_unique<Worker>();
    worker_->camera_id = camera_id;
    worker_->name = chosen->name;
    Worker* worker = worker_.get();
    const CameraDevice camera = *chosen;
    const LUID adapter = adapter_();
    const CameraRelayConfig config = config_;
    worker->thread = std::thread(
        [worker, adapter, config, camera] { worker->run(adapter, config, camera); });
    events.push_back("camera \"" + chosen->name + "\": opening");
}

void CameraRelay::drive(std::vector<std::string>& events) {
    Worker& worker = *worker_;
    bool resolved = false;
    bool ready = false;
    bool failed = false;
    bool ended = false;
    bool answered = false;
    std::string failure;
    std::string detail;
    {
        std::lock_guard<std::mutex> lock(worker.mutex);
        resolved = worker.resolved;
        ready = worker.ready;
        failed = worker.failed;
        ended = worker.ended;
        answered = worker.answered;
        failure = worker.failure;
        detail = worker.detail;
    }
    if (worker.stop.load()) {
        if (!ended) {
            return;
        }
        // A viewer stop already finished the stream in the transport and the
        // destructor cannot reach here; only the thread is left to collect.
        if (worker_->thread.joinable()) {
            worker_->thread.join();
        }
        worker_.reset();
        return;
    }
    if (!resolved) {
        return;
    }
    if (!answered) {
        if (!ready) {
            ++failures_;
            session_.send_camera_result(
                CameraResult{worker.camera_id, CameraCode::Failed, failure});
            events.push_back("camera \"" + worker.name + "\": cannot start: " + failure);
            worker.stop = true;
            return;
        }
        const Error sent = session_.send_camera_result(
            CameraResult{worker.camera_id, CameraCode::Ok, {}});
        if (!sent.ok()) {
            // The viewer cancelled while the camera opened, the permission was
            // revoked, or the session is going away.
            events.push_back("camera \"" + worker.name + "\": cannot open the stream: " +
                             sent.to_string());
            worker.stop = true;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(worker.mutex);
            worker.answered = true;
        }
        ++started_;
        events.push_back("camera \"" + worker.name + "\": streaming " + detail);
        answered = true;
    }
    if (ended) {
        if (failed) {
            ++failures_;
            events.push_back("camera \"" + worker.name + "\": " + failure);
        } else {
            events.push_back("camera \"" + worker.name + "\": the device stopped");
        }
        // The viewer learns about the end from the stream FIN; finish_camera
        // is idempotent when the transport already closed it.
        session_.finish_camera();
        if (worker_->thread.joinable()) {
            worker_->thread.join();
        }
        worker_.reset();
    }
}

void CameraRelay::send_frames(std::vector<std::string>& events) {
    Worker& worker = *worker_;
    if (!worker.answered) {
        return;
    }
    for (;;) {
        Worker::Packet packet;
        {
            std::lock_guard<std::mutex> lock(worker.mutex);
            if (worker.frames.empty()) {
                break;
            }
            packet = std::move(worker.frames.front());
            worker.frames.pop_front();
            worker.frame_bytes -= packet.payload.size();
        }
        const Error sent =
            session_.send_camera_frame(packet.header, packet.payload.data(),
                                       packet.payload.size());
        if (sent.ok()) {
            ++frames_;
            continue;
        }
        if (sent.status() == Status::WaitTimeout) {
            // The transport dropped this packet; the reference chain is broken
            // from here, so the encoder has to produce an IDR next.
            worker.force_keyframe = true;
            continue;
        }
        events.push_back("camera \"" + worker.name + "\": cannot send a frame: " +
                         sent.to_string());
        worker.stop = true;
        break;
    }
}

std::vector<std::string> CameraRelay::pump() {
    std::vector<std::string> events;
    if (session_.take_camera_list_request()) {
        send_list();
    }
    uint16_t camera_id = 0;
    while (session_.take_camera_start(camera_id)) {
        start_camera(camera_id, events);
    }
    uint16_t stopped = 0;
    while (session_.take_camera_stop(stopped)) {
        if (worker_ && worker_->camera_id == stopped) {
            worker_->stop = true;
            events.push_back("camera \"" + worker_->name + "\": stopped by the viewer");
        }
    }
    if (worker_) {
        drive(events);
    }
    if (worker_) {
        send_frames(events);
    }
    return events;
}

}  // namespace chast
