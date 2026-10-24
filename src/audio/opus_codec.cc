// Opus through the standalone vcpkg library (Opus::opus).
//
// The PCM crossing this boundary is interleaved s16 at 48 kHz, which is what
// the capture path already produces and what the renderer wants. Opus only
// speaks 48 kHz here: its other rates would add resampling without a benefit
// for either the loopback mix or a speech uplink.
//
// Timestamps are assigned from the capture timeline, exactly like the AAC
// path: libopus returns bytes, not a presentation clock. Each packet carries
// the steady-clock time of its first input sample, and the decoder rebuilds
// the timeline from AudioHeader plus the audible samples it has produced.
// The encoder's lookahead is published as the standard 19-byte OpusHead, so a
// peer that joins mid-stream can drop the pre-skip without private metadata.

#include "chast/audio.h"

#include <opus/opus.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace chast {

namespace {

constexpr int64_t kGapThresholdUs = 100 * 1000;
constexpr uint32_t kOpusRate = 48000;
constexpr int kOpusMaxPacket = 4000;
constexpr int kOpusMaxFrameSamples = 5760;  // 120 ms at 48 kHz
constexpr size_t kOpusHeadSize = 19;

std::string opus_error(int code, const char* what) {
    return std::string(what) + " failed: " + opus_strerror(code) + " (" +
           std::to_string(code) + ")";
}

void put_u16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(uint8_t* out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint16_t read_u16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

class OpusEncoderImpl final : public IAudioEncoder {
public:
    explicit OpusEncoderImpl(AudioEncoderConfig config) : config_(config) {}
    ~OpusEncoderImpl() override {
        if (encoder_) {
            opus_encoder_destroy(encoder_);
        }
    }

    Error open();
    Error encode(const int16_t* samples, size_t frames, uint64_t capture_ms,
                 std::vector<EncodedAudio>& out) override;
    Error flush(std::vector<EncodedAudio>& out) override;

    uint32_t sample_rate() const noexcept override { return kOpusRate; }
    uint16_t channels() const noexcept override { return config_.channels; }
    uint16_t frame_samples() const noexcept override {
        return static_cast<uint16_t>(kOpusFrameSamples);
    }
    uint32_t bitrate() const noexcept override { return config_.bitrate; }
    const std::vector<uint8_t>& extradata() const noexcept override { return extradata_; }

private:
    size_t pending_frames() const {
        return pending_.size() / config_.channels - pending_offset_;
    }
    const int16_t* pending_data() const {
        return pending_.data() + pending_offset_ * config_.channels;
    }
    int64_t pending_duration_us() const {
        return pending_frames() * 1000000 / kOpusRate;
    }
    void compact_pending();
    Error encode_frame(uint64_t pts_ms, std::vector<EncodedAudio>& out);
    Error pad_pending(std::vector<EncodedAudio>& out);

    AudioEncoderConfig config_;
    OpusEncoder* encoder_ = nullptr;
    std::vector<uint8_t> extradata_;

    std::vector<int16_t> pending_;
    size_t pending_offset_ = 0;  // in frames
    int64_t pending_start_us_ = 0;
    bool have_start_ = false;
    bool flushed_ = false;
};

Error OpusEncoderImpl::open() {
    if (config_.sample_rate != kOpusRate) {
        return Error(Status::Unsupported,
                     "the Opus encoder only accepts 48 kHz, not " +
                         std::to_string(config_.sample_rate) + " Hz");
    }
    if (config_.channels != 1 && config_.channels != 2) {
        return Error(Status::Unsupported,
                     "the Opus encoder accepts mono or stereo, not " +
                         std::to_string(config_.channels) + " channels");
    }
    if (config_.bitrate < 6000 || config_.bitrate > 510000) {
        return Error(Status::Unsupported,
                     "the Opus bitrate " + std::to_string(config_.bitrate) +
                         " bps is outside 6-510 kbps");
    }

    int status = OPUS_OK;
    encoder_ = opus_encoder_create(
        static_cast<opus_int32>(kOpusRate), config_.channels,
        config_.voice ? OPUS_APPLICATION_VOIP : OPUS_APPLICATION_AUDIO, &status);
    if (!encoder_ || status != OPUS_OK) {
        return Error(Status::CaptureFailed, opus_error(status, "opus_encoder_create"));
    }
    const int set = opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(static_cast<opus_int32>(
                                                 config_.bitrate)));
    if (set != OPUS_OK) {
        return Error(Status::CaptureFailed, opus_error(set, "OPUS_SET_BITRATE"));
    }

    opus_int32 lookahead = 0;
    const int queried = opus_encoder_ctl(encoder_, OPUS_GET_LOOKAHEAD(&lookahead));
    if (queried != OPUS_OK || lookahead < 0 || lookahead > 0xFFFF) {
        return Error(Status::CaptureFailed,
                     opus_error(queried, "OPUS_GET_LOOKAHEAD"));
    }

    extradata_.assign(kOpusHeadSize, 0);
    std::memcpy(extradata_.data(), "OpusHead", 8);
    extradata_[8] = 1;  // version
    extradata_[9] = static_cast<uint8_t>(config_.channels);
    put_u16(extradata_.data() + 10, static_cast<uint16_t>(lookahead));
    put_u32(extradata_.data() + 12, kOpusRate);
    // Output gain and channel mapping family stay zero: one stream, mono or
    // stereo, no gain adjustment.
    return Error::success();
}

void OpusEncoderImpl::compact_pending() {
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

Error OpusEncoderImpl::encode(const int16_t* samples, size_t frames, uint64_t capture_ms,
                              std::vector<EncodedAudio>& out) {
    if (flushed_) {
        return Error(Status::InvalidCall, "encode after the Opus encoder was flushed");
    }
    if (!samples && frames > 0) {
        return Error(Status::InvalidCall, "the Opus encoder needs samples");
    }
    if (frames == 0) {
        return Error::success();
    }

    const int64_t start_us = static_cast<int64_t>(capture_ms) * 1000;
    if (have_start_) {
        const int64_t expected_us = pending_start_us_ + pending_duration_us();
        if (start_us > expected_us + kGapThresholdUs) {
            // The device was idle: close the partial packet so the receiver
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

    while (pending_frames() >= kOpusFrameSamples) {
        const uint64_t pts_ms = static_cast<uint64_t>((pending_start_us_ + 500) / 1000);
        Error encoded = encode_frame(pts_ms, out);
        if (!encoded.ok()) {
            return encoded;
        }
        pending_offset_ += kOpusFrameSamples;
        pending_start_us_ +=
            static_cast<int64_t>(kOpusFrameSamples) * 1000000 / kOpusRate;
    }
    compact_pending();
    return Error::success();
}

Error OpusEncoderImpl::pad_pending(std::vector<EncodedAudio>& out) {
    while (pending_frames() > 0) {
        const size_t fill = kOpusFrameSamples - pending_frames();
        const size_t old_frames = pending_frames();
        pending_.resize((old_frames + fill) * config_.channels, 0);
        const uint64_t pts_ms = static_cast<uint64_t>((pending_start_us_ + 500) / 1000);
        Error encoded = encode_frame(pts_ms, out);
        if (!encoded.ok()) {
            return encoded;
        }
        pending_offset_ += kOpusFrameSamples;
        pending_start_us_ +=
            static_cast<int64_t>(kOpusFrameSamples) * 1000000 / kOpusRate;
    }
    compact_pending();
    return Error::success();
}

Error OpusEncoderImpl::encode_frame(uint64_t pts_ms, std::vector<EncodedAudio>& out) {
    uint8_t packet[kOpusMaxPacket];
    const opus_int32 size =
        opus_encode(encoder_, pending_data(), static_cast<int>(kOpusFrameSamples), packet,
                    static_cast<opus_int32>(sizeof(packet)));
    if (size < 0) {
        return Error(Status::CaptureFailed,
                     opus_error(size, "opus_encode"));
    }
    if (size == 0) {
        return Error(Status::CaptureFailed, "opus_encode produced an empty packet");
    }
    EncodedAudio encoded;
    encoded.data.assign(packet, packet + size);
    encoded.capture_ms = pts_ms;
    out.push_back(std::move(encoded));
    return Error::success();
}

Error OpusEncoderImpl::flush(std::vector<EncodedAudio>& out) {
    if (flushed_) {
        return Error::success();
    }
    flushed_ = true;
    return pad_pending(out);
}

class OpusDecoderImpl final : public IAudioDecoder {
public:
    explicit OpusDecoderImpl(AudioInfo info) : info_(std::move(info)) {}
    ~OpusDecoderImpl() override {
        if (decoder_) {
            opus_decoder_destroy(decoder_);
        }
    }

    Error open();
    Error decode(const AudioHeader& header, const uint8_t* data, size_t size,
                 std::vector<DecodedAudio>& out) override;
    Error flush(std::vector<DecodedAudio>& out) override;

    uint32_t sample_rate() const noexcept override { return kOpusRate; }
    uint16_t channels() const noexcept override { return info_.channels; }

private:
    AudioInfo info_;
    OpusDecoder* decoder_ = nullptr;
    int pre_skip_remaining_ = 0;
    uint64_t audible_frames_ = 0;
    bool flushed_ = false;
};

Error OpusDecoderImpl::open() {
    if (info_.sample_rate != kOpusRate) {
        return Error(Status::Unsupported,
                     "the Opus decoder only accepts 48 kHz, not " +
                         std::to_string(info_.sample_rate) + " Hz");
    }
    if (info_.channels != 1 && info_.channels != 2) {
        return Error(Status::Unsupported,
                     "the Opus decoder accepts mono or stereo, not " +
                         std::to_string(info_.channels) + " channels");
    }
    if (info_.extradata.size() < kOpusHeadSize ||
        std::memcmp(info_.extradata.data(), "OpusHead", 8) != 0) {
        return Error(Status::InvalidCall, "Opus playback needs an OpusHead");
    }
    if (info_.extradata[8] != 1) {
        return Error(Status::Unsupported,
                     "OpusHead version " + std::to_string(info_.extradata[8]) +
                         " is not supported");
    }
    if (info_.extradata[9] != info_.channels) {
        return Error(Status::InvalidCall,
                     "OpusHead names a different channel count than the stream");
    }
    if (info_.extradata[18] != 0) {
        return Error(Status::Unsupported,
                     "Opus channel mapping family " +
                         std::to_string(info_.extradata[18]) + " is not supported");
    }

    int status = OPUS_OK;
    decoder_ = opus_decoder_create(static_cast<opus_int32>(kOpusRate), info_.channels,
                                   &status);
    if (!decoder_ || status != OPUS_OK) {
        return Error(Status::CaptureFailed, opus_error(status, "opus_decoder_create"));
    }
    pre_skip_remaining_ = read_u16(info_.extradata.data() + 10);
    return Error::success();
}

Error OpusDecoderImpl::decode(const AudioHeader& header, const uint8_t* data, size_t size,
                              std::vector<DecodedAudio>& out) {
    if (flushed_) {
        return Error(Status::InvalidCall, "decode after the Opus decoder was flushed");
    }
    if (!data || size == 0) {
        return Error(Status::InvalidCall, "the Opus decoder needs a packet");
    }
    if (header.channels != info_.channels || header.sample_rate != kOpusRate) {
        return Error(Status::InvalidCall,
                     "the Opus packet disagrees with the announced format");
    }

    std::vector<int16_t> pcm(static_cast<size_t>(kOpusMaxFrameSamples) * info_.channels);
    const int frames = opus_decode(decoder_, data, static_cast<opus_int32>(size),
                                   pcm.data(), kOpusMaxFrameSamples, 0);
    if (frames < 0) {
        return Error(Status::CaptureFailed, opus_error(frames, "opus_decode"));
    }
    if (frames == 0) {
        return Error::success();
    }

    size_t skip = 0;
    if (pre_skip_remaining_ > 0) {
        skip = (std::min)(static_cast<size_t>(pre_skip_remaining_),
                          static_cast<size_t>(frames));
        pre_skip_remaining_ -= static_cast<int>(skip);
    }
    const size_t audible = static_cast<size_t>(frames) - skip;
    if (audible > 0) {
        DecodedAudio block;
        block.sample_rate = kOpusRate;
        block.channels = info_.channels;
        block.capture_ms = header.capture_ms;
        // Pre-skip removes the encoder's lookahead, so the first audible
        // sample is the packet's first input sample.
        block.pts_ms = header.pts_ms + audible_frames_ * 1000 / kOpusRate;
        const size_t channels = info_.channels;
        block.samples.assign(pcm.data() + skip * channels,
                             pcm.data() + static_cast<size_t>(frames) * channels);
        audible_frames_ += audible;
        out.push_back(std::move(block));
    }
    return Error::success();
}

Error OpusDecoderImpl::flush(std::vector<DecodedAudio>& out) {
    (void)out;
    flushed_ = true;
    return Error::success();
}

}  // namespace

std::unique_ptr<IAudioEncoder> create_opus_encoder(const AudioEncoderConfig& config,
                                                   std::string* error) {
    auto encoder = std::make_unique<OpusEncoderImpl>(config);
    Error opened = encoder->open();
    if (!opened.ok()) {
        if (error) {
            *error = opened.to_string();
        }
        return nullptr;
    }
    return encoder;
}

std::unique_ptr<IAudioDecoder> create_opus_decoder(const AudioInfo& info, std::string* error) {
    auto decoder = std::make_unique<OpusDecoderImpl>(info);
    Error opened = decoder->open();
    if (!opened.ok()) {
        if (error) {
            *error = opened.to_string();
        }
        return nullptr;
    }
    return decoder;
}

std::unique_ptr<IAudioEncoder> create_audio_encoder(const AudioEncoderConfig& config,
                                                    std::string* error) {
    switch (config.codec) {
        case AudioCodec::Opus:
            return create_opus_encoder(config, error);
        case AudioCodec::AAC:
            if (error) {
                *error =
                    "unsupported: AAC encoding is disabled; system audio and microphone use Opus";
            }
            return nullptr;
    }
    if (error) {
        *error = "unknown audio codec " + std::to_string(static_cast<int>(config.codec));
    }
    return nullptr;
}

std::unique_ptr<IAudioDecoder> create_audio_decoder(const AudioInfo& info, std::string* error) {
    switch (info.codec) {
        case AudioCodec::AAC:
            return create_aac_decoder(info, error);
        case AudioCodec::Opus:
            return create_opus_decoder(info, error);
    }
    if (error) {
        *error = "unknown audio codec " + std::to_string(static_cast<int>(info.codec));
    }
    return nullptr;
}

}  // namespace chast
