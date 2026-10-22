// AAC-LC codec through FFmpeg's native encoder and decoder.
//
// Only the compressed access units cross this boundary; the PCM that goes in
// and comes out is s16 interleaved because that is what WASAPI hands over in
// the common mix formats and what the player schedules. libswresample does the
// planar/interleaved conversion, and on the player side also adapts the host's
// rate and channel count to whatever endpoint the viewer has.
//
// Timestamps are assigned here, not borrowed from FFmpeg: the native AAC
// encoder's packet pts is not a stable real-time clock across flush and gap
// handling, while the capture timeline is. Each packet therefore carries the
// host steady-clock time of its first sample, derived from the caller's
// capture_ms and the number of samples already encoded.

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
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chast {

namespace {

constexpr int64_t kGapThresholdUs = 100 * 1000;

Error av_error(int code, std::string_view what) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return Error(Status::CaptureFailed,
                 std::string(what) + " failed: " + text + " (" + std::to_string(code) + ")");
}

std::string channels_text(uint16_t channels) {
    return channels == 1 ? "mono" : std::to_string(channels) + " channels";
}

class AacEncoder final : public IAudioEncoder {
public:
    explicit AacEncoder(AudioEncoderConfig config) : config_(config) {}
    ~AacEncoder() override { close(); }

    Error open();
    Error encode(const int16_t* samples, size_t frames, uint64_t capture_ms,
                 std::vector<EncodedAudio>& out) override;
    Error flush(std::vector<EncodedAudio>& out) override;

    uint32_t sample_rate() const noexcept override { return config_.sample_rate; }
    uint16_t channels() const noexcept override { return config_.channels; }
    uint16_t frame_samples() const noexcept override { return frame_size_; }
    uint32_t bitrate() const noexcept override { return config_.bitrate; }
    const std::vector<uint8_t>& extradata() const noexcept override { return extradata_; }

private:
    void close();
    Error encode_frame(const int16_t* samples, uint64_t pts_ms,
                       std::vector<EncodedAudio>& out);
    Error drain(std::vector<EncodedAudio>& out);
    Error pad_pending(std::vector<EncodedAudio>& out);
    size_t pending_frames() const {
        return pending_.size() / config_.channels - pending_offset_;
    }
    const int16_t* pending_data() const {
        return pending_.data() + pending_offset_ * config_.channels;
    }
    int64_t pending_duration_us() const {
        return pending_frames() * 1000000 / config_.sample_rate;
    }
    void compact_pending();

    AudioEncoderConfig config_;
    const AVCodec* codec_ = nullptr;
    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwrContext* swr_ = nullptr;
    std::vector<uint8_t> extradata_;

    std::vector<int16_t> pending_;
    size_t pending_offset_ = 0;  // in frames
    int64_t pending_start_us_ = 0;
    bool have_start_ = false;
    bool flushed_ = false;
    uint16_t frame_size_ = 0;
    uint64_t frame_duration_ms_ = 0;

    // The native AAC encoder delays its output, so a packet returned by
    // receive_packet() belongs to an earlier submitted frame. The queue keeps
    // one timestamp per accepted frame; packets pop from the front.
    std::deque<uint64_t> packet_times_;
    uint64_t last_packet_ms_ = 0;
};

Error AacEncoder::open() {
    if (config_.channels < 1 || config_.channels > 2) {
        return Error(Status::Unsupported,
                     "AAC capture supports mono or stereo, not " +
                         channels_text(config_.channels));
    }
    if (config_.sample_rate < 8000 || config_.sample_rate > 96000) {
        return Error(Status::Unsupported,
                     "the device mixes at " + std::to_string(config_.sample_rate) +
                         " Hz, which this AAC encoder cannot represent");
    }

    codec_ = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec_) {
        return Error(Status::Unsupported, "this FFmpeg build has no AAC encoder");
    }
    context_ = avcodec_alloc_context3(codec_);
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!context_ || !packet_ || !frame_) {
        return Error(Status::CaptureFailed, "FFmpeg AAC encoder allocation failed");
    }

    context_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    context_->sample_rate = static_cast<int>(config_.sample_rate);
    context_->bit_rate = static_cast<int64_t>(config_.bitrate);
    context_->profile = AV_PROFILE_AAC_LOW;
    context_->time_base = AVRational{1, static_cast<int>(config_.sample_rate)};
    context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    // One frame in flight keeps encode() synchronous; a thread pool would only
    // add latency to a 21 ms access unit.
    context_->thread_count = 1;
    av_channel_layout_default(&context_->ch_layout, config_.channels);

    int status = avcodec_open2(context_, codec_, nullptr);
    if (status < 0) {
        return av_error(status, "avcodec_open2(AAC encoder)");
    }
    if (context_->frame_size != static_cast<int>(kAudioFrameSamples)) {
        return Error(Status::Unsupported,
                     "the AAC encoder uses " + std::to_string(context_->frame_size) +
                         " samples per frame, the protocol requires " +
                         std::to_string(kAudioFrameSamples));
    }
    frame_size_ = static_cast<uint16_t>(context_->frame_size);
    frame_duration_ms_ = (static_cast<uint64_t>(frame_size_) * 1000 +
                          config_.sample_rate / 2) /
                         config_.sample_rate;
    if (frame_duration_ms_ == 0) {
        frame_duration_ms_ = 1;
    }
    if (context_->extradata_size <= 0 ||
        static_cast<size_t>(context_->extradata_size) > kMaxAudioExtradata) {
        return Error(Status::CaptureFailed,
                     "the AAC encoder produced no usable AudioSpecificConfig");
    }
    extradata_.assign(context_->extradata,
                      context_->extradata + context_->extradata_size);

    AVChannelLayout in_layout;
    AVChannelLayout out_layout;
    av_channel_layout_default(&in_layout, config_.channels);
    av_channel_layout_default(&out_layout, config_.channels);
    status = swr_alloc_set_opts2(&swr_, &out_layout, context_->sample_fmt,
                                 context_->sample_rate, &in_layout, AV_SAMPLE_FMT_S16,
                                 context_->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);
    if (status < 0 || !swr_) {
        return av_error(status < 0 ? status : AVERROR(ENOMEM), "swr_alloc_set_opts2");
    }
    status = swr_init(swr_);
    if (status < 0) {
        return av_error(status, "swr_init");
    }
    return Error::success();
}

void AacEncoder::close() {
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

void AacEncoder::compact_pending() {
    if (pending_offset_ == 0) {
        return;
    }
    const size_t frames = pending_frames();
    if (frames == 0) {
        pending_.clear();
        pending_offset_ = 0;
        return;
    }
    std::memmove(pending_.data(), pending_data(),
                 frames * config_.channels * sizeof(int16_t));
    pending_.resize(frames * config_.channels);
    pending_offset_ = 0;
}

Error AacEncoder::encode(const int16_t* samples, size_t frames, uint64_t capture_ms,
                         std::vector<EncodedAudio>& out) {
    if (flushed_) {
        return Error(Status::InvalidCall, "encode after the AAC encoder was flushed");
    }
    if (!samples && frames > 0) {
        return Error(Status::InvalidCall, "the AAC encoder needs samples");
    }
    if (frames == 0) {
        return Error::success();
    }

    const int64_t start_us = static_cast<int64_t>(capture_ms) * 1000;
    if (have_start_) {
        const int64_t expected_us =
            pending_start_us_ + pending_duration_us();
        if (start_us > expected_us + kGapThresholdUs) {
            // The loopback was idle: close the partial frame so the timeline
            // resumes at the real capture time instead of drifting behind.
            Error padded = pad_pending(out);
            if (!padded.ok()) {
                return padded;
            }
            have_start_ = false;
        }
    }
    if (!have_start_) {
        pending_start_us_ = start_us;
        have_start_ = true;
    }

    compact_pending();
    const size_t old_frames = pending_frames();
    pending_.resize((old_frames + frames) * config_.channels);
    std::memcpy(pending_.data() + old_frames * config_.channels, samples,
                frames * config_.channels * sizeof(int16_t));

    while (pending_frames() >= frame_size_) {
        const uint64_t pts_ms =
            static_cast<uint64_t>((pending_start_us_ + 500) / 1000);
        Error encoded = encode_frame(pending_data(), pts_ms, out);
        if (!encoded.ok()) {
            return encoded;
        }
        pending_offset_ += frame_size_;
        pending_start_us_ +=
            static_cast<int64_t>(frame_size_) * 1000000 / config_.sample_rate;
    }
    compact_pending();
    return Error::success();
}

Error AacEncoder::pad_pending(std::vector<EncodedAudio>& out) {
    while (pending_frames() > 0) {
        const size_t fill = frame_size_ - pending_frames();
        const size_t old_frames = pending_frames();
        pending_.resize((old_frames + fill) * config_.channels, 0);
        const uint64_t pts_ms = static_cast<uint64_t>((pending_start_us_ + 500) / 1000);
        Error encoded = encode_frame(pending_data(), pts_ms, out);
        if (!encoded.ok()) {
            return encoded;
        }
        pending_offset_ += frame_size_;
        pending_start_us_ +=
            static_cast<int64_t>(frame_size_) * 1000000 / config_.sample_rate;
    }
    compact_pending();
    return Error::success();
}

Error AacEncoder::encode_frame(const int16_t* samples, uint64_t pts_ms,
                               std::vector<EncodedAudio>& out) {
    av_frame_unref(frame_);
    frame_->nb_samples = frame_size_;
    frame_->format = context_->sample_fmt;
    frame_->sample_rate = context_->sample_rate;
    frame_->pts = static_cast<int64_t>(pts_ms);
    int status = av_channel_layout_copy(&frame_->ch_layout, &context_->ch_layout);
    if (status < 0) {
        return av_error(status, "av_channel_layout_copy");
    }
    status = av_frame_get_buffer(frame_, 0);
    if (status < 0) {
        return av_error(status, "av_frame_get_buffer");
    }

    const uint8_t* input = reinterpret_cast<const uint8_t*>(samples);
    const int converted = swr_convert(swr_, frame_->extended_data, frame_size_, &input,
                                      static_cast<int>(frame_size_));
    if (converted < 0) {
        return av_error(converted, "swr_convert(AAC input)");
    }
    if (converted < frame_size_) {
        for (int channel = 0; channel < config_.channels; ++channel) {
            auto* plane = reinterpret_cast<float*>(frame_->extended_data[channel]);
            std::fill(plane + converted, plane + frame_size_, 0.0f);
        }
    }

    for (;;) {
        status = avcodec_send_frame(context_, frame_);
        if (status != AVERROR(EAGAIN)) {
            break;
        }
        Error drained = drain(out);
        if (!drained.ok()) {
            return drained;
        }
    }
    if (status < 0) {
        return av_error(status, "avcodec_send_frame(AAC)");
    }
    packet_times_.push_back(pts_ms);
    return drain(out);
}

Error AacEncoder::drain(std::vector<EncodedAudio>& out) {
    for (;;) {
        const int status = avcodec_receive_packet(context_, packet_);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
            return Error::success();
        }
        if (status < 0) {
            return av_error(status, "avcodec_receive_packet(AAC)");
        }
        uint64_t pts_ms = last_packet_ms_;
        if (!packet_times_.empty()) {
            pts_ms = packet_times_.front();
            packet_times_.pop_front();
        } else if (last_packet_ms_ > 0) {
            // The AAC encoder can emit one more packet than it consumed (its
            // priming/delay frame). That packet follows the last timestamped
            // frame on the timeline.
            pts_ms = last_packet_ms_ + frame_duration_ms_;
        }
        last_packet_ms_ = pts_ms;
        EncodedAudio packet;
        packet.data.assign(packet_->data, packet_->data + packet_->size);
        packet.capture_ms = pts_ms;
        out.push_back(std::move(packet));
        av_packet_unref(packet_);
    }
}

Error AacEncoder::flush(std::vector<EncodedAudio>& out) {
    if (flushed_) {
        return Error::success();
    }
    flushed_ = true;
    Error padded = pad_pending(out);
    if (!padded.ok()) {
        return padded;
    }
    const int status = avcodec_send_frame(context_, nullptr);
    if (status < 0 && status != AVERROR_EOF) {
        return av_error(status, "avcodec_send_frame(AAC flush)");
    }
    return drain(out);
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

std::unique_ptr<IAudioEncoder> create_aac_encoder(const AudioEncoderConfig& config,
                                                  std::string* error) {
    auto encoder = std::make_unique<AacEncoder>(config);
    Error opened = encoder->open();
    if (!opened.ok()) {
        if (error) {
            *error = opened.to_string();
        }
        return nullptr;
    }
    return encoder;
}

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
