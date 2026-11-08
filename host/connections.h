#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "connection_control.h"

namespace chast { class HostServer; }
namespace chast::host {
// UI-thread-owned connection manager. A selected ID and its draft/history
// survive list refreshes; server replacement never reuses a connection ID.
class ConnectionsWindow {
public:
    ConnectionsWindow(HWND owner, HINSTANCE instance, HFONT font, int dpi,
                      std::function<HostServer*()> server);
    ConnectionsWindow(HWND owner, HINSTANCE instance, HFONT font, int dpi,
                      ConnectionActions actions);
    ~ConnectionsWindow();
    void show(bool activate = true);
    void receive_chat(uint64_t connection_id, const std::string& text);
    void notify(uint64_t connection_id, const std::string& text);
    bool translate(MSG& message);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
