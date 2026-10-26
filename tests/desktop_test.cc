// Desktop tracker checks against the interactive desktop. A secure desktop
// needs a locked workstation or a real UAC prompt and SYSTEM rights, so those
// paths stay manual.

#include <windows.h>

#include <cstdio>
#include <string>

#include "chast/desktop.h"

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int failures = 0;
    const auto check = [&](bool condition, const char* what) {
        std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
        if (!condition) {
            ++failures;
        }
    };

    const chast::InputDesktopStatus status = chast::input_desktop_status();
    check(status.kind == chast::InputDesktopKind::Default,
          "an interactive session reports the Default desktop");
    check(!status.locked, "an interactive test session is not locked");
    check(status.error.empty(), "a reachable desktop reports no error");
    check(!status.name.empty(), "the desktop name is read back");
    check(_stricmp(status.name.c_str(), "Default") == 0,
          "the reported name is the Default desktop");

    check(chast::on_input_desktop(), "the test thread is attached to the input desktop");

    std::string error;
    bool switched = true;
    check(chast::follow_input_desktop(&error, &switched),
          "following an already-current desktop succeeds");
    check(!switched, "an already-current desktop is not reported as a switch");
    check(error.empty(), "a no-op follow reports no error");

    if (failures != 0) {
        std::printf("desktop checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("desktop checks passed\n");
    return 0;
}
