// Lists the outputs DXGI can duplicate, including the color state that decides
// whether frames compose in FP16.
//
//   chast-dump [--json]

#include <cstdio>
#include <string>

#include "chast/capture.h"

namespace {

const char* yes_no(bool value) { return value ? "yes" : "no"; }

void print_text(const std::vector<chast::DisplayInfo>& displays) {
    std::printf("duplication supported: %s\n", chast::duplication_supported() ? "yes" : "no");
    std::printf("displays: %zu\n\n", displays.size());
    for (size_t i = 0; i < displays.size(); ++i) {
        const chast::DisplayInfo& d = displays[i];
        std::printf("[%zu] %ls%s\n", i, d.device_name.c_str(), d.primary ? " (primary)" : "");
        std::printf("    adapter       : %ls (LUID %08lX:%08lX, vendor %04X)\n",
                    d.adapter_name.c_str(),
                    static_cast<unsigned long>(d.adapter_luid.HighPart),
                    static_cast<unsigned long>(d.adapter_luid.LowPart), d.adapter_vendor_id);
        std::printf("    monitor       : %p\n", static_cast<void*>(d.monitor));
        std::printf("    desktop rect  : %ld,%ld - %ld,%ld\n", d.rect.left, d.rect.top,
                    d.rect.right, d.rect.bottom);
        std::printf("    frame size    : %ux%u (rotation %u)\n", d.width, d.height,
                    d.rotation_degrees);
        std::printf("    hdr           : %s\n", yes_no(d.hdr));
        std::printf("    advanced SDR  : %s\n", yes_no(d.advanced_color_sdr));
        std::printf("    sdr white     : %u (%s)\n", d.sdr_white_level,
                    d.sdr_white_level ? "displayconfig units" : "not reported");
        std::printf("    peak luminance: %.1f nits\n", d.max_luminance_nits);
        std::printf("    fp16 compose  : %s\n", yes_no(d.fp16_composition()));
        std::printf("\n");
    }
}

void print_json(const std::vector<chast::DisplayInfo>& displays) {
    std::printf("{\"duplication_supported\":%s,\"displays\":[",
                chast::duplication_supported() ? "true" : "false");
    for (size_t i = 0; i < displays.size(); ++i) {
        const chast::DisplayInfo& d = displays[i];
        std::printf(
            "%s{\"index\":%zu,\"device\":\"%ls\",\"adapter\":\"%ls\",\"left\":%ld,\"top\":%ld,"
            "\"width\":%u,\"height\":%u,\"rotation\":%u,\"primary\":%s,\"hdr\":%s,"
            "\"advanced_color_sdr\":%s,\"sdr_white_level\":%u,\"max_luminance_nits\":%.1f}",
            i ? "," : "", i, d.device_name.c_str(), d.adapter_name.c_str(), d.rect.left,
            d.rect.top, d.width, d.height, d.rotation_degrees, d.primary ? "true" : "false",
            d.hdr ? "true" : "false", d.advanced_color_sdr ? "true" : "false",
            d.sdr_white_level, d.max_luminance_nits);
    }
    std::printf("]}\n");
}

}  // namespace

int main(int argc, char** argv) {
    const bool json = argc > 1 && std::string(argv[1]) == "--json";
    const std::vector<chast::DisplayInfo> displays = chast::enumerate_displays();
    if (json) {
        print_json(displays);
    } else {
        print_text(displays);
    }
    return displays.empty() ? 1 : 0;
}
