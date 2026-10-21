// Standalone decoder harness: an Annex B elementary stream in, NV12 frame
// statistics (and optionally a raw .yuv) out.
//
//   chast-decode --in capture.h264 [--codec h264|hevc] [--dump-yuv out.yuv] [--quiet]
//
// The file is split into access units at AUD NAL units, the same units the
// encoder emits and the QUIC sender puts on the wire, so this exercises exactly
// the per-access-unit path the receiver uses.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "annexb.h"
#include "chast/decoder.h"
#include "chast/protocol.h"
#include "crash_handler.h"

namespace {

struct Options {
    std::string in_path;
    chast::VideoCodec codec = chast::VideoCodec::H264;
    std::string yuv_path;
    bool quiet = false;
};

bool parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--in" && i + 1 < argc) {
            options.in_path = argv[++i];
        } else if (arg == "--codec" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "h264" || value == "avc") {
                options.codec = chast::VideoCodec::H264;
            } else if (value == "hevc" || value == "h265") {
                options.codec = chast::VideoCodec::Hevc;
            } else {
                std::fprintf(stderr, "unknown codec: %s\n", value.c_str());
                return false;
            }
        } else if (arg == "--dump-yuv" && i + 1 < argc) {
            options.yuv_path = argv[++i];
        } else if (arg == "--quiet") {
            options.quiet = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    if (options.in_path.empty()) {
        std::fprintf(stderr, "usage: chast-decode --in FILE [--codec h264|hevc] "
                             "[--dump-yuv FILE] [--quiet]\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    chast_tools::install_crash_handler();
    Options options;
    if (!parse_args(argc, argv, options)) {
        return 2;
    }

    FILE* input = nullptr;
    if (fopen_s(&input, options.in_path.c_str(), "rb") != 0 || !input) {
        std::fprintf(stderr, "cannot open %s\n", options.in_path.c_str());
        return 1;
    }
    std::vector<uint8_t> data;
    uint8_t chunk[64 * 1024];
    for (;;) {
        const size_t got = fread(chunk, 1, sizeof(chunk), input);
        data.insert(data.end(), chunk, chunk + got);
        if (got != sizeof(chunk)) {
            break;
        }
    }
    fclose(input);
    if (data.empty()) {
        std::fprintf(stderr, "%s is empty\n", options.in_path.c_str());
        return 1;
    }

    const std::vector<chast_tools::AccessUnit> units =
        chast_tools::split_access_units(data, options.codec);
    if (units.empty()) {
        std::fprintf(stderr, "no NAL units found; is this an Annex B %s stream?\n",
                     chast_tools::codec_name(options.codec));
        return 1;
    }

    chast::DecoderConfig config;
    config.codec = options.codec;
    config.copy_frames = !options.yuv_path.empty();
    std::string error;
    std::unique_ptr<chast::IVideoDecoder> decoder = chast::create_decoder(config, &error);
    if (!decoder) {
        std::fprintf(stderr, "decoder creation failed: %s\n", error.c_str());
        return 1;
    }
    if (!options.quiet) {
        std::printf("decoder: %s\n", decoder->implementation().c_str());
        std::printf("input: %s, %zu access units\n", options.in_path.c_str(), units.size());
    }

    FILE* yuv = nullptr;
    if (!options.yuv_path.empty() &&
        (fopen_s(&yuv, options.yuv_path.c_str(), "wb") != 0 || !yuv)) {
        std::fprintf(stderr, "cannot open %s\n", options.yuv_path.c_str());
        return 1;
    }

    std::vector<chast::DecodedFrame> frames;
    size_t keyframes = 0;
    uint64_t unique_hashes = 0;
    double luma_sum = 0.0;
    double variance_sum = 0.0;
    size_t errors = 0;
    for (size_t i = 0; i < units.size(); ++i) {
        if (units[i].keyframe) {
            ++keyframes;
        }
        std::vector<chast::DecodedFrame> produced;
        const uint64_t pts = static_cast<uint64_t>(i) * 33;
        chast::Error err = decoder->decode(units[i].bytes.data(), units[i].bytes.size(), pts,
                                           units[i].keyframe, produced);
        if (!err.ok()) {
            std::fprintf(stderr, "unit %zu: %s\n", i, err.to_string().c_str());
            ++errors;
            // Before the first key frame every unit is expected to be refused;
            // after it, keep going so one bad unit does not hide the rest.
            if (!decoder->initialized()) {
                continue;
            }
        }
        for (chast::DecodedFrame& frame : produced) {
            frames.push_back(std::move(frame));
        }
    }
    std::vector<chast::DecodedFrame> tail;
    chast::Error flushed = decoder->flush(tail);
    if (!flushed.ok()) {
        std::fprintf(stderr, "flush: %s\n", flushed.to_string().c_str());
        ++errors;
    }
    for (chast::DecodedFrame& frame : tail) {
        frames.push_back(std::move(frame));
    }

    uint64_t previous_hash = 0;
    bool have_previous = false;
    uint64_t digest = chast::kFrameDigestSeed;
    for (const chast::DecodedFrame& frame : frames) {
        if (!have_previous || frame.hash != previous_hash) {
            ++unique_hashes;
        }
        previous_hash = frame.hash;
        have_previous = true;
        digest = chast::mix_frame_hash(digest, frame.hash);
        luma_sum += frame.luma_mean;
        variance_sum += frame.luma_variance;
        if (yuv) {
            fwrite(frame.nv12.data(), 1, frame.nv12.size(), yuv);
        }
    }
    if (yuv) {
        fclose(yuv);
    }

    if (frames.empty()) {
        std::fprintf(stderr, "decoded no frames (%zu key frames, %zu units)\n", keyframes,
                     units.size());
        return 1;
    }
    const chast::DecodedFrame& first = frames.front();
    const double count = static_cast<double>(frames.size());
    std::printf("codec=%s size=%ux%u units=%zu keyframes=%zu decoded=%zu errors=%zu "
                "unique-hashes=%llu digest=%016llX\n",
                chast_tools::codec_name(options.codec), first.width, first.height, units.size(),
                keyframes, frames.size(), errors,
                static_cast<unsigned long long>(unique_hashes),
                static_cast<unsigned long long>(digest));
    std::printf("luma-mean=%.2f luma-variance=%.2f first-pts=%llu last-pts=%llu%s%s\n",
                luma_sum / count, variance_sum / count,
                static_cast<unsigned long long>(first.pts_ms),
                static_cast<unsigned long long>(frames.back().pts_ms),
                options.yuv_path.empty() ? "" : " yuv=", options.yuv_path.c_str());
    return 0;
}
