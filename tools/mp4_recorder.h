// MP4 recording of the encoded stream the viewer receives.
//
// RustDesk records the remote screen on the viewer side (src/client.rs
// record_screen -> libs/scrap/src/common/record.rs), but it re-encodes the
// decoded frames. This recorder keeps the stream the host already encoded:
// the access units are muxed into MP4 unchanged, so recording costs no
// encoder and no pixel copy.
//
// The wire format is Annex B with the parameter sets repeated on every key
// frame (see chast/decoder.h), while MP4 stores samples as length-prefixed
// NAL units plus an avcC (H.264) or hvcC (HEVC) record. The first key frame
// therefore supplies the parameter sets and every sample is rewritten from
// start codes to 4-byte lengths.
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
}

#include "annexb.h"
#include "chast/protocol.h"

namespace chast_tools {

// H.264 and HEVC: both are muxed from the parameter sets the host repeats on
// every key frame. An unsupported codec is refused with a clear message
// instead of producing a file players cannot open.
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
            *error = "MP4 recording supports H.264 and HEVC only; the host encoded " +
                     std::string(chast_tools::codec_name(codec));
            failed_ = true;
            return false;
        }
        if (width == 0 || height == 0) {
            *error = "the stream has no picture size to record";
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
        if (header.codec != codec_ || header.width != width_ || header.height != height_) {
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
            finalize_segment();
            std::string error;
            if (!start(header, data, size, &error)) {
                failed_ = true;
                error_ = error;
            }
            return;
        }
        write_packet(header, data, size);
    }

    void close() {
        finalize_segment();
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
        return codec == chast::VideoCodec::H264 || codec == chast::VideoCodec::Hevc;
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
            expected == chast::VideoCodec::Hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
        if (!stream || stream->codecpar->codec_id != want ||
            stream->codecpar->width == 0 || stream->codecpar->height == 0 ||
            stream->codecpar->extradata_size == 0) {
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
    bool start(const chast::FrameHeader& header, const uint8_t* data, size_t size,
               std::string* error) {
        codec_ = header.codec;
        if (!supported(codec_)) {
            *error = "MP4 recording supports H.264 and HEVC only; the host encoded " +
                     std::string(chast_tools::codec_name(codec_));
            return false;
        }
        std::vector<uint8_t> extradata;
        if (!build_extradata(data, size, &extradata)) {
            *error = std::string("the key frame carried no parameter sets to build ") +
                     (codec_ == chast::VideoCodec::Hevc ? "hvcC" : "avcC");
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
        stream_->codecpar->codec_id =
            codec_ == chast::VideoCodec::Hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
        // hvc1 keeps the parameter sets in hvcC; the in-band copies the host
        // repeats on key frames are identical to them for a given encoder.
        if (codec_ == chast::VideoCodec::Hevc) {
            stream_->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
        }
        stream_->codecpar->width = static_cast<int>(width_);
        stream_->codecpar->height = static_cast<int>(height_);
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
        write_packet(header, data, size);
        return !failed_;
    }

    // Finalises and releases the current segment. Safe to call when no segment
    // is open, so close() and a rollover share it.
    void finalize_segment() {
        if (!format_) {
            return;
        }
        if (header_started_) {
            av_write_trailer(format_);
        }
        if (format_->pb) {
            avio_closep(&format_->pb);
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
        std::vector<uint8_t> sample;
        if (!to_length_prefixed(data, size, &sample)) {
            return;
        }
        if (!packet_) {
            packet_ = av_packet_alloc();
        }
        if (av_new_packet(packet_, static_cast<int>(sample.size())) < 0) {
            failed_ = true;
            error_ = "out of memory for a recording sample";
            return;
        }
        std::memcpy(packet_->data, sample.data(), sample.size());
        packet_->pts = static_cast<int64_t>(header.pts_ms);
        packet_->dts = packet_->pts;
        packet_->stream_index = stream_->index;
        packet_->flags = header.keyframe ? AV_PKT_FLAG_KEY : 0;
        if (av_interleaved_write_frame(format_, packet_) < 0) {
            failed_ = true;
            error_ = "could not write a sample to " +
                     (segments_.empty() ? base_path_ : segments_.back());
            av_packet_unref(packet_);
            return;
        }
        av_packet_unref(packet_);
        ++samples_;
        if (header.keyframe) {
            ++write_keyframes_;
        }
    }

    bool build_extradata(const uint8_t* data, size_t size, std::vector<uint8_t>* out) const {
        return codec_ == chast::VideoCodec::Hevc ? build_hvcc(data, size, out)
                                                 : build_avcc(data, size, out);
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
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    AVFormatContext* format_ = nullptr;
    AVStream* stream_ = nullptr;
    AVPacket* packet_ = nullptr;
    bool header_started_ = false;
    bool failed_ = false;
    std::string error_;
    uint64_t samples_ = 0;
    uint64_t write_keyframes_ = 0;
    uint64_t skipped_ = 0;
};

}  // namespace chast_tools