#pragma once

// Windows integration for the tray host: the registry autostart entry, the
// firewall rule (which needs elevation) and small text helpers. Kept out of
// main.cc so the Win32 UI code stays about the UI.

#include <windows.h>

#include <cstdint>
#include <string>

namespace chast::host {

std::wstring widen(const std::string& text);
std::string narrow(const std::wstring& text);

// Adds or removes the current executable from
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run. The value launches the
// host with --tray, so autostart does not open a window at login.
bool set_autostart(bool enabled, std::string* error);

// Applies the inbound UDP rule for this executable. Runs netsh; the caller
// must already be elevated. Returns the netsh exit code (0 on success).
int apply_firewall_rule(uint16_t port);

// Relaunches this executable with --firewall <port> through the UAC prompt and
// waits for it. False when the user cancels or netsh fails.
bool request_firewall_rule(uint16_t port, std::string* error);

void copy_text(HWND owner, const std::wstring& text);
std::wstring executable_path();

}  // namespace chast::host
