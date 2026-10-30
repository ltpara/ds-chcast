#pragma once

#include <map>
#include <string>

namespace chast::host {

// Picks the host UI language from CHAST_UI_LANGUAGE (tests, portable runs) or,
// when unset, from the viewer's %APPDATA%\chast\ui.json. Called once before any
// window is created; changing the language later needs a restart.
void load_ui_language();

// Chinese is the source language. Returns source unchanged when the active
// language is Chinese or the string has no English rendering, so a missing
// entry never blanks a control.
const wchar_t* tr(const wchar_t* source);

// True when the English table is active. The Win32 layout uses it to reserve
// the extra width English labels need.
bool english_ui();

// English table keyed by Chinese source, exposed for coverage tests.
const std::map<std::wstring, std::wstring>& english_strings();

}  // namespace chast::host
