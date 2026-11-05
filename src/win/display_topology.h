#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include "chast/virtual_display.h"

namespace chast::display_topology {
struct Snapshot {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};
bool query(Snapshot& snapshot, UINT flags, std::string* error);
// Pure construction and validation, also used by tests with synthetic CCD
// paths. No GDI/CPU capture and no registry edits are involved.
bool isolate(const Snapshot& before, size_t path_index, const VirtualDisplayMode& mode,
             Snapshot& after, std::string* error);
bool valid(const Snapshot& snapshot);
}  // namespace chast::display_topology
