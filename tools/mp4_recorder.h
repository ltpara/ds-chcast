// MP4 recording of the encoded stream the viewer receives.
//
// RustDesk records the remote screen on the viewer side (src/client.rs
// record_screen -> libs/scrap/src/common/record.rs), but it re-encodes the
// decoded frames. This recorder keeps the stream the host already encoded:
// the access units are muxed into MP4 unchanged, so recording costs no
// encoder and no pixel copy.
//
// H.264/HEVC arrive as Annex B with parameter sets repeated on key frames;
// MP4 needs length-prefixed NAL units plus avcC/hvcC. AV1 arrives as OBUs;
// its first sequence header supplies av1C without the NAL conversion.
//
// A mid-session picture-size change (chast's resolution ladder stepping down
// under pressure, or a display switch) or a codec change (the viewer's stream
// options) cannot be expressed by one MP4 sample description, so the recording
// rolls over: the current segment is finalised and the key frame carrying the
// new parameter sets opens the next one, `out.mp4`, `out-2.mp4`, ... RustDesk
// restarts its recorder on a display change for the same reason.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include "annexb.h"
#include "chast/protocol.h"

namespace chast_tools {

// H.264/HEVC use Annex B parameter sets; AV1 uses low-overhead OBUs.
class Mp4Recorder {
public:
    ~Mp4Recorder() { close(); }

    Mp4Recorder(const Mp4Recorder&) = delete;
    Mp4Recorder& operator=(const Mp4Recorder&) = delete;
    Mp4Recorder() = default;

    bool open(const std::string& path, chast::VideoCodec codec, uint32_t width,
              uint32_t height, std::string* error) {
        base_path_ = path;
        codec_ = codec;
        if (!supported(codec)) {
            error_ = "unsupported recording codec: " + std::string(codec_name(codec));
            if (error) *error = error_;
            failed_ = true;
            return false;
        }
        if (width == 0 || height == 0) {
            error_ = "the stream has no picture size to record";
            if (error) *error = error_;
            failed_ = true;
            return false;
        }
        return true;
    }

    // One received access unit. A segment is opened on its first key frame,
    // because that is where the parameter sets the configuration record needs
    // live; units arriving before it (a viewer that joined mid-GOP) are
    // skipped and counted.
    void write(const chast::FrameHeader& header, const uint8_t* data, size_t size) {
        if (failed_) {
            return;
        }
        if (!header_started_) {
            if (!header.keyframe) {
                ++skipped_;
                return;
            }
            std::string error;
            if (!start(header, data, size, &error)) {
                failed_ = true;
                error_ = error;
            }
            return;
        }
        if (header.codec != codec_ || header.chroma != chroma_ || header.width != width_ || header.height != height_) {
            // The host re-announced a new picture size (the resolution ladder
            // stepped down or the display changed) or a new codec (the viewer
            // changed the stream options). One MP4 sample description cannot
            // describe both, so finalise this segment and open the next one on
            // the key frame that carries the new parameter sets. A non-key
            // frame with the new description is dropped until that key frame.
            if (!header.keyframe) {
                ++skipped_;
                return;
            }
            finalize_segment(header.pts_ms);
            if (failed_) return;
            std::string error;
            if (!start(header, data, size, &error)) {
                failed_ = true;
                error_ = error;
            }
            return;
        }
        write_packet(header, data, size);
    }

    // An optional source-clock end preserves the quiet tail of a desktop
    // session without encoding duplicate images just to advance a timestamp.
    void close(uint64_t end_pts_ms = 0) {
        finalize_segment(end_pts_ms);
        if (packet_) {
            av_packet_free(&packet_);
        }
    }

    uint64_t samples() const { return samples_; }
    uint64_t keyframes() const { return write_keyframes_; }
    uint64_t skipped() const { return skipped_; }
    // Every file this recording produced, in order: base_path_, then -2, -3…
    const std::vector<std::string>& segments() const { return segments_; }
    // The codec each segment was described with, parallel to segments().
    const std::vector<chast::VideoCodec>& segment_codecs() const { return segment_codecs_; }
    bool failed() const { return failed_; }
    const std::string& error() const { return error_; }

    static bool supported(chast::VideoCodec codec) {
        return codec == chast::VideoCodec::H264 || codec == chast::VideoCodec::Hevc ||
               codec == chast::VideoCodec::Av1 || codec == chast::VideoCodec::Vp9;
    }

    // Re-opens the finished file with the FFmpeg demuxer and counts the
    // samples it can read back, so a run proves the container is parseable
    // without an external tool. expected is the codec the viewer received;
    // a mismatch means the muxer described the stream wrongly.
    static bool verify(const std::string& path, chast::VideoCodec expected,
                       uint64_t* samples, uint64_t* keyframes, std::string* error) {
        AVFormatContext* format = nullptr;
        if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0 || !format) {
            *error = "could not reopen " + path;
            return false;
        }
        if (avformat_find_stream_info(format, nullptr) < 0) {
            avformat_close_input(&format);
            *error = "could not read the stream info of " + path;
            return false;
        }
        AVStream* stream = format->nb_streams > 0 ? format->streams[0] : nullptr;
        const AVCodecID want =
            codec_id(expected);
        if (!stream || stream->codecpar->codec_id != want ||
            stream->codecpar->width == 0 || stream->codecpar->height == 0 ||
            (expected != chast::VideoCodec::Vp9 && stream->codecpar->extradata_size == 0)) {
            avformat_close_input(&format);
            *error = "the recording has no usable " + std::string(codec_name(expected)) +
                     " stream";
            return false;
        }
        *samples = 0;
        *keyframes = 0;
        AVPacket* packet = av_packet_alloc();
        for (;;) {
            const int read = av_read_frame(format, packet);
            if (read < 0) {
                break;
            }
            if (packet->stream_index == stream->index) {
                ++*samples;
                if ((packet->flags & AV_PKT_FLAG_KEY) != 0) {
                    ++*keyframes;
                }
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        avformat_close_input(&format);
        return *samples > 0;
    }

private:
    static AVCodecID codec_id(chast::VideoCodec codec) {
        return codec == chast::VideoCodec::Hevc ? AV_CODEC_ID_HEVC :
               codec == chast::VideoCodec::Av1 ? AV_CODEC_ID_AV1 :
               codec == chast::VideoCodec::Vp9 ? AV_CODEC_ID_VP9 : AV_CODEC_ID_H264;
    }
    bool start(const chast::FrameHeader& header, const uint8_t* data, size_t size,
               std::string* error) {
        codec_ = header.codec;
        chroma_ = header.chroma;
        if (!supported(codec_) || !chast::valid_video_format(codec_, chroma_)) {
            *error = "unsupported recording codec: " + std::string(codec_name(codec_));
            return false;
        }
        std::vector<uint8_t> extradata;
        if (!build_extradata(data, size, &extradata)) {
            *error = std::string("the key frame carried no parameter sets to build ") +
                     (codec_ == chast::VideoCodec::Hevc ? "hvcC" :
                      codec_ == chast::VideoCodec::Av1 ? "av1C" : codec_ == chast::VideoCodec::Vp9 ? "vpcC" : "avcC");
            return false;
        }
        // The sample description follows the encoded picture, which is what the
        // frame header carries; the announced size is only a sanity check.
        width_ = header.width;
        height_ = header.height;
        const std::string path = segment_path();
        if (avformat_alloc_output_context2(&format_, nullptr, "mp4", path.c_str()) < 0 ||
            !format_) {
            *error = "could not create the MP4 muxer for " + path;
            return false;
        }
        stream_ = avformat_new_stream(format_, nullptr);
        if (!stream_) {
            *error = "could not create the MP4 video stream";
            return false;
        }
        stream_->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        stream_->codecpar->codec_id = codec_id(codec_);
        // Our realtime VP9 encoder emits one visible frame per access unit
        // (no alt-ref or lookahead). Its packets are already MP4 samples;
        // the optional vp9_superframe filter is neither needed nor bundled.
        if (codec_ == chast::VideoCodec::Vp9) format_->flags &= ~AVFMT_FLAG_AUTO_BSF;
        // hvc1 keeps the parameter sets in hvcC; the in-band copies the host
        // repeats on key frames are identical to them for a given encoder.
        if (codec_ == chast::VideoCodec::Hevc) {
            stream_->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
        }
        stream_->codecpar->width = static_cast<int>(width_);
        stream_->codecpar->height = static_cast<int>(height_);
        if (codec_ == chast::VideoCodec::Vp9) {
            const bool full = chroma_ == chast::VideoChroma::Yuv444;
            stream_->codecpar->format = full ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUV420P;
            stream_->codecpar->profile = full ? AV_PROFILE_VP9_1 : AV_PROFILE_VP9_0;
            stream_->codecpar->color_range = AVCOL_RANGE_MPEG;
            stream_->codecpar->color_space = AVCOL_SPC_SMPTE170M;
            stream_->codecpar->color_primaries = AVCOL_PRI_BT709;
            stream_->codecpar->color_trc = AVCOL_TRC_BT709;
            stream_->codecpar->chroma_location = AVCHROMA_LOC_LEFT;
        }
        if (!extradata.empty()) {
        stream_->codecpar->extradata =
            static_cast<uint8_t*>(av_malloc(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!stream_->codecpar->extradata) {
            *error = "out of memory for the decoder configuration record";
            return false;
        }
        std::memcpy(stream_->codecpar->extradata, extradata.data(), extradata.size());
        std::memset(stream_->codecpar->extradata + extradata.size(), 0,
                    AV_INPUT_BUFFER_PADDING_SIZE);
        stream_->codecpar->extradata_size = static_cast<int>(extradata.size());
        }
        stream_->time_base = AVRational{1, 1000};
        if (avio_open(&format_->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
            *error = "could not open " + path + " for writing";
            return false;
        }
        if (avformat_write_header(format_, nullptr) < 0) {
            *error = "could not write the MP4 header to " + path;
            return false;
        }
        segments_.push_back(path);
        segment_codecs_.push_back(codec_);
        header_started_ = true;
        first_pts_ms_ = -1;
        last_pts_ms_ = -1;
        write_packet(header, data, size);
        if (failed_) *error = error_;
        return !failed_;
    }

    // Finalises and releases the current segment. Safe to call when no segment
    // is open, so close() and a rollover share it.
    void finalize_segment(uint64_t end_pts_ms = 0) {
        if (!format_) {
            return;
        }
        if (header_started_) {
            flush_packet(end_pts_ms);
            const int result = av_write_trailer(format_);
            if (result < 0 || (format_->pb && format_->pb->error < 0)) {
                failed_ = true;
                if (error_.empty()) error_ = "could not finalize the MP4 recording: " + base_path_;
            }
        }
        if (format_->pb) {
            if (avio_closep(&format_->pb) < 0) {
                failed_ = true;
                if (error_.empty()) error_ = "could not close the MP4 recording: " + base_path_;
            }
        }
        // The context owns the stream's codecpar (including extradata), so
        // freeing the context releases both.
        avformat_free_context(format_);
        format_ = nullptr;
        stream_ = nullptr;
        header_started_ = false;
    }

    // base_path_ for the first segment, then -2, -3… before the extension:
    // out.mp4, out-2.mp4, out-3.mp4.
    std::string segment_path() const {
        if (segments_.empty()) {
            return base_path_;
        }
        const size_t dot = base_path_.find_last_of('.');
        const std::string stem = dot == std::string::npos ? base_path_ : base_path_.substr(0, dot);
        const std::string ext = dot == std::string::npos ? std::string() : base_path_.substr(dot);
        return stem + "-" + std::to_string(segments_.size() + 1) + ext;
    }

    void write_packet(const chast::FrameHeader& header, const uint8_t* data, size_t size) {
        if (!data || !size || size > INT_MAX ||
            (last_pts_ms_ >= 0 && header.pts_ms <= static_cast<uint64_t>(last_pts_ms_))) {
            ++skipped_;
            return;
        }
        std::vector<uint8_t> sample;
        if (codec_ == chast::VideoCodec::Av1 || codec_ == chast::VideoCodec::Vp9) sample.assign(data, data + size);
        else if (!to_length_prefixed(data, size, &sample)) {
            failed_ = true;
            error_ = "invalid Annex B recording sample";
            return;
        }
        flush_packet(header.pts_ms);
        if (failed_) return;
        if (!packet_) {
            packet_ = av_packet_alloc();
        }
        if (!packet_ || sample.size() > INT_MAX || av_new_packet(packet_, static_cast<int>(sample.size())) < 0) {
            failed_ = true;
            error_ = "out of memory for a recording sample";
            return;
        }
        std::memcpy(packet_->data, sample.data(), sample.size());
        // The host timestamps pictures from the start of its session, but a
        // file should start at zero. The muxer may also pick its own track
        // timescale in write_header (mov uses 1/16000 here), so convert from
        // milliseconds into whatever time base the stream now carries.
        const int64_t pts_ms = static_cast<int64_t>(header.pts_ms);
        if (first_pts_ms_ < 0) {
            first_pts_ms_ = pts_ms;
        }
        const int64_t relative_ms = pts_ms > first_pts_ms_ ? pts_ms - first_pts_ms_ : 0;
        packet_->pts = av_rescale_q(relative_ms, AVRational{1, 1000}, stream_->time_base);
        packet_->dts = packet_->pts;
        // The muxer infers each sample's duration from the next sample's
        // timestamp; the final sample keeps the duration it was given, and a
        // zero one ends the track's edit list before that sample, so players
        // drop the last picture. The previous spacing is the best estimate
        // available for the one that has nothing following it.
        const int64_t spacing_ms =
            last_pts_ms_ >= 0 && pts_ms > last_pts_ms_ ? pts_ms - last_pts_ms_ : 33;
        packet_->duration =
            av_rescale_q(spacing_ms, AVRational{1, 1000}, stream_->time_base);
        last_pts_ms_ = pts_ms;
        packet_->stream_index = stream_->index;
        packet_->flags = header.keyframe ? AV_PKT_FLAG_KEY : 0;
    }

    void flush_packet(uint64_t end_pts_ms) {
        if (!packet_ || !packet_->size) return;
        if (end_pts_ms > static_cast<uint64_t>(last_pts_ms_)) {
            packet_->duration = av_rescale_q(static_cast<int64_t>(end_pts_ms) - last_pts_ms_,
                                             AVRational{1, 1000}, stream_->time_base);
        }
        const bool keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
        if (av_interleaved_write_frame(format_, packet_) < 0) {
            failed_ = true;
            error_ = "could not write a sample to " +
                     (segments_.empty() ? base_path_ : segments_.back());
            av_packet_unref(packet_);
            return;
        }
        av_packet_unref(packet_);
        ++samples_;
        if (keyframe) {
            ++write_keyframes_;
        }
    }

    bool build_extradata(const uint8_t* data, size_t size, std::vector<uint8_t>* out) const {
        if (codec_ == chast::VideoCodec::Vp9) {
            // VP9 has no initialization data. FFmpeg writes vpcC from the
            // profile, pixel format and colorimetry on codecpar. Require a
            // matching, independently decodable 8-bit keyframe first.
            if (!data || size < 4 || (data[0] & 0xcc) != 0x80 ||
                data[1] != 0x49 || data[2] != 0x83 || data[3] != 0x42) return false;
            const unsigned profile = ((data[0] >> 5) & 1) | (((data[0] >> 4) & 1) << 1);
            return profile == (chroma_ == chast::VideoChroma::Yuv444 ? 1u : 0u);
        }
        if (codec_ == chast::VideoCodec::Av1) return av1_sequence(data, size, out);
        return codec_ == chast::VideoCodec::Hevc ? build_hvcc(data, size, out)
                                                 : build_avcc(data, size, out);
    }

    // The MP4 muxer builds av1C from a raw sequence-header OBU. Parse only
    // OBU framing here; FFmpeg owns the sequence-header syntax and profile.
    static bool av1_sequence(const uint8_t* data, size_t size, std::vector<uint8_t>* out) {
        size_t pos = 0;
        while (pos < size) {
            const size_t start = pos;
            const uint8_t header = data[pos++];
            if ((header & 0x81) || !(header & 2)) return false;
            if (header & 4) {
                if (pos == size || (data[pos++] & 7)) return false;
            }
            uint64_t length = 0;
            bool complete = false;
            for (unsigned i = 0; i < 8 && pos < size; ++i) {
                const uint8_t byte = data[pos++];
                length |= uint64_t(byte & 127) << (i * 7);
                if (!(byte & 128)) { complete = true; break; }
            }
            if (!complete || length > size - pos) return false;
            if (((header >> 3) & 15) == 1) {
                if (!length || !out->empty()) return false;
                out->assign(data + start, data + pos + static_cast<size_t>(length));
            }
            pos += static_cast<size_t>(length);
        }
        return !out->empty();
    }

    // AVCDecoderConfigurationRecord (ISO/IEC 14496-15, 5.2.4.1.1) built from
    // the SPS and PPS that precede the first IDR.
    bool build_avcc(const uint8_t* data, size_t size, std::vector<uint8_t>* out) const {
        std::vector<uint8_t> buffer(data, data + size);
        const std::vector<NalUnit> nals = scan_nals(buffer);
        const uint8_t* sps = nullptr;
        size_t sps_size = 0;
        const uint8_t* pps = nullptr;
        size_t pps_size = 0;
        for (size_t i = 0; i < nals.size(); ++i) {
            const size_t end = i + 1 < nals.size() ? nals[i + 1].start : buffer.size();
            if (nals[i].payload >= end) {
                continue;
            }
            size_t nal_end = end;
            while (nal_end > nals[i].payload && buffer[nal_end - 1] == 0) {
                --nal_end;
            }
            const unsigned type = nal_type(buffer, nals[i], chast::VideoCodec::H264);
            if (type == 7 && !sps) {
                sps = buffer.data() + nals[i].payload;
                sps_size = nal_end - nals[i].payload;
            } else if (type == 8 && !pps) {
                pps = buffer.data() + nals[i].payload;
                pps_size = nal_end - nals[i].payload;
            }
        }
        if (!sps || sps_size < 4 || !pps || pps_size == 0) {
            return false;
        }
        out->reserve(11 + sps_size + pps_size);
        out->push_back(1);
        out->push_back(sps[1]);
        out->push_back(sps[2]);
        out->push_back(sps[3]);
        out->push_back(0xFF);  // lengthSizeMinusOne = 3
        out->push_back(0xE1);  // one SPS
        put_u16(out, static_cast<uint16_t>(sps_size));
        out->insert(out->end(), sps, sps + sps_size);
        out->push_back(1);  // one PPS
        put_u16(out, static_cast<uint16_t>(pps_size));
        out->insert(out->end(), pps, pps + pps_size);
        return true;
    }

    // HEVCDecoderConfigurationRecord (ISO/IEC 14496-15, 8.3.3.1.1) built from
    // the VPS, SPS and PPS that precede the first IRAP picture. The general
    // profile/level fields are copied from the SPS profile_tier_level; the
    // chroma format and bit depths are the only syntax elements hvcC needs
    // that profile_tier_level does not carry, so the SPS is parsed just far
    // enough for them.
    bool build_hvcc(const uint8_t* data, size_t size, std::vector<uint8_t>* out) const {
        std::vector<uint8_t> buffer(data, data + size);
        const std::vector<NalUnit> nals = scan_nals(buffer);
        const uint8_t* vps = nullptr;
        size_t vps_size = 0;
        const uint8_t* sps = nullptr;
        size_t sps_size = 0;
        const uint8_t* pps = nullptr;
        size_t pps_size = 0;
        for (size_t i = 0; i < nals.size(); ++i) {
            const size_t end = i + 1 < nals.size() ? nals[i + 1].start : buffer.size();
            if (nals[i].payload >= end) {
                continue;
            }
            size_t nal_end = end;
            while (nal_end > nals[i].payload && buffer[nal_end - 1] == 0) {
                --nal_end;
            }
            const unsigned type = nal_type(buffer, nals[i], chast::VideoCodec::Hevc);
            const size_t nal_size = nal_end - nals[i].payload;
            if (type == 32 && !vps) {
                vps = buffer.data() + nals[i].payload;
                vps_size = nal_size;
            } else if (type == 33 && !sps) {
                sps = buffer.data() + nals[i].payload;
                sps_size = nal_size;
            } else if (type == 34 && !pps) {
                pps = buffer.data() + nals[i].payload;
                pps_size = nal_size;
            }
        }
        if (!vps || vps_size < 2 || !sps || sps_size < 3 || !pps || pps_size == 0) {
            return false;
        }
        HvccProfile profile;
        // The two-byte NAL unit header precedes the SPS RBSP.
        if (!parse_hvcc_profile(sps + 2, sps_size - 2, &profile)) {
            return false;
        }
        if (profile.chroma_format_idc > 3 || profile.bit_depth_luma_minus8 > 7 ||
            profile.bit_depth_chroma_minus8 > 7) {
            return false;
        }
        out->reserve(23 + 3 * 3 + vps_size + sps_size + pps_size);
        out->push_back(1);  // configurationVersion
        out->push_back(profile.profile);
        out->insert(out->end(), profile.compat, profile.compat + 4);
        out->insert(out->end(), profile.constraints, profile.constraints + 6);
        out->push_back(profile.level);
        out->push_back(0xF0);  // reserved bits, min_spatial_segmentation_idc = 0
        out->push_back(0x00);
        out->push_back(0xFC);  // reserved bits, parallelismType = 0
        out->push_back(static_cast<uint8_t>(0xFC | profile.chroma_format_idc));
        out->push_back(static_cast<uint8_t>(0xF8 | profile.bit_depth_luma_minus8));
        out->push_back(static_cast<uint8_t>(0xF8 | profile.bit_depth_chroma_minus8));
        put_u16(out, 0);       // avgFrameRate
        out->push_back(0x0F);  // one temporal layer, nested, 4-byte lengths
        out->push_back(3);     // VPS, SPS, PPS
        append_hvcc_array(out, 32, vps, vps_size);
        append_hvcc_array(out, 33, sps, sps_size);
        append_hvcc_array(out, 34, pps, pps_size);
        return true;
    }

    struct HvccProfile {
        uint8_t profile = 0;  // profile_space(2) | tier(1) | profile_idc(5)
        uint8_t compat[4] = {};
        uint8_t constraints[6] = {};
        uint8_t level = 0;
        unsigned chroma_format_idc = 1;
        unsigned bit_depth_luma_minus8 = 0;
        unsigned bit_depth_chroma_minus8 = 0;
    };

    struct BitReader {
        const std::vector<uint8_t>& bytes;
        size_t bit = 0;

        bool u(unsigned count, uint32_t* value) {
            uint32_t v = 0;
            for (unsigned i = 0; i < count; ++i) {
                const size_t byte = bit >> 3;
                if (byte >= bytes.size()) {
                    return false;
                }
                v = (v << 1) | ((bytes[byte] >> (7 - (bit & 7))) & 1);
                ++bit;
            }
            *value = v;
            return true;
        }

        bool skip(size_t count) {
            if (count > bytes.size() * 8 || bit + count > bytes.size() * 8) {
                return false;
            }
            bit += count;
            return true;
        }

        // Unsigned Exp-Golomb, ISO/IEC 14496-10, 9.1.
        bool ue(uint32_t* value) {
            unsigned zeros = 0;
            for (;;) {
                uint32_t one = 0;
                if (!u(1, &one)) {
                    return false;
                }
                if (one != 0) {
                    break;
                }
                if (++zeros > 31) {
                    return false;
                }
            }
            uint32_t rest = 0;
            if (zeros > 0 && !u(zeros, &rest)) {
                return false;
            }
            *value = ((1u << zeros) - 1) + rest;
            return true;
        }
    };

    // Emulation-prevention bytes (00 00 03) are not syntax; strip them before
    // reading the SPS bitstream.
    static std::vector<uint8_t> strip_emulation_prevention(const uint8_t* data, size_t size) {
        std::vector<uint8_t> rbsp;
        rbsp.reserve(size);
        unsigned zeros = 0;
        for (size_t i = 0; i < size; ++i) {
            if (zeros >= 2 && data[i] == 0x03) {
                zeros = 0;
                continue;
            }
            rbsp.push_back(data[i]);
            if (data[i] == 0) {
                ++zeros;
            } else {
                zeros = 0;
            }
        }
        return rbsp;
    }

    static bool parse_hvcc_profile(const uint8_t* rbsp, size_t size, HvccProfile* out) {
        const std::vector<uint8_t> clean = strip_emulation_prevention(rbsp, size);
        BitReader reader{clean};
        uint32_t value = 0;
        if (!reader.skip(4)) {  // sps_video_parameter_set_id
            return false;
        }
        uint32_t max_sub_layers_minus1 = 0;
        if (!reader.u(3, &max_sub_layers_minus1) || !reader.skip(1)) {
            return false;  // sps_max_sub_layers_minus1, sps_temporal_id_nesting_flag
        }
        // profile_tier_level(1, sps_max_sub_layers_minus1)
        if (!reader.u(8, &value)) {
            return false;
        }
        out->profile = static_cast<uint8_t>(value);
        for (int i = 0; i < 4; ++i) {
            if (!reader.u(8, &value)) {
                return false;
            }
            out->compat[i] = static_cast<uint8_t>(value);
        }
        for (int i = 0; i < 6; ++i) {
            if (!reader.u(8, &value)) {
                return false;
            }
            out->constraints[i] = static_cast<uint8_t>(value);
        }
        if (!reader.u(8, &value)) {
            return false;
        }
        out->level = static_cast<uint8_t>(value);
        if (max_sub_layers_minus1 > 0) {
            std::vector<uint32_t> profile_present(max_sub_layers_minus1);
            std::vector<uint32_t> level_present(max_sub_layers_minus1);
            for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
                if (!reader.u(1, &profile_present[i]) || !reader.u(1, &level_present[i])) {
                    return false;
                }
            }
            if (!reader.skip((8 - max_sub_layers_minus1) * 2)) {
                return false;
            }
            for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
                if (profile_present[i] && !reader.skip(88)) {
                    return false;
                }
                if (level_present[i] && !reader.skip(8)) {
                    return false;
                }
            }
        }
        // sps_seq_parameter_set_id, then the fields hvcC mirrors.
        if (!reader.ue(&value)) {
            return false;
        }
        if (!reader.ue(&out->chroma_format_idc)) {
            return false;
        }
        if (out->chroma_format_idc == 3 && !reader.skip(1)) {
            return false;  // separate_colour_plane_flag
        }
        if (!reader.ue(&value) || !reader.ue(&value)) {
            return false;  // pic_width_in_luma_samples, pic_height_in_luma_samples
        }
        uint32_t conformance_window = 0;
        if (!reader.u(1, &conformance_window)) {
            return false;
        }
        if (conformance_window != 0) {
            for (int i = 0; i < 4; ++i) {
                if (!reader.ue(&value)) {
                    return false;
                }
            }
        }
        return reader.ue(&out->bit_depth_luma_minus8) &&
               reader.ue(&out->bit_depth_chroma_minus8);
    }

    static void append_hvcc_array(std::vector<uint8_t>* out, unsigned type,
                                  const uint8_t* nal, size_t size) {
        out->push_back(static_cast<uint8_t>(0x80 | type));  // array_completeness = 1
        put_u16(out, 1);
        put_u16(out, static_cast<uint16_t>(size));
        out->insert(out->end(), nal, nal + size);
    }

    // Annex B -> one 4-byte big-endian length per NAL unit.
    static bool to_length_prefixed(const uint8_t* data, size_t size,
                                   std::vector<uint8_t>* out) {
        std::vector<uint8_t> buffer(data, data + size);
        const std::vector<NalUnit> nals = scan_nals(buffer);
        if (nals.empty()) {
            return false;
        }
        out->clear();
        for (size_t i = 0; i < nals.size(); ++i) {
            const size_t end = i + 1 < nals.size() ? nals[i + 1].start : buffer.size();
            if (nals[i].payload >= end) {
                continue;
            }
            size_t nal_end = end;
            while (nal_end > nals[i].payload && buffer[nal_end - 1] == 0) {
                --nal_end;
            }
            const size_t nal_size = nal_end - nals[i].payload;
            put_u32(out, static_cast<uint32_t>(nal_size));
            out->insert(out->end(), buffer.begin() + nals[i].payload, buffer.begin() + nal_end);
        }
        return out->size() > 4;
    }

    static void put_u16(std::vector<uint8_t>* out, uint16_t value) {
        out->push_back(static_cast<uint8_t>(value >> 8));
        out->push_back(static_cast<uint8_t>(value & 0xFF));
    }

    static void put_u32(std::vector<uint8_t>* out, uint32_t value) {
        out->push_back(static_cast<uint8_t>(value >> 24));
        out->push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        out->push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        out->push_back(static_cast<uint8_t>(value & 0xFF));
    }

    std::string base_path_;
    std::vector<std::string> segments_;
    std::vector<chast::VideoCodec> segment_codecs_;
    chast::VideoCodec codec_ = chast::VideoCodec::H264;
    chast::VideoChroma chroma_ = chast::VideoChroma::Yuv420;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    AVFormatContext* format_ = nullptr;
    AVStream* stream_ = nullptr;
    AVPacket* packet_ = nullptr;
    // Host timestamp of this segment's first picture; subtracted so every
    // segment starts at zero.
    int64_t first_pts_ms_ = -1;
    // Host timestamp of the previous picture, the spacing estimate for the
    // sample that ends the segment.
    int64_t last_pts_ms_ = -1;
    bool header_started_ = false;
    bool failed_ = false;
    std::string error_;
    uint64_t samples_ = 0;
    uint64_t write_keyframes_ = 0;
    uint64_t skipped_ = 0;
};

}  // namespace chast_tools
