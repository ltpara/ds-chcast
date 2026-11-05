#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "chast/host_server.h"
#include "chast/decoder.h"
#include "chast/com_ptr.h"
#include "chast/capture.h"

using namespace std::chrono_literals;
namespace {
int failures = 0;
void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", message);
    if (!ok) ++failures;
}
bool until(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
bool gpu_picture(chast::QuicSession& session) {
    chast::ComPtr<ID3D11Device> device;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
    if (FAILED(hr)) return false;
    bool received = false;
    chast::DecoderConfig config;
    config.d3d11_device = device.get();
    config.gpu_handler = [&](const chast::GpuFrame& frame) {
        D3D11_TEXTURE2D_DESC desc{};
        if (frame.texture) frame.texture->GetDesc(&desc);
        received = frame.texture && frame.width && frame.height &&
                   desc.Usage == D3D11_USAGE_DEFAULT && desc.CPUAccessFlags == 0;
        return chast::Error::success();
    };
    std::string error;
    auto decoder = chast::create_decoder(config, &error);
    if (!decoder) return false;
    // This helper creates a fresh decoder even when inspecting a stream that
    // has been running for a while. Queued delta frames need their old decoder.
    if (!session.request_keyframe().ok()) return false;
    bool started = false;
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!received && std::chrono::steady_clock::now() < deadline) {
        chast::FrameHeader header;
        std::vector<uint8_t> payload;
        auto result = session.receive_frame(header, payload, 1s);
        if (!result.ok()) continue;
        if (!started && !header.keyframe) continue;
        started = true;
        std::vector<chast::DecodedFrame> cpu_frames;
        result = decoder->decode(payload.data(), payload.size(), header.pts_ms,
                                 header.keyframe, cpu_frames);
        if (!result.ok() || !cpu_frames.empty()) return false;
    }
    return received;
}
void shared_capture() {
    const auto displays = chast::enumerate_displays();
    check(!displays.empty(), "GPU capture test has an attached output");
    if (displays.empty()) return;
    chast::CaptureConfig cfg;
    cfg.nv12.enabled = true;
    std::string error;
    auto a = chast::create_shared_capturer(displays[0], cfg, &error);
    auto b = chast::create_shared_capturer(displays[0], cfg, &error);
    auto merged = chast::create_merged_capturer({displays[0]}, cfg, &error);
    check(a && b && merged, "individual and merged captures share one GPU duplication");
    if (!a || !b || !merged) { std::printf("%s\n", error.c_str()); return; }
    auto frame = [](chast::IDxgiCapturer& capture) {
        return until([&] { return capture.capture(30ms) == chast::CaptureResult::Frame; });
    };
    check(frame(*a) && frame(*b) && frame(*merged), "each capture reader publishes its own GPU frame");
    const UINT width = a->frame().nv12_width, height = a->frame().nv12_height;
    check(a->frame().texture != b->frame().texture &&
          a->frame().nv12_texture != b->frame().nv12_texture,
          "viewer encoders cannot overwrite another viewer's frame surfaces");
    check(b->set_nv12_scale(50).ok() && frame(*b) && b->frame().nv12_width < width &&
          b->frame().nv12_height < height && a->frame().nv12_width == width,
          "one viewer changes GPU scale without changing another viewer's resolution");
    check(a->reinitialize().ok() && b->capture(0ms) == chast::CaptureResult::ReinitNeeded,
          "readers detect a shared output rebuild");
    a.reset();
    check(b->reinitialize().ok() && frame(*b),
          "a reader survives another reader closing after a rebuild");
    merged.reset();
    check(b->set_nv12_scale(75).ok() && frame(*b),
          "closing the merged capture leaves the individual GPU reader usable");
}
void run(bool desktop, uint16_t port) {
    chast::HostServerConfig cfg;
    cfg.max_sessions = 3;
    cfg.settings.port = port;
    cfg.settings.bind_address = "127.0.0.1";
    cfg.settings.carrier = false;
    cfg.settings.token_auth = true;
    cfg.settings.access_token = "multi-session-test";
    cfg.settings.token_mode = chast::HostTokenMode::Reusable;
    cfg.settings.allow_audio = false;
    cfg.settings.allow_block_input = true;
    cfg.settings.allow_privacy_mode = true;
    cfg.settings.privacy_mode_impl = "virtual_display";
    cfg.dry_run_input = true;
    cfg.finish_timeout_ms = 200;
    cfg.log = [](const std::string& text) { std::printf("host: %s\n", text.c_str()); };
    std::string error;
    auto host = chast::HostServer::create(cfg, &error);
    check(host != nullptr, "concurrent listener starts");
    if (!host) { std::printf("%s\n", error.c_str()); return; }
    host->start();
    auto connect = [&](const std::string& token) {
        chast::QuicClientConfig cc;
        cc.port = port;
        cc.access_token = token;
        cc.pinned_fingerprint = host->status().fingerprint;
        cc.session_mode = desktop ? chast::SessionMode::Desktop : chast::SessionMode::FileTransfer;
        auto viewer = chast::QuicSession::create_client(cc, &error);
        if (!viewer) return viewer;
        chast::ServerInfo info;
        auto result = viewer->connect(3s);
        if (result.ok()) result = viewer->handshake(info, 30s);
        if (!result.ok()) {
            std::printf("handshake: %s\n", result.to_string().c_str());
            viewer.reset();
        }
        return viewer;
    };
    auto first = connect(cfg.settings.access_token);
    auto second = connect(cfg.settings.access_token);
    check(first && second, desktop ? "two GPU desktop sessions coexist" : "two file sessions coexist");
    if (!first || !second) { first.reset(); second.reset(); host->stop(); return; }
    if (desktop) {
        check(gpu_picture(*first) && gpu_picture(*second),
              "both connections deliver pictures decoded directly into GPU textures");
    }
    auto bad = connect("wrong-token");
    check(!bad, "a failed login does not displace either viewer");
    auto peers = host->sessions();
    check(peers.size() == 2 && host->status().active_sessions == 2,
          "operator sees two independent session identities");
    if (peers.size() != 2) { first.reset(); second.reset(); host->stop(); return; }
    const uint16_t bit = desktop ? chast::kPermissionInput : chast::kPermissionFileTransfer;
    check(host->set_session_permissions(peers[0].session_id, chast::kPermissionAll & ~bit),
          "operator can revoke one viewer's permission");
    check(until([&] { return !(first->permissions() & bit); }) && (second->permissions() & bit),
          "revocation reaches only the selected viewer");
    host->set_permissions(chast::kPermissionAll & ~bit);
    check(until([&] { return !(second->permissions() & bit); }),
          "global gate revokes the permission from every viewer");
    check(host->set_session_permissions(peers[1].session_id, chast::kPermissionAll),
          "operator updates a viewer while the global gate is closed");
    host->set_permissions(chast::kPermissionAll);
    check(until([&] { return second->permissions() & bit; }) && !(first->permissions() & bit),
          "restoring a global gate preserves the first viewer's individual restriction");
    if (desktop) {
        chast::InputEvent gesture;
        gesture.kind = chast::InputKind::Gesture;
        gesture.gesture = {chast::GestureAction::Zoom, 0, 120};
        check(!first->send_input(gesture).ok(), "revoked input also denies touch gestures");
        check(second->send_input(gesture).ok(), "authorized viewer sends a pinch over QUIC");
        check(until([&] {
            for (const auto& p : host->sessions())
                if (p.session_id == peers[1].session_id) return p.stats.input_events == 1;
            return false;
        }), "pinch reaches only its own host session");
        check(host->send_session_chat(peers[1].session_id, "only-second"), "chat targets selected viewer");
        std::string text;
        check(until([&] { return second->take_chat(text); }) && text == "only-second" &&
              !first->take_chat(text), "chat does not leak into another viewer");
        check(second->set_block_input(true).ok(), "second viewer requests input blocking");
        bool enabled = true;
        chast::BlockInputCode code = chast::BlockInputCode::Ok;
        std::string reason;
        check(until([&] { return second->take_block_input_state(enabled, code, reason); }) &&
              !enabled && code == chast::BlockInputCode::Denied,
              "exclusive desktop operation cannot interfere with another viewer");
        check(second->set_privacy_mode(true).ok(), "viewer requests virtual display privacy");
        chast::PrivacyModeCode privacy_code = chast::PrivacyModeCode::Ok;
        check(until([&] { return second->take_privacy_mode_state(enabled, privacy_code, reason); }) &&
              !enabled && privacy_code == chast::PrivacyModeCode::Denied,
              "virtual display privacy refuses multiple viewers before changing any display");
    } else {
        check(second->request_file_list(101, "").ok(), "remaining viewer can browse files");
        chast::FileListResult list;
        check(until([&] { return second->take_file_list_result(list); }) && list.request_id == 101,
              "file responses stay on their own connection");
    }
    check(host->disconnect_session(peers[0].session_id), "operator disconnects selected viewer");
    first.reset();
    check(until([&] { return host->sessions().size() == 1; }) && second->peer_alive(),
          "disconnecting one viewer preserves the other");
    if (desktop) {
        // A held recovery lease deterministically refuses activation before
        // any topology change, even on a machine with usbmmidd installed.
        HANDLE lease = CreateMutexW(nullptr, FALSE, L"Local\\chast-display-privacy-v1");
        check(lease != nullptr, "test reserves display recovery without altering monitors");
        if (lease) {
            check(second->set_privacy_mode(true).ok(), "single viewer requests privacy while recovery is busy");
            bool enabled = true;
            chast::PrivacyModeCode code = chast::PrivacyModeCode::Ok;
            std::string reason;
            check(until([&] { return second->take_privacy_mode_state(enabled, code, reason); }) &&
                  !enabled && code == chast::PrivacyModeCode::Failed &&
                  reason.find("recovering") != std::string::npos,
                  "busy recovery refuses privacy with an explicit reason");
            CloseHandle(lease);
            check(second->peer_alive() && gpu_picture(*second),
                  "failed virtual privacy activation leaves a working GPU video session");
        }
    }
    auto third = connect(cfg.settings.access_token);
    check(third && second->peer_alive(), "new viewer joins while another remains connected");
    third.reset();
    second.reset();
    host->stop();
    check(!host->running(), "listener and all session workers shut down");
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    run(false, 45526);
    run(true, 45527);
    shared_capture();
    return failures ? 1 : 0;
}
