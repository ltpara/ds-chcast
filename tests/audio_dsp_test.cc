// Echo cancellation and noise suppression against synthetic signals. The
// echo path is a fixed delay plus a two-tap filter, the reference is fed
// through AudioReference exactly as the players feed it, and the near end is
// processed in endpoint-sized blocks. No device, no network, deterministic.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
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

constexpr uint32_t kRate = 48000;
constexpr size_t kFrame = 480;
constexpr size_t kEchoDelay = kRate * 140 / 1000;  // 140 ms, audibly 20 ms behind the reference read
constexpr size_t kReferenceDelayMs = 120;
constexpr double kEchoGain = 0.5;
constexpr double kPi = 3.14159265358979323846;

int16_t clamp_sample(double value) {
    return static_cast<int16_t>(std::clamp(value, -32767.0, 32767.0));
}

struct Signal {
    std::vector<int16_t> far_pcm;
    std::vector<int16_t> near_pcm;
    std::vector<int16_t> speech;  // near-end talker only
};

// White noise far end, a delayed/filtered copy as the echo, and optional
// double talk in the middle. Everything is at 48 kHz mono.
Signal make_echo_scene(size_t frames, size_t double_talk_start, size_t double_talk_frames,
                       bool with_speech) {
    Signal signal;
    signal.far_pcm.resize(frames);
    signal.near_pcm.resize(frames);
    signal.speech.assign(frames, 0);
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 0.15);
    std::normal_distribution<double> speech(0.0, 0.10);
    for (size_t frame = 0; frame < frames; ++frame) {
        signal.far_pcm[frame] = clamp_sample(noise(rng) * 32768.0);
    }
    for (size_t frame = 0; frame < frames; ++frame) {
        double echo = 0.0;
        if (frame >= kEchoDelay) {
            echo = kEchoGain * signal.far_pcm[frame - kEchoDelay];
            if (frame > kEchoDelay) {
                echo += 0.2 * signal.far_pcm[frame - kEchoDelay - 1];
            }
        }
        double talk = 0.0;
        if (with_speech && frame >= double_talk_start &&
            frame < double_talk_start + double_talk_frames) {
            talk = speech(rng) * 32768.0;
            signal.speech[frame] = clamp_sample(talk);
        }
        signal.near_pcm[frame] = clamp_sample(echo + talk);
    }
    return signal;
}

double energy(const int16_t* samples, size_t first, size_t frames, uint16_t channels) {
    double sum = 0.0;
    for (size_t frame = first; frame < first + frames; ++frame) {
        for (uint16_t ch = 0; ch < channels; ++ch) {
            const double value = samples[frame * channels + ch] / 32768.0;
            sum += value * value;
        }
    }
    return sum;
}

double rms(const std::vector<int16_t>& samples, size_t first_frame, size_t frames,
           uint16_t channels = 1) {
    return std::sqrt(energy(samples.data(), first_frame, frames, channels) /
                     static_cast<double>(frames * channels));
}

double correlation(const std::vector<int16_t>& left, const std::vector<int16_t>& right,
                   size_t first_frame, size_t frames) {
    double mean_left = 0.0;
    double mean_right = 0.0;
    for (size_t frame = first_frame; frame < first_frame + frames; ++frame) {
        mean_left += left[frame];
        mean_right += right[frame];
    }
    mean_left /= static_cast<double>(frames);
    mean_right /= static_cast<double>(frames);
    double covariance = 0.0;
    double var_left = 0.0;
    double var_right = 0.0;
    for (size_t frame = first_frame; frame < first_frame + frames; ++frame) {
        const double a = left[frame] - mean_left;
        const double b = right[frame] - mean_right;
        covariance += a * b;
        var_left += a * a;
        var_right += b * b;
    }
    if (var_left <= 0.0 || var_right <= 0.0) {
        return 0.0;
    }
    return covariance / std::sqrt(var_left * var_right);
}

// Processes `near` in blocks of `block` frames, pushing the matching far-end
// slice into the reference first. Returns the processed copy.
std::vector<int16_t> process_scene(const Signal& scene, chast::AudioPreprocessor& dsp,
                                   chast::AudioReference& reference, size_t block) {
    std::vector<int16_t> output = scene.near_pcm;
    for (size_t offset = 0; offset < scene.near_pcm.size(); offset += block) {
        const size_t frames = (std::min)(block, scene.near_pcm.size() - offset);
        reference.push(scene.far_pcm.data() + offset, frames, 1);
        dsp.process(output.data() + offset, frames);
    }
    return output;
}

void test_echo_cancellation() {
    constexpr size_t kSeconds = 6;
    constexpr size_t kFrames = kRate * kSeconds;
    const size_t double_talk_start = kRate * 2;
    const size_t double_talk_frames = kRate;
    const Signal scene = make_echo_scene(kFrames, double_talk_start, double_talk_frames, true);

    auto reference = std::make_shared<chast::AudioReference>();
    reference->set_active(true);
    chast::AudioDspConfig config;
    config.near_channels = 1;
    config.echo_cancellation = true;
    config.noise_suppression = false;
    config.reference_delay_ms = kReferenceDelayMs;
    config.reference = reference;
    std::string error;
    auto dsp = chast::AudioPreprocessor::create(config, &error);
    check(dsp != nullptr, "the echo canceller opens with a live reference");
    if (!dsp) {
        std::printf("     %s\n", error.c_str());
        return;
    }

    const std::vector<int16_t> output =
        process_scene(scene, *dsp, *reference, kFrame);
    check(output.size() == scene.near_pcm.size(), "the preprocessor keeps the frame count");

    // Measure steady-state echo suppression after the filter has converged,
    // outside the double-talk window.
    const size_t steady_start = kRate * 4;
    const size_t steady_frames = kRate * 2;
    const double near_energy =
        energy(scene.near_pcm.data(), steady_start, steady_frames, 1);
    const double out_energy = energy(output.data(), steady_start, steady_frames, 1);
    const double erle = 10.0 * std::log10((near_energy + 1e-12) / (out_energy + 1e-12));
    std::printf("     echo ERLE %.2f dB\n", erle);
    check(erle >= 20.0, "the echo canceller reaches at least 20 dB ERLE");

    // Double talk must survive: the processed block should still track the
    // near-end talker, not be suppressed along with the echo.
    const double talk_correlation =
        correlation(output, scene.speech, double_talk_start, double_talk_frames);
    std::printf("     double-talk correlation %.3f\n", talk_correlation);
    check(talk_correlation >= 0.9, "double talk stays correlated with the near-end talker");

    const chast::AudioDspStats stats = dsp->stats();
    check(stats.processed_frames >= kFrames - kFrame,
          "the DSP reports the frames it processed");
    check(stats.reference_underruns <= 20,
          "a continuously fed reference only underruns while it warms up");

    // A third of a second of reference history cannot satisfy a 120 ms delayed
    // 10 ms block at the very start, but once past the delay it is complete.
    auto fresh_reference = std::make_shared<chast::AudioReference>();
    fresh_reference->set_active(true);
    config.reference = fresh_reference;
    auto fresh = chast::AudioPreprocessor::create(config, &error);
    check(fresh != nullptr, "a second echo canceller opens");
    if (fresh) {
        std::vector<int16_t> block(kFrame * 30, 1000);
        for (size_t i = 0; i < 30; ++i) {
            fresh_reference->push(block.data() + i * kFrame, kFrame, 1);
            fresh->process(block.data() + i * kFrame, kFrame);
        }
        const chast::AudioDspStats fresh_stats = fresh->stats();
        check(fresh_stats.reference_underruns > 0,
              "a short reference is reported as an underrun");
        check(fresh_stats.reference_underruns < 30,
              "the underrun count stops once the delay is covered");
    }
}

void test_inactive_reference() {
    auto reference = std::make_shared<chast::AudioReference>();
    chast::AudioDspConfig config;
    config.near_channels = 1;
    config.echo_cancellation = true;
    config.noise_suppression = false;
    config.reference = reference;
    std::string error;
    auto dsp = chast::AudioPreprocessor::create(config, &error);
    check(dsp != nullptr, "the echo canceller opens before playback starts");
    if (!dsp) {
        std::printf("     %s\n", error.c_str());
        return;
    }

    std::mt19937 rng(11);
    std::uniform_int_distribution<int> values(-8000, 8000);
    std::vector<int16_t> input(1000);
    for (int16_t& sample : input) {
        sample = static_cast<int16_t>(values(rng));
    }
    std::vector<int16_t> output = input;
    dsp->process(output.data(), output.size());
    check(output == input, "an inactive reference passes audio through unchanged");

    // Activate, feed real reference data, then deactivate again: the reset
    // must restore the strict pass-through, including for a partial block.
    reference->set_active(true);
    std::vector<int16_t> far_pcm(kRate, 2000);
    reference->push(far_pcm.data(), far_pcm.size(), 1);
    std::vector<int16_t> active(2000, 0);
    dsp->process(active.data(), active.size());
    reference->set_active(false);

    std::vector<int16_t> after = input;
    dsp->process(after.data(), after.size());
    check(after == input, "deactivating the reference restores pass-through");
    check(dsp->stats().resets >= 1, "deactivation resets the adaptive filter");

    // A block shorter than one DSP frame is held, not dropped: once the rest
    // arrives the output covers every input sample.
    std::vector<int16_t> first(137, 444);
    std::vector<int16_t> second(800, 555);
    dsp->process(first.data(), first.size());
    dsp->process(second.data(), second.size());
    check(first == std::vector<int16_t>(137, 444) &&
              second == std::vector<int16_t>(800, 555),
          "partial frames pass through untouched while bypassed");
}

void test_noise_suppression() {
    constexpr size_t kFrames = kRate * 4;
    std::mt19937 rng(23);
    std::normal_distribution<double> noise(0.0, 0.10);
    std::vector<int16_t> noisy(kFrames);
    for (int16_t& sample : noisy) {
        sample = clamp_sample(noise(rng) * 32768.0);
    }

    chast::AudioDspConfig config;
    config.near_channels = 1;
    config.noise_suppression = true;
    std::string error;
    auto dsp = chast::AudioPreprocessor::create(config, &error);
    check(dsp != nullptr, "the noise suppressor opens without a reference");
    if (!dsp) {
        std::printf("     %s\n", error.c_str());
        return;
    }

    std::vector<int16_t> output = noisy;
    for (size_t offset = 0; offset < output.size(); offset += kFrame) {
        dsp->process(output.data() + offset, kFrame);
    }
    const size_t measure_start = kRate * 2;
    const size_t measure_frames = kRate * 2;
    const double input_level = rms(noisy, measure_start, measure_frames);
    const double output_level = rms(output, measure_start, measure_frames);
    const double reduction = 20.0 * std::log10((input_level + 1e-12) / (output_level + 1e-12));
    std::printf("     noise floor %.2f dB -> %.2f dB (%.2f dB)\n", 20.0 * std::log10(input_level),
                20.0 * std::log10(output_level), reduction);
    check(reduction >= 6.0, "noise suppression lowers a stationary floor by at least 6 dB");

    // A modulated harmonic stands in for voiced speech: it must not be
    // treated as a stationary floor and removed wholesale. A pure constant
    // tone is stationary too, and Speex legitimately folds it into the noise
    // estimate, so the envelope is what makes this a speech-like signal.
    std::vector<int16_t> tone(kFrames);
    for (size_t frame = 0; frame < tone.size(); ++frame) {
        const double t = static_cast<double>(frame) / kRate;
        const double envelope = 0.55 + 0.45 * std::sin(2.0 * kPi * 3.0 * t);
        const double voiced =
            std::sin(2.0 * kPi * 440.0 * t) +
            0.5 * std::sin(2.0 * kPi * 880.0 * t) +
            0.25 * std::sin(2.0 * kPi * 1320.0 * t);
        tone[frame] = clamp_sample(voiced / 1.75 * 12000.0 * envelope);
    }
    std::vector<int16_t> kept = tone;
    for (size_t offset = 0; offset < kept.size(); offset += kFrame) {
        dsp->process(kept.data() + offset, kFrame);
    }
    const double tone_input = rms(tone, measure_start, measure_frames);
    const double tone_output = rms(kept, measure_start, measure_frames);
    const double tone_ratio = tone_output / (tone_input + 1e-12);
    std::printf("     tone kept at %.2f%%\n", tone_ratio * 100.0);
    check(tone_ratio > 0.3, "noise suppression keeps voice-band energy");
}

void test_stereo_and_partial_blocks() {
    constexpr size_t kFrames = kRate * 5;
    const size_t double_talk_start = kRate * 2;
    const Signal scene = make_echo_scene(kFrames, double_talk_start, 0, false);

    std::vector<int16_t> stereo(scene.near_pcm.size() * 2);
    for (size_t frame = 0; frame < scene.near_pcm.size(); ++frame) {
        stereo[frame * 2] = scene.near_pcm[frame];
        stereo[frame * 2 + 1] = clamp_sample(scene.near_pcm[frame] * 0.7);
    }

    auto reference = std::make_shared<chast::AudioReference>();
    reference->set_active(true);
    chast::AudioDspConfig config;
    config.near_channels = 2;
    config.echo_cancellation = true;
    config.reference = reference;
    std::string error;
    auto dsp = chast::AudioPreprocessor::create(config, &error);
    check(dsp != nullptr, "the echo canceller opens for stereo near end");
    if (!dsp) {
        std::printf("     %s\n", error.c_str());
        return;
    }

    const size_t steady_start = kRate * 3;
    const size_t steady_frames = kRate * 2;
    // 317 samples is not a multiple of the 480-sample DSP frame; the
    // preprocessor must buffer the remainder and still cover every sample.
    for (const size_t kBlock : {size_t{480}, size_t{317}}) {
        std::vector<int16_t> output = stereo;
        for (size_t offset = 0; offset < scene.near_pcm.size(); offset += kBlock) {
            const size_t frames = (std::min)(kBlock, scene.near_pcm.size() - offset);
            reference->push(scene.far_pcm.data() + offset, frames, 1);
            dsp->process(output.data() + offset * 2, frames);
        }
        check(output.size() == stereo.size(), "stereo output keeps every sample");
        const double near_energy = energy(stereo.data(), steady_start, steady_frames, 2);
        const double out_energy = energy(output.data(), steady_start, steady_frames, 2);
        const double erle = 10.0 * std::log10((near_energy + 1e-12) / (out_energy + 1e-12));
        std::printf("     stereo block %zu ERLE %.2f dB\n", kBlock, erle);
        check(erle >= 20.0, "stereo cancellation works");
    }
    check(dsp->stats().processed_frames + kFrame >= kFrames,
          "every full DSP frame was processed");

    chast::AudioDspConfig missing;
    missing.echo_cancellation = true;
    error.clear();
    check(chast::AudioPreprocessor::create(missing, &error) == nullptr,
          "echo cancellation without a reference is refused");

    chast::AudioDspConfig channels;
    channels.near_channels = 3;
    error.clear();
    check(chast::AudioPreprocessor::create(channels, &error) == nullptr,
          "more than two near-end channels is refused");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_echo_cancellation();
    test_inactive_reference();
    test_noise_suppression();
    test_stereo_and_partial_blocks();

    if (failures == 0) {
        std::printf("audio DSP checks passed\n");
        return 0;
    }
    std::printf("%d audio DSP check(s) failed\n", failures);
    return 1;
}
