#define NOMINMAX
#include "host_media_stub.h"
#include "../src/host/desktop_stream.h"
#include <cstdio>
#include <stdexcept>

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
        DesktopStreamPool::request_keyframe(*capture);
        frame(*capture);
        encoder = DesktopStreamPool::encoder(*capture, config, &error);
        check(encoder && encoder->chroma() == chroma, "source adopts the negotiated chroma");
        std::vector<EncodedPacket> packets;
        check(encoder->encode(nullptr, 1, false, packets).ok() && !packets.empty() && packets.front().idr,
              "chroma change supplies an independently decodable picture");
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
    check(a.encoder && a.encoder->codec() == VideoCodec::H264 &&
              a.encoder->backend() == EncoderBackend::Software,
          "restrictive peer leaving completes software fallback without reconnecting");
    check(error.empty(), "successful software negotiation clears rejected candidates' errors");
    std::vector<EncodedPacket> resumed;
    check(a.encoder->encode(nullptr, 0, false, resumed).ok() &&
              !resumed.empty() && resumed.front().idr,
          "software recovery starts from a newly encoded IDR");
    b = open(L"runtime-right", right, kVideoCodecAll);
    check(b.encoder->codec() == VideoCodec::H264 && b.encoder->backend() == EncoderBackend::Vpl,
          "all displays agree on H264 while a healthy display keeps hardware encoding");
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

int main() {
    try {
        runtime_encoder_recovery();
        recovery_preserves_display_identity();
        full_color_voting();
        global_codec_voting();
        {
            DesktopStreamPool quality_pool;
            auto a = quality_pool.connect_quality(60, 0.67f, false);
            auto b = quality_pool.connect_quality(60, 0.67f, false);
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
        codec_admission_rollback();
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
        const auto encoded_before_delivery = state->encodes + state->repeats;
        encode(*encoder, *first);
        encode(*slow_encoder, *slow);
        check(state->encodes + state->repeats == encoded_before_delivery,
              "subscriber delivery never encodes the same image twice");
        DesktopStreamPool::request_keyframe(*first);
        frame(*first);
        const auto requested_idr_count = state->encodes + state->repeats;
        DesktopStreamPool::request_keyframe(*first);
        DesktopStreamPool::request_keyframe(*first);
        std::this_thread::sleep_for(80ms);
        check(state->encodes + state->repeats == requested_idr_count,
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
        state->moving = false;
        state->reset_requested = true;
        CaptureResult result = CaptureResult::NoNewFrame;
        for (int i = 0; i < 100 && result != CaptureResult::ReinitNeeded; ++i) {
            result = slow->capture(5ms);
            if (result == CaptureResult::Frame) encode(*slow_encoder, *slow);
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
