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

// Applies the inbound rules for the host and, when tunnel_port is non-zero,
// for its sibling chast-tunnel.exe (a separate executable needs its own
// program-scoped rule). The host gets UDP on its QUIC port and TCP on port + 1
// for the weak-network fallback; the tunnel also gets the fixed LAN discovery
// port. Runs netsh; the caller must already be elevated. Returns the netsh exit
// code (0 on success).
int apply_firewall_rule(uint16_t port, uint16_t tunnel_port);

// Deletes every program-scoped rule (host UDP/TCP and tunnel). Used by the
// service uninstaller, which cannot leave a stale allowance behind.
void remove_firewall_rules();

// Relaunches this executable with --firewall <port> [<tunnel_port>] through
// the UAC prompt and waits for it. False when the user cancels or netsh fails.
bool request_firewall_rule(uint16_t port, uint16_t tunnel_port, std::string* error);

void copy_text(HWND owner, const std::wstring& text);
std::wstring executable_path();

}  // namespace chast::host
