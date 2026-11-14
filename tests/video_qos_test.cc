#include "../src/host/video_qos.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>

using chast::VideoQos;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) throw std::runtime_error(what);
}
bool near(float a, float b) { return std::abs(a - b) < 0.0001f; }
void warm(VideoQos& qos, uint64_t id, uint64_t start, unsigned count) {
    for (unsigned n = 0; n < count; ++n) qos.delay(id, 10, start + n * 100);
}
int main() {
    try {
        VideoQos policy;
        policy.open(1, 0);
        check(policy.state(0).fps == 15, "new connection starts at 15 FPS");
        policy.delay(1, 10, 1100);
        check(policy.state(1100).fps == 16, "first good reply increases one step");
        policy.delay(1, 10, 1600);
        check(policy.state(1600).fps == 30, "two clean replies double the startup rate");
        policy.delay(1, 10, 2100);
        check(policy.state(2100).fps == 30, "startup acceleration stops at the cap");
        policy.open(2, 2200);
        check(policy.state(2200).fps == 30, "a late viewer preserves the established shared rate");
        policy.delay(2, 10, 2250);
        check(policy.state(2250).fps == 30, "a late viewer's first reply starts from the proven rate");
        policy.open(3, 2300);
        check(policy.state(2300).fps == 30, "repeated joins do not restart the shared warm-up");
        policy.pending(3, 3000, 5300);
        check(policy.state(5300).fps < 30, "a late viewer with no receipt still triggers congestion protection");
        policy.close(3, 5400);
        check(policy.state(5400).fps == 30, "removing the stalled late viewer releases its constraint");
        policy.options(2, 10, std::nullopt, 5500);
        check(policy.state(5500).fps == 10, "late viewer startup isolation retains explicit FPS caps");

        VideoQos initial_cohort(60);
        initial_cohort.open(1, 0);
        initial_cohort.open(2, 100);
        check(initial_cohort.state(100).fps == 15,
              "viewers joining before a proven stream retain conservative startup");

        VideoQos jitter;
        jitter.open(1, 0);
        warm(jitter, 1, 1100, 12);
        check(jitter.state(2300).fps == 30, "warm viewer reaches the cap");
        uint64_t now = 2400;
        for (unsigned n = 0; n < 20; ++n) {
            for (uint32_t delay : {800u, 10u, 10u}) {
                jitter.delay(1, delay, now);
                now += 100;
                check(jitter.state(now).fps == 30,
                      "one spike without confirmation does not accumulate congestion");
            }
        }

        VideoQos recovery;
        recovery.open(1, 0);
        warm(recovery, 1, 1100, 12);
        now = 2400;
        for (unsigned n = 0; n < 2; ++n) {
            recovery.delay(1, 400, now += 100);
            check(recovery.state(now).fps == 30, "first two bad replies only confirm");
        }
        recovery.delay(1, 400, now += 100);
        const auto first_cut = recovery.state(now).fps;
        check(first_cut >= 24 && first_cut < 30, "third bad reply cuts by at most a fifth");
        for (unsigned n = 0; n < 2; ++n) {
            recovery.delay(1, 400, now += 100);
            check(recovery.state(now).fps == first_cut, "cut waits for fresh evidence");
        }
        recovery.delay(1, 400, now += 100);
        const auto second_cut = recovery.state(now).fps;
        check(second_cut < first_cut && second_cut >= first_cut - first_cut / 5,
              "second confirmed cut is bounded too");

        VideoQos timeout;
        timeout.open(1, 0);
        warm(timeout, 1, 1100, 12);
        timeout.pending(1, 3000, 3000);
        check(timeout.state(3000).fps == 7, "a probe three seconds out quarters the rate");
        timeout.delay(1, 3200, 3100);
        check(timeout.state(3100).fps == 7, "the late reply belongs to the braked stall");
        timeout.pending(1, 0, 3200);
        check(timeout.state(3200).fps == 7, "clearing the probe does not lift the brake");
        timeout.delay(1, 10, 3300);
        check(timeout.state(3300).fps == 8, "one good reply does not restore the full rate");
        timeout.delay(1, 10, 3400);
        check(timeout.state(3400).fps == 30, "two good replies restore the held level");
        timeout.delay(1, 10, 3500);
        check(timeout.state(3500).fps == 30, "the restored rate stays");

        VideoQos restore;
        restore.open(1, 0);
        warm(restore, 1, 1100, 12);
        restore.delay(1, 1200, 4000);
        restore.delay(1, 1200, 4100);
        check(restore.state(4100).fps == 8, "one second of excess halves twice");
        restore.delay(1, 10, 4200);
        restore.delay(1, 10, 4300);
        check(restore.state(4300).fps == 30, "two good replies restore the previous level");
        for (unsigned n = 0; n < 2; ++n) {
            restore.delay(1, 400, 4400 + n * 100);
            check(restore.state(4400 + n * 100).fps == 30,
                  "a restored level tolerates two bad replies");
        }
        restore.delay(1, 400, 4600);
        check(restore.state(4600).fps < 30, "a restore that congests immediately is rolled back");
        restore.delay(1, 10, 4700);
        restore.delay(1, 10, 4800);
        check(restore.state(4800).fps < 30, "the failed level is not the level to return to");
        restore.delay(1, 10, 4900);
        check(restore.state(4900).fps == 30, "ordinary recovery can still reach the cap");

        VideoQos bitrate(30, 1.5f);
        bitrate.open(1, 0);
        bitrate.display(10, 1, 5000, true, 0);
        bitrate.delay(1, 10, 1100);
        bitrate.delay(1, 300, 3200);
        bitrate.delay(1, 300, 3500);
        check(near(bitrate.state(3500).ratio, 1.35f),
              "two confirmed bad replies cut the ratio by ten percent");
        for (unsigned n = 0; n < 3; ++n) {
            bitrate.delay(1, 10, 3600 + n * 100);
            bitrate.state(3600 + n * 100);
        }
        bitrate.display(10, 0, 5000, true, 4100);
        check(near(bitrate.state(4100).ratio, 1.35f),
              "static screen cannot raise the ratio");
        bitrate.display(10, 6, 5000, true, 6600);
        check(near(bitrate.state(6600).ratio, 1.35f * 1.03f),
              "dynamic screen increase is bounded by 150 kbps per three seconds");
        bitrate.display(20, 0, 5000, false, 6700);
        check(near(bitrate.state(6700).ratio, 1.5f),
              "any fixed-quality display disables adaptive ratio globally");

        VideoQos delivery_stall(30, 1.5f);
        delivery_stall.open(1, 0);
        delivery_stall.display(10, 1, 5000, true, 0);
        delivery_stall.delay(1, 10, 1100);
        delivery_stall.pending(1, 1900, 2000);
        check(near(delivery_stall.state(2000).ratio, 1.5f),
              "a sub-two-second outstanding receipt is treated as jitter");
        delivery_stall.pending(1, 2100, 2500);
        check(near(delivery_stall.state(2500).ratio, 0.45f),
              "the first receipt beyond two seconds cuts bitrate to the quality floor");
        delivery_stall.delay(1, 10, 2600);
        delivery_stall.delay(1, 10, 2700);
        delivery_stall.delay(1, 10, 2800);
        check(delivery_stall.state(2800).fps > 15,
              "completing a delivery stall starts frame-rate recovery");
        delivery_stall.delay(1, 10, 2900);
        delivery_stall.delay(1, 10, 3000);
        delivery_stall.delay(1, 10, 3100);
        check(delivery_stall.state(3100).fps == 30,
              "a clean delivery path still restores the previous frame rate");
        delivery_stall.display(10, 6, 5000, true, 5500);
        const auto delivery_ratio = delivery_stall.state(5500).ratio;
        check(near(delivery_ratio, 0.5175f),
              "normal bounded bitrate recovery follows the urgent cut");

        // RustDesk feeds its periodic adjustment once a second. An elevated
        // but unconfirmed delay must not clear the dynamic counter on every
        // tick, or a moving screen loses the evidence needed to recover.
        VideoQos cadence(30, 0.67f);
        cadence.open(1, 0);
        cadence.display(10, 1, 5000, true, 0);
        cadence.delay(1, 10, 1100);
        cadence.delay(1, 300, 3200);
        cadence.delay(1, 300, 3300);
        check(near(cadence.state(3300).ratio, 0.603f),
              "two bad replies cut the ratio before the cadence check");
        cadence.delay(1, 40, 3400);
        cadence.state(6300);
        for (uint64_t tick = 6333; tick <= 7268; tick += 33)
            cadence.display(10, 1, 5000, true, tick);
        const float cadence_before = cadence.state(7268).ratio;
        cadence.delay(1, 40, 7290);
        check(cadence.state(7300).ratio > cadence_before,
              "an unconfirmed high delay keeps the dynamic window on its one-second cadence");

        VideoQos multi(60, 1.5f);
        multi.open(1, 0);
        multi.open(2, 0);
        for (unsigned n = 0; n < 12; ++n) {
            multi.delay(1, 10, 1100 + n * 100);
            multi.delay(2, 10, 1100 + n * 100);
        }
        check(multi.state(2200).fps == 60, "both viewers warm up to the shared cap");
        for (unsigned n = 0; n < 3; ++n) multi.delay(2, 400, 2400 + n * 100);
        const auto slow = multi.state(2600).fps;
        check(slow == 48, "the slow viewer's confirmed cut constrains the stream");
        warm(multi, 1, 2700, 10);
        check(multi.state(3600).fps == slow, "a fast viewer cannot overwrite the slow limit");
        multi.close(2, 3700);
        check(multi.state(3700).fps == 60, "leaving viewer releases the global constraint");

        VideoQos settings(60, 1.5f, false, false);
        settings.open(1, 0);
        settings.open(2, 0);
        settings.options(1, 10, VideoQos::Quality::Smooth, 1000);
        settings.options(2, 45, VideoQos::Quality::High, 1000);
        check(settings.state(1000).fps == 10 && near(settings.state(1000).ratio, 1.5f),
              "FPS takes the minimum and the latest quality wins");
        settings.options(1, 12, std::nullopt, 2000);
        check(near(settings.state(2000).ratio, 1.5f),
              "an FPS-only update does not steal the quality preference");
        settings.options(1, 20, std::nullopt, 2100);
        check(settings.state(2100).fps == 20,
              "raising a viewer's FPS cap raises the shared stream at once");
        settings.close(2, 3000);
        check(settings.state(3000).fps == 20 && near(settings.state(3000).ratio, 0.5f),
              "leaving quality owner restores the surviving request");
        settings.close(1, 4000);
        check(settings.state(4000).fps == 60 && near(settings.state(4000).ratio, 1.5f),
              "last close resets the controller for the next session");

        VideoQos abr_off(60, 1.5f, false);
        abr_off.open(1, 0);
        check(abr_off.state(0).fps == 15,
              "ABR off still starts at the RustDesk initial FPS");
        warm(abr_off, 1, 1100, 4);
        check(abr_off.state(1400).fps == 60,
              "ABR off keeps delay-driven FPS adaptation");
        for (unsigned n = 0; n < 3; ++n) abr_off.delay(1, 400, 1500 + n * 100);
        check(abr_off.state(1700).fps < 60,
              "confirmed congestion lowers FPS with ABR off");
        check(near(abr_off.state(1700).ratio, 1.5f),
              "ABR off holds the quality ratio at the viewer target");

        // A path that can no longer shed bitrate has to shed frames: holding
        // the soft floor left the queue full and the delay growing. The delay
        // keeps growing, so the policy cannot mistake it for a high-RTT path.
        VideoQos congested(30, 1.5f, false);
        congested.open(1, 0);
        warm(congested, 1, 1100, 12);
        uint32_t settled = 30;
        for (unsigned n = 0; n < 48; ++n) {
            congested.delay(1, 700 + n * 40, 2300 + n * 100);
            settled = congested.state(2300 + n * 100).fps;
        }
        check(settled == 1,
              "a saturated path that cannot shed bitrate sheds frames instead");
        congested.delay(1, 10, 7500);
        congested.delay(1, 10, 7600);
        check(congested.state(7600).fps == 30,
              "clean replies still restore the rate the floor gave up");

        // While the bitrate can still come down, fewer frames do not send fewer
        // bytes: the frame rate keeps its soft floor there.
        VideoQos soft_floor(30, 1.5f, true);
        soft_floor.open(1, 0);
        soft_floor.display(10, 1, 5000, true, 0);
        warm(soft_floor, 1, 1100, 12);
        for (unsigned n = 0; n < 10; ++n) soft_floor.delay(1, 800, 2300 + n * 100);
        check(soft_floor.state(3300).fps >= 5,
              "a frame-rate floor holds while the bitrate can still be cut");
        check(near(soft_floor.state(3300).ratio, 1.5f) == false,
              "the same congestion is answered with bitrate, not only frames");

        // A probe that is still unanswered means nothing is arriving at all.
        VideoQos stalled(30, 1.5f, false);
        stalled.open(1, 0);
        warm(stalled, 1, 1100, 12);
        stalled.pending(1, 5000, 5000);
        check(stalled.state(5000).fps == 1,
              "an unanswered probe for five seconds drops to the trickle rate");

        VideoQos fixed_rates(60, 1.5f, false, false);
        fixed_rates.open(1, 0);
        fixed_rates.delay(1, 400, 1100);
        fixed_rates.pending(1, 3000, 1200);
        check(fixed_rates.state(1200).fps == 60 && near(fixed_rates.state(1200).ratio, 1.5f),
              "adaptive_fps=false pins configured rates for exact-rate probes");

        VideoQos voted(60, 1.5f, false, false);
        voted.open(1, 0);
        voted.open(2, 0);
        voted.options(1, 10, VideoQos::Quality::Smooth, 1000);
        voted.options(2, 15, std::nullopt, 1100, 1.2f);
        check(voted.state(1100).fps == 10 && near(voted.state(1100).ratio, 1.2f),
              "a numeric custom quality becomes the latest vote");
        voted.options(2, 15, std::nullopt, 1200);
        check(near(voted.state(1200).ratio, 1.2f),
              "an FPS-only update keeps the custom quality vote");
        voted.options(1, 10, VideoQos::Quality::High, 1300);
        check(near(voted.state(1300).ratio, 1.5f),
              "a newer preset replaces the custom quality");
        voted.options(2, 15, std::nullopt, 1400, 0.5f);
        check(near(voted.state(1400).ratio, 0.5f),
              "a newer custom vote replaces the preset");

        VideoQos custom(30, 2.0f);
        custom.open(1, 0);
        warm(custom, 1, 1100, 2);
        for (unsigned n = 1; n <= 10; ++n) {
            custom.delay(1, 2000, 3000ull * n);
            custom.delay(1, 2000, 3000ull * n + 100);
            custom.delay(1, 2000, 3000ull * n + 200);
            custom.state(3000ull * n + 3000);
        }
        check(near(custom.state(33000).ratio, 0.1f),
              "custom quality uses the custom floor rather than a preset floor");
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL %s\n", error.what());
        return 1;
    }
}
