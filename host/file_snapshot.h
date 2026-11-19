#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace chast::host::detail {

class FileSnapshot {
public:
    DWORD capture(const std::wstring& path);
    DWORD restore() const;
    bool existed() const { return existed_; }

private:
    std::wstring path_;
    std::vector<char> contents_;
    bool captured_ = false;
    bool existed_ = false;
};

}  // namespace chast::host::detail
