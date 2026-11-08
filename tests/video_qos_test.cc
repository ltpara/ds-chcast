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
int main() {
    try {
        VideoQos policy;
        policy.open(1, 0);
        check(policy.state(0).fps == 15, "new connection starts at 15 FPS");
        policy.delay(1, 10, 1200);
        policy.delay(1, 10, 1700);
        policy.delay(1, 10, 2200);
        check(policy.state(2200).fps == 23, "three good replies use RustDesk fast increase and stability step");
        policy.open(2, 2300);
        check(policy.state(2300).fps == 15, "new viewer temporarily limits existing displays");
        policy.delay(2, 600, 3500);
        check(policy.state(3500).fps == 2, "600 ms delay reduces Balanced FPS to two");
        policy.delay(1, 10, 4000);
        check(policy.state(4000).fps == 2, "fast viewer cannot overwrite slow viewer's constraint");
        policy.close(2, 4500);
        check(policy.state(4500).fps > 2, "leaving viewer releases global constraint");
        policy.pending(1, 2001, 5000);
        check(policy.state(5000).fps == 2, "unanswered application probe clamps global FPS");
        policy.pending(1, 0, 5100);
        check(policy.state(5100).fps > 2, "reply clears outstanding-response constraint");

        VideoQos settings(60, 1.5f, false);
        settings.open(1, 0); settings.open(2, 0);
        settings.options(1, 10, VideoQos::Quality::Smooth, 1000);
        settings.options(2, 45, VideoQos::Quality::High, 1000);
        check(settings.state(1000).fps == 10 && near(settings.state(1000).ratio, 1.5f),
              "FPS takes minimum, same-millisecond quality requests use most recent order");
        settings.options(1, 12, std::nullopt, 2000);
        check(near(settings.state(2000).ratio, 1.5f), "FPS-only update does not steal quality preference");
        settings.close(2, 3000);
        check(settings.state(3000).fps == 12 && near(settings.state(3000).ratio, 0.5f),
              "leaving quality owner restores surviving request");
        settings.close(1, 4000);
        check(settings.state(4000).fps == 60 && near(settings.state(4000).ratio, 1.5f),
              "last close resets controller for next session");

        VideoQos rtt;
        rtt.open(1, 0);
        for (unsigned n = 1; n <= 9; ++n) rtt.delay(1, 400, n * 500);
        const auto congested = rtt.state(4500).fps;
        rtt.delay(1, 430, 5000);
        check(congested < 10 && rtt.state(5000).fps >= 10,
              "after ten samples the historical RTT is removed from average delay");

        VideoQos bitrate(30, 1.5f);
        bitrate.open(1, 0);
        bitrate.display(10, 0, 5000, true, 0);
        bitrate.delay(1, 300, 1100);
        const auto lowered = bitrate.state(1100).ratio;
        check(near(lowered, 1.275f), "first 300 ms reply lowers ratio by 15 percent");
        for (unsigned n = 0; n < 3; ++n) bitrate.delay(1, 10, 1200 + n * 100);
        bitrate.display(10, 0, 5000, true, 4100);
        check(near(bitrate.state(4100).ratio, lowered), "static screen does not raise bitrate");
        bitrate.display(10, 6, 5000, true, 7100);
        check(near(bitrate.state(7100).ratio, lowered * 1.03f),
              "dynamic screen increase is bounded by 150 kbps per three seconds");
        bitrate.display(20, 0, 5000, false, 7200);
        check(near(bitrate.state(7200).ratio, 1.5f),
              "any fixed-quality display disables adaptive ratio globally");
        bitrate.remove_display(20);
        VideoQos custom(30, 2.0f);
        custom.open(1, 0);
        for (uint64_t n = 1; n <= 30; ++n) {
            custom.delay(1, 2000, n * 3000);
            custom.state(n * 3000);
        }
        check(near(custom.state(90000).ratio, 0.1f),
              "custom quality uses the custom floor rather than Best preset floor");
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL %s\n", error.what());
        return 1;
    }
}
