// The viewer records the stream it receives, not the decoded screen: the host
// encoded access units are muxed unchanged (tools/mp4_recorder.h). An external
// player relies on properties FFmpeg's muxers get right for free but a custom
// Annex B -> MP4 rewrite does not: the first sample must sit at zero even
// though the host timestamps from the start of its session, the container
// (mvhd) and track (mdhd) durations must describe the same span, key frames
// must stay flagged so seeking works, and a picture-size change must open a
// new file rather than silently mixing two sample descriptions.
//
// The key-frame fixture is a real encoder access unit (AUD, SPS, PPS and the
// start of the IDR slice); the delta frames only need a syntactically valid
// non-IDR NAL because the muxer never parses slice data. No GPU, no network.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "mp4_recorder.h"

namespace {

// The first 528 bytes of a 2560x1440 H.264 key frame produced by the host
// encoder: access unit delimiter, SPS, PPS and the start of the IDR slice.
const unsigned char kKeyAccessUnit[] = {
    0x00, 0x00, 0x00, 0x01, 0x09, 0x10, 0x00, 0x00, 0x00, 0x01, 0x27, 0x64,
    0x00, 0x32, 0xac, 0x2b, 0x60, 0x14, 0x00, 0x5a, 0xd8, 0x0b, 0x50, 0x60,
    0x60, 0x64, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xf2,
    0x36, 0x87, 0x0c, 0xb8, 0x00, 0x00, 0x00, 0x01, 0x28, 0xee, 0x3c, 0xb0,
    0x00, 0x00, 0x01, 0x25, 0x88, 0x80, 0x23, 0xff, 0xcb, 0xdb, 0x33, 0xcb,
    0x47, 0x3c, 0x01, 0x20, 0x6c, 0xe9, 0xb9, 0x2f, 0x05, 0xdd, 0x8d, 0x25,
    0xf7, 0xec, 0x0c, 0x0c, 0xbd, 0x57, 0xc9, 0x14, 0x85, 0x59, 0x4d, 0xf9,
    0x35, 0xeb, 0xeb, 0xe3, 0xd1, 0xb1, 0x5b, 0xb5, 0x30, 0xc7, 0x69, 0x76,
    0x32, 0x5e, 0x52, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x21, 0x70, 0xb2, 0x10,
    0x68, 0x41, 0x4a, 0xa8, 0xe3, 0x5f, 0xd7, 0x3b, 0x0b, 0x89, 0xfc, 0x4d,
    0xf5, 0x68, 0x91, 0x32, 0xd7, 0x16, 0x58, 0x15, 0x8f, 0x09, 0x88, 0x69,
    0x68, 0x5b, 0xd9, 0xc4, 0x72, 0x15, 0xb4, 0x3e, 0xf4, 0x8e, 0x47, 0x09,
    0x80, 0x2d, 0x4a, 0x25, 0xf4, 0x29, 0xb3, 0x3c, 0x8e, 0x40, 0x2e, 0x3e,
    0x5b, 0x2f, 0x8b, 0xd7, 0x4a, 0x5d, 0xb7, 0xff, 0xa3, 0x61, 0x2a, 0x03,
    0xe4, 0x74, 0x66, 0xec, 0x23, 0x5b, 0xd2, 0x8b, 0x39, 0x6f, 0x51, 0xcd,
    0x72, 0x1c, 0x9d, 0x93, 0x6e, 0xc6, 0xe7, 0x1a, 0xe6, 0x2c, 0xcc, 0x44,
    0x98, 0x3c, 0xef, 0x2a, 0x65, 0xbb, 0x75, 0x94, 0x6b, 0x51, 0x99, 0x81,
    0xb7, 0xc4, 0x4a, 0x33, 0x0d, 0x6c, 0xf8, 0xb9, 0x61, 0x19, 0x5c, 0xd8,
    0xc1, 0xb9, 0x5f, 0x11, 0x48, 0x92, 0xce, 0x79, 0x62, 0xa0, 0xed, 0x9e,
    0x35, 0xcc, 0x53, 0xf9, 0x02, 0x9e, 0xc0, 0x26, 0x05, 0xe4, 0xd3, 0xae,
    0x43, 0xe4, 0x59, 0x7c, 0x7b, 0xff, 0x86, 0x04, 0x7e, 0xf7, 0xf0, 0xff,
    0x92, 0x88, 0xc9, 0xee, 0xc1, 0x8f, 0x58, 0x94, 0xfb, 0xbb, 0xcf, 0x70,
    0x86, 0x4b, 0x99, 0x9f, 0xe8, 0xad, 0xd6, 0xf3, 0x7a, 0x32, 0x69, 0x7b,
    0x24, 0x09, 0x17, 0xd6, 0x45, 0xf9, 0x54, 0x60, 0x4d, 0x97, 0x6f, 0x18,
    0xdf, 0xf0, 0x3f, 0x2f, 0x23, 0x89, 0xc2, 0x52, 0xb7, 0x6f, 0xe2, 0x81,
    0x3b, 0xa1, 0x8b, 0x32, 0x19, 0x28, 0x3a, 0xf7, 0x84, 0x8c, 0xb2, 0xb1,
    0x2a, 0x37, 0x6e, 0x6c, 0x4f, 0x04, 0xf5, 0x39, 0x0b, 0xc3, 0x6b, 0x8d,
    0x5f, 0xb3, 0x0d, 0xa3, 0xb4, 0xf7, 0xe9, 0x3e, 0x9a, 0x64, 0xde, 0x41,
    0x56, 0xeb, 0x4d, 0xdd, 0x0a, 0x86, 0xd0, 0x2e, 0xfa, 0x66, 0xb7, 0xaf,
    0xbc, 0x22, 0x8e, 0x9a, 0x29, 0x56, 0x63, 0xc1, 0x43, 0x9a, 0x27, 0x19,
    0xcc, 0x34, 0xa5, 0x64, 0x0d, 0x8f, 0x8b, 0xc9, 0xc4, 0xc9, 0x5f, 0x57,
    0x09, 0xa9, 0x89, 0x77, 0x17, 0x9e, 0x16, 0xcf, 0xf1, 0x17, 0x16, 0x6b,
    0x5c, 0x1f, 0x54, 0x48, 0x5b, 0x09, 0x0c, 0x28, 0xce, 0xd6, 0xa9, 0xb6,
    0x32, 0x66, 0xba, 0xcf, 0x3b, 0xbf, 0x39, 0xf1, 0x05, 0xb7, 0x80, 0x17,
    0xb6, 0xe0, 0x54, 0x78, 0xad, 0x62, 0x41, 0x68, 0x64, 0x5d, 0xf7, 0xf4,
    0xf9, 0x7a, 0xc8, 0xa8, 0x0f, 0x09, 0x67, 0x52, 0x27, 0x05, 0x45, 0x97,
    0x80, 0x56, 0x75, 0xed, 0x94, 0x5c, 0x89, 0x4b, 0x29, 0xc3, 0x47, 0xbb,
    0xf4, 0x7e, 0xbb, 0x97, 0x3d, 0xc6, 0xdd, 0xcd, 0xa3, 0xee, 0x46, 0xb4,
    0x6f, 0xad, 0x8f, 0x8b, 0x66, 0x42, 0x10, 0x51, 0x57, 0x32, 0x4b, 0xf5,
    0x6d, 0x39, 0x7c, 0xdb, 0x0d, 0x0b, 0x0b, 0x46, 0x80, 0xe0, 0x8c, 0xae,
    0xe8, 0xc9, 0x66, 0x47, 0x78, 0xee, 0x9c, 0xd9, 0xcf, 0xb7, 0x27, 0x99,
    0x4a, 0x5f, 0xc2, 0x69, 0x41, 0x49, 0xc5, 0x52, 0xae, 0x9c, 0x3c, 0xb4,
    0x47, 0x81, 0x9f, 0xee, 0x51, 0xee, 0xe1, 0x6e, 0xd8, 0xcb, 0x08, 0x2e,
};

// One non-IDR slice NAL (nal_ref_idc = 2, type = 1). The muxer does not parse
// the payload, it only rewrites the start code into a length.
const unsigned char kDeltaAccessUnit[] = {
    0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x00, 0x03,
    0x00, 0x00, 0x03, 0x00, 0x80,
};

struct StreamInfo {
    int64_t first_dts = -1;
    std::vector<double> sample_ms;
    int64_t frames = 0;
    int64_t keyframes = 0;
    // Container and track duration, both in seconds, read from the demuxer
    // (mvhd and mdhd respectively after libavformat's parsing).
    double container_seconds = 0.0;
    double track_seconds = 0.0;
};

bool inspect(const std::string& path, StreamInfo* info, std::string* error) {
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0 || !format) {
        *error = "could not reopen " + path;
        return false;
    }
    if (avformat_find_stream_info(format, nullptr) < 0 || format->nb_streams == 0) {
        avformat_close_input(&format);
        *error = "could not read the stream info of " + path;
        return false;
    }
    AVStream* stream = format->streams[0];
    if (stream->duration == AV_NOPTS_VALUE) {
        avformat_close_input(&format);
        *error = "the track in " + path + " has no duration";
        return false;
    }
    info->container_seconds = format->duration == AV_NOPTS_VALUE
                                  ? 0.0
                                  : static_cast<double>(format->duration) / AV_TIME_BASE;
    info->track_seconds = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream->index) {
            if (packet->dts != AV_NOPTS_VALUE) {
                if (info->first_dts < 0) {
                    info->first_dts = packet->dts;
                }
                info->sample_ms.push_back(static_cast<double>(packet->dts) *
                                          av_q2d(stream->time_base) * 1000.0);
            }
            ++info->frames;
            if ((packet->flags & AV_PKT_FLAG_KEY) != 0) {
                ++info->keyframes;
            }
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&format);
    return info->frames > 0;
}

struct Write {
    uint64_t pts_ms;
    bool keyframe;
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> bytes;
    const char* what;
};

// A segment that opened on a key frame must start at zero, carry the key
// frame flag on exactly the key pictures, and describe the same span in the
// container and the track.
bool check_segment(const std::string& path, chast::VideoCodec codec,
                   const std::vector<uint64_t>& expected_pts,
                   uint64_t expected_keyframes) {
    StreamInfo info;
    std::string error;
    if (!inspect(path, &info, &error)) {
        std::fprintf(stderr, "FAIL %s\n", error.c_str());
        return false;
    }
    uint64_t samples = 0;
    uint64_t keyframes = 0;
    if (!chast_tools::Mp4Recorder::verify(path, codec, &samples, &keyframes, &error)) {
        std::fprintf(stderr, "FAIL %s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    if (samples != info.frames || samples != expected_pts.size()) {
        std::fprintf(stderr,
                     "FAIL %s: verify counted %llu samples, demuxer %lld, expected %zu\n",
                     path.c_str(), static_cast<unsigned long long>(samples),
                     static_cast<long long>(info.frames), expected_pts.size());
        return false;
    }
    if (info.first_dts != 0) {
        std::fprintf(stderr, "FAIL %s: first sample dts is %lld, not zero\n",
                     path.c_str(), static_cast<long long>(info.first_dts));
        return false;
    }
    for (size_t i = 0; i < expected_pts.size() && i < info.sample_ms.size(); ++i) {
        const double want = static_cast<double>(expected_pts[i]);
        if (info.sample_ms[i] < want - 1.0 || info.sample_ms[i] > want + 1.0) {
            std::fprintf(stderr, "FAIL %s: sample %zu is at %.1f ms, expected %.1f\n",
                         path.c_str(), i, info.sample_ms[i], want);
            return false;
        }
    }
    if (keyframes != expected_keyframes || info.keyframes != static_cast<int64_t>(expected_keyframes)) {
        std::fprintf(stderr, "FAIL %s: %llu/%lld key frames, expected %llu\n",
                     path.c_str(), static_cast<unsigned long long>(keyframes),
                     static_cast<long long>(info.keyframes),
                     static_cast<unsigned long long>(expected_keyframes));
        return false;
    }
    const double span = static_cast<double>(expected_pts.back()) / 1000.0;
    if (info.track_seconds + 0.002 < span || info.container_seconds + 0.002 < span) {
        std::fprintf(stderr,
                     "FAIL %s: durations track=%.3fs container=%.3fs, expected %.3fs\n",
                     path.c_str(), info.track_seconds, info.container_seconds, span);
        return false;
    }
    if (info.container_seconds - info.track_seconds > 0.010 ||
        info.track_seconds - info.container_seconds > 0.010) {
        std::fprintf(stderr, "FAIL %s: container duration %.3fs disagrees with track %.3fs\n",
                     path.c_str(), info.container_seconds, info.track_seconds);
        return false;
    }
    std::printf("ok   %s: %llu samples from dts 0, %llu key frames, %.3fs\n", path.c_str(),
                static_cast<unsigned long long>(samples),
                static_cast<unsigned long long>(keyframes), info.track_seconds);
    return true;
}

}  // namespace

int main() {
    // libavformat probes the stream by decoding the first sample, and the
    // fixture's IDR is deliberately truncated; its decode errors are expected
    // and would drown the test's own result lines.
    av_log_set_level(AV_LOG_QUIET);
    const std::string base = "mp4_recorder_test.mp4";
    const std::string second = "mp4_recorder_test-2.mp4";
    std::remove(base.c_str());
    std::remove(second.c_str());

    chast_tools::Mp4Recorder recorder;
    std::string error;
    if (!recorder.open(base, chast::VideoCodec::H264, 2560, 1440, &error)) {
        std::fprintf(stderr, "FAIL the recorder refused the fixture: %s\n", error.c_str());
        return 1;
    }

    const std::vector<uint8_t> key(std::begin(kKeyAccessUnit), std::end(kKeyAccessUnit));
    const std::vector<uint8_t> delta(std::begin(kDeltaAccessUnit), std::end(kDeltaAccessUnit));
    const std::vector<Write> writes = {
        // Arrives mid-GOP: there is no configuration record yet, so the
        // recorder must skip it rather than open a segment on a delta frame.
        {4967, false, 2560, 1440, delta, "pre-key delta"},
        // The host timestamps from the start of its session; the file must
        // not inherit that origin.
        {5000, true, 2560, 1440, key, "first key frame"},
        {5033, false, 2560, 1440, delta, "delta 2"},
        {5066, false, 2560, 1440, delta, "delta 3"},
        // The resolution ladder steps down: one sample description cannot
        // describe both pictures, so this key frame opens out-2.mp4.
        {5100, true, 1280, 720, key, "second key frame"},
        {5133, false, 1280, 720, delta, "delta 4"},
    };
    for (const Write& write : writes) {
        chast::FrameHeader header;
        header.pts_ms = write.pts_ms;
        header.width = write.width;
        header.height = write.height;
        header.codec = chast::VideoCodec::H264;
        header.keyframe = write.keyframe;
        recorder.write(header, write.bytes.data(), write.bytes.size());
        if (recorder.failed()) {
            std::fprintf(stderr, "FAIL while writing the %s: %s\n", write.what,
                         recorder.error().c_str());
            return 1;
        }
    }
    recorder.close();

    if (recorder.skipped() != 1) {
        std::fprintf(stderr, "FAIL %llu pre-key frames skipped, expected 1\n",
                     static_cast<unsigned long long>(recorder.skipped()));
        return 1;
    }
    if (recorder.samples() != 5 || recorder.keyframes() != 2) {
        std::fprintf(stderr, "FAIL the recorder wrote %llu samples / %llu key frames, expected 5 / 2\n",
                     static_cast<unsigned long long>(recorder.samples()),
                     static_cast<unsigned long long>(recorder.keyframes()));
        return 1;
    }
    const std::vector<std::string>& segments = recorder.segments();
    if (segments.size() != 2 || segments[0] != base || segments[1] != second) {
        std::fprintf(stderr, "FAIL the size change did not roll the recording over\n");
        return 1;
    }

    // Each file starts its own timeline at zero: the second segment's first
    // key frame arrived at 5100 ms on the host clock.
    if (!check_segment(base, chast::VideoCodec::H264, {0, 33, 66}, 1)) {
        return 1;
    }
    if (!check_segment(second, chast::VideoCodec::H264, {0, 33}, 1)) {
        return 1;
    }
    std::printf("ok   a picture-size change opened a second segment on its key frame\n");
    std::printf("MP4 recorder PASSED\n");
    return 0;
}
