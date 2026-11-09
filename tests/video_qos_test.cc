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
        check(policy.state(2200).fps == 15, "a new viewer's first second clamps the stream to 15");

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

        VideoQos settings(60, 1.5f, false);
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
