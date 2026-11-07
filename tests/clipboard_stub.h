#pragma once

#include "chast/host_server.h"

// Control/login tests must not depend on another application's delayed
// clipboard rendering, or read/write the operator's clipboard.
inline void use_empty_clipboard(chast::HostServerConfig& config) {
    config.clipboard_read = [](chast::ClipboardContent& content) {
        content = {};
        return true;
    };
    config.clipboard_read_files = [](std::vector<std::string>& paths) {
        paths.clear();
        return true;
    };
    config.clipboard_write = [](const chast::ClipboardContent&) { return true; };
    config.clipboard_write_files = [](const std::vector<std::string>&) { return true; };
}
