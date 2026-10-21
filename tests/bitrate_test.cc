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
    chast::BitrateController controller(10000000, 1000000);
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
    chast::BitrateController controller(10000000, 1000000);
    controller.update(healthy_sample(1000), 1000);

    chast::BitrateSample inflated = healthy_sample(1100);
    inflated.rtt_us = 50000;
    chast::BitrateDecision decision = controller.update(inflated, 1000);
    check(decision.changed && decision.target_bps == 8500000 &&
              std::string(decision.reason) == "rtt",
          "RTT inflation drops the target by 15%");
}

void test_queue_pressure() {
    chast::BitrateController controller(10000000, 1000000);
    controller.update(healthy_sample(1000), 1000);

    chast::BitrateSample backed_up = healthy_sample(1100);
    backed_up.queued_bytes = 700000;  // 70% of the congestion window
    chast::BitrateDecision decision = controller.update(backed_up, 1000);
    check(decision.changed && decision.target_bps == 8500000 &&
              std::string(decision.reason) == "queue",
          "a filling send queue drops the target");

    chast::BitrateController severe(10000000, 1000000);
    severe.update(healthy_sample(1000), 1000);
    backed_up.queued_bytes = 1200000;
    decision = severe.update(backed_up, 1000);
    check(decision.changed && decision.target_bps == 7000000,
          "a queue past the congestion window drops the target by 30%");
}

void test_recovery_and_ceiling() {
    chast::BitrateController controller(10000000, 1000000);
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
    chast::BitrateController controller(2000000, 1500000);
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

    chast::BitrateController inverted(1000000, 5000000);
    check(inverted.ceiling_bps() == 1000000 && inverted.floor_bps() == 1000000 &&
              inverted.target_bps() == 1000000,
          "an inverted floor/ceiling pair collapses to the ceiling");

    chast::BitrateController empty(0, 0);
    check(empty.target_bps() == 1 && empty.floor_bps() == 1,
          "a zero-bitrate configuration is clamped to a usable value");
}

void test_no_congestion_window() {
    chast::BitrateController controller(10000000, 1000000);
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

}  // namespace

int main() {
    test_severe_loss_and_cooldown();
    test_rtt_inflation();
    test_queue_pressure();
    test_recovery_and_ceiling();
    test_floor_and_bounds();
    test_no_congestion_window();
    if (failures != 0) {
        std::printf("bitrate checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("bitrate checks passed\n");
    return 0;
}
