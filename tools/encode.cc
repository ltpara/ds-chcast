// End-to-end harness for the hardware encode path:
//
//   DXGI duplication -> Nv12Pass -> FrameInfo::nv12_texture
//     -> oneVPL ImportFrameSurface -> MFXVideoENCODE
//     -> elementary stream file
//
// The desktop image stays in VRAM the whole way: the runtime either maps the
// NV12 texture or GPU-copies it into its own pool. The only bytes that reach
// the CPU are the compressed access units this tool writes out, which is what
// the real server will hand to the network.
//
//   chast-encode [--display N] [--seconds S] [--codec h264|hevc] [--out file]
//                [--bitrate kbps] [--keyframe-interval MS] [--bitrate-switch MS:KBPS]
//                [--quality RATIO] [--framerate F] [--no-hdr] [--no-cursor] [--quiet]
//
// Verification with ffmpeg (elementary stream, so the codec must be named):
//   ffmpeg -f h264 -i out.h264 -f framemd5 -     (or -f hevc)
//   ffmpeg -f h264 -i out.h264 -vf "select=eq(pict_type\,I)" ...

#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "chast/capture.h"
#include "chast/encoder.h"
#include "crash_handler.h"

namespace {

struct Options {
    size_t display = 0;
    double seconds = 5.0;
    chast::VideoCodec codec = chast::VideoCodec::H264;
    std::string out_path = "capture.h264";
    UINT bitrate_kbps = 0;      // 0 = RustDesk's base_bitrate for the frame size
    UINT keyframe_ms = 0;       // 0 = only the first frame and on request
    UINT change_bitrate_ms = 0; // >0: switch to change_bitrate_kbps at this time
    UINT change_bitrate_kbps = 0;
    float quality = 0.67f;      // RustDesk Quality::Balanced
    UINT framerate = 30;
    chast::CaptureConfig config;
    bool quiet = false;
};

bool parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--display" && i + 1 < argc) {
            options.display = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (arg == "--seconds" && i + 1 < argc) {
            options.seconds = std::stod(argv[++i]);
        } else if (arg == "--codec" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "h264") {
                options.codec = chast::VideoCodec::H264;
                options.out_path = "capture.h264";
            } else if (value == "hevc" || value == "h265") {
                options.codec = chast::VideoCodec::Hevc;
                options.out_path = "capture.h265";
            } else {
                std::fprintf(stderr, "unknown codec: %s\n", value.c_str());
                return false;
            }
        } else if (arg == "--out" && i + 1 < argc) {
            options.out_path = argv[++i];
        } else if (arg == "--bitrate" && i + 1 < argc) {
            options.bitrate_kbps = static_cast<UINT>(std::stoul(argv[++i]));
        } else if (arg == "--keyframe-interval" && i + 1 < argc) {
            options.keyframe_ms = static_cast<UINT>(std::stoul(argv[++i]));
        } else if (arg == "--bitrate-switch" && i + 2 < argc) {
            // "after-ms:kbps": exercises set_bitrate() mid-stream, the way the
            // quality setting moves during a session.
            const std::string value = argv[++i];
            const size_t colon = value.find(':');
            if (colon == std::string::npos) {
                std::fprintf(stderr, "--bitrate-switch wants MS:KBPS\n");
                return false;
            }
            options.change_bitrate_ms = static_cast<UINT>(std::stoul(value.substr(0, colon)));
            options.change_bitrate_kbps = static_cast<UINT>(std::stoul(value.substr(colon + 1)));
        } else if (arg == "--quality" && i + 1 < argc) {
            options.quality = std::stof(argv[++i]);
        } else if (arg == "--framerate" && i + 1 < argc) {
            options.framerate = static_cast<UINT>(std::stoul(argv[++i]));
        } else if (arg == "--no-hdr") {
            options.config.want_hdr = false;
        } else if (arg == "--no-cursor") {
            options.config.capture_cursor = false;
        } else if (arg == "--quiet") {
            options.quiet = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    // The hardware encoder consumes NV12 and nothing else.
    options.config.nv12.enabled = true;
    options.config.nv12.colorimetry = chast::Nv12Colorimetry::Bt601Limited;
    return true;
}

const char* codec_name(chast::VideoCodec codec) {
    return codec == chast::VideoCodec::H264 ? "H.264" : "HEVC";
}

}  // namespace

int main(int argc, char** argv) {
    chast_tools::install_crash_handler();
    Options options;
    if (!parse_args(argc, argv, options)) {
        return 2;
    }

    std::string error;
    std::unique_ptr<chast::IDxgiCapturer> capturer =
        chast::create_capturer(options.display, options.config, &error);
    if (!capturer) {
        std::fprintf(stderr, "capturer creation failed: %s\n", error.c_str());
        return 1;
    }

    const chast::DisplayInfo& display = capturer->display();
    const chast::AdapterDevice adapter = capturer->adapter_device();
    if (!options.quiet) {
        std::printf("capturing %ls: %ux%u, hdr=%d, rotation=%u\n", display.device_name.c_str(),
                    display.width, display.height, display.hdr, display.rotation_degrees);
        std::printf("device: vendor %04X, LUID %08lX:%08lX\n", adapter.vendor_id,
                    static_cast<unsigned long>(adapter.luid.HighPart),
                    static_cast<unsigned long>(adapter.luid.LowPart));
        std::printf("encoder: %s available on this machine: %d\n", codec_name(options.codec),
                    chast::hardware_encoder_available(options.codec) ? 1 : 0);
    }

    // The encoder needs the first NV12 frame to know the surface size; the
    // capture config already fixes the frame size, so the encoder can be built
    // from the display description before any frame arrives.
    chast::EncoderConfig encoder_config;
    encoder_config.codec = options.codec;
    encoder_config.width = display.width;
    encoder_config.height = display.height;
    encoder_config.bitrate = options.bitrate_kbps * 1000u;
    encoder_config.framerate = options.framerate;
    encoder_config.quality_ratio = options.quality;
    encoder_config.gop_size = 0;  // key frames on request, like RustDesk's MAX_GOP

    std::unique_ptr<chast::IVideoEncoder> encoder =
        chast::create_encoder(adapter.device, adapter.luid, encoder_config, &error);
    if (!encoder) {
        std::fprintf(stderr, "encoder creation failed: %s\n", error.c_str());
        return 1;
    }
    if (!options.quiet) {
        std::printf("encoder: %s via %s, target %u kbps\n", codec_name(encoder->codec()),
                    encoder->implementation().empty() ? "oneVPL" : encoder->implementation().c_str(),
                    encoder->bitrate() / 1000u);
    }
    FILE* sink = nullptr;
    if (fopen_s(&sink, options.out_path.c_str(), "wb") != 0 || !sink) {
        std::fprintf(stderr, "could not open %s\n", options.out_path.c_str());
        return 1;
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto start = std::chrono::steady_clock::now();
    auto last_keyframe = start;
    uint64_t frames = 0;
    uint64_t encoded = 0;
    uint64_t keyframes = 0;
    uint64_t bytes = 0;
    uint64_t pointer_only = 0;
    uint64_t timeouts = 0;
    uint64_t errors = 0;
    bool force_keyframe = true;  // the first frame is always an IDR
    bool bitrate_changed = false;
    std::vector<chast::EncodedPacket> packets;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        if (options.seconds > 0.0 && elapsed >= options.seconds) {
            break;
        }

        const chast::CaptureResult result = capturer->capture(std::chrono::milliseconds(100));
        switch (result) {
            case chast::CaptureResult::NoNewFrame:
                ++timeouts;
                continue;
            case chast::CaptureResult::ReinitNeeded: {
                if (!options.quiet) {
                    std::printf("reinitializing: %s\n",
                                capturer->last_error().to_string().c_str());
                }
                chast::Error err = capturer->reinitialize();
                if (!err.ok()) {
                    std::fprintf(stderr, "reinit failed: %s\n", err.to_string().c_str());
                    break;
                }
                continue;
            }
            case chast::CaptureResult::FatalError:
                std::fprintf(stderr, "capture failed: %s\n",
                             capturer->last_error().to_string().c_str());
                fclose(sink);
                return 1;
            case chast::CaptureResult::Frame:
                break;
        }

        ++frames;
        const chast::FrameInfo& frame = capturer->frame();
        if (!frame.desktop_updated) {
            ++pointer_only;
        }
        if (!frame.nv12_texture) {
            // Without NV12 there is nothing the hardware encoder can take;
            // there is no CPU conversion to fall back on.
            ++errors;
            if (errors == 1 && !options.quiet) {
                std::fprintf(stderr, "frame has no NV12 texture: %s\n",
                             capturer->last_error().to_string().c_str());
            }
            continue;
        }

        if (options.keyframe_ms > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_keyframe).count() >=
                static_cast<long long>(options.keyframe_ms)) {
            force_keyframe = true;
        }

        if (options.change_bitrate_ms > 0 && !bitrate_changed &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >=
                static_cast<long long>(options.change_bitrate_ms)) {
            bitrate_changed = true;
            const chast::Error change = encoder->set_bitrate(options.change_bitrate_kbps * 1000u);
            if (!change.ok()) {
                std::fprintf(stderr, "set_bitrate failed: %s\n", change.to_string().c_str());
                ++errors;
            } else if (!options.quiet) {
                std::printf("bitrate switched to %u kbps at %u ms\n", encoder->bitrate() / 1000u,
                            options.change_bitrate_ms);
            }
        }

        // A pointer-only frame re-uses the previous NV12 image. It is still a
        // new frame for the remote side (the cursor moved), so it is encoded
        // again from the same texture rather than dropped.
        const uint64_t pts_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        packets.clear();
        const chast::Error err =
            encoder->encode(frame.nv12_texture, pts_ms, force_keyframe, packets);
        if (!err.ok()) {
            ++errors;
            std::fprintf(stderr, "encode failed: %s\n", err.to_string().c_str());
            if (err.status() == chast::Status::DeviceRemoved) {
                break;
            }
            continue;
        }
        for (const chast::EncodedPacket& packet : packets) {
            fwrite(packet.data.data(), 1, packet.data.size(), sink);
            bytes += packet.data.size();
            ++encoded;
            if (packet.keyframe) {
                ++keyframes;
                last_keyframe = now;
                force_keyframe = false;
            }
        }
    }

    fclose(sink);

    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("frames=%llu encoded=%llu keyframes=%llu errors=%llu pointer-only=%llu "
                "timeouts=%llu bytes=%llu kbps=%.0f fps=%.1f\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(encoded),
                static_cast<unsigned long long>(keyframes),
                static_cast<unsigned long long>(errors),
                static_cast<unsigned long long>(pointer_only),
                static_cast<unsigned long long>(timeouts),
                static_cast<unsigned long long>(bytes),
                elapsed > 0.0 ? static_cast<double>(bytes) * 8.0 / elapsed / 1000.0 : 0.0,
                elapsed > 0.0 ? static_cast<double>(encoded) / elapsed : 0.0);
    std::printf("wrote %s (%s, %llu bytes)\n", options.out_path.c_str(),
                codec_name(encoder->codec()), static_cast<unsigned long long>(bytes));
    // Where the frames went on the GPU: mapped in place, or GPU-copied into the
    // runtime's own surface pool. A system-memory path never exists here.
    std::printf("import: %s\n", encoder->shared_import()
                                    ? "shared (texture mapped, no copy)"
                                    : "copy (GPU to GPU, no system memory)");
    return errors == 0 ? 0 : 1;
}
