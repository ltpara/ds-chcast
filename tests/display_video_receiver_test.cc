#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include "../app/windows/viewer/viewer_engine.h"
#include "chast/quic.h"
#include "mp4_recorder.h"

using namespace chast;
using namespace chast_viewer;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", message);
    if (!ok) throw std::runtime_error(message);
}
template<class P> bool until(P p) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do { if (p()) return true; std::this_thread::sleep_for(2ms); } while (std::chrono::steady_clock::now() < deadline);
    return p();
}
struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool hold = false;
    std::atomic<unsigned> entered{0}, created{0}, destroyed{0}, wrong_thread{0};
    std::atomic<bool> fail{false}, cpu{false};
    void release() { { std::lock_guard lock(mutex); hold = false; } cv.notify_all(); }
};
struct Sink : FrameSink {
    std::atomic<unsigned> frames{0}, last_width{0};
    std::atomic<unsigned> screenshots{0};
    bool CaptureBgra(std::vector<uint8_t>&, uint32_t&, uint32_t&, std::string* error) override {
        ++screenshots; *error = "test screenshot source"; return false;
    }
    ID3D11Device* device() override { return reinterpret_cast<ID3D11Device*>(this); }
    VideoCodecMask DecodingCodecs() override { return kVideoCodecAll; }
    bool OnGpuFrame(ID3D11Texture2D*, uint32_t, uint32_t width, uint32_t, uint32_t, uint32_t, uint64_t) override {
        last_width = width; ++frames; return true;
    }
    std::string error() const override { return {}; }
};
struct Decoder : IVideoDecoder {
    DecoderConfig config;
    std::shared_ptr<State> state;
    std::thread::id owner = std::this_thread::get_id();
    UINT w = 0;
    std::string name = "test GPU boundary";
    Decoder(DecoderConfig c, std::shared_ptr<State> s) : config(std::move(c)), state(std::move(s)) { ++state->created; }
    ~Decoder() { if (owner != std::this_thread::get_id()) ++state->wrong_thread; ++state->destroyed; }
    Error decode(const uint8_t* bytes, size_t size, uint64_t pts, bool, std::vector<DecodedFrame>& out) override {
        if (owner != std::this_thread::get_id()) ++state->wrong_thread;
        ++state->entered;
        { std::unique_lock lock(state->mutex); state->cv.wait(lock, [&] { return !state->hold; }); }
        if (state->fail) return Error(Status::Unavailable, "test corrupt stream");
        if (state->cpu) { out.emplace_back(); return Error::success(); }
        if (size < 2) return Error(Status::InvalidCall, "test packet");
        w = bytes[0] | (UINT(bytes[1]) << 8);
        return config.gpu_handler({nullptr, 0, w, 480, 0, 0, pts});
    }
    Error flush(std::vector<DecodedFrame>&) override { return Error::success(); }
    bool initialized() const noexcept override { return w != 0; }
    UINT width() const noexcept override { return w; }
    UINT height() const noexcept override { return 480; }
    const std::string& implementation() const noexcept override { return name; }
};
DisplayStreamInfo description(uint32_t id, uint16_t index, uint16_t width) {
    ServerInfo info; info.width = info.source_width = width; info.height = info.source_height = 480;
    return {id, index, info};
}
bool push(DisplayVideoReceiver& receiver, uint32_t id, uint16_t width, bool idr = true, bool corrupt = false) {
    std::vector<uint8_t> bytes{static_cast<uint8_t>(width), static_cast<uint8_t>(width >> 8)};
    FrameHeader header; header.stream_id = id; header.width = width; header.height = 480;
    header.idr = header.keyframe = idr; header.payload_crc32 = crc32(bytes.data(), bytes.size());
    if (corrupt) ++header.payload_crc32;
    return receiver.Push(header, std::move(bytes));
}
void independent_workers() {
    auto a = std::make_shared<State>(), b = std::make_shared<State>();
    auto first = std::make_shared<Sink>(), second = std::make_shared<Sink>();
    DisplayVideoReceiver receiver([&](const DecoderConfig& c, std::string*) {
        return std::make_unique<Decoder>(c, c.d3d11_device == first->device() ? a : b);
    });
    // Always unblock before receiver destruction, even if an assertion throws.
    struct Release { std::shared_ptr<State> state; ~Release() { state->release(); } } release{a};
    CaptureDisplaysResult result; result.streams = {description(7, 0, 640), description(9, 1, 800)};
    std::string error;
    check(receiver.Apply(result, &error), "create two source handlers");
    check(receiver.Bind(7, first) && receiver.Bind(9, second) && !receiver.Bind(9, first),
          "each source binds an independent sink");
    check(receiver.TakeKeyframeRequests() == std::vector<uint32_t>({7, 9}) && receiver.TakeKeyframeRequests().empty(),
          "initial keyframe requests are targeted and rate limited");
    check(!push(receiver, 7, 640, false), "new decoder rejects delta pictures before IDR");
    { std::lock_guard lock(a->mutex); a->hold = true; }
    check(push(receiver, 7, 640) && until([&] { return a->entered == 1; }), "hold first decoder inside its worker");
    check(push(receiver, 9, 800) && until([&] { return second->frames == 1; }) && first->frames == 0,
          "blocked first decoder does not stop second display rendering");
    for (int i = 0; i < 100; ++i) push(receiver, 7, 640, false);
    const auto snapshot = receiver.Snapshot();
    check(snapshot[0].queued_frames <= 60 && snapshot[0].queued_bytes <= 8 * 1024 * 1024 &&
          snapshot[0].waiting_idr && snapshot[0].dropped > 0,
          "stalled worker queue is bounded and loses references only with IDR recovery");
    check(!push(receiver, 7, 640, false) && push(receiver, 9, 800, false) &&
          until([&] { return second->frames == 2; }), "overflow on one source does not invalidate another decoder");
    a->release();
    check(push(receiver, 7, 640) && until([&] { return first->frames >= 2; }), "fresh IDR recovers the stalled stream");
    check(!push(receiver, 7, 640, false, true) && !push(receiver, 7, 640, false),
          "CRC failure forces IDR before accepting further deltas");
    check(push(receiver, 7, 640) && until([&] { return first->frames >= 3; }), "CRC recovery remains source-local");

    auto changed = result.streams[0]; changed.info.width = changed.info.source_width = 320;
    check(receiver.Update(changed, &error) && !push(receiver, 7, 640) && push(receiver, 7, 320) &&
          until([&] { return first->last_width == 320; }) && b->created == 1,
          "format change rebuilds only the target decoder and rejects in-flight old dimensions");
    result.streams = {description(9, 0, 800)}; // topology renumbering preserves stream identity
    check(receiver.Apply(result, &error) && !push(receiver, 7, 320) && a->destroyed == a->created && b->created == 1,
          "unsubscribe joins removed worker while surviving renumbered stream keeps its decoder");
    result.streams.push_back(description(11, 1, 640));
    check(receiver.Apply(result, &error) && receiver.Bind(11, first) && !push(receiver, 11, 640, false) &&
          push(receiver, 11, 640) && until([&] { return first->last_width == 640; }),
          "re-added source starts with a fresh decoder and IDR");
    auto invalid = result; invalid.streams[1].stream_id = 9;
    check(!receiver.Apply(invalid, &error) && receiver.Contains(11) && receiver.Contains(9),
          "malformed replacement cannot destroy working subscriptions");
    a->cpu = true;
    check(push(receiver, 11, 640) && until([&] {
        for (const auto& item : receiver.Snapshot()) if (item.stream.stream_id == 11) return item.decode_errors != 0;
        return false;
    }), "unexpected CPU-decoded output is rejected");
    check(push(receiver, 9, 800) && until([&] { return second->frames == 3; }), "bad decoder does not end another display");
    check(until([&] { return receiver.Snapshot()[0].ready; }), "ready reports a presented frame");
    auto readback = std::make_shared<std::promise<FrameSink*>>();
    auto readback_result = readback->get_future();
    check(receiver.WithFrame(9, [readback](FrameSink* sink) { readback->set_value(sink); }) &&
          readback_result.wait_for(3s) == std::future_status::ready && readback_result.get() == second.get(),
          "readback runs on the selected source even after other sources were retired");
    const auto totals = receiver.Totals();
    check(totals.published >= 7, "counters include retired streams");
    result.streams.clear();
    check(receiver.Apply(result, &error) && receiver.empty() && a->created == a->destroyed && b->created == b->destroyed &&
          a->wrong_thread == 0 && b->wrong_thread == 0, "empty set releases all decoders on their owning workers");
    check(receiver.Totals().published == totals.published && !receiver.WithFrame(9, [](FrameSink*) {}),
          "empty selection preserves cumulative counters and rejects retired readback");
}

void screenshot_rebind() {
    auto state = std::make_shared<State>();
    auto sink = std::make_shared<Sink>();
    DisplayVideoReceiver receiver([&](const DecoderConfig& c, std::string*) { return std::make_unique<Decoder>(c, state); });
    struct Release { std::shared_ptr<State> state; ~Release() { state->release(); } } release{state};
    CaptureDisplaysResult selection; selection.streams = {description(1, 0, 640)};
    check(receiver.Apply(selection, nullptr) && receiver.Bind(1, sink) && push(receiver, 1, 640) &&
          until([&] { return receiver.Snapshot()[0].ready; }), "screenshot source has a ready picture");
    { std::lock_guard lock(state->mutex); state->hold = true; }
    check(push(receiver, 1, 640) && until([&] { return state->entered == 2; }), "hold presentation before readback");
    auto readback = std::make_shared<std::promise<FrameSink*>>(); auto result = readback->get_future();
    check(receiver.WithFrame(1, [readback](FrameSink* s) { readback->set_value(s); }) &&
          result.wait_for(10ms) == std::future_status::timeout, "readback is serialized with presentation");
    check(receiver.Bind(1, nullptr) && !receiver.Snapshot()[0].ready, "detach invalidates ready picture");
    state->release();
    check(result.wait_for(3s) == std::future_status::ready && result.get() == nullptr,
          "queued screenshot cannot read a detached output");
    check(receiver.Bind(1, sink) && !receiver.WithFrame(1, [](FrameSink*) {}), "rebind waits for a fresh picture");
}

uint16_t port() {
    const auto handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    const bool ok = bind(handle, reinterpret_cast<sockaddr*>(&address), length) == 0 &&
        getsockname(handle, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    closesocket(handle);
    check(ok, "allocate viewer test port");
    return ntohs(address.sin_port);
}
void engine_transport() {
    QuicServerConfig config;
    config.bind_address = "127.0.0.1"; config.port = port();
    config.authenticate = [](const ClientRequest&) {
        ClientAuthResult result; result.action = ClientAuthAction::Allow; result.permissions = kPermissionRecord; return result;
    };
    std::string error;
    auto host = QuicSession::create_server(config, &error);
    check(host != nullptr, "create real QUIC endpoint");
    auto announce = std::async(std::launch::async, [&] {
        if (!host->wait_for_client(5s).ok()) return false;
        auto info = description(0, 0, 640).info;
        info.displays = {{"left", 0, 0, 640, 480, 0, true}, {"right", 640, 0, 800, 480, 0, false}};
        return host->announce(info).ok();
    });
    auto legacy = std::make_shared<Sink>(), left = std::make_shared<Sink>(), right = std::make_shared<Sink>();
    auto a = std::make_shared<State>(), b = std::make_shared<State>(), old = std::make_shared<State>();
    ViewerEngine viewer(legacy.get(), nullptr, [&](const DecoderConfig& c, std::string*) {
        return std::make_unique<Decoder>(c, c.d3d11_device == left->device() ? a :
            c.d3d11_device == right->device() ? b : old);
    });
    struct Release { std::shared_ptr<State> state; ~Release() { state->release(); } } release{a};
    check(viewer.Start("127.0.0.1", config.port, 0, host->server_fingerprint()) && announce.get() &&
          until([&] { return viewer.Snapshot().state == ViewerState::kStreaming; }), "native engine completes real pinned QUIC handshake");
    check(!viewer.Snapshot().input_enabled, "native viewer is view-only");
    auto operation = [&](CaptureDisplaysRequest request, const std::vector<DisplayStreamInfo>& streams) {
        check(viewer.CaptureDisplays(request), "queue native subscription operation");
        CaptureDisplaysRequest received;
        check(until([&] { return host->take_capture_displays_request(received); }) &&
              received.request_id == request.request_id && received.add == request.add && received.set == request.set && received.sub == request.sub,
              "native request reaches host with ordered selection semantics");
        CaptureDisplaysResult result; result.request_id = request.request_id; result.streams = streams;
        check(host->send_capture_displays_result(result).ok() && until([&] {
            const auto state = viewer.Snapshot();
            return state.multiple_displays && state.display_subscription_request == request.request_id &&
                state.display_streams.size() == streams.size();
        }), "native engine applies complete subscription result");
    };
    std::vector<DisplayStreamInfo> streams{description(7, 0, 640), description(9, 1, 800)};
    streams[1].info.origin_x = 640;
    operation({1, {}, {}, {0, 1}}, streams);
    const auto replies = viewer.Snapshot().display_subscription_results;
    CaptureDisplaysResult topology; topology.streams = streams;
    topology.streams[1].info.origin_x = -800;
    check(host->send_capture_displays_result(topology).ok() && until([&] {
        return viewer.Snapshot().display_subscription_results > replies;
    }) && viewer.Snapshot().display_subscription_request == 1,
          "unsolicited topology updates retain the latest explicit selection acknowledgement");
    check(viewer.BindDisplay(7, left) && viewer.BindDisplay(9, right), "bind native per-source outputs");
    std::set<uint32_t> requests;
    check(until([&] { uint32_t id; while (host->take_display_keyframe_request(id)) requests.insert(id);
        return requests.count(7) && requests.count(9); }), "new output bindings request separate IDRs");
    auto send = [&](uint32_t id, uint16_t width) {
        FrameHeader header; header.stream_id = id; header.width = width; header.height = 480; header.keyframe = header.idr = true;
        const uint8_t bytes[]{static_cast<uint8_t>(width), static_cast<uint8_t>(width >> 8)};
        check(host->send_frame_nowait(header, bytes, sizeof(bytes)).ok(), "send display frame through QUIC");
    };
    { std::lock_guard lock(a->mutex); a->hold = true; }
    send(7, 640); send(9, 800);
    check(until([&] { return a->entered == 1 && right->frames == 1; }) && left->frames == 0,
          "native session continues routing another display while one decoder is blocked");
    CursorUpdate cursor; cursor.visible = true; cursor.width = cursor.height = 2; cursor.shape_id = 1;
    cursor.sprite.assign(16, 0x40); cursor.x = 25;
    check(host->send_display_cursor({9, cursor}).ok(), "send source-specific cursor while a decoder waits");
    DisplayCursorUpdate received_cursor;
    check(until([&] { return viewer.TakeDisplayCursor(received_cursor); }) && received_cursor.stream_id == 9 &&
          received_cursor.cursor.x == 25 && received_cursor.cursor.sprite == cursor.sprite,
          "native cursor routing survives blocked decode without changing source coordinates");
    a->release();
    check(until([&] { return left->frames == 1; }), "first native output resumes");
    check(viewer.FocusDisplay(9) && !viewer.FocusDisplay(999), "focus accepts only subscribed sources");
    check(viewer.SaveScreenshot("") && viewer.FocusDisplay(7) && until([&] { return right->screenshots == 1; }) &&
          left->screenshots == 0 && legacy->screenshots == 0,
          "screenshot stays pinned to its chosen display when focus changes");
    streams.erase(streams.begin());
    operation({2, {}, {1}, {}}, streams);
    send(7, 640); send(9, 800);
    check(until([&] { return right->frames == 2; }) && left->frames == 1 && a->created == a->destroyed,
          "native unsubscribe releases the decoder and drops late removed-source video");
    operation({3, {}, {}, {}}, {});
    check(!viewer.SaveScreenshot("") && !viewer.Snapshot().focused_display,
          "empty selection has no screenshot target");
    check(!viewer.BindDisplay(9, right) && b->created == b->destroyed, "zero subscriptions remove all decoder bindings");
    // Exceed the old single-screen frame-stall deadline without media.
    const auto deadline = std::chrono::steady_clock::now() + 21s;
    while (std::chrono::steady_clock::now() < deadline && viewer.Snapshot().state == ViewerState::kStreaming)
        std::this_thread::sleep_for(50ms);
    check(viewer.Snapshot().state == ViewerState::kStreaming, "empty video selection survives the old 20-second frame timeout");
    streams = {description(11, 1, 800)};
    operation({4, {1}, {}, {}}, streams);
    check(viewer.BindDisplay(11, right), "rebind a newly subscribed source");
    check(until([&] { uint32_t id; while (host->take_display_keyframe_request(id)) if (id == 11) return true; return false; }),
          "re-added native source asks for its own keyframe");
    send(11, 800);
    check(until([&] { return right->frames == 3; }), "same native connection renders after empty-set resubscribe");
    streams.push_back(description(13, 0, 640));
    operation({5, {0}, {}, {}}, streams);
    check(viewer.BindDisplay(13, left) && viewer.FocusDisplay(11), "bind another output before focused recording");
    const auto folder = std::filesystem::temp_directory_path() /
        ("chast-display-recording-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    check(viewer.SetRecording(true, folder.string()) && viewer.FocusDisplay(13),
          "recording captures its target before focus changes");
    // SPS/PPS and the start of a real H.264 IDR from mp4_recorder_test.cc.
    // This verifies mux routing, not decoding of the truncated slice.
    const uint8_t access_unit[] = {
        0,0,0,1,9,0x10,0,0,0,1,0x27,0x64,0,0x32,0xac,0x2b,0x60,0x14,
        0,0x5a,0xd8,0x0b,0x50,0x60,0x60,0x64,0,0,3,0,4,0,0,3,0,0xf2,
        0x36,0x87,0x0c,0xb8,0,0,0,1,0x28,0xee,0x3c,0xb0,0,0,1,0x25,0x88,0x80,0x23,0xff
    };
    auto encoded = [&](uint32_t id, uint16_t width, uint64_t pts) {
        FrameHeader header; header.stream_id = id; header.width = width; header.height = 480;
        header.codec = VideoCodec::H264; header.keyframe = header.idr = true; header.pts_ms = pts;
        return host->send_frame_nowait(header, access_unit, sizeof(access_unit)).ok();
    };
    check(encoded(13, 640, 1000) && encoded(11, 800, 1000) &&
          until([&] { const auto s = viewer.Snapshot();
              return s.recording && !s.record_path.empty() && s.record_samples == 0; }),
          "recorder ignores the newly focused display and records only its pinned stream");
    // The muxer retains the newest sample until the next source timestamp (or
    // close) gives it a duration. Only the previous sample is written so far.
    check(encoded(13, 640, 1033) && encoded(11, 800, 1033) &&
          until([&] { return viewer.Snapshot().record_samples == 1; }), "interleaved sources never mix MP4 samples");
    const auto record_path = viewer.Snapshot().record_path;
    streams.erase(streams.begin());
    operation({6, {}, {0}, {}}, streams);
    check(until([&] { const auto s = viewer.Snapshot(); return !s.recording &&
          s.record_samples == 2 &&
          s.record_error == "the recorded display was unsubscribed"; }),
          "unsubscribing the recorded source finalizes recording without stopping its neighbour");
    uint64_t samples = 0, keyframes = 0;
    check(chast_tools::Mp4Recorder::verify(record_path, VideoCodec::H264, &samples, &keyframes, &error) &&
          samples == 2 && keyframes == 2, "finished focused recording contains exactly two selected-source samples");
    std::filesystem::remove(record_path);
    std::filesystem::remove(folder);
    viewer.Stop();
    check(a->created == a->destroyed && b->created == b->destroyed && old->created == old->destroyed &&
          a->wrong_thread == 0 && b->wrong_thread == 0, "native shutdown joins every decoder worker before returning");
}
}
int main(int argc, char**) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    int result = 0;
    try { if (argc > 1) engine_transport(); else { independent_workers(); screenshot_rebind(); } }
    catch (const std::exception& e) { std::printf("FAIL %s\n", e.what()); result = 1; }
    WSACleanup(); return result;
}
