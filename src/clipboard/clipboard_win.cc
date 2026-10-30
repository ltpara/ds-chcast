#define NOMINMAX

#include "chast/clipboard.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>

#include "chast/protocol.h"

namespace chast {

namespace {

constexpr int kOpenAttempts = 10;
constexpr DWORD kOpenRetryMs = 20;

bool open_clipboard() {
    for (int attempt = 0; attempt < kOpenAttempts; ++attempt) {
        if (OpenClipboard(nullptr)) {
            return true;
        }
        Sleep(kOpenRetryMs);
    }
    return false;
}

UINT html_format() {
    static const UINT format = RegisterClipboardFormatW(L"HTML Format");
    return format;
}

UINT rtf_format() {
    static const UINT format = RegisterClipboardFormatW(L"Rich Text Format");
    return format;
}

bool wide_to_utf8(const wchar_t* data, size_t length, std::string& text) {
    if (length == 0 || length > static_cast<size_t>(INT_MAX)) {
        return false;
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data,
                                         static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return false;
    }
    text.resize(static_cast<size_t>(size));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data,
                               static_cast<int>(length), text.data(), size, nullptr,
                               nullptr) == size;
}

// Copies a clipboard handle into `out`. Registered text formats may carry a
// trailing NUL that is not part of the document; CF_DIB must keep every byte,
// because pixel rows can legitimately end in zeros.
bool read_bytes(UINT format, size_t limit, bool strip_trailing_nul,
                std::vector<uint8_t>& out) {
    const HANDLE handle = GetClipboardData(format);
    if (!handle) {
        return false;
    }
    const SIZE_T bytes = GlobalSize(handle);
    if (bytes == 0 || bytes > limit) {
        return false;
    }
    const auto* data = static_cast<const uint8_t*>(GlobalLock(handle));
    if (!data) {
        return false;
    }
    size_t length = bytes;
    if (strip_trailing_nul) {
        while (length > 0 && data[length - 1] == 0) {
            --length;
        }
    }
    out.assign(data, data + length);
    GlobalUnlock(handle);
    return !out.empty();
}

bool read_text(std::vector<uint8_t>& out) {
    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
        return false;
    }
    const SIZE_T bytes = GlobalSize(handle);
    const size_t capacity = bytes / sizeof(wchar_t);
    if (capacity == 0) {
        return false;
    }
    // Every UTF-16 unit turns into at most three UTF-8 bytes, so a UTF-16
    // unit count past the byte cap cannot fit; skip the conversion.
    if (capacity > kMaxClipboardTextBytes) {
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
    std::string text;
    const bool converted = length > 0 && wide_to_utf8(data, length, text);
    GlobalUnlock(handle);
    // accepts() re-validates against the stream limits; the legacy control
    // message's 256 KiB text cap does not apply to a rich item.
    if (!converted) {
        return false;
    }
    out.assign(text.begin(), text.end());
    return true;
}

bool accepts(ClipboardContent& content, ClipboardFormat format,
             std::vector<uint8_t> data) {
    ClipboardContent single;
    ClipboardFormatData entry;
    entry.format = format;
    entry.data = std::move(data);
    single.formats.push_back(std::move(entry));
    if (!valid_clipboard_content(single, nullptr)) {
        return false;
    }
    content.formats.push_back(std::move(single.formats[0]));
    return true;
}

UINT win32_format(ClipboardFormat format) {
    switch (format) {
        case ClipboardFormat::Text:
            return CF_UNICODETEXT;
        case ClipboardFormat::Html:
            return html_format();
        case ClipboardFormat::Rtf:
            return rtf_format();
        case ClipboardFormat::Dib:
            return CF_DIB;
    }
    return 0;
}

bool set_global(UINT format, const std::vector<uint8_t>& data, bool terminate) {
    if (format == 0) {
        return false;
    }
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, data.size() + (terminate ? 1 : 0));
    if (!handle) {
        return false;
    }
    auto* target = static_cast<uint8_t*>(GlobalLock(handle));
    if (!target) {
        GlobalFree(handle);
        return false;
    }
    if (!data.empty()) {
        std::memcpy(target, data.data(), data.size());
    }
    if (terminate) {
        target[data.size()] = 0;
    }
    GlobalUnlock(handle);
    if (!SetClipboardData(format, handle)) {
        GlobalFree(handle);
        return false;
    }
    return true;
}

bool set_text(const std::vector<uint8_t>& data) {
    const std::string text(data.begin(), data.end());
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
    auto* target = static_cast<wchar_t*>(GlobalLock(handle));
    if (!target ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), target, length) != length) {
        GlobalUnlock(handle);
        GlobalFree(handle);
        return false;
    }
    target[length] = L'\0';
    GlobalUnlock(handle);
    if (!SetClipboardData(CF_UNICODETEXT, handle)) {
        GlobalFree(handle);
        return false;
    }
    return true;
}

}  // namespace

bool read_clipboard(ClipboardContent& content, std::string* error) {
    content = ClipboardContent{};
    if (!open_clipboard()) {
        if (error) {
            *error = "the clipboard is held by another process";
        }
        return false;
    }
    struct Closer {
        ~Closer() { CloseClipboard(); }
    } closer;

    std::vector<uint8_t> data;
    if (read_text(data)) {
        accepts(content, ClipboardFormat::Text, std::move(data));
    }
    if (read_bytes(html_format(), kMaxClipboardMarkupBytes, true, data)) {
        accepts(content, ClipboardFormat::Html, std::move(data));
    }
    if (read_bytes(rtf_format(), kMaxClipboardMarkupBytes, true, data)) {
        accepts(content, ClipboardFormat::Rtf, std::move(data));
    }
    if (read_bytes(CF_DIB, kMaxClipboardImageBytes, false, data)) {
        accepts(content, ClipboardFormat::Dib, std::move(data));
    }
    if (content.formats.empty()) {
        if (error) {
            *error = "the clipboard holds no supported format";
        }
        return false;
    }
    if (!valid_clipboard_content(content, error)) {
        content = ClipboardContent{};
        return false;
    }
    content.hash = clipboard_content_hash(content);
    return true;
}

bool write_clipboard(const ClipboardContent& content, std::string* error) {
    if (!valid_clipboard_content(content, error)) {
        return false;
    }
    if (!open_clipboard()) {
        if (error) {
            *error = "the clipboard is held by another process";
        }
        return false;
    }
    bool any = false;
    if (EmptyClipboard()) {
        // Text goes in last: a paste target that knows none of the richer
        // formats should still find the plain fallback.
        for (auto it = content.formats.rbegin(); it != content.formats.rend(); ++it) {
            if (it->format == ClipboardFormat::Text) {
                any = set_text(it->data) || any;
            } else {
                any = set_global(win32_format(it->format), it->data, false) || any;
            }
        }
    }
    CloseClipboard();
    if (!any && error) {
        *error = "the clipboard refused every format";
    }
    return any;
}

bool read_clipboard_files(std::vector<std::string>& paths, std::string* error) {
    paths.clear();
    if (!open_clipboard()) {
        if (error) {
            *error = "the clipboard is held by another process";
        }
        return false;
    }
    struct Closer {
        ~Closer() { CloseClipboard(); }
    } closer;
    const HANDLE handle = GetClipboardData(CF_HDROP);
    if (!handle) {
        if (error) {
            *error = "the clipboard holds no files";
        }
        return false;
    }
    const HDROP drop = static_cast<HDROP>(handle);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
    if (count == 0) {
        if (error) {
            *error = "the clipboard file list is empty";
        }
        return false;
    }
    if (count > kMaxClipboardFileEntries) {
        if (error) {
            *error = "the clipboard holds more files than a copy can carry";
        }
        return false;
    }
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) {
            continue;
        }
        std::wstring wide(static_cast<size_t>(length) + 1, L'\0');
        if (DragQueryFileW(drop, index, wide.data(), length + 1) != length) {
            continue;
        }
        if ((GetFileAttributesW(wide.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        std::string utf8;
        if (!wide_to_utf8(wide.data(), length, utf8)) {
            continue;
        }
        paths.push_back(std::move(utf8));
    }
    if (paths.empty()) {
        if (error) {
            *error = "the clipboard holds no files or folders";
        }
        return false;
    }
    return true;
}

bool write_clipboard_files(const std::vector<std::string>& paths, std::string* error) {
    if (paths.empty() || paths.size() > kMaxClipboardFileEntries) {
        if (error) {
            *error = "the file list cannot be put on the clipboard";
        }
        return false;
    }
    std::wstring list;
    for (const std::string& path : paths) {
        if (path.empty()) {
            continue;
        }
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                               static_cast<int>(path.size()), nullptr, 0);
        if (length <= 0) {
            if (error) {
                *error = "a clipboard file path is not valid UTF-8";
            }
            return false;
        }
        std::wstring wide(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                            static_cast<int>(path.size()), wide.data(), length);
        list.append(wide);
        list.push_back(L'\0');
    }
    if (list.empty()) {
        if (error) {
            *error = "the file list is empty";
        }
        return false;
    }
    // DROPFILES carries the path list after its own header; the list itself
    // ends with a second NUL.
    list.push_back(L'\0');
    const SIZE_T bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle) {
        if (error) {
            *error = "the clipboard file list could not be allocated";
        }
        return false;
    }
    auto* drop = static_cast<DROPFILES*>(GlobalLock(handle));
    if (!drop) {
        GlobalFree(handle);
        if (error) {
            *error = "the clipboard file list could not be locked";
        }
        return false;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    std::memcpy(reinterpret_cast<uint8_t*>(drop) + sizeof(DROPFILES), list.data(),
                list.size() * sizeof(wchar_t));
    GlobalUnlock(handle);
    if (!open_clipboard()) {
        GlobalFree(handle);
        if (error) {
            *error = "the clipboard is held by another process";
        }
        return false;
    }
    bool written = EmptyClipboard() && SetClipboardData(CF_HDROP, handle) != nullptr;
    if (!written) {
        GlobalFree(handle);
    }
    CloseClipboard();
    if (!written && error) {
        *error = "the clipboard refused the file list";
    }
    return written;
}

bool read_clipboard_text(std::string& text) {
    text.clear();
    ClipboardContent content;
    if (!read_clipboard(content, nullptr)) {
        return false;
    }
    text = content.text();
    return !text.empty();
}

bool write_clipboard_text(const std::string& text) {
    if (!valid_clipboard_text(text)) {
        return false;
    }
    ClipboardContent content;
    ClipboardFormatData entry;
    entry.format = ClipboardFormat::Text;
    entry.data.assign(text.begin(), text.end());
    content.formats.push_back(std::move(entry));
    return write_clipboard(content, nullptr);
}

}  // namespace chast
