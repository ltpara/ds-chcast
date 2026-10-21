#pragma once

// Text clipboard access for the host. Only CF_UNICODETEXT is read and
// written; the wire format is UTF-8 (see chast/protocol.h).

#include <string>

namespace chast {

// Reads the current text clipboard as UTF-8. Returns false when the clipboard
// is held by another process, holds no text, is empty, or exceeds the
// protocol limit. The caller treats false as "no text right now".
bool read_clipboard_text(std::string& text);

// Replaces the text clipboard. Returns false when the clipboard is busy or the
// text is not valid clipboard text. A successful call hands the data to the
// system, so the next read returns the same text.
bool write_clipboard_text(const std::string& text);

}  // namespace chast
