// Live check of the shared desktop duplication: several readers plus the
// merged compositor duplicate one output once, yet each reader owns its GPU
// textures, NV12 scaler and resolution. Needs an interactive desktop, so it
// reports a skip when no output can be duplicated.

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "chast/capture.h"

using namespace std::chrono_literals;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

void skip(const char* what) {
    std::printf("skip %s\n", what);
}

bool until(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool frame(chast::IDxgiCapturer& capture) {
    return until([&] { return capture.capture(30ms) == chast::CaptureResult::Frame; });
}

void run() {
    const std::vector<chast::DisplayInfo> displays = chast::enumerate_displays();
    if (displays.empty()) {
        skip("shared capture needs an attached output");
        return;
    }
    chast::CaptureConfig config;
    config.nv12.enabled = true;
    std::string error;
    std::unique_ptr<chast::IDxgiCapturer> a =
        chast::create_shared_capturer(displays[0], config, &error);
    std::unique_ptr<chast::IDxgiCapturer> b =
        chast::create_shared_capturer(displays[0], config, &error);
    std::unique_ptr<chast::IDxgiCapturer> merged =
        chast::create_merged_capturer({displays[0]}, config, &error);
    if (!a || !b || !merged) {
        const std::string reason = "shared capture unavailable: " + error;
        skip(reason.c_str());
        return;
    }
    check(frame(*a) && frame(*b) && frame(*merged),
          "individual and merged captures share one GPU duplication");
    const UINT width = a->frame().nv12_width;
    const UINT height = a->frame().nv12_height;
    check(width > 0 && height > 0 && a->frame().texture != b->frame().texture &&
              a->frame().nv12_texture != b->frame().nv12_texture,
          "viewer encoders cannot overwrite another viewer's frame surfaces");
    check(b->set_nv12_scale(50).ok() && frame(*b) && b->frame().nv12_width < width &&
              b->frame().nv12_height < height && a->frame().nv12_width == width,
          "one viewer changes GPU scale without changing another viewer's resolution");
    check(a->reinitialize().ok() && b->capture(0ms) == chast::CaptureResult::ReinitNeeded,
          "readers detect a shared output rebuild");
    a.reset();
    check(b->reinitialize().ok() && frame(*b),
          "a reader survives another reader closing after a rebuild");
    merged.reset();
    check(b->set_nv12_scale(75).ok() && frame(*b),
          "closing the merged capture leaves the individual GPU reader usable");
}

// A04: the CPU-only backend must publish pixels without a D3D device, and two
// readers on it still own their scale. Runs after run() so the GPU-keyed source
// is gone and the backend policy check does not reject the new source.
void cpu_readers() {
    const std::vector<chast::DisplayInfo> displays = chast::enumerate_displays();
    if (displays.empty()) {
        skip("CPU shared capture needs an attached output");
        return;
    }
    chast::CaptureConfig config;
    config.backend = chast::CaptureBackend::GdiCpu;
    config.nv12.enabled = true;
    std::string error;
    std::unique_ptr<chast::IDxgiCapturer> a =
        chast::create_shared_capturer(displays[0], config, &error);
    std::unique_ptr<chast::IDxgiCapturer> b =
        chast::create_shared_capturer(displays[0], config, &error);
    if (!a || !b) {
        const std::string reason = "CPU shared capture unavailable: " + error;
        skip(reason.c_str());
        return;
    }
    check(frame(*a) && frame(*b), "CPU-only readers produce frames");
    check(a->frame().cpu_bgra && !a->frame().nv12_texture &&
              !a->adapter_device().device && !a->frame().device,
          "CPU-only frames carry no GPU texture or device");
    check(std::string(a->implementation()) == "gdi-cpu",
          "the CPU-only reader reports the gdi-cpu backend");
    const UINT width = a->frame().nv12_width;
    check(b->set_nv12_scale(50).ok() && frame(*b) && width > 0 &&
              b->frame().nv12_width < width && a->frame().nv12_width == width,
          "a CPU-only reader scales without changing another reader's image");
}

// Merged capture runs child readers on the compositor's GPU device. A CPU-only
// child must still cross the shared-reader boundary as immutable BGRA so the
// compositor can upload it at that child's layout offset.
void merged_cpu_reader() {
    const std::vector<chast::DisplayInfo> displays = chast::enumerate_displays();
    if (displays.empty()) {
        skip("merged CPU capture needs an attached output");
        return;
    }
    chast::CaptureConfig config;
    config.backend = chast::CaptureBackend::GdiCpu;
    config.nv12.enabled = true;
    config.capture_cursor = false;
    std::string error;
    std::unique_ptr<chast::IDxgiCapturer> merged =
        chast::create_merged_capturer({displays[0]}, config, &error);
    if (!merged) {
        const std::string reason = "merged CPU capture unavailable: " + error;
        skip(reason.c_str());
        return;
    }
    check(frame(*merged), "merged capture consumes an external CPU child frame");
    check(merged->frame().texture && merged->frame().nv12_texture &&
              !merged->frame().cpu_bgra,
          "the merged canvas uploads CPU child pixels before NV12 conversion");
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    run();
    cpu_readers();
    merged_cpu_reader();
    return failures ? 1 : 0;
}
