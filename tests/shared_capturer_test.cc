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

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    run();
    return failures ? 1 : 0;
}
