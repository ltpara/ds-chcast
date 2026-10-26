// AAC-LC decoder through FFmpeg, retained for the protocol enum and any
// AudioInfo that still names AAC. chast never encodes AAC: system sound and
// microphone both use the standalone Opus encoder (opus_codec.cc).
//
// Only the compressed access units cross this boundary; the PCM that comes out
// is s16 interleaved because that is what the player schedules. libswresample
// adapts the host's rate and channel count to whatever endpoint the viewer has.
//
// The decoder still removes FFmpeg's fixed 1024-sample priming on the first
// block, so a viewer that joins mid-stream starts on the announced timeline.

#include "chast/audio.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chast {

namespace {

Error av_error(int code, std::string_view what) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return Error(Status::CaptureFailed,
                 std::string(what) + " failed: " + text + " (" + std::to_string(code) + ")");
}

std::string channels_text(uint16_t channels) {
    return channels == 1 ? "mono" : std::to_string(channels) + " channels";
}

class AacDecoder final : public IAudioDecoder {
public:
    explicit AacDecoder(AudioInfo info) : info_(std::move(info)) {}
    ~AacDecoder() override { close(); }

    Error open();
    Error decode(const AudioHeader& header, const uint8_t* data, size_t size,
                 std::vector<DecodedAudio>& out) override;
    Error flush(std::vector<DecodedAudio>& out) override;

    uint32_t sample_rate() const noexcept override { return info_.sample_rate; }
    uint16_t channels() const noexcept override { return info_.channels; }

private:
    void close();
    Error ensure_swr(const AVFrame& frame);
    Error deliver(const AVFrame& frame, uint64_t base_pts_ms, uint64_t capture_ms,
                  size_t& samples_before, std::vector<DecodedAudio>& out);
    Error receive(uint64_t base_pts_ms, uint64_t capture_ms, size_t& samples_before,
                  std::vector<DecodedAudio>& out);

    AudioInfo info_;
    const AVCodec* codec_ = nullptr;
    AVCodecContext* context_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwrContext* swr_ = nullptr;
    std::vector<uint8_t> pcm_;
    uint64_t priming_remaining_ = 0;
    bool flushed_ = false;
};

Error AacDecoder::open() {
    if (info_.channels < 1 || info_.channels > 2) {
        return Error(Status::Unsupported,
                     "AAC playback supports mono or stereo, not " +
                         channels_text(info_.channels));
    }
    if (info_.sample_rate < 8000 || info_.sample_rate > 96000) {
        return Error(Status::Unsupported,
                     "the announced AAC rate " + std::to_string(info_.sample_rate) +
                         " Hz is not playable");
    }
    if (info_.extradata.empty() || info_.extradata.size() > kMaxAudioExtradata) {
        return Error(Status::InvalidCall,
                     "AAC playback needs the AudioSpecificConfig");
    }

    codec_ = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec_) {
        return Error(Status::Unsupported, "this FFmpeg build has no AAC decoder");
    }
    context_ = avcodec_alloc_context3(codec_);
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!context_ || !packet_ || !frame_) {
        return Error(Status::CaptureFailed, "FFmpeg AAC decoder allocation failed");
    }

    context_->sample_rate = static_cast<int>(info_.sample_rate);
    av_channel_layout_default(&context_->ch_layout, info_.channels);
    context_->extradata = static_cast<uint8_t*>(
        av_mallocz(info_.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!context_->extradata) {
        return Error(Status::CaptureFailed, "FFmpeg AAC extradata allocation failed");
    }
    std::memcpy(context_->extradata, info_.extradata.data(), info_.extradata.size());
    context_->extradata_size = static_cast<int>(info_.extradata.size());
    context_->pkt_timebase = AVRational{1, 1000};

    const int status = avcodec_open2(context_, codec_, nullptr);
    if (status < 0) {
        return av_error(status, "avcodec_open2(AAC decoder)");
    }
    // The AAC encoder delays its output; the first decoded frame carries the
    // encoder's priming samples rather than sound.
    priming_remaining_ = kAudioFrameSamples;
    return Error::success();
}

void AacDecoder::close() {
    if (swr_) {
        swr_free(&swr_);
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (packet_) {
        av_packet_free(&packet_);
    }
    if (context_) {
        avcodec_free_context(&context_);
    }
}

Error AacDecoder::ensure_swr(const AVFrame& frame) {
    if (swr_) {
        return Error::success();
    }
    AVChannelLayout in_layout;
    AVChannelLayout out_layout;
    if (frame.ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&in_layout, &frame.ch_layout);
    } else {
        av_channel_layout_default(&in_layout, info_.channels);
    }
    av_channel_layout_default(&out_layout, info_.channels);
    const int status =
        swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_S16, info_.sample_rate,
                            &in_layout, static_cast<AVSampleFormat>(frame.format),
                            frame.sample_rate > 0 ? frame.sample_rate : info_.sample_rate,
                            0, nullptr);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);
    if (status < 0 || !swr_) {
        return av_error(status < 0 ? status : AVERROR(ENOMEM), "swr_alloc_set_opts2");
    }
    const int initialized = swr_init(swr_);
    if (initialized < 0) {
        return av_error(initialized, "swr_init(AAC decoder)");
    }
    return Error::success();
}

Error AacDecoder::deliver(const AVFrame& frame, uint64_t base_pts_ms, uint64_t capture_ms,
                          size_t& samples_before, std::vector<DecodedAudio>& out) {
    Error ready = ensure_swr(frame);
    if (!ready.ok()) {
        return ready;
    }
    const int in_rate = frame.sample_rate > 0 ? frame.sample_rate : context_->sample_rate;
    const int out_rate = static_cast<int>(info_.sample_rate);
    const int capacity = static_cast<int>(av_rescale_rnd(
        swr_get_delay(swr_, in_rate) + frame.nb_samples, out_rate, in_rate, AV_ROUND_UP));
    if (capacity <= 0) {
        return Error::success();
    }
    pcm_.resize(static_cast<size_t>(capacity) * info_.channels * sizeof(int16_t));
    uint8_t* output = pcm_.data();
    const int converted = swr_convert(swr_, &output, capacity,
                                      frame.extended_data, frame.nb_samples);
    if (converted < 0) {
        return av_error(converted, "swr_convert(AAC output)");
    }
    if (converted == 0) {
        return Error::success();
    }

    size_t skip = 0;
    if (priming_remaining_ > 0) {
        skip = std::min<uint64_t>(priming_remaining_, static_cast<uint64_t>(converted));
        priming_remaining_ -= skip;
    }
    const size_t frames = static_cast<size_t>(converted) - skip;
    if (frames > 0) {
        DecodedAudio block;
        block.sample_rate = info_.sample_rate;
        block.channels = info_.channels;
        block.capture_ms = capture_ms;
        // The caller counts samples produced at the output rate; the priming
        // skip moves this block's first sample forward by that many samples.
        block.pts_ms = base_pts_ms +
                       (samples_before + skip) * 1000 / info_.sample_rate;
        const auto* source = reinterpret_cast<const int16_t*>(pcm_.data());
        block.samples.assign(source + skip * info_.channels,
                             source + static_cast<size_t>(converted) * info_.channels);
        out.push_back(std::move(block));
    }
    samples_before += static_cast<size_t>(converted);
    return Error::success();
}

Error AacDecoder::receive(uint64_t base_pts_ms, uint64_t capture_ms,
                          size_t& samples_before, std::vector<DecodedAudio>& out) {
    for (;;) {
        av_frame_unref(frame_);
        const int status = avcodec_receive_frame(context_, frame_);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
            return Error::success();
        }
        if (status < 0) {
            return av_error(status, "avcodec_receive_frame(AAC)");
        }
        Error delivered = deliver(*frame_, base_pts_ms, capture_ms, samples_before, out);
        if (!delivered.ok()) {
            return delivered;
        }
    }
}

Error AacDecoder::decode(const AudioHeader& header, const uint8_t* data, size_t size,
                         std::vector<DecodedAudio>& out) {
    if (flushed_) {
        return Error(Status::InvalidCall, "decode after the AAC decoder was flushed");
    }
    if (!data || size == 0) {
        return Error(Status::InvalidCall, "the AAC decoder needs an access unit");
    }
    // avcodec_send_packet may read a few bytes past the end of the access unit;
    // give it FFmpeg's required padding instead of borrowing the caller's
    // buffer, which is sized exactly.
    av_packet_unref(packet_);
    int status = av_new_packet(packet_, static_cast<int>(size));
    if (status < 0) {
        return av_error(status, "av_new_packet(AAC)");
    }
    std::memcpy(packet_->data, data, size);
    packet_->pts = static_cast<int64_t>(header.pts_ms);
    packet_->dts = packet_->pts;

    size_t samples_before = 0;
    for (;;) {
        status = avcodec_send_packet(context_, packet_);
        if (status != AVERROR(EAGAIN)) {
            break;
        }
        Error drained = receive(header.pts_ms, header.capture_ms, samples_before, out);
        if (!drained.ok()) {
            return drained;
        }
    }
    if (status < 0 && status != AVERROR_EOF) {
        return av_error(status, "avcodec_send_packet(AAC)");
    }
    return receive(header.pts_ms, header.capture_ms, samples_before, out);
}

Error AacDecoder::flush(std::vector<DecodedAudio>& out) {
    if (flushed_) {
        return Error::success();
    }
    flushed_ = true;
    const int status = avcodec_send_packet(context_, nullptr);
    if (status < 0 && status != AVERROR_EOF) {
        return av_error(status, "avcodec_send_packet(AAC flush)");
    }
    size_t samples_before = 0;
    return receive(0, 0, samples_before, out);
}

}  // namespace

std::unique_ptr<IAudioDecoder> create_aac_decoder(const AudioInfo& info, std::string* error) {
    auto decoder = std::make_unique<AacDecoder>(info);
    Error opened = decoder->open();
    if (!opened.ok()) {
        if (error) {
            *error = opened.to_string();
        }
        return nullptr;
    }
    return decoder;
}

}  // namespace chast
