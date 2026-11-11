#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <future>
#include <map>
#include <stdexcept>
#include <thread>
#include "chast/quic.h"
#include "../src/host/display_selection.h"

using namespace chast;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) throw std::runtime_error(what);
}
template<class Predicate> bool until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}
std::vector<uint8_t> payload_of(const std::vector<uint8_t>& message) {
    check(message.size() >= kControlHeaderSize, "message encodes");
    return {message.begin() + kControlHeaderSize, message.end()};
}
ServerInfo description(uint16_t width = 640) {
    ServerInfo info;
    info.width = info.source_width = width;
    info.height = info.source_height = 480;
    info.framerate = 30;
    return info;
}
void selection() {
    using Indices = std::vector<uint16_t>;
    CaptureDisplaysRequest request;
    request.add = {1, 1, 99}; request.sub = {1}; request.set = {2};
    check(select_capture_displays({0}, request, 3) == Indices({0, 1}), "add takes precedence, ignores unavailable indices and deduplicates");
    request.add.clear(); request.sub = {1, 2, 99};
    check(select_capture_displays({0, 1}, request, 3) == Indices({1}), "sub keeps listed existing subscriptions without adding display 2");
    request.sub.clear(); request.set = {2, 99, 2};
    check(select_capture_displays({0, 1}, request, 3) == Indices({2}), "set replaces the entire subscription set");
    request.add = {99}; request.set = {1};
    check(select_capture_displays({0}, request, 3) == Indices({0}), "invalid add preserves current selection even when set is valid");
    request.add.clear(); request.sub = {99};
    check(select_capture_displays({0}, request, 3) == Indices({0}), "invalid sub cannot unsubscribe everything");
    request.sub.clear(); request.set = {99};
    check(select_capture_displays({0}, request, 3) == Indices({0}), "invalid set preserves the working subscription");
    request.set.clear();
    check(select_capture_displays({0, 1}, request, 3).empty(), "empty set unsubscribes every display");
    request.sub = {2};
    check(select_capture_displays({0, 1}, request, 3).empty(), "valid sub can remove all existing subscriptions but never creates the listed source");
    request.sub.clear(); request.add = {kDisplayAll, kDisplayAuto};
    check(select_capture_displays({0}, request, 3) == Indices({0}), "subscription API does not confuse merged or automatic selection with display indices");
}
void protocol() {
    std::string error;
    CaptureDisplaysRequest sent{12, {0, 1, 99}, {1}, {0, 2}}, got;
    auto payload = payload_of(make_capture_displays(sent));
    check(parse_capture_displays(payload, got, &error) && got.request_id == 12 &&
          got.add == sent.add && got.sub == sent.sub && got.set == sent.set, "capture request preserves every operation and its request ID");
    payload.push_back(1);
    check(!parse_capture_displays(payload, got, &error), "request rejects trailing data");
    payload = payload_of(make_capture_displays(sent));
    payload[4] = kMaxDisplayCount + 1;
    check(!parse_capture_displays(payload, got, &error), "request rejects excessive list counts");
    sent.add.resize(kMaxDisplayCount + 1);
    check(make_capture_displays(sent).empty(), "oversized local request is not silently truncated");
    CaptureDisplaysResult result, parsed;
    result.request_id = 12;
    result.streams = {{7, 0, description()}, {9, 1, description(800)}};
    payload = payload_of(make_capture_displays_result(result));
    check(parse_capture_displays_result(payload, parsed, &error) && parsed.streams.size() == 2 &&
          parsed.streams[0].stream_id == 7 && parsed.streams[1].info.width == 800,
          "result carries separate source metadata and stable stream IDs");
    const size_t second = 9 + make_display_stream_info(result.streams[0]).size() - kControlHeaderSize;
    payload[second] = 7;
    check(!parse_capture_displays_result(payload, parsed, &error), "duplicate stream IDs are rejected on receive");
    result.streams[1].display_index = 0;
    check(make_capture_displays_result(result).empty(), "duplicate display subscriptions are rejected on send");
    result.streams.clear();
    check(parse_capture_displays_result(payload_of(make_capture_displays_result(result)), parsed, &error) &&
          parsed.streams.empty(), "empty subscription result is valid");
    DisplayStreamInfo stream{9, 1, description(800)}, decoded;
    payload = payload_of(make_display_stream_info(stream));
    check(parse_display_stream_info(payload, decoded, &error) && decoded.display_index == 1,
          "individual stream update round-trips");
    payload.push_back(1);
    check(!parse_display_stream_info(payload, decoded, &error), "individual update rejects trailing data");
    stream.info.files_only = true;
    check(make_display_stream_info(stream).empty(), "file-only descriptions cannot masquerade as desktop streams");
    uint32_t id = 0;
    payload = payload_of(make_display_keyframe_request(0xFFFFFFFF));
    check(parse_display_keyframe_request(payload, id, &error) && id == 0xFFFFFFFF,
          "targeted keyframe request preserves its complete ID");
    payload.pop_back();
    check(!parse_display_keyframe_request(payload, id, &error), "truncated keyframe request is rejected");
}
uint16_t free_port() {
    const auto socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int bound = bind(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int size = sizeof(address);
    const int read = getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address), &size);
    closesocket(socket_handle);
    check(bound == 0 && read == 0, "allocate loopback port");
    return ntohs(address.sin_port);
}
struct Pair {
    std::unique_ptr<QuicSession> host, viewer;
    Pair(SessionMode mode = SessionMode::Desktop) {
        QuicServerConfig config;
        config.bind_address = "127.0.0.1";
        config.port = free_port();
        config.allow_file_transfer = true;
        config.authenticate = [](const ClientRequest&) {
            ClientAuthResult result;
            result.action = ClientAuthAction::Allow;
            result.permissions = kPermissionFileTransfer;
            return result;
        };
        std::string error;
        host = QuicSession::create_server(config, &error);
        check(host != nullptr, "create QUIC server");
        auto serving = std::async(std::launch::async, [&] {
            auto ready = host->wait_for_client(5s);
            if (!ready.ok()) return ready;
            auto info = description();
            if (mode == SessionMode::FileTransfer) { info.files_only = true; info.mode = mode; }
            ready = host->announce(info);
            if (ready.ok() && mode == SessionMode::Desktop) {
                CaptureDisplaysResult initial;
                initial.streams = {{0, 0, info}};
                ready = host->send_capture_displays_result(initial);
            }
            return ready;
        });
        QuicClientConfig client;
        client.host = "127.0.0.1"; client.port = config.port;
        client.pinned_fingerprint = host->server_fingerprint();
        client.requested_permissions = kPermissionFileTransfer;
        client.session_mode = mode;
        viewer = QuicSession::create_client(client, &error);
        check(viewer && viewer->connect(3s).ok(), "connect pinned viewer");
        ServerInfo info;
        const auto handshaken = viewer->handshake(info, 5s);
        if (!handshaken.ok()) throw std::runtime_error("complete handshake: " + handshaken.to_string());
        check(true, "complete handshake");
        check(serving.get().ok(), "host announces stream");
    }
};
void transport() {
    Pair pair;
    const auto permissions = pair.viewer->permissions();
    check((permissions & kPermissionInput) == 0, "viewer has no input permission");
    CaptureDisplaysResult initial;
    check(until([&] { return pair.viewer->take_capture_displays_result(initial); }) &&
          initial.request_id == 0 && initial.streams.size() == 1,
          "initial subscriptions sent immediately after HelloAck survive handshake delivery");
    check(pair.viewer->request_screenshot(0).ok(), "request literal stream zero screenshot");
    ScreenshotRequest shot_request;
    check(until([&] { return pair.host->take_screenshot_request(shot_request); }) && shot_request.stream_id == 0,
          "stream zero is distinct from current-screen sentinel");
    ScreenshotResponse shot;
    shot.request_id = shot_request.request_id; shot.stream_id = 0;
    shot.width = 1920; shot.height = 1080;
    shot.total = 2 * 1024 * 1024;
    shot.count = (shot.total + kScreenshotChunkBytes - 1) / kScreenshotChunkBytes;
    for (uint32_t index = 0; index < shot.count; ++index) {
        shot.index = index;
        shot.data.assign(std::min(kScreenshotChunkBytes, shot.total - index * kScreenshotChunkBytes),
                         static_cast<uint8_t>(index));
        check(until([&] { return pair.host->send_screenshot_response(shot).ok(); }),
              "dedicated screenshot stream drains bounded chunks");
        if (index == 0) {
            ScreenshotResult incomplete;
            check(!pair.viewer->take_screenshot(incomplete), "partial screenshot remains incomplete");
            check(pair.host->send_chat("control during screenshot").ok(), "queue control after first screenshot chunk");
            std::string chat;
            check(until([&] { return pair.viewer->take_chat(chat); }) && chat == "control during screenshot",
                  "control arrives while screenshot stream is still open");
        }
    }
    ScreenshotResult picture;
    check(until([&] { return pair.viewer->take_screenshot(picture); }) && picture.png.size() == shot.total &&
          picture.png.front() == 0 && picture.png.back() == static_cast<uint8_t>(shot.count - 1),
          "multi-megabyte screenshot assembles with correct bytes and stream identity");
    for (uint32_t i = 1; i <= 4; ++i) {
        CaptureDisplaysRequest request;
        request.request_id = i;
        if (i <= 2) request.add = {static_cast<uint16_t>(i - 1)};
        if (i == 3) request.sub = {1};
        check(pair.viewer->request_capture_displays(request).ok(), "view-only viewer sends ordered mutation");
    }
    std::vector<uint16_t> current;
    for (uint32_t i = 1; i <= 4; ++i) {
        CaptureDisplaysRequest request;
        check(until([&] { return pair.host->take_capture_displays_request(request); }) && request.request_id == i,
              "QUIC preserves subscription operations rather than coalescing them");
        current = select_capture_displays(current, request, 2);
        check(current == (i == 1 ? std::vector<uint16_t>{0} : i == 2 ? std::vector<uint16_t>{0, 1} :
                          i == 3 ? std::vector<uint16_t>{1} : std::vector<uint16_t>{}),
              "ordered add/sub/empty-set produces the RustDesk subscription state");
    }
    CaptureDisplaysResult result;
    result.request_id = 5;
    result.streams = {{7, 0, description()}, {9, 1, description(800)}};
    result.streams[0].info.input_enabled = true; // stream metadata cannot grant a connection permission
    check(pair.host->send_capture_displays_result(result).ok(), "send independent subscriptions");
    CaptureDisplaysResult received;
    check(until([&] { return pair.viewer->take_capture_displays_result(received); }) && received.request_id == 5 &&
          received.streams.size() == 2 && pair.viewer->permissions() == permissions,
          "viewer receives both streams without changing connection permissions");
    for (uint32_t i = 0; i < 6; ++i) {
        const auto& stream = result.streams[i % 2];
        FrameHeader header;
        header.stream_id = stream.stream_id; header.width = stream.info.width; header.height = 480;
        header.idr = header.keyframe = i < 2;
        const uint8_t bytes[] = {static_cast<uint8_t>(stream.stream_id), static_cast<uint8_t>(i)};
        check(pair.host->send_frame_nowait(header, bytes, sizeof(bytes)).ok(), "send interleaved display frame");
    }
    for (uint32_t i = 0; i < 6; ++i) {
        FrameHeader header;
        std::vector<uint8_t> bytes;
        check(pair.viewer->receive_frame(header, bytes, 2s).ok() && header.seq == i &&
              header.stream_id == result.streams[i % 2].stream_id && bytes.size() == 2 &&
              bytes[0] == header.stream_id && bytes[1] == i && crc32(bytes.data(), bytes.size()) == header.payload_crc32,
              "interleaved frames retain stream identity, order and integrity");
    }
    for (const auto id : {7u, 9u, 7u}) check(pair.viewer->request_display_keyframe(id).ok(), "request per-stream IDR");
    uint32_t id = 0;
    check(until([&] { return pair.host->take_display_keyframe_request(id); }) && id == 7, "first targeted IDR reaches its source");
    check(until([&] { return pair.host->take_display_keyframe_request(id); }) && id == 9, "second targeted IDR remains distinct");
    result.streams[0].info.width = 320;
    result.streams[1].info.width = 400;
    for (const auto& stream : result.streams)
        check(pair.host->refresh_display_stream(stream).ok(), "send separate stream update");
    std::map<uint32_t, uint16_t> updates;
    check(until([&] {
        DisplayStreamInfo info;
        while (pair.viewer->take_display_stream_update(info)) updates[info.stream_id] = info.info.width;
        return updates.size() == 2;
    }) && updates[7] == 320 && updates[9] == 400, "format updates never overwrite a different display");
    const auto stale = result.streams[0];
    result.streams.erase(result.streams.begin());
    check(pair.host->send_capture_displays_result(result).ok(), "unsubscribe first display");
    check(until([&] { return pair.viewer->take_capture_displays_result(received); }), "receive reduced subscription set");
    check(pair.host->refresh_display_stream(stale).ok() && pair.host->refresh_display_stream(result.streams[0]).ok(),
          "send obsolete and current metadata");
    DisplayStreamInfo info;
    check(until([&] { return pair.viewer->take_display_stream_update(info); }) && info.stream_id == 9 &&
          !pair.viewer->take_display_stream_update(info), "unsubscribed source cannot leave a stale pending format");
    check(!pair.host->request_capture_displays({}).ok() && !pair.viewer->send_capture_displays_result(result).ok(),
          "subscription APIs enforce sender roles");
    check(pair.viewer->request_capture_displays({100, {0}, {}, {}}).ok() &&
          pair.viewer->request_capture_displays({101, {1}, {}, {}}).ok(), "queue mutations before disconnect");
    CaptureDisplaysRequest pending;
    check(until([&] { return pair.host->take_capture_displays_request(pending); }) && pending.request_id == 100,
          "leave a later ordered mutation pending during teardown");
    pair.viewer.reset();
    pair.host->reset_for_next_client();
    check(!pair.host->take_capture_displays_request(pending) && !pair.host->take_display_keyframe_request(id),
          "connection reset removes all subscription requests and targeted keyframes");
}
void cursor_transport() {
    Pair pair;
    CaptureDisplaysResult initial;
    check(until([&] { return pair.viewer->take_capture_displays_result(initial); }), "consume initial subscription");
    CursorUpdate shape;
    shape.visible = true; shape.x = 12; shape.y = -7;
    shape.width = shape.height = 2; shape.shape_id = 123;
    shape.sprite.assign(16, 0x40);
    auto move = shape;
    move.sprite.clear(); move.x = 35;
    check(pair.host->send_cursor(shape).ok() && pair.host->send_cursor(move).ok(), "send legacy shape then motion");
    uint64_t count = 2;
    check(until([&] { return pair.viewer->cursor_updates_received() == count; }), "legacy motion reached transport");
    CursorUpdate legacy;
    check(pair.viewer->take_cursor_update(legacy) && legacy.x == 35 && legacy.sprite == shape.sprite,
          "slow legacy consumer retains the first shape");

    CaptureDisplaysResult result, got;
    result.streams = {{7, 0, description()}, {9, 1, description(800)}};
    check(pair.host->send_capture_displays_result(result).ok() &&
          until([&] { return pair.viewer->take_capture_displays_result(got); }), "announce cursors' subscribed sources");
    auto send = [&](uint32_t id, const CursorUpdate& cursor) {
        check(pair.host->send_display_cursor({id, cursor}).ok(), "send per-display pointer state");
    };
    send(7, shape); send(9, shape); send(7, move);
    move.visible = false; move.x = -100;
    send(9, move);
    count += 4;
    check(until([&] { return pair.viewer->cursor_updates_received() == count; }), "two sources coalesce independently");
    DisplayCursorUpdate cursor;
    check(pair.viewer->take_display_cursor_update(cursor) && cursor.stream_id == 7 &&
          cursor.cursor.visible && cursor.cursor.x == 35 && cursor.cursor.sprite == shape.sprite,
          "first source retains its own motion and unread shape");
    check(pair.viewer->take_display_cursor_update(cursor) && cursor.stream_id == 9 &&
          !cursor.cursor.visible && cursor.cursor.x == -100 && cursor.cursor.sprite == shape.sprite &&
          !pair.viewer->take_display_cursor_update(cursor), "second source hide cannot overwrite first source");

    send(7, shape); send(9, shape);
    result.streams.erase(result.streams.begin());
    check(pair.host->send_capture_displays_result(result).ok() &&
          until([&] { return pair.viewer->take_capture_displays_result(got); }), "remove source with unread pointer");
    send(7, move); send(9, move); // stale source must not be counted or queued
    count += 3;
    check(until([&] { return pair.viewer->cursor_updates_received() == count; }), "late removed pointer is discarded");
    check(pair.viewer->take_display_cursor_update(cursor) && cursor.stream_id == 9 &&
          cursor.cursor.sprite == shape.sprite && !pair.viewer->take_display_cursor_update(cursor),
          "subscription replacement prunes removed state and retains surviving unread shape");
    send(9, move);
    ++count;
    check(until([&] { return pair.viewer->cursor_updates_received() == count; }) &&
          pair.viewer->take_display_cursor_update(cursor) && cursor.cursor.sprite.empty(),
          "consumed shape is not redundantly attached to later motion");

    result.streams.push_back({11, 0, description()});
    check(pair.host->send_capture_displays_result(result).ok() &&
          until([&] { return pair.viewer->take_capture_displays_result(got); }), "re-add source with fresh identity");
    send(7, move); send(11, shape); send(11, move);
    count += 2;
    check(until([&] { return pair.viewer->cursor_updates_received() == count; }) &&
          pair.viewer->take_display_cursor_update(cursor) && cursor.stream_id == 11 &&
          cursor.cursor.sprite == shape.sprite, "re-added source gets a fresh shape without reviving old identity");
    send(9, shape); send(11, shape);
    result.streams.clear();
    check(pair.host->send_capture_displays_result(result).ok() &&
          until([&] { return pair.viewer->take_capture_displays_result(got); }) &&
          !pair.viewer->take_display_cursor_update(cursor), "empty subscription clears all pending source cursors");
    check(!pair.viewer->send_display_cursor({7, shape}).ok(), "viewer cannot publish display cursor");
}
void file_session() {
    Pair pair(SessionMode::FileTransfer);
    check(!pair.viewer->request_capture_displays({}).ok() && !pair.viewer->request_display_keyframe(0).ok(),
          "non-desktop session cannot request video subscriptions");
    check(!pair.host->send_capture_displays_result({}).ok(), "non-desktop host cannot announce video subscriptions");
    check(!pair.host->send_display_cursor({}).ok(), "non-desktop host cannot publish a display cursor");
}
} // namespace

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    int result = 0;
    try { selection(); protocol(); transport(); cursor_transport(); file_session(); }
    catch (const std::exception& error) { std::printf("FAIL %s\n", error.what()); result = 1; }
    WSACleanup();
    return result;
}
