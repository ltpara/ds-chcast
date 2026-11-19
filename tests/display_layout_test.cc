#include "display_layout.h"

#include <fstream>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool same(const RECT& actual, const RECT& expected) {
    return actual.left == expected.left && actual.top == expected.top &&
           actual.right == expected.right && actual.bottom == expected.bottom;
}

}  // namespace

int main() {
    const RECT remote_desktop{-1920, 0, 1920, 1080};
    const RECT remote_left{-1920, 0, 0, 1080};
    const RECT same_layout{-1920, 0, 1920, 1080};
    const auto identity =
        chast::MapRemoteDisplayBounds(remote_left, remote_desktop, same_layout);
    check(identity && same(*identity, remote_left),
          "matching local topology keeps remote monitor coordinates");

    const RECT local_desktop{0, 0, 2560, 1440};
    const auto scaled =
        chast::MapRemoteDisplayBounds(remote_left, remote_desktop, local_desktop);
    check(scaled && same(*scaled, RECT{0, 360, 1280, 1080}),
          "different desktop sizes preserve monitor proportions and aspect ratio");

    const auto right = chast::MapRemoteDisplayBounds(
        RECT{0, 0, 1920, 1080}, remote_desktop, local_desktop);
    check(right && same(*right, RECT{1280, 360, 2560, 1080}),
          "remote monitor adjacency is preserved in the local layout");

    check(!chast::MapRemoteDisplayBounds(RECT{0, 0, 0, 1080}, remote_desktop,
                                         local_desktop),
          "empty remote monitor bounds are rejected");
    check(!chast::MapRemoteDisplayBounds(RECT{1920, 0, 2000, 1080},
                                         remote_desktop, local_desktop),
          "a monitor outside the announced remote desktop is rejected");

    const std::vector<RECT> local_monitors{
        RECT{0, 0, 1920, 1080}, RECT{1920, 0, 3840, 1080}};
    check(chast::HasUsableDisplayIntersection(RECT{1800, 100, 2300, 600},
                                               local_monitors),
          "a window spanning monitors remains in place with usable area");
    check(!chast::HasUsableDisplayIntersection(RECT{3820, 900, 4320, 1400},
                                                local_monitors),
          "a narrow visible sliver requests relocation after unplug");
    check(!chast::HasUsableDisplayIntersection(RECT{4000, 0, 4500, 600},
                                                local_monitors),
          "a fully offscreen window requests relocation");

    MONITORINFO primary_info{};
    primary_info.cbSize = sizeof(primary_info);
    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTONEAREST);
    if (primary != nullptr && GetMonitorInfoW(primary, &primary_info)) {
        check(chast::DisplayBoundsHaveUsableVisibility(primary_info.rcMonitor),
              "the current primary monitor provides a usable restore area");
        const auto automatic = chast::AutoDisplayBounds(
            primary_info.rcMonitor, primary_info.rcMonitor, 1);
        check(automatic &&
                  chast::DisplayBoundsHaveUsableVisibility(*automatic),
              "single-display placement resolves to a visible local monitor");

        const LONG monitor_width = primary_info.rcMonitor.right -
                                   primary_info.rcMonitor.left;
        const LONG monitor_height = primary_info.rcMonitor.bottom -
                                    primary_info.rcMonitor.top;
        if (monitor_width >= 320 && monitor_height >= 240) {
            const LONG width = std::min<LONG>(640, monitor_width);
            const LONG height = std::min<LONG>(480, monitor_height);
            const auto place = [&](LONG x, LONG y) {
                return RECT{x, y, x + width, y + height};
            };
            const RECT first = place(primary_info.rcMonitor.left,
                                     primary_info.rcMonitor.top);
            const RECT second{
                primary_info.rcMonitor.left, primary_info.rcMonitor.top,
                primary_info.rcMonitor.left + chast::kMinDisplayWindowWidth,
                primary_info.rcMonitor.top + chast::kMinDisplayWindowHeight};
            const RECT third = place(primary_info.rcMonitor.right - width,
                                      primary_info.rcMonitor.bottom - height);

            std::error_code temp_error;
            const auto temp_parent = std::filesystem::temp_directory_path(temp_error);
            check(!temp_error, "the temporary directory is available");
            if (!temp_error) {
                const auto temp_root = temp_parent /
                    ("chast-display-layout-" + std::to_string(GetCurrentProcessId()) +
                     "-" + std::to_string(GetTickCount64()));
                const bool created = std::filesystem::create_directory(temp_root,
                                                                        temp_error);
                check(created && !temp_error,
                      "an isolated display-layout test directory is created");
                if (created && !temp_error) {
                    const auto file = temp_root / "display-windows.tsv";
                    chast::SaveDisplayBounds(file, "peer-a", 0, first);
                    chast::SaveDisplayBounds(file, "peer-a", 1, second);
                    chast::SaveDisplayBounds(file, "peer-b", 0, third);
                    const auto peer_a_first =
                        chast::LoadDisplayBounds(file, "peer-a", 0);
                    const auto peer_a_second =
                        chast::LoadDisplayBounds(file, "peer-a", 1);
                    const auto peer_b_first =
                        chast::LoadDisplayBounds(file, "peer-b", 0);
                    check(peer_a_first && same(*peer_a_first, first),
                          "saved bounds round-trip by peer and display index");
                    check(peer_a_second && same(*peer_a_second, second),
                          "another display index keeps its own saved bounds");
                    check(peer_b_first && same(*peer_b_first, third),
                          "another peer keeps its own saved bounds");
                    check(!chast::LoadDisplayBounds(file, "peer-a", 2),
                          "an unsaved display has no inherited placement");

                    const std::vector<RECT> stale_candidates{
                        RECT{-32000, -32000, -31360, -31520},
                        RECT{32000, 32000, 32640, 32480},
                        RECT{-32000, 32000, -31360, 32480},
                        RECT{32000, -32000, 32640, -31520},
                    };
                    const auto stale = std::find_if(
                        stale_candidates.begin(), stale_candidates.end(),
                        [](const RECT& bounds) {
                            return !chast::DisplayBoundsVisible(bounds);
                        });
                    check(stale != stale_candidates.end(),
                          "an inactive-monitor placement is available for the stale-entry check");
                    if (stale != stale_candidates.end()) {
                        const auto stale_file = temp_root / "stale-display-windows.tsv";
                        std::ofstream output(stale_file,
                                            std::ios::binary | std::ios::trunc);
                        const long long stale_width =
                            static_cast<long long>(stale->right) - stale->left;
                        const long long stale_height =
                            static_cast<long long>(stale->bottom) - stale->top;
                        output << "61\t0\t" << stale->left << '\t' << stale->top
                               << '\t' << stale_width << '\t' << stale_height
                               << '\n';
                        output.close();
                        check(!output.fail(),
                              "a stale saved-placement fixture is written locally");
                        check(!chast::LoadDisplayBounds(stale_file, "a", 0),
                              "a saved placement outside the active monitor topology is ignored");
                    }

                    if (!same(first, third)) {
                        chast::SaveDisplayBounds(file, "peer-a", 0, third);
                        const auto updated =
                            chast::LoadDisplayBounds(file, "peer-a", 0);
                        const auto unaffected =
                            chast::LoadDisplayBounds(file, "peer-a", 1);
                        check(updated && same(*updated, third),
                              "saving a placement replaces only its matching key");
                        check(unaffected && same(*unaffected, second),
                              "updating one placement preserves sibling entries");
                    }

                    std::error_code cleanup_error;
                    std::filesystem::remove_all(temp_root, cleanup_error);
                    check(!cleanup_error,
                          "the isolated display-layout test directory is removed");
                }
            }
        }
    } else {
        std::cout << "live monitor checks skipped\n";
    }

    if (failures != 0) return 1;
    std::cout << "display layout geometry checks passed\n";
    return 0;
}
