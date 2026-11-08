#pragma once

#include <windows.h>
#include <imm.h>
#include <functional>
#include <string>

namespace chast_viewer {

// Owns an IMM context only while the remote canvas accepts local text. The
// previous Flutter context is restored for chat, terminal and local controls.
class LocalTextInput {
public:
    LocalTextInput(HWND view, std::function<bool(const std::string&)> send,
                   std::function<void(bool)> composing);
    ~LocalTextInput();
    bool Enable(bool enabled);
    void Cancel();
    bool Handle(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result);
    // The same commit path is used for GCS_RESULTSTR, WM_CHAR and WM_UNICHAR.
    bool Commit(const std::wstring& text);
    bool enabled() const { return enabled_; }
    bool composing() const { return composing_; }

private:
    void Position();
    HWND view_;
    HIMC context_ = nullptr;
    HIMC previous_ = nullptr;
    bool enabled_ = false;
    bool composing_ = false;
    bool cancelling_ = false;
    bool injected_key_ = false;
    wchar_t surrogate_ = 0;
    std::function<bool(const std::string&)> send_;
    std::function<void(bool)> composition_changed_;
};

}  // namespace chast_viewer
