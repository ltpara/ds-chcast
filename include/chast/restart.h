#pragma once

// Remote restart support. Scheduling the reboot is one Win32 call after the
// process enables SeShutdownPrivilege, which a LocalSystem service holds and
// an interactive user normally holds too. The shutdown is delayed so the
// RestartResult reaches the viewer first and forced so an unsaved-dialog
// prompt cannot silently cancel it.

#include <string>

namespace chast {

// Schedules a restart of this machine. Returns true when Windows accepted the
// request, not when the machine has rebooted; false fills `error`.
bool schedule_restart(std::string* error);

}  // namespace chast
