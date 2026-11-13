#define NOMINMAX
#include "host_media_stub.h"
#include "../src/host/desktop_stream.h"
#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <mutex>
#include <vector>

using namespace chast;
using namespace std::chrono_literals;

void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", message);
    if (!ok) throw std::runtime_error(message);
}
void frame(IDxgiCapturer& capture) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = capture.capture(5ms);
        if (result == CaptureResult::Frame) return;
        check(result == CaptureResult::NoNewFrame, "capture remains usable");
    }
    throw std::runtime_error("timed out waiting for shared frame");
}

void codec_admission_rollback() {
    DesktopStreamPool pool;
    auto state = std::make_shared<host_test::MediaState>();
    bool fallback = false;
    std::string error;
    auto subscribe = [&](VideoCodecMask codecs) {
        return pool.subscribe(L"codecs", [state](std::string*) {
            return std::make_unique<host_test::Capture>(state);
        }, [state, &fallback](ID3D11Device*, EncoderConfig config, std::string*) {
            if (fallback) config.codec = VideoCodec::Hevc;
            return std::make_unique<host_test::Encoder>(state, config);
        }, &error, {}, codecs);
    };
    EncoderConfig config;
    config.codec = VideoCodec::Hevc;
    auto first = subscribe(kVideoCodecHevc);
    check(first != nullptr, "admit HEVC-only receiver");
    frame(*first);
    auto encoder = DesktopStreamPool::encoder(*first, config, &error);
    check(encoder && encoder->codec() == VideoCodec::Hevc, "start shared HEVC stream");
    auto alive = [&] {
        DesktopStreamPool::request_keyframe(*first);
        frame(*first);
        std::vector<EncodedPacket> packets;
        check(encoder->encode(nullptr, 0, false, packets).ok() && !packets.empty(),
              "rejected admission leaves existing compressed video usable");
    };
    alive();
    auto incompatible = subscribe(kVideoCodecH264);
    check(!incompatible && state->encoder_created == 1,
          "no common decoder rejects newcomer without replacing the encoder");
    alive();
    auto flexible = subscribe(kVideoCodecAll);
    check(flexible != nullptr, "compatible viewer joins unchanged pipeline");
    encoder.reset(); first.reset();
    first = std::move(flexible);
    frame(*first);
    encoder = DesktopStreamPool::encoder(*first, config, &error);
    alive();

    fallback = true;
    incompatible = subscribe(kVideoCodecH264);
    check(!incompatible, "hardware fallback outside decoder intersection is refused");
    alive();
    fallback = false;
    state->fail_encode_codecs = kVideoCodecH264;
    incompatible = subscribe(kVideoCodecH264);
    check(!incompatible, "replacement encoder failure refuses admission");
    alive();
    state->fail_encode_codecs = 0;
    state->silent_codecs = kVideoCodecH264;
    incompatible = subscribe(kVideoCodecH264);
    check(!incompatible, "replacement without an IDR refuses admission");
    alive();
    state->silent_codecs = 0;
    auto restricted = subscribe(kVideoCodecH264);
    check(restricted != nullptr, "same display accepts retry after encoder recovers");
    UINT rate = 0, fps = 0;
    check(!DesktopStreamPool::read_rate(*encoder, rate, fps),
          "old connection format cannot announce the replacement encoder's rate");
    frame(*restricted);
    auto restricted_encoder = DesktopStreamPool::encoder(*restricted, config, &error);
    std::vector<EncodedPacket> packets;
    check(restricted_encoder && restricted_encoder->codec() == VideoCodec::H264 &&
          restricted_encoder->encode(nullptr, 0, false, packets).ok() &&
          !packets.empty() && packets.front().idr,
          "successful retry announces compatible codec and starts with IDR");
    check(DesktopStreamPool::read_rate(*restricted_encoder, rate, fps) &&
          rate == restricted_encoder->bitrate() && fps == restricted_encoder->framerate(),
          "replacement format can report its consistent rate and FPS");
    encoder.reset(); first.reset();
    restricted_encoder.reset(); restricted.reset();
    check(state->capture_created == 1 && state->capture_destroyed == 1 &&
          state->encoder_created == state->encoder_destroyed && state->wrong_thread == 0,
          "failed admission leaks no pipelines or off-thread GPU resources");
}

void independent_cursor_sampling() {
    for (auto fps : {5u, 30u}) {
        DesktopStreamPool pool;
        auto state = std::make_shared<host_test::MediaState>();
        state->moving = true;
        std::string error;
        auto capture = pool.subscribe(L"independent-pointer", [state](std::string*) {
            return std::make_unique<host_test::Capture>(state);
        }, [state](ID3D11Device*, const EncoderConfig& config, std::string*) {
            return std::make_unique<host_test::Encoder>(state, config);
        }, &error, {}, kVideoCodecAll, {}, {},
        [state](const DisplayInfo&, CursorInfo& cursor) {
            std::lock_guard lock(state->cursor_mutex);
            cursor = state->pointer;
            return true;
        });
        check(capture != nullptr, "create independently sampled pointer source");
        frame(*capture);
        EncoderConfig config; config.framerate = fps;
        auto encoder = DesktopStreamPool::encoder(*capture, config, &error);
        check(encoder != nullptr, "configure video for pointer isolation test");
        auto received = std::make_shared<std::atomic<LONG>>(0);
        DesktopStreamPool::set_cursor_sink(*capture, [received](const CursorInfo& cursor) {
            received->store(cursor.position.x);
        });
        auto move_and_wait = [&](LONG position) {
            CursorInfo cursor; cursor.position_valid = cursor.visible = true;
            cursor.position.x = position;
            const auto start = std::chrono::steady_clock::now();
            state->set_cursor(cursor);
            while (received->load() != position && std::chrono::steady_clock::now() - start < 150ms)
                std::this_thread::sleep_for(1ms);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            std::printf("     independent cursor %u FPS: %.2f ms\n", fps,
                std::chrono::duration<double, std::milli>(elapsed).count());
            check(received->load() == position && elapsed < 150ms,
                  "pointer arrives before a low-FPS capture tick or blocked encoder returns");
        };
        move_and_wait(1);
        state->encode_delay_ms = 300;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!state->encoding_delayed && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        check(state->encoding_delayed, "video encoder is blocked for 300 ms");
        move_and_wait(2);
        state->encode_delay_ms = 0;
        DesktopStreamPool::set_cursor_sink(*capture, {});
    }
}

void cpu_encoding_without_device() {
    std::vector<uint8_t> pixels(64 * 48 * 4, 255);
    EncoderConfig h264;
    h264.width = 64; h264.height = 48;
    h264.codec = VideoCodec::H264; h264.backend = EncoderBackend::Software;
    std::string h264_error;
    check(create_encoder(nullptr, h264, &h264_error) == nullptr,
          "software H.264 is unavailable; VP9 is the only CPU codec");
    EncoderConfig config;
    config.width = 64; config.height = 48; config.framerate = 30;
    config.gop_size = 0; config.codec = VideoCodec::Vp9; config.backend = EncoderBackend::Software;
    for (auto chroma : {VideoChroma::Yuv420, VideoChroma::Yuv444}) {
        config.chroma = chroma;
        std::string error;
        auto encoder = create_encoder(nullptr, config, &error);
        check(encoder != nullptr, "software encoder opens with no D3D device");
        std::vector<EncodedPacket> packets;
        check(encoder->encode_cpu_bgra(pixels.data(), 64, 48, 0, true, packets).ok() &&
            !packets.empty() && packets.front().idr, "CPU BGRA produces an initial IDR");
        packets.clear();
        check(encoder->encode_cpu_bgra(pixels.data(), 64, 48, 33, false, packets).ok(),
              "CPU BGRA encodes subsequent delta input");
        packets.clear();
        check(encoder->encode_repeat(66, true, packets).ok() && !packets.empty() && packets.front().idr,
              "CPU repeat honors an on-demand IDR without a capture device");
    }
}

void cpu_capture_pipeline() {
    const auto displays = enumerate_displays();
    if (displays.empty()) { std::printf("skip CPU GDI pipeline: no display\n"); return; }
    CaptureConfig config; config.backend = CaptureBackend::GdiCpu; config.nv12.enabled = true;
    config.capture_cursor = false;
    DesktopStreamPool pool;
    std::string error;
    auto capture = pool.subscribe(L"cpu-gdi", [&](std::string* out) {
        return create_shared_capturer(displays.front(), config, out);
    }, create_encoder, &error);
    if (!capture) { std::printf("     CPU GDI: %s\n", error.c_str()); }
    check(capture != nullptr, "shared GDI CPU capture opens without GPU resources");
    frame(*capture);
    check(capture->frame().cpu_bgra && !capture->frame().nv12_texture && !capture->adapter_device().device,
          "shared CPU frame has pixels and no GPU texture or device");
    EncoderConfig encoder_config; encoder_config.codec = VideoCodec::Vp9; encoder_config.gop_size = 0;
    auto encoder = DesktopStreamPool::encoder(*capture, encoder_config, &error);
    if (!encoder) std::printf("     CPU encoder: %s\n", error.c_str());
    check(encoder && encoder->backend() == EncoderBackend::Software,
          "Auto selects software VP9 for CPU-only capture");
    std::vector<EncodedPacket> packets;
    check(encoder->encode(nullptr, 0, true, packets).ok() && !packets.empty() && packets.front().idr,
          "shared CPU pipeline delivers an encoded keyframe");
    std::vector<uint8_t> screenshot;
    uint32_t width = 0, height = 0;
    check(DesktopStreamPool::screenshot(*capture, screenshot, width, height, &error) &&
          screenshot.size() == size_t(width) * height * 4, "CPU screenshot reads retained BGRA directly");
    auto pending = DesktopStreamPool::screenshot_async(*capture);
    encoder.reset();
    capture.reset();
    const auto detached = pending.get();
    check(detached.error.empty() && detached.width == width && detached.height == height &&
          detached.bgra.size() == screenshot.size(),
          "pending screenshot retains its source safely after the last viewer unsubscribes");
}

void full_color_voting() {
    DesktopStreamPool pool;
    auto state = std::make_shared<host_test::MediaState>();
    state->original_bgra = true;
    std::string error;
    auto a = pool.connect_codecs(kVideoCodecVp9, VideoCodec::Vp9, kVideoCodecVp9, VideoChroma::Yuv444);
    auto capture = pool.subscribe(L"full-color", [state](std::string*) {
        return std::make_unique<host_test::Capture>(state);
    }, [state](ID3D11Device*, const EncoderConfig& config, std::string*) {
        return std::make_unique<host_test::Encoder>(state, config);
    }, &error, {}, kVideoCodecVp9, a);
    check(capture != nullptr, "create full-color subscription");
    frame(*capture);
    EncoderConfig config; config.codec = VideoCodec::Vp9;
    auto encoder = DesktopStreamPool::encoder(*capture, config, &error);
    auto expect = [&](VideoChroma chroma) {
        // Static refinement may leave repeats queued ahead of the requested
        // IDR; keep consuming until the delivered picture is decodable alone.
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        bool format = false, idr = false;
        while (!idr && std::chrono::steady_clock::now() < deadline) {
            DesktopStreamPool::request_keyframe(*capture);
            frame(*capture);
            encoder = DesktopStreamPool::encoder(*capture, config, &error);
            if (!encoder) break;
            format = encoder->chroma() == chroma;
            if (!format) break;
            std::vector<EncodedPacket> packets;
            idr = encoder->encode(nullptr, 1, false, packets).ok() && !packets.empty() &&
                  packets.front().idr;
        }
        check(encoder && format, "source adopts the negotiated chroma");
        check(idr, "chroma change supplies an independently decodable picture");
    };
    expect(VideoChroma::Yuv444);
    auto b = pool.connect_codecs(kVideoCodecVp9, VideoCodec::Vp9);
    check(DesktopStreamPool::vote_peer(*b, VideoCodec::Vp9, &error, VideoChroma::Yuv444),
          "peer without full-color capability can retain full-color preference");
    expect(VideoChroma::Yuv420);
    b.reset(); expect(VideoChroma::Yuv444);
    b = pool.connect_codecs(kVideoCodecVp9, VideoCodec::Vp9, kVideoCodecVp9);
    check(DesktopStreamPool::vote_peer(*b, VideoCodec::Vp9, &error), "one 420 preference vetoes full color");
    expect(VideoChroma::Yuv420);
    state->fail_444 = true;
    check(DesktopStreamPool::vote_peer(*b, VideoCodec::Vp9, &error, VideoChroma::Yuv444),
          "failed 444 proof keeps the same codec in 420");
    expect(VideoChroma::Yuv420);
    state->fail_444 = false;
    check(DesktopStreamPool::vote_peer(*b, VideoCodec::Vp9, &error, VideoChroma::Yuv444), "retry full color after proof recovery");
    expect(VideoChroma::Yuv444);
    state->fail_encode_codecs = kVideoCodecAll;
    check(!DesktopStreamPool::vote_peer(*b, VideoCodec::Vp9, &error), "failed candidate refuses the preference transaction");
    check(encoder->chroma() == VideoChroma::Yuv444, "failed negotiation keeps the old format");
    state->fail_encode_codecs = 0;
}

void multi_display_full_color() {
    DesktopStreamPool pool;
    auto left = std::make_shared<host_test::MediaState>();
    auto right = std::make_shared<host_test::MediaState>();
    left->original_bgra = true;
    right->original_bgra = true;
    std::string error;
    EncoderConfig config;
    config.codec = VideoCodec::Vp9;
    auto a = pool.connect_codecs(kVideoCodecVp9, VideoCodec::Vp9, kVideoCodecVp9, VideoChroma::Yuv444);
    auto open = [&](const wchar_t* key, const std::shared_ptr<host_test::MediaState>& media) {
        auto capture = pool.subscribe(key, [media](std::string*) {
            return std::make_unique<host_test::Capture>(media);
        }, [media](ID3D11Device*, const EncoderConfig& cfg, std::string*) {
            return std::make_unique<host_test::Encoder>(media, cfg);
        }, &error, {}, kVideoCodecVp9, a);
        check(capture != nullptr, "subscribe display to the shared full-color session");
        frame(*capture);
        return capture;
    };
    auto l = open(L"full-left", left);
    auto r = open(L"full-right", right);
    auto await_idr = [&](IDxgiCapturer& display, VideoChroma chroma) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            DesktopStreamPool::request_keyframe(display);
            frame(display);
            auto e = DesktopStreamPool::encoder(display, config, &error);
            std::vector<EncodedPacket> packets;
            if (e && e->chroma() == chroma && e->encode(nullptr, 1, false, packets).ok() &&
                !packets.empty() && packets.front().idr) return true;
        }
        return false;
    };
    auto both = [&](VideoChroma chroma, const char* message) {
        check(await_idr(*l, chroma) && await_idr(*r, chroma), message);
    };
    both(VideoChroma::Yuv444, "both displays start in full color");
    auto b = pool.connect_codecs(kVideoCodecVp9, VideoCodec::Vp9);
    check(DesktopStreamPool::vote_peer(*b, VideoCodec::Vp9, &error),
          "one 4:2:0 viewer restricts the shared session");
    both(VideoChroma::Yuv420, "the 4:2:0 restriction renegotiates every display");
    // The 4:4:4 proof of the right display fails while the left one can
    // produce it. Nothing may commit full color for only part of the session.
    right->fail_444 = true;
    b.reset();
    both(VideoChroma::Yuv420, "a failing display keeps every source in 4:2:0 on peer removal");
    check(DesktopStreamPool::vote_peer(*a, VideoCodec::Vp9, &error, VideoChroma::Yuv444),
          "a failed full-color proof falls back to the same codec in 4:2:0");
    both(VideoChroma::Yuv420, "explicit full color stays all-or-nothing across displays");
    right->fail_444 = false;
    check(DesktopStreamPool::vote_peer(*a, VideoCodec::Vp9, &error, VideoChroma::Yuv444),
          "full color returns when every display can prove it again");
    both(VideoChroma::Yuv444, "both displays commit full color together");
    l.reset();
    r.reset();
    check(left->capture_destroyed == 1 && right->capture_destroyed == 1 &&
          left->encoder_created == left->encoder_destroyed &&
          right->encoder_created == right->encoder_destroyed &&
          left->wrong_thread == 0 && right->wrong_thread == 0,
          "shared full-color negotiation releases both pipelines on their service threads");
}

void global_codec_voting() {
    DesktopStreamPool pool;
    struct Viewer {
        std::shared_ptr<DesktopStreamPool::CodecPeer> peer;
        std::unique_ptr<IDxgiCapturer> capture;
        std::unique_ptr<IVideoEncoder> encoder;
    };
    auto left = std::make_shared<host_test::MediaState>();
    auto right = std::make_shared<host_test::MediaState>();
    EncoderConfig config;
    std::string error;
    auto open = [&](const wchar_t* display, std::shared_ptr<host_test::MediaState> media,
                    VideoCodecMask decoders, std::optional<VideoCodec> preference) {
        Viewer viewer;
        viewer.peer = pool.connect_codecs(decoders, preference);
        viewer.capture = pool.subscribe(display, [media](std::string*) {
            return std::make_unique<host_test::Capture>(media);
        }, [media](ID3D11Device*, const EncoderConfig& cfg, std::string*) {
            return std::make_unique<host_test::Encoder>(media, cfg);
        }, &error, {}, decoders, viewer.peer);
        check(viewer.capture != nullptr, "create display subscription for global voter");
        frame(*viewer.capture);
        viewer.encoder = DesktopStreamPool::encoder(*viewer.capture, config, &error);
        return viewer;
    };
    auto refresh = [&](Viewer& viewer, VideoCodec codec) {
        frame(*viewer.capture);
        viewer.encoder = DesktopStreamPool::encoder(*viewer.capture, config, &error);
        check(viewer.encoder && viewer.encoder->codec() == codec, "display applies globally negotiated codec");
        std::vector<EncodedPacket> packets;
        check(viewer.encoder->encode(nullptr, 0, false, packets).ok() && !packets.empty(),
              "global format has a usable encoded picture");
    };
    // Observe an existing subscription without re-registering its source or
    // preference: doing so would conceal a broken switch rollback association.
    auto observes_codec = [&](Viewer& viewer, VideoCodec codec) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto result = viewer.capture->capture(5ms);
            if (result == CaptureResult::NoNewFrame) continue;
            if (result != CaptureResult::Frame) return false;
            const auto actual = DesktopStreamPool::codec(*viewer.capture);
            std::vector<EncodedPacket> packets;
            if (!viewer.encoder->encode(nullptr, 0, false, packets).ok()) return false;
            if (actual == codec && !packets.empty() && packets.front().idr) return true;
        }
        return false;
    };
    auto a = open(L"left", left, kVideoCodecAll, {});
    check(a.encoder && a.encoder->codec() == VideoCodec::Hevc, "Auto prefers HEVC over H264 and AV1");
    refresh(a, VideoCodec::Hevc);
    auto b = open(L"right", right, kVideoCodecH264, VideoCodec::Hevc);
    check(b.encoder && b.encoder->codec() == VideoCodec::H264, "cross-display capability restricts global codec");
    refresh(a, VideoCodec::H264);
    refresh(b, VideoCodec::H264);
    a.encoder = DesktopStreamPool::vote_codec(*a.capture, config, VideoCodec::Av1, &error);
    check(a.encoder && a.encoder->codec() == VideoCodec::H264, "unusable preference is accepted without breaking compatibility");
    b.encoder.reset(); b.capture.reset(); b.peer.reset();
    refresh(a, VideoCodec::Av1);
    auto c = open(L"right", right, kVideoCodecAll, VideoCodec::H264);
    check(c.encoder && c.encoder->codec() == VideoCodec::Av1, "equal votes use stable oldest-connection tie order");
    refresh(c, VideoCodec::Av1);
    auto d = open(L"right", right, kVideoCodecAll, VideoCodec::H264);
    check(d.encoder && d.encoder->codec() == VideoCodec::H264, "majority preference overrides older minority across screens");
    refresh(a, VideoCodec::H264);
    refresh(c, VideoCodec::H264);
    a.encoder = DesktopStreamPool::vote_codec(*a.capture, config, {}, &error);
    check(a.encoder && a.encoder->codec() == VideoCodec::H264, "Auto removes a vote without overriding explicit majority");
    c.encoder.reset(); c.capture.reset(); c.peer.reset();
    d.encoder.reset(); d.capture.reset(); d.peer.reset();
    refresh(a, VideoCodec::Hevc);
    // Prove every active GPU before replacing any: a second display refuses
    // the proposed common family, while the existing source can produce it.
    auto failing = std::make_shared<host_test::MediaState>();
    failing->fail_encode_codecs = kVideoCodecH264;
    auto refused = open(L"failed", failing, kVideoCodecH264, {});
    check(!refused.encoder, "candidate failing on another display refuses admission atomically");
    refused.capture.reset(); refused.peer.reset();
    a.encoder = DesktopStreamPool::encoder(*a.capture, config, &error);
    check(a.encoder && a.encoder->codec() == VideoCodec::Hevc,
          "failed global admission leaves existing negotiated encoder unchanged");
    auto temporary_media = std::make_shared<host_test::MediaState>();
    temporary_media->fail_encode_codecs = kVideoCodecHevc | kVideoCodecAv1;
    auto temporary = pool.subscribe(L"temporary", [temporary_media](std::string*) {
        return std::make_unique<host_test::Capture>(temporary_media);
    }, [temporary_media](ID3D11Device*, const EncoderConfig& cfg, std::string*) {
        return std::make_unique<host_test::Encoder>(temporary_media, cfg);
    }, &error, {}, kVideoCodecAll, a.peer);
    frame(*temporary);
    auto temporary_encoder = DesktopStreamPool::encoder(*temporary, config, &error);
    check(temporary_encoder && temporary_encoder->codec() == VideoCodec::H264,
          "temporary switch target participates while original subscription is still alive");
    check(observes_codec(a, VideoCodec::H264),
          "original source stays in global negotiation while a switch target is tentative");
    temporary_encoder.reset(); temporary.reset();
    check(observes_codec(a, VideoCodec::Hevc),
          "abandoning a switch restores original source without another vote or registration");
    check(temporary_media->capture_destroyed == 1 &&
          temporary_media->encoder_created == temporary_media->encoder_destroyed &&
          temporary_media->wrong_thread == 0,
          "abandoned switch releases all temporary GPU resources on their service thread");
    a.encoder = DesktopStreamPool::vote_codec(*a.capture, config, VideoCodec::Av1, &error);
    check(a.encoder && a.encoder->codec() == VideoCodec::Av1,
          "abandoned switch target leaves no stale capability or source association");
    check(left->capture_created == 1 && left->capture_destroyed == 0 && left->wrong_thread == 0 && right->wrong_thread == 0,
          "global votes keep capture alive and all GPU work on service threads");
}

void recovery_preserves_display_identity() {
    for (bool refresh : {false, true}) {
        DesktopStreamPool pool;
        auto state = std::make_shared<host_test::MediaState>();
        std::string error;
        auto capture = pool.subscribe(L"original", [state](std::string*) {
            return std::make_unique<host_test::Capture>(state);
        }, [state](ID3D11Device*, const EncoderConfig& config, std::string*) {
            return std::make_unique<host_test::Encoder>(state, config);
        }, &error);
        check(capture != nullptr, "open original adapter identity");
        frame(*capture);
        auto replacement = host_test::display();
        replacement.adapter_luid.LowPart = 42;
        state->set_display(replacement);
        auto expected = host_test::display();
        expected.rect.left = -640; expected.rect.right = 0;
        bool changed = false;
        const auto result = refresh ? DesktopStreamPool::refresh_display(*capture, expected, &changed) :
            capture->reinitialize();
        check(!result.ok() && capture->display().adapter_luid.LowPart == 0 &&
              capture->capture(0ms) == CaptureResult::ReinitNeeded,
              "adapter replaced during recovery cannot be adopted under the old stream identity");
    }
}

void runtime_encoder_recovery() {
    struct Viewer {
        std::shared_ptr<DesktopStreamPool::CodecPeer> peer;
        std::unique_ptr<IDxgiCapturer> capture;
        std::unique_ptr<IVideoEncoder> encoder;
    };
    DesktopStreamPool pool;
    std::string error;
    EncoderConfig config;
    auto open = [&](const wchar_t* key, const std::shared_ptr<host_test::MediaState>& media,
                    VideoCodecMask codecs, bool global = true) {
        Viewer viewer;
        if (global) viewer.peer = pool.connect_codecs(codecs, VideoCodec::Hevc);
        viewer.capture = pool.subscribe(key, [media](std::string*) {
            return std::make_unique<host_test::Capture>(media);
        }, [media](ID3D11Device*, const EncoderConfig& cfg, std::string*) {
            return std::make_unique<host_test::Encoder>(media, cfg);
        }, &error, {}, codecs, viewer.peer);
        check(viewer.capture != nullptr, "open runtime recovery display");
        frame(*viewer.capture);
        viewer.encoder = DesktopStreamPool::encoder(*viewer.capture, config, &error);
        check(viewer.encoder != nullptr, "start runtime recovery encoder");
        std::vector<EncodedPacket> initial;
        check(viewer.encoder->encode(nullptr, 0, false, initial).ok() && !initial.empty(),
              "consume initial picture before fault injection");
        return viewer;
    };
    auto wait_state = [&](Viewer& viewer, CaptureResult expected) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto result = viewer.capture->capture(5ms);
            if (result == expected) return true;
            if (result == CaptureResult::Frame) {
                std::vector<EncodedPacket> packets;
                viewer.encoder->encode(nullptr, 0, false, packets);
            } else if (result != CaptureResult::NoNewFrame) return false;
        }
        return false;
    };

    auto left = std::make_shared<host_test::MediaState>();
    auto right = std::make_shared<host_test::MediaState>();
    auto a = open(L"runtime-left", left, kVideoCodecAll);
    auto b = open(L"runtime-right", right, kVideoCodecHevc);
    const auto healthy_encoders = right->encoder_created.load();
    left->fail_hardware = true;
    DesktopStreamPool::request_keyframe(*a.capture);
    check(wait_state(a, CaptureResult::ReinitNeeded),
          "persistent hardware errors trigger recovery on a static desktop");
    std::vector<EncodedPacket> stale;
    check(a.encoder->encode(nullptr, 0, false, stale).ok() && stale.empty(),
          "old adapter cannot replay a picture after failed encoder generation");
    check(a.capture->reinitialize().ok() && left->resets == 0,
          "encoder recovery retains the working capture device");
    frame(*a.capture);
    auto unavailable = DesktopStreamPool::encoder(*a.capture, config, &error);
    check(!unavailable && b.encoder->codec() == VideoCodec::Hevc &&
              right->encoder_created == healthy_encoders,
          "HEVC-only peer prevents incompatible fallback without replacing a healthy stream");
    DesktopStreamPool::request_keyframe(*b.capture);
    frame(*b.capture);
    std::vector<EncodedPacket> healthy;
    check(b.encoder->encode(nullptr, 0, false, healthy).ok() && !healthy.empty(),
          "failed global fallback preserves other display delivery");
    b.encoder.reset(); b.capture.reset(); b.peer.reset();
    a.encoder = DesktopStreamPool::encoder(*a.capture, config, &error);
    check(a.encoder && a.encoder->codec() == VideoCodec::Vp9 &&
              a.encoder->backend() == EncoderBackend::Software,
          "restrictive peer leaving completes software fallback without reconnecting");
    check(error.empty(), "successful software negotiation clears rejected candidates' errors");
    std::vector<EncodedPacket> resumed;
    check(a.encoder->encode(nullptr, 0, false, resumed).ok() &&
              !resumed.empty() && resumed.front().idr,
          "software recovery starts from a newly encoded IDR");
    b = open(L"runtime-right", right, kVideoCodecAll);
    check(b.encoder->codec() == VideoCodec::Vp9 && b.encoder->backend() == EncoderBackend::Vpl,
          "all displays agree on VP9 while a healthy display keeps hardware encoding");
    const auto software_count = left->software_created.load();
    a.encoder = DesktopStreamPool::vote_codec(*a.capture, config, VideoCodec::Hevc, &error);
    check(a.encoder && a.encoder->backend() == EncoderBackend::Software &&
              left->software_created == software_count,
          "new preferences cannot reselect the failing hardware on this source");
    left->reset_requested = true;
    check(wait_state(a, CaptureResult::ReinitNeeded) && a.capture->reinitialize().ok(),
          "software source still follows real capture resets");
    frame(*a.capture);
    a.encoder = DesktopStreamPool::encoder(*a.capture, config, &error);
    check(a.encoder && a.encoder->backend() == EncoderBackend::Software && left->resets == 1,
          "capture rebuild retains the failed-hardware exclusion");
    a.encoder.reset(); a.capture.reset(); a.peer.reset();
    b.encoder.reset(); b.capture.reset(); b.peer.reset();
    check(left->wrong_thread == 0 && right->wrong_thread == 0 &&
              left->encoder_created == left->encoder_destroyed &&
              right->encoder_created == right->encoder_destroyed,
          "runtime failover releases encoder resources on their owning threads");

    for (auto mode : {0, 1, 2}) {
        auto media = std::make_shared<host_test::MediaState>();
        config.backend = mode == 2 ? EncoderBackend::Vpl : EncoderBackend::Auto;
        auto viewer = open(L"runtime-local", media, kVideoCodecAll, false);
        media->hardware_error = mode == 1 ? Status::DeviceRemoved : Status::Unavailable;
        if (mode == 2) media->fail_hardware = true;
        else media->fail_hardware_once = true;
        DesktopStreamPool::request_keyframe(*viewer.capture);
        if (mode == 0) {
            frame(*viewer.capture);
            std::vector<EncodedPacket> packets;
            check(viewer.encoder->encode(nullptr, 0, false, packets).ok() &&
                      !packets.empty() && packets.front().idr && media->encoder_created == 1,
                  "transient repeat failure recovers through a fresh IDR without changing backend");
        } else if (mode == 1) {
            check(wait_state(viewer, CaptureResult::ReinitNeeded) && viewer.capture->reinitialize().ok(),
                  "device loss rebuilds capture instead of reading back a dead GPU");
            frame(*viewer.capture);
            viewer.encoder = DesktopStreamPool::encoder(*viewer.capture, config, &error);
            check(viewer.encoder && viewer.encoder->backend() == EncoderBackend::Vpl && media->resets == 1,
                  "device recovery can retain hardware encoding on the rebuilt device");
        } else {
            check(wait_state(viewer, CaptureResult::FatalError),
                  "persistent failure of a pinned hardware backend is reported");
        }
        check(media->software_created == 0, "transient, device and pinned failures do not silently switch backend");
    }
}

void static_refresh_budget() {
    // RustDesk video_service/static_refresh.rs max_repeat_frames() table.
    check(DesktopStreamPool::static_refresh_budget(VideoCodec::H264, 0.67f, EncoderBackend::Vpl) == 130,
          "balanced QSV H.264 gets the RustDesk static-refresh budget");
    check(DesktopStreamPool::static_refresh_budget(VideoCodec::H264, 0.67f, EncoderBackend::Ffmpeg,
                                                   "FFmpeg 7 h264_nvenc (NVIDIA, D3D11)") == 60,
          "an FFmpeg NVENC implementation selects its own budget");
    check(DesktopStreamPool::static_refresh_budget(VideoCodec::Hevc, 0.5f, EncoderBackend::Ffmpeg,
                                                   "FFmpeg 7 hevc_qsv (Intel, D3D11)") == 30,
          "backend and quality band both drive the budget");
    check(DesktopStreamPool::static_refresh_budget(VideoCodec::Vp9, 0.1f, EncoderBackend::Software) == 40,
          "software VP9 uses the low-quality band");
    check(DesktopStreamPool::static_refresh_budget(VideoCodec::Av1, 0.67f, EncoderBackend::Vpl) == 0,
          "AV1 disables static refinement");

    DesktopStreamPool pool;
    auto state = std::make_shared<host_test::MediaState>();
    std::string error;
    auto display = pool.subscribe(L"static", [state](std::string*) {
        return std::make_unique<host_test::Capture>(state);
    }, [state](ID3D11Device*, const EncoderConfig& config, std::string*) {
        return std::make_unique<host_test::Encoder>(state, config);
    }, &error);
    check(display != nullptr, "subscribe to a static desktop");
    frame(*display);
    EncoderConfig config;
    config.codec = VideoCodec::Vp9;
    config.width = 640; config.height = 480;
    config.framerate = 60;
    auto encoder = DesktopStreamPool::encoder(*display, config, &error);
    check(encoder != nullptr, "start the static-refresh encoder");

    // 100 ms VP9 interval quantized by the 16 ms service tick: allow for
    // ~116 ms per refinement instead of assuming an exact 100 ms cadence.
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (state->repeats < 20 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    check(state->repeats == 20, "idle desktop refines the cached picture up to the VP9 budget");
    const unsigned encodes = state->encodes;
    std::this_thread::sleep_for(300ms);
    check(state->repeats == 20 && state->encodes == encodes,
          "an exhausted static-refresh budget stops repeating without new captures");

    state->moving = true;
    const auto moving_deadline = std::chrono::steady_clock::now() + 1500ms;
    while (state->encodes < encodes + 2 && std::chrono::steady_clock::now() < moving_deadline)
        std::this_thread::sleep_for(5ms);
    check(state->encodes >= encodes + 2, "new desktop frames resume after the burst");
    state->moving = false;
    const auto restart = std::chrono::steady_clock::now() + 1500ms;
    while (state->repeats == 20 && std::chrono::steady_clock::now() < restart)
        std::this_thread::sleep_for(5ms);
    check(state->repeats > 20, "a new captured frame restarts the static-refresh budget");

    encoder.reset();
    display.reset();
}

void subscriber_queue_age() {
    DesktopStreamPool pool;
    auto state = std::make_shared<host_test::MediaState>();
    state->moving = true;
    std::string error;
    auto subscribe = [&] {
        return pool.subscribe(L"queue-age", [state](std::string*) {
            return std::make_unique<host_test::Capture>(state);
        }, [state](ID3D11Device*, const EncoderConfig& config, std::string*) {
            return std::make_unique<host_test::Encoder>(state, config);
        }, &error);
    };
    auto fast = subscribe();
    check(fast != nullptr, "subscribe the fast queue-age viewer");
    frame(*fast);
    EncoderConfig config;
    config.framerate = 30;
    auto fast_encoder = DesktopStreamPool::encoder(*fast, config, &error);
    auto slow = subscribe();
    check(fast_encoder && slow, "a second viewer shares the queue-age source");
    frame(*slow);
    auto slow_encoder = DesktopStreamPool::encoder(*slow, config, &error);
    check(slow_encoder != nullptr, "create the slow viewer's encoder adapter");
    const auto encoders = state->encoder_created.load();
    const auto before = state->encodes.load();
    unsigned fast_frames = 0;
    // The slow viewer holds the frame returned by capture(), but does not
    // call encode(). The fast viewer keeps draining the same source.
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(DesktopStreamPool::kMaxSubscriberQueueAgeMs + 150);
    while (std::chrono::steady_clock::now() < deadline) {
        if (fast->capture(5ms) != CaptureResult::Frame) continue;
        std::vector<EncodedPacket> packets;
        check(fast_encoder->encode(nullptr, 0, false, packets).ok(), "fast viewer drains normally");
        if (!packets.empty()) ++fast_frames;
    }
    check(fast_frames >= 3 && state->encodes - before < 60,
          "fast viewer advances while the slow queue stays below its frame-count limit");
    std::vector<EncodedPacket> packets;
    check(slow_encoder->encode(nullptr, 0, false, packets).ok() && packets.empty(),
          "a stall between capture and encode discards the expired dependency chain");
    frame(*slow);
    check(slow_encoder->encode(nullptr, 0, false, packets).ok() &&
              !packets.empty() && packets.front().idr,
          "slow viewer resumes on a fresh on-demand IDR");
    check(state->encoder_created == encoders,
          "stale queue recovery retains the shared encoder");
    const auto expired = DesktopStreamPool::queue_stats(*slow);
    check(expired.age_resyncs == 1 && expired.expired_frames > 0 &&
              expired.delivered_wait_ms <= DesktopStreamPool::kMaxSubscriberQueueAgeMs,
          "queue telemetry counts the expired chain and fresh delivery waiting");
    check(DesktopStreamPool::queue_stats(*fast).age_resyncs == 0,
          "queue recovery counters belong to the affected subscriber");

    // Also cover a stalled reader with no unconsumed current picture.
    std::this_thread::sleep_for(std::chrono::milliseconds(
        DesktopStreamPool::kMaxSubscriberQueueAgeMs + 150));
    check(slow->capture(0ms) == CaptureResult::NoNewFrame,
          "capture rejects an aged queue before returning any stale frame");
    packets.clear();
    frame(*slow);
    check(slow_encoder->encode(nullptr, 0, false, packets).ok() &&
              !packets.empty() && packets.front().idr,
          "an expired unread queue also restarts with an IDR");
    check(DesktopStreamPool::queue_stats(*slow).age_resyncs == 2,
          "both capture and encode age recovery are observable");
    frame(*fast);
    packets.clear();
    check(fast_encoder->encode(nullptr, 0, false, packets).ok() && !packets.empty(),
          "the other viewer remains usable after stale queue recovery");
    slow_encoder.reset(); slow.reset();
    fast_encoder.reset(); fast.reset();
    check(state->wrong_thread == 0, "age recovery keeps GPU operations on the source thread");
}

void slow_encode_is_not_queue_wait() {
    DesktopStreamPool pool;
    auto state = std::make_shared<host_test::MediaState>();
    state->encode_delay_ms = DesktopStreamPool::kMaxSubscriberQueueAgeMs + 100;
    std::string error;
    auto viewer = pool.subscribe(L"slow-encode-age", [state](std::string*) {
        return std::make_unique<host_test::Capture>(state);
    }, [state](ID3D11Device*, const EncoderConfig& config, std::string*) {
        return std::make_unique<host_test::Encoder>(state, config);
    }, &error);
    check(viewer != nullptr, "subscribe with a slow encoder");
    frame(*viewer);
    EncoderConfig config;
    config.framerate = 1;
    auto encoder = DesktopStreamPool::encoder(*viewer, config, &error);
    check(encoder != nullptr, "slow encoder proves its initial IDR");
    frame(*viewer);
    std::vector<EncodedPacket> packets;
    check(encoder->encode(nullptr, 0, false, packets).ok() && !packets.empty(),
          "encoding longer than the queue budget does not expire freshly published output");
    check(DesktopStreamPool::queue_stats(*viewer).age_resyncs == 0,
          "slow source work produces no false backlog recovery events");
    encoder.reset(); viewer.reset();
}

void slow_viewer_backpressure() {
    DesktopStreamPool pool;
    auto state = std::make_shared<host_test::MediaState>();
    state->moving = true;
    std::string error;
    auto viewer = pool.subscribe(L"backpressure", [state](std::string*) {
        return std::make_unique<host_test::Capture>(state);
    }, [state](ID3D11Device*, const EncoderConfig& config, std::string*) {
        return std::make_unique<host_test::Encoder>(state, config);
    }, &error);
    check(viewer != nullptr, "subscribe a viewer that never reads");
    frame(*viewer);
    EncoderConfig config;
    config.width = 640; config.height = 480;
    config.framerate = 60;
    auto encoder = DesktopStreamPool::encoder(*viewer, config, &error);
    check(encoder != nullptr, "start encoding for the stalled viewer");
    // Filling the 60-picture window at 60 fps takes about a second.
    const auto fill = std::chrono::steady_clock::now() + 8s;
    while (state->encodes < 55 && std::chrono::steady_clock::now() < fill)
        std::this_thread::sleep_for(5ms);
    check(state->encodes >= 55, "service publishes into the bounded window");
    // Production must stop once every queue is at the high-water mark. While
    // full, the service still samples capture every tick, so require real
    // ticks with no encode instead of a wall-clock quiet window: a
    // descheduled service thread can outlast any fixed window while the last
    // picture is still in flight.
    unsigned observed = state->encodes;
    unsigned observed_captures = state->captures;
    const auto settle = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < settle) {
        std::this_thread::sleep_for(5ms);
        if (state->encodes != observed) {
            observed = state->encodes;
            observed_captures = state->captures;
        } else if (state->captures - observed_captures >= 10) {
            break;
        }
    }
    const unsigned paused_encodes = state->encodes, paused_captures = state->captures,
                   paused_idrs = state->idrs;
    std::this_thread::sleep_for(500ms);
    // RustDesk's pointer service runs beside video, so the capture tick keeps
    // sampling the desktop while every video queue is full; encoding and
    // forced IDRs still stop.
    check(state->encodes == paused_encodes && state->idrs == paused_idrs &&
              state->captures > paused_captures,
          "encoding and forced IDRs pause while every viewer is behind, pointer sampling continues");
    // One delivered picture reopens room in the window.
    frame(*viewer);
    std::vector<EncodedPacket> packets;
    check(encoder->encode(nullptr, 0, false, packets).ok() && !packets.empty(),
          "deliver one queued picture");
    const auto resume = std::chrono::steady_clock::now() + 2s;
    while ((state->captures <= paused_captures || state->encodes <= paused_encodes) &&
           std::chrono::steady_clock::now() < resume)
        std::this_thread::sleep_for(5ms);
    check(state->captures > paused_captures && state->encodes > paused_encodes,
          "one delivered picture resumes capture and encoding");
    encoder.reset();
    viewer.reset();
}

void cursor_only_wake() {
    DesktopStreamPool pool;
    auto state = std::make_shared<host_test::MediaState>();
    state->moving = true;
    std::string error;
    auto display = pool.subscribe(L"cursor", [state](std::string*) {
        return std::make_unique<host_test::Capture>(state);
    }, [state](ID3D11Device*, const EncoderConfig& config, std::string*) {
        return std::make_unique<host_test::Encoder>(state, config);
    }, &error);
    check(display != nullptr, "subscribe to a desktop for cursor updates");
    frame(*display);
    EncoderConfig config;
    config.width = 640; config.height = 480;
    config.framerate = 60;
    auto encoder = DesktopStreamPool::encoder(*display, config, &error);
    check(encoder != nullptr, "start the cursor test encoder");
    std::vector<EncodedPacket> packets;
    const auto encoded = std::chrono::steady_clock::now() + 2s;
    while (packets.empty() && std::chrono::steady_clock::now() < encoded) {
        packets.clear();
        check(encoder->encode(nullptr, 0, false, packets).ok(), "poll the first picture");
        if (packets.empty()) std::this_thread::sleep_for(5ms);
    }
    check(!packets.empty(), "deliver the moving desktop picture");
    state->moving = false;
    std::this_thread::sleep_for(100ms);
    CursorInfo pointer;
    pointer.visible = true;
    pointer.position_valid = true;
    pointer.position = {321, 123};
    pointer.width = pointer.height = 32;
    state->set_cursor(pointer);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    bool cursor_arrived = false;
    while (std::chrono::steady_clock::now() < deadline && !cursor_arrived) {
        const auto snapshot = DesktopStreamPool::cursor_snapshot(*display);
        cursor_arrived = snapshot && snapshot->position.x == 321 && snapshot->position.y == 123;
        if (!cursor_arrived) std::this_thread::sleep_for(5ms);
    }
    check(cursor_arrived, "pointer-only wake reaches subscribers as a cursor update");
    const unsigned encodes = state->encodes;
    std::this_thread::sleep_for(150ms);
    check(state->encodes == encodes,
          "pointer movement over a static desktop does not encode a video frame");
    encoder.reset();
    display.reset();
}

void cursor_sink_while_full() {
    DesktopStreamPool pool;
    auto state = std::make_shared<host_test::MediaState>();
    state->moving = true;
    std::string error;
    auto viewer = pool.subscribe(L"cursor-backpressure", [state](std::string*) {
        return std::make_unique<host_test::Capture>(state);
    }, [state](ID3D11Device*, const EncoderConfig& config, std::string*) {
        return std::make_unique<host_test::Encoder>(state, config);
    }, &error);
    check(viewer != nullptr, "subscribe to a display whose video queue will saturate");
    frame(*viewer);
    EncoderConfig config;
    config.width = 640; config.height = 480;
    config.framerate = 60;
    auto encoder = DesktopStreamPool::encoder(*viewer, config, &error);
    check(encoder != nullptr, "start the saturated-viewer encoder");
    const auto fill = std::chrono::steady_clock::now() + 8s;
    while (state->encodes < 60 && std::chrono::steady_clock::now() < fill)
        std::this_thread::sleep_for(5ms);
    check(state->encodes >= 60, "fill the stalled viewer's video window");
    // The encoder counter advances before the picture reaches the subscriber
    // queue, so the last encode can be in flight when the counter is first
    // read. Wait for real service ticks with no encode, not a wall-clock
    // quiet window: the capture tick keeps running while every queue is full.
    unsigned observed = state->encodes;
    unsigned observed_captures = state->captures;
    const auto settle = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < settle) {
        std::this_thread::sleep_for(5ms);
        if (state->encodes != observed) {
            observed = state->encodes;
            observed_captures = state->captures;
        } else if (state->captures - observed_captures >= 10) {
            break;
        }
    }

    // The subscriber sink models the host session's CursorPublisher: it runs
    // on the display service thread, beside video, like RustDesk's cursor
    // service.
    std::mutex seen_mutex;
    std::vector<CursorInfo> seen;
    DesktopStreamPool::set_cursor_sink(*viewer, [&](const CursorInfo& info) {
        std::lock_guard<std::mutex> lock(seen_mutex);
        seen.push_back(info);
    });
    const unsigned encodes = state->encodes;
    state->moving = false;
    CursorInfo pointer;
    pointer.visible = true;
    pointer.position_valid = true;
    pointer.position = {77, 55};
    pointer.width = pointer.height = 24;
    state->set_cursor(pointer);
    bool sunk = false;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!sunk && std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(seen_mutex);
            sunk = std::any_of(seen.begin(), seen.end(), [](const CursorInfo& info) {
                return info.position_valid && info.position.x == 77 && info.position.y == 55;
            });
        }
        if (!sunk) std::this_thread::sleep_for(5ms);
    }
    check(sunk, "the source hands pointer motion to the sink while every video queue is full");
    check(state->encodes == encodes,
          "pointer delivery beside saturated video encodes no frame");

    // A desktop change captured while every queue was full must reach the
    // encoder once a viewer drains a picture, not be replaced by a repeat of
    // the encoder's older input. While the queues stay full and capture has
    // no new frame, the change must stay pending rather than be encoded into
    // a window that would drop it.
    state->moving = true;
    // A desktop change must be sampled by the service thread while the
    // window is still full; a wall-clock window can pass without a tick.
    const unsigned caught = state->captures;
    const auto caught_deadline = std::chrono::steady_clock::now() + 2s;
    while (state->captures - caught < 3 && std::chrono::steady_clock::now() < caught_deadline)
        std::this_thread::sleep_for(5ms);
    state->moving = false;
    const unsigned before_resume = state->encodes;
    std::this_thread::sleep_for(300ms);
    check(state->encodes == before_resume,
          "a change caught under full queues stays pending while capture is idle");
    std::vector<EncodedPacket> packets;
    check(encoder->encode(nullptr, 0, false, packets).ok(), "drain one queued picture");
    const auto resume = std::chrono::steady_clock::now() + 2s;
    while (state->encodes == before_resume && std::chrono::steady_clock::now() < resume)
        std::this_thread::sleep_for(5ms);
    check(state->encodes > before_resume,
          "a desktop change captured while full is encoded once room opens");
    encoder.reset();
    viewer.reset();
}

int main() {
    try {
        runtime_encoder_recovery();
        recovery_preserves_display_identity();
        full_color_voting();
        multi_display_full_color();
        global_codec_voting();
        static_refresh_budget();
        subscriber_queue_age();
        slow_encode_is_not_queue_wait();
        slow_viewer_backpressure();
        {
            DesktopStreamPool quality_pool;
            auto a = quality_pool.connect_quality(60, 0.67f, false, false);
            auto b = quality_pool.connect_quality(60, 0.67f, false, false);
            auto media_a = std::make_shared<host_test::MediaState>();
            auto media_b = std::make_shared<host_test::MediaState>();
            std::string failure;
            auto open = [&](const wchar_t* key, std::shared_ptr<host_test::MediaState> media) {
                return quality_pool.subscribe(key, [media](std::string*) {
                    return std::make_unique<host_test::Capture>(media);
                }, [media](ID3D11Device*, const EncoderConfig& config, std::string*) {
                    return std::make_unique<host_test::Encoder>(media, config);
                }, &failure);
            };
            auto display_a = open(L"a", media_a), display_b = open(L"b", media_b);
            frame(*display_a); frame(*display_b);
            EncoderConfig config;
            config.framerate = 60;
            auto encoder_a = DesktopStreamPool::encoder(*display_a, config, &failure);
            auto encoder_b = DesktopStreamPool::encoder(*display_b, config, &failure);
            check(encoder_a && encoder_b, "start two displays under one quality policy");
            const auto initial_rate = encoder_a->bitrate();
            b->options(10, VideoQos::Quality::Smooth);
            encoder_a->set_rate(0, 0); encoder_b->set_rate(0, 0);
            check(encoder_a->framerate() == 10 && encoder_b->framerate() == 10 &&
                  encoder_a->bitrate() < initial_rate, "one viewer's cap and quality reach both display services");
            a->options(60, VideoQos::Quality::High);
            encoder_a->set_rate(123, 1); encoder_b->set_rate(0, 0);
            check(encoder_a->framerate() == 10 && encoder_b->framerate() == 10 &&
                  encoder_a->bitrate() > initial_rate, "latest quality wins without overriding another FPS cap");
            encoder_b.reset(); display_b.reset(); b.reset();
            encoder_a->set_rate(123, 1);
            check(encoder_a->framerate() == 60 && media_a->encoder_created == 1,
                  "leaving viewer restores remaining rate without rebuilding encoder");
        }
        independent_cursor_sampling();
        cpu_encoding_without_device();
        cpu_capture_pipeline();
        codec_admission_rollback();
        cursor_only_wake();
        cursor_sink_while_full();
        DesktopStreamPool pool;
        auto state = std::make_shared<host_test::MediaState>();
        CursorInfo pointer;
        pointer.position_valid = pointer.shape_valid = pointer.visible = true;
        pointer.position = {1, 2}; pointer.width = pointer.height = 2;
        pointer.content_id = 1; pointer.bitmap.assign(16, 0x80);
        state->set_cursor(pointer);
        auto subscribe = [&] {
            std::string error;
            auto result = pool.subscribe(L"test", [&](std::string*) {
                return std::make_unique<host_test::Capture>(state);
            }, [&](ID3D11Device*, const EncoderConfig& config, std::string*) {
                return std::make_unique<host_test::Encoder>(state, config);
            }, &error);
            check(result != nullptr, "subscribe to display");
            return result;
        };
        EncoderConfig config;
        config.width = 640; config.height = 480;
        config.framerate = 1000;
        std::string error;
        auto first = subscribe();
        frame(*first);
        auto encoder = DesktopStreamPool::encoder(*first, config, &error);
        check(encoder != nullptr, "create first shared encoder");
        std::vector<EncodedPacket> packets;
        auto encode = [&](IVideoEncoder& out, IDxgiCapturer& capture) {
            packets.clear();
            check(out.encode(capture.frame().nv12_texture, 0, false, packets).ok(), "deliver compressed picture");
            check(!packets.empty(), "picture is not dropped");
        };
        encode(*encoder, *first);
        auto slow = subscribe();
        const auto initial_cursor = DesktopStreamPool::cursor_snapshot(*first);
        check(initial_cursor && initial_cursor->position.x == 1, "snapshot current cursor independently of video delivery");
        frame(*slow);
        auto slow_encoder = DesktopStreamPool::encoder(*slow, config, &error);
        check(slow_encoder != nullptr, "late viewer attaches to encoder");
        frame(*first);
        // Static refinement repeats are not picture encodes; only a new
        // capture would make the service encode this image again.
        const auto encoded_before_delivery = state->encodes.load();
        encode(*encoder, *first);
        encode(*slow_encoder, *slow);
        check(state->encodes == encoded_before_delivery,
              "subscriber delivery never encodes the same image twice");
        // Start from a subscriber that is explicitly waiting for a fresh IDR,
        // so queued refinement repeats cannot stand in for the request.
        DesktopStreamPool::discard_backlog(*first);
        DesktopStreamPool::request_keyframe(*first);
        frame(*first);
        const auto requested_idr_count = state->idrs.load();
        DesktopStreamPool::request_keyframe(*first);
        DesktopStreamPool::request_keyframe(*first);
        std::this_thread::sleep_for(80ms);
        check(state->idrs == requested_idr_count,
              "repeated requests coalesce while the requested IDR awaits delivery");
        encode(*encoder, *first);
        state->moving = true;
        pointer.position.x = 99;
        state->set_cursor(pointer);
        const unsigned before_pause = state->encodes;
        std::this_thread::sleep_for(120ms);
        check(state->encodes > before_pause + 5,
              "display service keeps capturing and encoding while every viewer is idle");
        const auto latest_cursor = DesktopStreamPool::cursor_snapshot(*slow);
        check(latest_cursor && latest_cursor->position.x == 99 && initial_cursor->position.x == 1,
              "stalled subscriber sees latest cursor while an older immutable snapshot stays valid");
        check(state->capture_thread != GetCurrentThreadId() &&
              state->capture_thread == state->encoder_thread,
              "capture and encoding belong to the same independent service thread");
        // Both idle queues are full by now, so production is paused. Deliver a
        // picture to reopen the window before holding a real encode below.
        frame(*first);
        encode(*encoder, *first);
        state->encode_delay_ms = 700;
        const auto slow_deadline = std::chrono::steady_clock::now() + 2s;
        while (!state->encoding_delayed && std::chrono::steady_clock::now() < slow_deadline)
            std::this_thread::sleep_for(1ms);
        check(state->encoding_delayed, "hold an actual encode on the service thread");
        const auto metadata_started = std::chrono::steady_clock::now();
        const auto busy_cursor = DesktopStreamPool::cursor_snapshot(*first);
        UINT busy_rate = 0, busy_fps = 0;
        const bool busy_rate_valid = DesktopStreamPool::read_rate(*encoder, busy_rate, busy_fps);
        const auto metadata_elapsed = std::chrono::steady_clock::now() - metadata_started;
        state->encode_delay_ms = 0;
        check(busy_cursor && busy_cursor->position.x == 99 && busy_rate_valid &&
                  busy_rate == encoder->bitrate() && busy_fps == encoder->framerate(),
              "telemetry preserves cursor and matching format/rate during slow encoding");
        check(metadata_elapsed < 100ms,
              "session telemetry does not wait for a slow video encode");
        for (int i = 0; i < 75; ++i) {
            frame(*first);
            encode(*encoder, *first);
        }
        frame(*slow);
        encode(*slow_encoder, *slow);
        check(packets.front().idr, "slow subscriber resumes with IDR after bounded queue overflow");
        const auto resumed_at = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        DesktopStreamPool::discard_backlog(*slow);
        frame(*slow);
        encode(*slow_encoder, *slow);
        check(packets.front().idr && DesktopStreamPool::capture_time(*slow) >= resumed_at,
              "network recovery discards old references and starts with a newly captured IDR");
        frame(*first);
        encode(*encoder, *first);
        check(!packets.empty(), "another subscriber keeps its own frame queue during recovery");
        check(state->capture_created == 1 && state->encoder_created == 1,
              "slow subscriber does not reopen capture or encoder");
        encoder.reset();
        first.reset();
        check(state->capture_destroyed == 0 && state->encoder_destroyed == 0,
              "original subscriber leaving preserves the shared pipeline");
        // A stale generation must not replay the previous desktop's texture.
        // Pictures queued before the reset are dropped instead of delivered:
        // encoding one needs a generation that the reset is about to bump.
        state->moving = false;
        state->reset_requested = true;
        CaptureResult result = CaptureResult::NoNewFrame;
        for (int i = 0; i < 100 && result != CaptureResult::ReinitNeeded; ++i) {
            result = slow->capture(5ms);
            if (result == CaptureResult::Frame) DesktopStreamPool::discard_backlog(*slow);
        }
        state->hold_frames = true;
        check(result == CaptureResult::ReinitNeeded && slow->reinitialize().ok(), "reset shared capture");
        DesktopStreamPool::request_keyframe(*slow);
        check(slow->capture(5ms) == CaptureResult::NoNewFrame, "no cached desktop replay after reset");
        check(!DesktopStreamPool::cursor_snapshot(*slow) && initial_cursor->bitmap == pointer.bitmap,
              "rebuild clears live cursor state without invalidating an owned snapshot");
        state->hold_frames = false;
        frame(*slow);
        slow_encoder = DesktopStreamPool::encoder(*slow, config, &error);
        encode(*slow_encoder, *slow);
        check(packets.front().idr && state->capture_created == 1 && state->encoder_created == 2,
              "capture reset recreates encoder once and starts with IDR");
        slow_encoder.reset(); slow.reset();
        check(state->capture_destroyed == 1 && state->encoder_destroyed == 2,
              "last subscription releases every media resource");
        check(state->wrong_thread == 0, "creation, use, reconfiguration and destruction stay on the service thread");
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL %s\n", error.what());
        return 1;
    }
}
