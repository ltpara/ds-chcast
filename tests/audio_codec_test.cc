// AAC-LC round trip: encode a synthetic 440 Hz tone, decode it, and check the
// stream's format, timestamps and audible content. No device, no network.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "chast/audio.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kRate = 48000;
constexpr uint16_t kChannels = 2;
constexpr double kToneHz = 440.0;
constexpr double kAmplitude = 0.5;
constexpr uint64_t kCaptureBaseMs = 123456;

std::vector<int16_t> make_tone(size_t frames, uint16_t channels = kChannels,
                               double amplitude = kAmplitude) {
    std::vector<int16_t> pcm(frames * channels);
    for (size_t frame = 0; frame < frames; ++frame) {
        const double sample =
            std::sin(2.0 * kPi * kToneHz * static_cast<double>(frame) / kRate);
        const auto value =
            static_cast<int16_t>(sample * amplitude * 32767.0 + 0.5);
        for (uint16_t channel = 0; channel < channels; ++channel) {
            pcm[frame * channels + channel] = value;
        }
    }
    return pcm;
}

struct Decoded {
    std::vector<int16_t> samples;
    size_t frames(uint16_t channels = kChannels) const {
        return samples.size() / channels;
    }
};

double rms(const std::vector<int16_t>& samples, size_t first_frame, size_t frames,
           uint16_t channels = kChannels) {
    double sum = 0.0;
    for (size_t frame = first_frame; frame < first_frame + frames; ++frame) {
        const double value = samples[frame * channels] / 32768.0;
        sum += value * value;
    }
    return std::sqrt(sum / static_cast<double>(frames));
}

double zero_crossing_hz(const std::vector<int16_t>& samples, size_t first_frame,
                        size_t frames, uint16_t channels = kChannels) {
    size_t crossings = 0;
    int16_t previous = samples[first_frame * channels];
    for (size_t frame = first_frame + 1; frame < first_frame + frames; ++frame) {
        const int16_t current = samples[frame * channels];
        if ((previous < 0 && current >= 0) || (previous >= 0 && current < 0)) {
            ++crossings;
        }
        previous = current;
    }
    const double seconds = static_cast<double>(frames) / kRate;
    return static_cast<double>(crossings) / (2.0 * seconds);
}

void test_round_trip() {
    chast::AudioEncoderConfig config;
    config.sample_rate = kRate;
    config.channels = kChannels;
    config.bitrate = 160000;
    std::string error;
    auto encoder = chast::create_aac_encoder(config, &error);
    check(encoder != nullptr, "the AAC encoder opens");
    if (!encoder) {
        std::printf("     %s\n", error.c_str());
        return;
    }
    check(encoder->sample_rate() == kRate && encoder->channels() == kChannels &&
              encoder->frame_samples() == chast::kAudioFrameSamples,
          "the encoder reports the announced format");
    check(!encoder->extradata().empty() &&
              encoder->extradata().size() <= chast::kMaxAudioExtradata,
          "the encoder produces an AudioSpecificConfig");

    constexpr size_t kFrames = kRate * 2;
    const std::vector<int16_t> pcm = make_tone(kFrames);
    std::vector<chast::EncodedAudio> packets;
    for (size_t offset = 0, chunk = 0; offset < kFrames; offset += chunk) {
        chunk = 317;  // deliberately not a multiple of the AAC frame size
        if (offset + chunk > kFrames) {
            chunk = kFrames - offset;
        }
        const uint64_t capture_ms =
            kCaptureBaseMs + static_cast<uint64_t>(offset) * 1000 / kRate;
        chast::Error encoded = encoder->encode(pcm.data() + offset * kChannels, chunk,
                                               capture_ms, packets);
        if (!encoded.ok()) {
            check(false, "encode() accepts an arbitrary block size");
            std::printf("     %s\n", encoded.to_string().c_str());
            return;
        }
    }
    check(true, "encode() accepts an arbitrary block size");
    chast::Error flushed = encoder->flush(packets);
    check(flushed.ok(), "flush() completes the stream");
    check(!packets.empty(), "the encoder produced packets");

    bool ordered = true;
    uint64_t last_ms = 0;
    bool first_packet = true;
    size_t payload_bytes = 0;
    for (const chast::EncodedAudio& packet : packets) {
        payload_bytes += packet.data.size();
        if (!first_packet && packet.capture_ms <= last_ms) {
            ordered = false;
        }
        last_ms = packet.capture_ms;
        first_packet = false;
    }
    check(ordered, "packet timestamps increase");
    check(payload_bytes > 0, "the packets carry payload");
    check(packets.front().capture_ms == kCaptureBaseMs,
          "the first packet carries the first sample's capture time");

    chast::AudioInfo info;
    info.enabled = true;
    info.codec = chast::AudioCodec::AAC;
    info.sample_rate = encoder->sample_rate();
    info.channels = encoder->channels();
    info.bitrate = encoder->bitrate();
    info.frame_samples = encoder->frame_samples();
    info.extradata = encoder->extradata();

    auto decoder = chast::create_aac_decoder(info, &error);
    check(decoder != nullptr, "the AAC decoder opens from the announced config");
    if (!decoder) {
        std::printf("     %s\n", error.c_str());
        return;
    }

    Decoded decoded;
    bool timestamps_ok = true;
    uint64_t previous_pts = 0;
    bool first_block = true;
    for (const chast::EncodedAudio& packet : packets) {
        chast::AudioHeader header;
        header.pts_ms = packet.capture_ms - kCaptureBaseMs;
        header.capture_ms = packet.capture_ms;
        header.sample_rate = kRate;
        header.frame_samples = chast::kAudioFrameSamples;
        header.channels = kChannels;
        header.codec = chast::AudioCodec::AAC;
        std::vector<chast::DecodedAudio> blocks;
        chast::Error result = decoder->decode(header, packet.data.data(),
                                              packet.data.size(), blocks);
        if (!result.ok()) {
            check(false, "every packet decodes");
            std::printf("     %s\n", result.to_string().c_str());
            return;
        }
        for (const chast::DecodedAudio& block : blocks) {
            if (!first_block && block.pts_ms <= previous_pts) {
                timestamps_ok = false;
            }
            previous_pts = block.pts_ms;
            first_block = false;
            decoded.samples.insert(decoded.samples.end(), block.samples.begin(),
                                   block.samples.end());
        }
    }
    check(timestamps_ok, "decoded block timestamps increase");

    const size_t frames = decoded.frames();
    const long long expected = static_cast<long long>(kFrames);
    check(std::llabs(static_cast<long long>(frames) - expected) <= 2048,
          "the decoded length matches the encoded length within a frame");
    if (frames < kRate / 2) {
        return;
    }

    const size_t middle_start = frames / 4;
    const size_t middle_frames = frames / 2;
    const double level = rms(decoded.samples, middle_start, middle_frames);
    check(level > 0.25 && level < 0.45,
          "the decoded tone keeps the expected energy");
    const double frequency =
        zero_crossing_hz(decoded.samples, middle_start, middle_frames);
    check(frequency > 430.0 && frequency < 450.0,
          "the decoded tone stays near 440 Hz");
    std::printf("     %zu packets, %zu bytes, %zu frames, RMS %.3f, %.1f Hz\n",
                packets.size(), payload_bytes, frames, level, frequency);
}

void test_gap_reanchors() {
    chast::AudioEncoderConfig config;
    std::string error;
    auto encoder = chast::create_aac_encoder(config, &error);
    check(encoder != nullptr, "the encoder opens for the gap case");
    if (!encoder) {
        return;
    }
    const std::vector<int16_t> pcm = make_tone(512);
    std::vector<chast::EncodedAudio> packets;
    check(encoder->encode(pcm.data(), 512, 1000, packets).ok(),
          "a partial frame is accepted");
    check(packets.empty(), "a partial frame is held until it is full");
    check(encoder->encode(pcm.data(), 512, 1600, packets).ok(),
          "a later block is accepted after an idle gap");
    check(encoder->flush(packets).ok(), "flush after the gap completes the stream");
    check(!packets.empty(), "the gap closes the partial frame");
    uint64_t previous = 0;
    bool ordered = true;
    for (const chast::EncodedAudio& packet : packets) {
        if (packet.capture_ms < previous) {
            ordered = false;
        }
        previous = packet.capture_ms;
    }
    check(ordered, "the re-anchored timestamps stay ordered");
    check(packets.front().capture_ms == 1000, "the pre-gap block keeps its own time");
    check(packets.back().capture_ms >= 1600,
          "the block after the gap is stamped near its capture time");
}

void test_opus_round_trip(uint16_t channels, uint32_t bitrate, const char* label) {
    chast::AudioEncoderConfig config;
    config.codec = chast::AudioCodec::Opus;
    config.sample_rate = kRate;
    config.channels = channels;
    config.bitrate = bitrate;
    config.voice = channels == 1;
    std::string error;
    auto encoder = chast::create_audio_encoder(config, &error);
    check(encoder != nullptr, label);
    if (!encoder) {
        std::printf("     %s\n", error.c_str());
        return;
    }
    check(encoder->sample_rate() == kRate && encoder->channels() == channels &&
              encoder->frame_samples() == chast::kOpusFrameSamples,
          "the Opus encoder reports 20 ms frames at 48 kHz");
    check(encoder->extradata().size() == 19 &&
              std::memcmp(encoder->extradata().data(), "OpusHead", 8) == 0,
          "the Opus encoder produces an OpusHead");

    constexpr size_t kFrames = kRate * 2;
    const std::vector<int16_t> pcm = make_tone(kFrames, channels);
    std::vector<chast::EncodedAudio> packets;
    for (size_t offset = 0, chunk = 0; offset < kFrames; offset += chunk) {
        chunk = 317;
        if (offset + chunk > kFrames) {
            chunk = kFrames - offset;
        }
        const uint64_t capture_ms =
            kCaptureBaseMs + static_cast<uint64_t>(offset) * 1000 / kRate;
        chast::Error encoded =
            encoder->encode(pcm.data() + offset * channels, chunk, capture_ms, packets);
        if (!encoded.ok()) {
            check(false, "Opus encode() accepts an arbitrary block size");
            std::printf("     %s\n", encoded.to_string().c_str());
            return;
        }
    }
    check(encoder->flush(packets).ok() && !packets.empty(),
          "the Opus encoder flushes its partial frame");
    check(packets.front().capture_ms == kCaptureBaseMs,
          "the first Opus packet carries the first sample's capture time");
    bool ordered = true;
    uint64_t previous_ms = 0;
    for (const chast::EncodedAudio& packet : packets) {
        if (packet.capture_ms < previous_ms) {
            ordered = false;
        }
        previous_ms = packet.capture_ms;
    }
    check(ordered, "Opus packet timestamps are ordered");

    chast::AudioInfo info;
    info.enabled = true;
    info.codec = chast::AudioCodec::Opus;
    info.sample_rate = encoder->sample_rate();
    info.channels = encoder->channels();
    info.bitrate = encoder->bitrate();
    info.frame_samples = encoder->frame_samples();
    info.extradata = encoder->extradata();

    auto decoder = chast::create_audio_decoder(info, &error);
    check(decoder != nullptr, "the Opus decoder opens from the announced config");
    if (!decoder) {
        std::printf("     %s\n", error.c_str());
        return;
    }

    Decoded decoded;
    bool first_block_pts = false;
    bool format_ok = true;
    for (const chast::EncodedAudio& packet : packets) {
        chast::AudioHeader header;
        header.pts_ms = packet.capture_ms - kCaptureBaseMs;
        header.capture_ms = packet.capture_ms;
        header.sample_rate = kRate;
        header.frame_samples = chast::kOpusFrameSamples;
        header.channels = channels;
        header.codec = chast::AudioCodec::Opus;
        std::vector<chast::DecodedAudio> blocks;
        chast::Error result =
            decoder->decode(header, packet.data.data(), packet.data.size(), blocks);
        if (!result.ok()) {
            check(false, "every Opus packet decodes");
            std::printf("     %s\n", result.to_string().c_str());
            return;
        }
        for (const chast::DecodedAudio& block : blocks) {
            if (!first_block_pts) {
                check(block.pts_ms == 0,
                      "the Opus pre-skip keeps the first audible sample at zero");
                first_block_pts = true;
            }
            if (block.sample_rate != kRate || block.channels != channels) {
                format_ok = false;
            }
            decoded.samples.insert(decoded.samples.end(), block.samples.begin(),
                                   block.samples.end());
        }
    }
    check(first_block_pts, "the Opus decoder produced audio");
    check(format_ok, "the Opus blocks keep the announced format");

    const size_t frames = decoded.frames(channels);
    const long long expected = static_cast<long long>(kFrames);
    check(std::llabs(static_cast<long long>(frames) - expected) <= 2048,
          "the Opus length matches the encoded length within the pre-skip");
    if (frames < kRate / 2) {
        return;
    }
    const size_t middle_start = frames / 4;
    const size_t middle_frames = frames / 2;
    const double level = rms(decoded.samples, middle_start, middle_frames, channels);
    check(level > 0.25 && level < 0.45,
          "the decoded Opus tone keeps the expected energy");
    const double frequency =
        zero_crossing_hz(decoded.samples, middle_start, middle_frames, channels);
    check(frequency > 430.0 && frequency < 450.0,
          "the decoded Opus tone stays near 440 Hz");
    std::printf("     %zu packets, %zu frames, RMS %.3f, %.1f Hz\n", packets.size(),
                frames, level, frequency);
}

void test_opus() {
    test_opus_round_trip(1, 32000, "the mono Opus encoder opens at 32 kbps");
    test_opus_round_trip(2, 128000, "the stereo Opus encoder opens at 128 kbps");

    std::string error;
    chast::AudioEncoderConfig wrong_rate;
    wrong_rate.codec = chast::AudioCodec::Opus;
    wrong_rate.sample_rate = 44100;
    check(chast::create_audio_encoder(wrong_rate, &error) == nullptr,
          "the Opus encoder refuses a rate other than 48 kHz");

    chast::AudioEncoderConfig unknown;
    unknown.codec = static_cast<chast::AudioCodec>(9);
    check(chast::create_audio_encoder(unknown, &error) == nullptr,
          "an unknown encoder codec is rejected");

    chast::AudioInfo bogus;
    bogus.codec = static_cast<chast::AudioCodec>(9);
    bogus.sample_rate = kRate;
    bogus.channels = 1;
    check(chast::create_audio_decoder(bogus, &error) == nullptr,
          "an unknown decoder codec is rejected");

    chast::AudioInfo missing;
    missing.enabled = true;
    missing.codec = chast::AudioCodec::Opus;
    missing.sample_rate = kRate;
    missing.channels = 1;
    missing.frame_samples = chast::kOpusFrameSamples;
    check(chast::create_opus_decoder(missing, &error) == nullptr,
          "the Opus decoder refuses a missing OpusHead");
}

void test_bad_config() {
    std::string error;
    chast::AudioEncoderConfig config;
    config.channels = 6;
    check(chast::create_aac_encoder(config, &error) == nullptr,
          "the encoder refuses surround channels");

    chast::AudioInfo info;
    info.enabled = true;
    info.sample_rate = 48000;
    info.channels = 2;
    check(chast::create_aac_decoder(info, &error) == nullptr,
          "the decoder refuses a missing AudioSpecificConfig");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_round_trip();
    test_gap_reanchors();
    test_opus();
    test_bad_config();

    if (failures == 0) {
        std::printf("audio codec checks passed\n");
        return 0;
    }
    std::printf("%d audio codec check(s) failed\n", failures);
    return 1;
}
