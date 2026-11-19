#pragma once

// Windows integration for the tray host: the registry autostart entry, the
// firewall rule (which needs elevation) and small text helpers. Kept out of
// main.cc so the Win32 UI code stays about the UI.

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace chast::host {

std::wstring widen(const std::string& text);
std::string narrow(const std::wstring& text);

// Starts a hidden child command and waits up to timeout_ms. On timeout it
// attempts to terminate and reap the child; false reports the Win32 failure.
// When requested, the caller owns the returned process handle and must close it.
bool run_hidden_command(const std::wstring& command, DWORD timeout_ms, DWORD* exit_code,
                       HANDLE* process_handle = nullptr);

// True only when a firewall rule's application path matches the expected
// product executable, ignoring Windows path case.
bool firewall_rule_application_matches(const std::wstring& actual,
                                       const std::wstring& expected,
                                       const std::wstring& alternate = {});

// Adds or removes the current executable from
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run. The value launches the
// host with --tray, so autostart does not open a window at login.
bool set_autostart(bool enabled, std::string* error);

// Holds the pre-update firewall snapshot until commit. Destroying an active
// update attempts rollback, allowing a larger install transaction to include
// the firewall stage.
class FirewallRuleUpdate {
public:
    FirewallRuleUpdate();
    FirewallRuleUpdate(const FirewallRuleUpdate&) = delete;
    FirewallRuleUpdate& operator=(const FirewallRuleUpdate&) = delete;
    FirewallRuleUpdate(FirewallRuleUpdate&&) noexcept;
    FirewallRuleUpdate& operator=(FirewallRuleUpdate&&) noexcept;
    ~FirewallRuleUpdate();

    int apply(uint16_t port, uint16_t tunnel_port, int* rollback_error = nullptr);
    int clear(int* rollback_error = nullptr);
    int rollback();
    void commit();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Applies the inbound rules for the host and, when tunnel_port is non-zero,
// for its sibling chast-tunnel.exe (a separate executable needs its own
// program-scoped rule). The host gets UDP on its QUIC port and TCP on port + 1
// for the weak-network fallback; the tunnel also gets the fixed LAN discovery
// port. The caller must already be elevated. Updates all four managed rules as
// one rollbackable operation. rollback_error receives a nonzero code if undo
// itself failed.
int apply_firewall_rule(uint16_t port, uint16_t tunnel_port,
                        int* rollback_error = nullptr);

// Deletes fixed host/tunnel rule names only when their application path belongs
// to the current product binaries. The managed set is restored on failure.
// rollback_error receives a nonzero code if undo itself failed.
int remove_firewall_rules(int* rollback_error = nullptr);

// Relaunches this executable elevated with the given arguments and waits for
// it. Returns the child exit code, or -1 when the user cancels or the process
// cannot be started; *cancelled distinguishes a UAC refusal.
int run_elevated(const std::wstring& arguments, DWORD timeout_ms, bool* cancelled,
                 std::string* error);

// Relaunches this executable with --firewall <port> [<tunnel_port>] through
// the UAC prompt and waits for it. False when the user cancels or netsh fails.
bool request_firewall_rule(uint16_t port, uint16_t tunnel_port, std::string* error);

void copy_text(HWND owner, const std::wstring& text);
std::wstring executable_path();

}  // namespace chast::host
