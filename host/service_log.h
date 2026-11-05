#pragma once

// Minimal append-only log for the headless service and its session worker.
// The tray shows a window; these two have no one to talk to, so every event
// goes to %ProgramData%\chast as one JSON object per line with a timestamp and
// a size cap. JSONL keeps the file machine-consumable without a second parser,
// and the escaping in log_event() makes an injected newline or quote inert.

#include <initializer_list>
#include <string>
#include <string_view>

namespace chast::host {

struct LogField {
    std::string_view key;
    std::string_view value;
};

// %ProgramData%\chast\<name>. Empty when ProgramData cannot be resolved.
std::string service_log_path(const char* name);

// Appends one JSON object followed by '\n':
//   {"ts":"2026-11-06T01:02:03.456Z","event":"worker.start",
//    "fields":{"pid":"1234","session":"2"}}
// `ts` is UTC with milliseconds. Values are always JSON strings; ASCII control
// characters and quotes are escaped and invalid UTF-8 is replaced with U+FFFD,
// so a reader never sees a broken line. Thread-safe and best-effort: logging
// never fails the caller. The file is rotated to "<path>.old" at 5 MB.
void log_event(const std::string& path, std::string_view event,
               std::initializer_list<LogField> fields = {});

// Free-text diagnostic line, recorded as event "message" with a "text" field.
void log_line(const std::string& path, const std::string& message);

}  // namespace chast::host
