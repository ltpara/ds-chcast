#pragma once

// Minimal append-only log for the headless service and its session worker.
// The tray shows a window; these two have no one to talk to, so every event
// goes to %ProgramData%\chast with a timestamp and a size cap.

#include <string>

namespace chast::host {

// %ProgramData%\chast\<name>. Empty when ProgramData cannot be resolved.
std::string service_log_path(const char* name);

// Appends one UTF-8 line. The file is rotated to "<path>.old" when it grows
// past 5 MB. Thread-safe and best-effort: logging never fails the caller.
void log_line(const std::string& path, const std::string& message);

}  // namespace chast::host
