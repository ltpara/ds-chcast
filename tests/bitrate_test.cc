// Pure-logic checks for the adaptive bitrate controller: no GPU, no network.

#include <cstdint>
#include <cstdio>
#include <string>

#include "chast/bitrate_controller.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

chast::BitrateSample healthy_sample(uint64_t packets) {
    chast::BitrateSample sample;
    sample.min_rtt_us = 10000;
    sample.rtt_us = 10000;
    sample.congestion_window = 1000000;
    sample.sent_packets = packets;
    return sample;
}

void test_severe_loss_and_cooldown() {
    chast::BitrateController controller(10000000, 1000000, 30);
    chast::BitrateDecision decision = controller.update(healthy_sample(1000), 1000);
    check(!decision.changed && controller.target_bps() == 10000000,
          "a clean baseline leaves the target at the ceiling");

    chast::BitrateSample lossy = healthy_sample(1100);
    lossy.lost_packets = 20;  // 20% of 100 packets
    decision = controller.update(lossy, 1000);
    check(decision.changed && decision.target_bps == 7000000 &&
              std::string(decision.reason) == "loss",
          "severe loss drops the target by 30%");

    lossy.sent_packets = 1200;
    lossy.lost_packets = 40;
    decision = controller.update(lossy, 1000);
    check(!decision.changed && controller.target_bps() == 7000000,
          "the down cooldown absorbs a second loss report");

    lossy.sent_packets = 1300;
    lossy.lost_packets = 60;
    decision = controller.update(lossy, 1000);
    check(decision.changed && decision.target_bps == 4900000,
          "persistent loss lowers the target again after the cooldown");
}

void test_rtt_inflation() {
    chast::BitrateController controller(10000000, 1000000, 30);
    controller.update(healthy_sample(1000), 1000);

    chast::BitrateSample inflated = healthy_sample(1100);
    inflated.rtt_us = 50000;
    chast::BitrateDecision decision = controller.update(inflated, 1000);
    check(decision.changed && decision.target_bps == 8500000 &&
              std::string(decision.reason) == "rtt",
          "RTT inflation drops the target by 15%");
}

void test_queue_pressure() {
    chast::BitrateController controller(10000000, 1000000, 30);
    controller.update(healthy_sample(1000), 1000);

    chast::BitrateSample backed_up = healthy_sample(1100);
    backed_up.queued_bytes = 700000;  // 70% of the congestion window
    chast::BitrateDecision decision = controller.update(backed_up, 1000);
    check(decision.changed && decision.target_bps == 8500000 &&
              std::string(decision.reason) == "queue",
          "a filling send queue drops the target");

    chast::BitrateController severe(10000000, 1000000, 30);
    severe.update(healthy_sample(1000), 1000);
    backed_up.queued_bytes = 1200000;
    decision = severe.update(backed_up, 1000);
    check(decision.changed && decision.target_bps == 7000000,
          "a queue past the congestion window drops the target by 30%");
}

void test_recovery_and_ceiling() {
    chast::BitrateController controller(10000000, 1000000, 30);
    controller.update(healthy_sample(1000), 1000);
    chast::BitrateSample inflated = healthy_sample(1100);
    inflated.rtt_us = 50000;
    controller.update(inflated, 1000);
    check(controller.target_bps() == 8500000, "recovery starts below the ceiling");

    uint64_t packets = 1200;
    bool recovered_once = false;
    // One 200 kbps step per 3 s of health, 8 steps from 8.5 to 10 Mbps.
    for (int i = 0; i < 30; ++i) {
        chast::BitrateSample clean = healthy_sample(packets);
        packets += 100;
        const chast::BitrateDecision decision = controller.update(clean, 1000);
        if (decision.changed) {
            check(std::string(decision.reason) == "recover" && decision.target_bps > 8500000,
                  "a clean window raises the target");
            recovered_once = true;
        }
        check(controller.target_bps() <= controller.ceiling_bps(),
              "the target never passes the ceiling");
    }
    check(recovered_once && controller.target_bps() == 10000000,
          "sustained health returns the target to the configured ceiling");
}

void test_floor_and_bounds() {
    chast::BitrateController controller(2000000, 1500000, 30);
    check(controller.floor_bps() == 1500000, "floor below the ceiling is kept");

    controller.update(healthy_sample(1000), 1000);
    chast::BitrateSample lossy = healthy_sample(1100);
    lossy.lost_packets = 30;
    chast::BitrateDecision decision = controller.update(lossy, 1000);
    check(decision.changed && decision.target_bps == 1500000,
          "a decrease stops at the floor");

    lossy.sent_packets = 1300;
    lossy.lost_packets = 60;
    decision = controller.update(lossy, 2000);
    check(!decision.changed && controller.target_bps() == 1500000,
          "the target stays at the floor under continuing loss");

    chast::BitrateController inverted(1000000, 5000000, 30);
    check(inverted.ceiling_bps() == 1000000 && inverted.floor_bps() == 1000000 &&
              inverted.target_bps() == 1000000,
          "an inverted floor/ceiling pair collapses to the ceiling");

    chast::BitrateController empty(0, 0, 30);
    check(empty.target_bps() == 1 && empty.floor_bps() == 1,
          "a zero-bitrate configuration is clamped to a usable value");
}

void test_no_congestion_window() {
    chast::BitrateController controller(10000000, 1000000, 30);
    chast::BitrateSample sample;
    sample.min_rtt_us = 1000;
    sample.rtt_us = 1000;
    // No congestion window, no queue knowledge: only loss and RTT count.
    for (uint64_t packets = 100; packets <= 500; packets += 100) {
        const chast::BitrateDecision decision = controller.update(sample, 1000);
        check(!decision.changed, "a connection without a congestion window stays put");
        sample.sent_packets = packets;
    }
}

void test_framerate_ladder() {
    using chast::BitrateController;
    check(BitrateController::framerate_for(10000000, 10000000, 30) == 30,
          "a full-rate target keeps the configured frame rate");
    check(BitrateController::framerate_for(7000000, 10000000, 30) == 30,
          "70% of the ceiling still counts as full rate");
    check(BitrateController::framerate_for(6999999, 10000000, 30) == 20,
          "just under 70% drops to two thirds");
    check(BitrateController::framerate_for(4500000, 10000000, 30) == 20,
          "45% is the bottom of the two-thirds step");
    check(BitrateController::framerate_for(4400000, 10000000, 30) == 15,
          "under 45% drops to half");
    check(BitrateController::framerate_for(2500000, 10000000, 30) == 15,
          "25% is the bottom of the half step");
    check(BitrateController::framerate_for(2400000, 10000000, 30) == 10,
          "under 25% drops to a third");
    check(BitrateController::framerate_for(1, 10000000, 30) == 10,
          "the ladder never goes below 10 fps");
    check(BitrateController::framerate_for(1000000, 1000000, 12) == 12,
          "a 12 fps configuration stays at 12");
    check(BitrateController::framerate_for(1, 1000000, 12) == 10,
          "a slow configuration still bottoms out at 10, not a third of 12");

    // The ladder is a function of target/ceiling, so a decision that lowers
    // the rate moves the frame rate with it, and recovery restores both.
    chast::BitrateController controller(10000000, 1000000, 30);
    chast::BitrateDecision decision = controller.update(healthy_sample(1000), 1000);
    check(!decision.changed && decision.framerate == 30,
          "a clean sample reports the configured frame rate");

    chast::BitrateSample lossy = healthy_sample(1100);
    lossy.lost_packets = 20;
    decision = controller.update(lossy, 1000);
    check(decision.changed && decision.target_bps == 7000000 && decision.framerate == 30,
          "a 30% cut stays at full frame rate");

    for (int i = 0; i < 3; ++i) {
        lossy.sent_packets += 100;
        lossy.lost_packets += 20;
        decision = controller.update(lossy, 1600);
    }
    const uint16_t lowest = decision.framerate;
    check(decision.target_bps < 5000000 && lowest < 30,
          "repeated cuts bring the frame rate down with the rate");

    uint64_t packets = lossy.sent_packets;
    bool saw_ladder_up = false;
    for (int i = 0; i < 300; ++i) {
        packets += 100;
        const chast::BitrateDecision up = controller.update(healthy_sample(packets), 1000);
        if (up.changed && up.framerate > lowest) {
            saw_ladder_up = true;
        }
    }
    check(saw_ladder_up, "recovery climbs the frame rate ladder back up");
    check(controller.target_bps() == 10000000 && controller.framerate() == 30,
          "a recovered path is back at full rate and full frame rate");
}

void test_resolution_ladder() {
    using chast::BitrateController;
    check(BitrateController::resolution_for(10000000, 10000000, 100) == 100,
          "a full target keeps full resolution");
    check(BitrateController::resolution_for(5000000, 10000000, 100) == 100,
          "half the ceiling is still full resolution");
    check(BitrateController::resolution_for(4999999, 10000000, 100) == 75,
          "just under half drops to three quarters");
    check(BitrateController::resolution_for(3000000, 10000000, 100) == 75,
          "30% is the bottom of the three-quarter step");
    check(BitrateController::resolution_for(2999999, 10000000, 100) == 50,
          "under 30% drops to half");
    check(BitrateController::resolution_for(1, 10000000, 100) == 50,
          "the ladder bottoms out at half resolution");

    // Hysteresis: recovering needs more headroom than dropping did, so a
    // target sitting on an edge cannot toggle the encoder size.
    check(BitrateController::resolution_for(5000000, 10000000, 75) == 75,
          "a 75% stream holds through the full-resolution down edge");
    check(BitrateController::resolution_for(5500000, 10000000, 75) == 100,
          "a 75% stream restores full size at 55%");
    check(BitrateController::resolution_for(2900000, 10000000, 75) == 50,
          "a 75% stream drops below 30%");
    check(BitrateController::resolution_for(3400000, 10000000, 50) == 75,
          "a 50% stream restores three quarters at 34%");
    check(BitrateController::resolution_for(3300000, 10000000, 50) == 50,
          "a 50% stream holds below 34%");
    check(BitrateController::resolution_for(5500000, 10000000, 50) == 100,
          "a large recovery skips a rung instead of resizing twice");

    // Decisions carry the size, and the frame rate and resolution move
    // together as the target falls.
    chast::BitrateController controller(10000000, 1000000, 30);
    chast::BitrateDecision decision = controller.update(healthy_sample(1000), 1000);
    check(!decision.changed && decision.resolution_percent == 100,
          "a clean sample reports full resolution");

    chast::BitrateSample lossy = healthy_sample(1100);
    lossy.lost_packets = 20;
    decision = controller.update(lossy, 1000);
    check(decision.changed && decision.target_bps == 7000000 &&
              decision.resolution_percent == 100,
          "a 30% cut keeps full resolution");

    lossy.sent_packets = 1200;
    lossy.lost_packets = 40;
    decision = controller.update(lossy, 1600);
    check(decision.changed && decision.target_bps == 4900000 && decision.framerate == 20 &&
              decision.resolution_percent == 75,
          "under half the ceiling the frame rate and resolution drop together");

    lossy.sent_packets = 1300;
    lossy.lost_packets = 60;
    decision = controller.update(lossy, 1600);
    check(decision.changed && decision.target_bps == 3430000 &&
              decision.resolution_percent == 75,
          "the 75% rung holds until the target falls below 30%");

    lossy.sent_packets = 1400;
    lossy.lost_packets = 80;
    decision = controller.update(lossy, 1600);
    check(decision.changed && decision.resolution_percent == 50 && decision.framerate == 10,
          "a deep cut reaches half resolution and a third of the frame rate");

    uint64_t packets = lossy.sent_packets;
    bool saw_size_up = false;
    for (int i = 0; i < 300; ++i) {
        packets += 100;
        const chast::BitrateDecision up = controller.update(healthy_sample(packets), 1000);
        if (up.changed && up.resolution_percent > 50) {
            saw_size_up = true;
        }
    }
    check(saw_size_up && controller.target_bps() == 10000000 &&
              controller.resolution_percent() == 100,
          "recovery restores full resolution with the target");

    chast::BitrateController resumed(10000000, 1000000, 30, 50);
    check(resumed.resolution_percent() == 50,
          "a controller rebuilt on the 50% rung does not jump to full size");
    chast::BitrateController snapped(10000000, 1000000, 30, 80);
    check(snapped.resolution_percent() == 75, "an unknown rung snaps to a real one");
}

void test_audio_budget() {
    using chast::BitrateController;
    check(chast::video_floor(10000000) == 1000000, "the video floor is a tenth of the budget");
    check(chast::video_floor(2000000) == 500000, "the video floor never drops below 500 kbps");
    check(chast::video_floor(300000) == 300000, "the floor never exceeds a small budget");

    check(chast::video_budget(10000000, 0) == 10000000,
          "no companion stream leaves the whole budget to video");
    check(chast::video_budget(10000000, 193600) == 9806400,
          "audio plus 10% overhead is reserved out of the budget");
    check(chast::video_budget(2000000, 193600) == 1806400,
          "the reserve scales with a smaller budget");
    check(chast::video_budget(1000000, 10000000) == 500000,
          "a reserve larger than the budget is capped at half of it");
    check(chast::video_budget(2000000, 1900000) == 1000000,
          "the floor wins when the reserve would starve video");

    // The ceiling the controller is built with is the budget, so recovery
    // stops at video + audio rather than at the configured number.
    chast::BitrateController controller(chast::video_budget(10000000, 193600),
                                        chast::video_floor(10000000), 30);
    check(controller.ceiling_bps() == 9806400, "the controller works against the video budget");
    controller.update(healthy_sample(1000), 1000);
    chast::BitrateSample lossy = healthy_sample(1100);
    lossy.lost_packets = 20;
    controller.update(lossy, 1000);
    check(controller.target_bps() == 6864480,
          "the first cut is a share of the budget, not of the configured rate");
}

}  // namespace

int main() {
    test_severe_loss_and_cooldown();
    test_rtt_inflation();
    test_queue_pressure();
    test_recovery_and_ceiling();
    test_floor_and_bounds();
    test_no_congestion_window();
    test_framerate_ladder();
    test_resolution_ladder();
    test_audio_budget();
    if (failures != 0) {
        std::printf("bitrate checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("bitrate checks passed\n");
    return 0;
}
