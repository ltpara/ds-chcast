// Adaptive playout lead: windowed observation, hysteresis, the raise step and
// ceiling, the give-back, and the startup/release grace. The player feeds the
// same slack values it computes from arrival time minus the block's scheduled
// play time; here the sequence is synthetic so every change is deterministic.
// The drift section uses the same approach for the long-window controller that
// corrects the host/local clock offset on a capture-anchored timeline.

#include <cstdint>
#include <cstdio>

#include "chast/audio.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

constexpr int64_t kBlockMs = 20;

// Feeds one block every 20 ms for `ms`, starting at `now`. The first
// observation opens the window, so feeding exactly 500 ms closes it.
void feed(chast::JitterLeadController& controller, int64_t& now, int64_t slack_ms,
          int64_t ms) {
    const int64_t start = now;
    controller.observe(slack_ms, now);
    while (now - start < ms) {
        now += kBlockMs;
        controller.observe(slack_ms, now);
    }
}

// Runs the startup grace with neutral slack. The first window opens at 0 with
// a 1000 ms grace, so the windows opening at 0 and 500 cannot change the lead;
// feeding 1500 ms closes the first adaptable window as well and leaves a fresh
// window open at a known time.
void settle(chast::JitterLeadController& controller, int64_t& now) {
    feed(controller, now, 0, 1500);
}

void test_clean_link_stays_at_zero() {
    chast::JitterLeadController controller;
    int64_t now = 0;
    controller.observe(40, now);
    feed(controller, now, 40, 4000);
    check(controller.lead_ms() == 0, "early arrivals never add lead");
    check(controller.raises() == 0 && controller.decays() == 0,
          "a clean link makes no adjustments");
}

void test_raise_is_step_limited_and_capped() {
    chast::JitterLeadController controller;
    int64_t now = 0;
    settle(controller, now);
    feed(controller, now, -60, 500);
    check(controller.lead_ms() == 40, "one late window raises by the step cap");
    feed(controller, now, -60, 500);
    check(controller.lead_ms() == 80, "the next late window raises again");
    feed(controller, now, -60, 500);
    check(controller.lead_ms() == 120, "the lead reaches the default ceiling");
    feed(controller, now, -60, 500);
    check(controller.lead_ms() == 120, "the lead stops at the ceiling");
    check(controller.raises() == 3, "only real increases are counted");
}

void test_raise_margin_and_custom_ceiling() {
    chast::JitterLeadController::Config config;
    config.max_lead_ms = 60;
    chast::JitterLeadController controller(config);
    int64_t now = 0;
    settle(controller, now);
    feed(controller, now, -25, 500);
    check(controller.lead_ms() == 35,
          "the raise covers the deficit plus the configured margin");
    for (int window = 0; window < 4; ++window) {
        feed(controller, now, -25, 500);
    }
    check(controller.lead_ms() == 60, "a custom ceiling caps the adaptive lead");
}

void test_single_late_block_is_ignored() {
    chast::JitterLeadController controller;
    int64_t now = 0;
    settle(controller, now);
    controller.observe(-60, now);  // one reordered packet opens the window late
    feed(controller, now, 40, 500);
    check(controller.lead_ms() == 0,
          "one late block per window is left to concealment");
}

void test_neutral_zone_holds() {
    chast::JitterLeadController controller;
    int64_t now = 0;
    settle(controller, now);
    feed(controller, now, -10, 500);
    feed(controller, now, 10, 4000);
    check(controller.lead_ms() == 0,
          "arrivals between the raise and decay thresholds leave the lead alone");
}

void test_decay_returns_the_lead() {
    chast::JitterLeadController controller;
    int64_t now = 0;
    settle(controller, now);
    feed(controller, now, -60, 500);
    check(controller.lead_ms() == 40, "the late window raised the lead");
    // The window that closes next still contains the last late block, so it
    // neither raises nor starts the early streak.
    feed(controller, now, 60, 500);
    check(controller.lead_ms() == 40, "the lead is held while the link settles");
    feed(controller, now, 60, 500);
    check(controller.lead_ms() == 40, "one early window only fills the hold");
    feed(controller, now, 60, 500);
    check(controller.lead_ms() == 40,
          "two early windows are required before giving anything back");
    feed(controller, now, 60, 500);
    check(controller.lead_ms() == 20, "the first decay gives back one step");
    feed(controller, now, 60, 500);
    check(controller.lead_ms() == 0, "decay continues to zero");
    feed(controller, now, 60, 500);
    check(controller.lead_ms() == 0 && controller.decays() == 2,
          "the lead never goes below zero");
}

void test_startup_grace_ignores_late_burst() {
    chast::JitterLeadController controller;
    int64_t now = 0;
    feed(controller, now, -60, 1000);
    check(controller.lead_ms() == 0 && controller.raises() == 0,
          "a late burst during the startup grace cannot raise the lead");
    feed(controller, now, -60, 500);
    check(controller.lead_ms() == 40,
          "adaptation resumes once the startup grace has passed");
}

void test_release_clears_lead_but_keeps_counters() {
    chast::JitterLeadController controller;
    int64_t now = 0;
    settle(controller, now);
    feed(controller, now, -60, 500);
    check(controller.lead_ms() == 40, "the lead was raised before the stall");
    controller.release(now);
    check(controller.lead_ms() == 0, "a stall release returns the lead to zero");
    feed(controller, now, 0, 4000);
    check(controller.lead_ms() == 0,
          "a settled link after the stall does not re-learn the lead");
    check(controller.raises() == 1,
          "release keeps the session's raise counter");
    controller.reset();
    check(controller.lead_ms() == 0 && controller.raises() == 0 &&
              controller.decays() == 0,
          "reset clears the lead and the counters");
}

void test_release_grace_suppresses_immediate_raise() {
    chast::JitterLeadController controller;
    int64_t now = 0;
    settle(controller, now);
    feed(controller, now, -60, 500);
    check(controller.lead_ms() == 40, "the lead was raised before the stall");
    controller.release(now);
    feed(controller, now, -60, 500);
    check(controller.lead_ms() == 0,
          "the recovery burst right after a stall cannot re-raise the lead");
    feed(controller, now, -60, 500);
    check(controller.lead_ms() == 40,
          "adaptation resumes after the release grace");
}

// One 5 s drift window with the raw phase ramping from `raw_start` to
// `raw_end`. The controller observes the raw phase minus the offset it has
// already applied (a positive step schedules playback earlier, which lowers
// the observed phase), so corrections show up in the next window.
void drift_window(chast::PlayoutDriftController& controller, int64_t& now,
                  int64_t raw_start, int64_t raw_end) {
    constexpr int64_t kWindowMs = 5000;
    constexpr int64_t kSteps = kWindowMs / kBlockMs;
    for (int64_t step = 0; step <= kSteps; ++step) {
        const int64_t raw =
            raw_start + (raw_end - raw_start) * step / kSteps;
        controller.observe(raw - controller.correction_ms(), now);
        if (step < kSteps) {
            now += kBlockMs;
        }
    }
}

// One 5 s window whose first sample is an outlier and whose remaining samples
// sit at `phase_ms`. `now` must be on a window boundary, and the call leaves
// it on the next one.
void drift_window_with_outlier(chast::PlayoutDriftController& controller,
                               int64_t& now, int64_t outlier_ms,
                               int64_t phase_ms) {
    constexpr int64_t kWindowMs = 5000;
    controller.observe(outlier_ms - controller.correction_ms(), now);
    for (int64_t elapsed = 0; elapsed < kWindowMs; elapsed += kBlockMs) {
        now += kBlockMs;
        controller.observe(phase_ms - controller.correction_ms(), now);
    }
}

// Runs the warmup window and learns the reference from the next one.
void settle_drift(chast::PlayoutDriftController& controller, int64_t& now,
                  int64_t phase_ms) {
    drift_window(controller, now, phase_ms, phase_ms);
    drift_window(controller, now, phase_ms, phase_ms);
}

void test_drift_stable_phase_holds() {
    chast::PlayoutDriftController controller;
    int64_t now = 0;
    settle_drift(controller, now, 40);
    drift_window(controller, now, 40, 40);
    check(controller.correction_ms() == 0 && controller.corrections() == 0,
          "a stable best-case phase never moves the clock offset");
}

void test_drift_dead_band_holds() {
    chast::PlayoutDriftController controller;
    int64_t now = 0;
    settle_drift(controller, now, 40);
    drift_window(controller, now, 55, 55);
    drift_window(controller, now, 25, 25);
    check(controller.correction_ms() == 0 && controller.corrections() == 0,
          "departures inside the dead band leave the offset alone");
}

void test_drift_late_timeline_moves_back() {
    chast::PlayoutDriftController controller;
    int64_t now = 0;
    settle_drift(controller, now, 40);
    drift_window(controller, now, 40, 20);  // its best case is still the start
    check(controller.corrections() == 0,
          "a falling window does not move the timeline by itself");
    drift_window(controller, now, 20, 20);  // one late window arms the step
    check(controller.corrections() == 0,
          "one late window only arms the correction");
    drift_window(controller, now, 20, 20);  // confirms and moves the offset
    check(controller.correction_ms() == -20 && controller.corrections() == 1,
          "a confirmed departure moves the timeline later by one step");
    drift_window(controller, now, 20, 20);  // the applied step restores slack
    check(controller.correction_ms() == -20 && controller.corrections() == 1,
          "the correction stops once the phase is back inside the band");
}

void test_drift_early_timeline_moves_forward() {
    chast::PlayoutDriftController controller;
    int64_t now = 0;
    settle_drift(controller, now, 40);
    drift_window(controller, now, 40, 60);  // its best case is the end
    drift_window(controller, now, 60, 60);  // confirms and moves the offset
    check(controller.correction_ms() == 20 && controller.corrections() == 1,
          "a confirmed early departure schedules playback earlier");
    drift_window(controller, now, 60, 60);
    check(controller.correction_ms() == 20 && controller.corrections() == 1,
          "the early correction stops at the reference");
}

void test_drift_single_outlier_is_ignored() {
    chast::PlayoutDriftController controller;
    int64_t now = 0;
    settle_drift(controller, now, 40);
    drift_window_with_outlier(controller, now, 0, 40);
    drift_window(controller, now, 40, 40);
    check(controller.correction_ms() == 0 && controller.corrections() == 0,
          "one late block per window is left to jitter");
}

void test_drift_startup_burst_does_not_poison_reference() {
    chast::PlayoutDriftController controller;
    int64_t now = 0;
    // The reference window opens with a stalled receiver delivering a burst:
    // the oldest block is far below its slot while the newest one is on time.
    drift_window(controller, now, 40, 40);  // warmup
    drift_window_with_outlier(controller, now, -400, 40);
    drift_window(controller, now, 40, 40);
    drift_window(controller, now, 40, 40);
    check(controller.correction_ms() == 0 && controller.corrections() == 0,
          "a startup burst cannot poison the drift reference");
}

void test_drift_warmup_window_is_not_a_reference() {
    chast::PlayoutDriftController controller;
    int64_t now = 0;
    // The first block anchors at its arrival, so the opening window can carry
    // a phase the steady-state path never sees.
    drift_window(controller, now, 0, 0);
    drift_window(controller, now, 40, 40);  // reference
    drift_window(controller, now, 40, 40);
    check(controller.correction_ms() == 0 && controller.corrections() == 0,
          "the startup anchor window is not a drift reference");
}

void test_drift_step_is_capped() {
    chast::PlayoutDriftController controller;
    int64_t now = 0;
    settle_drift(controller, now, 40);
    drift_window(controller, now, 40, -200);  // its best case is still the start
    drift_window(controller, now, -200, -200);  // arms
    drift_window(controller, now, -200, -200);  // confirms with a capped step
    check(controller.correction_ms() == -20 && controller.corrections() == 1,
          "a huge departure moves at most one capped step per window");
}

void test_drift_reset_relearns_and_clears() {
    chast::PlayoutDriftController controller;
    int64_t now = 0;
    settle_drift(controller, now, 40);
    drift_window(controller, now, 40, 60);  // arms
    drift_window(controller, now, 60, 60);  // confirms
    check(controller.correction_ms() == 20, "the controller learned a step");
    controller.reset();
    check(controller.correction_ms() == 0 && controller.corrections() == 0,
          "reset clears the reference and the accumulated correction");
    drift_window(controller, now, 100, 100);  // warmup
    drift_window(controller, now, 100, 100);  // new reference
    drift_window(controller, now, 100, 100);
    check(controller.corrections() == 0,
          "the rebuilt timeline learns its own reference");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_clean_link_stays_at_zero();
    test_raise_is_step_limited_and_capped();
    test_raise_margin_and_custom_ceiling();
    test_single_late_block_is_ignored();
    test_neutral_zone_holds();
    test_decay_returns_the_lead();
    test_startup_grace_ignores_late_burst();
    test_release_clears_lead_but_keeps_counters();
    test_release_grace_suppresses_immediate_raise();
    test_drift_stable_phase_holds();
    test_drift_dead_band_holds();
    test_drift_late_timeline_moves_back();
    test_drift_early_timeline_moves_forward();
    test_drift_single_outlier_is_ignored();
    test_drift_startup_burst_does_not_poison_reference();
    test_drift_warmup_window_is_not_a_reference();
    test_drift_step_is_capped();
    test_drift_reset_relearns_and_clears();

    if (failures == 0) {
        std::printf("audio playout checks passed\n");
        return 0;
    }
    std::printf("%d audio playout check(s) failed\n", failures);
    return 1;
}
