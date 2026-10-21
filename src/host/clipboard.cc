#define NOMINMAX

#include "chast/clipboard.h"

#include <windows.h>

#include <climits>
#include <string>

#include "chast/protocol.h"

namespace chast {

namespace {

// Another process may hold the clipboard for a few milliseconds while it
// copies; retrying briefly turns most of those into a successful access.
bool open_clipboard() {
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(nullptr)) {
            return true;
        }
        Sleep(10);
    }
    return false;
}

bool wide_to_utf8(const wchar_t* data, size_t length, std::string& text) {
    if (length == 0 || length > static_cast<size_t>(INT_MAX)) {
        return false;
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data,
                                         static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (size <= 0 || static_cast<size_t>(size) > kMaxClipboardBytes) {
        return false;
    }
    text.resize(static_cast<size_t>(size));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data,
                               static_cast<int>(length), text.data(), size, nullptr,
                               nullptr) == size;
}

}  // namespace

bool read_clipboard_text(std::string& text) {
    text.clear();
    if (!open_clipboard()) {
        return false;
    }
    struct Closer {
        ~Closer() { CloseClipboard(); }
    } closer;

    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
        return false;
    }
    const SIZE_T bytes = GlobalSize(handle);
    const size_t capacity = bytes / sizeof(wchar_t);
    if (capacity == 0) {
        return false;
    }
    const auto* data = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!data) {
        return false;
    }
    size_t length = 0;
    while (length < capacity && data[length] != L'\0') {
        ++length;
    }
    const bool converted = length > 0 && wide_to_utf8(data, length, text);
    GlobalUnlock(handle);
    if (!converted || !valid_clipboard_text(text)) {
        text.clear();
        return false;
    }
    return true;
}

bool write_clipboard_text(const std::string& text) {
    if (!valid_clipboard_text(text) || text.size() > static_cast<size_t>(INT_MAX)) {
        return false;
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return false;
    }
    const size_t bytes = (static_cast<size_t>(length) + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle) {
        return false;
    }
    auto* data = static_cast<wchar_t*>(GlobalLock(handle));
    if (!data ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), data, length) != length) {
        GlobalUnlock(handle);
        GlobalFree(handle);
        return false;
    }
    data[length] = L'\0';
    GlobalUnlock(handle);

    if (!open_clipboard()) {
        GlobalFree(handle);
        return false;
    }
    bool written = EmptyClipboard() != FALSE;
    if (written) {
        // On success the system owns the memory; freeing it here would corrupt
        // the clipboard.
        written = SetClipboardData(CF_UNICODETEXT, handle) != nullptr;
    }
    CloseClipboard();
    if (!written) {
        GlobalFree(handle);
    }
    return written;
}

}  // namespace chast
