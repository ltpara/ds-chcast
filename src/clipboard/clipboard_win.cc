#define NOMINMAX

#include "chast/clipboard.h"
#include "chast/clipboard_virtual.h"
#include "chast/png.h"

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

UINT png_format() {
    static const UINT format = RegisterClipboardFormatW(L"PNG");
    return format;
}

UINT png_mime_format() {
    static const UINT format = RegisterClipboardFormatW(L"image/png");
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
    const bool converted = length == 0 || wide_to_utf8(data, length, text);
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
        case ClipboardFormat::Png:
            return png_format();
        case ClipboardFormat::FileList:
            return 0;  // travels through read/write_clipboard_files
    }
    return 0;
}

std::vector<UINT> win32_formats(ClipboardFormat format) {
    if (format == ClipboardFormat::Png) {
        return {png_format(), png_mime_format()};
    }
    const UINT value = win32_format(format);
    return value == 0 ? std::vector<UINT>{} : std::vector<UINT>{value};
}

// Builds the handle a format is published with, without touching the
// clipboard: write_clipboard() prepares the whole item first so a failure
// cannot leave the previous owner's content cleared and nothing in its place.
HGLOBAL make_global(const std::vector<uint8_t>& data) {
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!handle) {
        return nullptr;
    }
    auto* target = static_cast<uint8_t*>(GlobalLock(handle));
    if (!target) {
        GlobalFree(handle);
        return nullptr;
    }
    if (!data.empty()) {
        std::memcpy(target, data.data(), data.size());
    }
    GlobalUnlock(handle);
    return handle;
}

HGLOBAL make_text(const std::vector<uint8_t>& data) {
    const std::string text(data.begin(), data.end());
    // The rich path carries text up to the stream's budget; the legacy
    // 256 KiB control-message limit (valid_clipboard_text) does not apply.
    if (text.empty() || text.size() > kMaxClipboardTextBytes ||
        text.size() > static_cast<size_t>(INT_MAX) || !valid_utf8(text)) {
        return nullptr;
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return nullptr;
    }
    const size_t bytes = (static_cast<size_t>(length) + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle) {
        return nullptr;
    }
    auto* target = static_cast<wchar_t*>(GlobalLock(handle));
    if (!target ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), target, length) != length) {
        GlobalUnlock(handle);
        GlobalFree(handle);
        return nullptr;
    }
    target[length] = L'\0';
    GlobalUnlock(handle);
    return handle;
}

}  // namespace

ClipboardReadState read_clipboard_state(ClipboardContent& content, std::string* error) {
    content = ClipboardContent{};
    if (error) error->clear();
    if (!open_clipboard()) {
        if (error) {
            *error = "the clipboard is held by another process";
        }
        return ClipboardReadState::Failed;
    }
    struct Closer {
        ~Closer() { CloseClipboard(); }
    } closer;

    const bool png_available = (png_format() != 0 &&
                                IsClipboardFormatAvailable(png_format())) ||
                               (png_mime_format() != 0 &&
                                IsClipboardFormatAvailable(png_mime_format()));
    const bool dib_available = IsClipboardFormatAvailable(CF_DIB);
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) &&
        !IsClipboardFormatAvailable(html_format()) &&
        !IsClipboardFormatAvailable(rtf_format()) &&
        !dib_available && !png_available) {
        return ClipboardReadState::Empty;
    }

    std::vector<uint8_t> data;
    bool failed = false;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (!read_text(data)) failed = true;
        else if (!data.empty() && !accepts(content, ClipboardFormat::Text, std::move(data))) failed = true;
    }
    const UINT formats[] = {html_format(), rtf_format()};
    const ClipboardFormat kinds[] = {ClipboardFormat::Html, ClipboardFormat::Rtf};
    for (size_t i = 0; i < 2; ++i) {
        if (!IsClipboardFormatAvailable(formats[i])) continue;
        if (!read_bytes(formats[i], kMaxClipboardMarkupBytes, true, data) ||
            !accepts(content, kinds[i], std::move(data))) {
            failed = true;
        }
    }

    const auto valid_single = [](ClipboardFormat format,
                                 const std::vector<uint8_t>& payload) {
        ClipboardContent single;
        ClipboardFormatData entry;
        entry.format = format;
        entry.data = payload;
        single.formats.push_back(std::move(entry));
        return valid_clipboard_content(single, nullptr);
    };

    std::vector<uint8_t> png_data;
    bool have_png = false;
    if (png_available) {
        const UINT candidates[] = {png_format(), png_mime_format()};
        for (UINT format : candidates) {
            if (format == 0 || !IsClipboardFormatAvailable(format)) continue;
            std::vector<uint8_t> candidate;
            if (read_bytes(format, kMaxClipboardPngBytes, false, candidate) &&
                valid_single(ClipboardFormat::Png, candidate)) {
                png_data = std::move(candidate);
                have_png = true;
                break;
            }
        }
    }

    std::vector<uint8_t> dib_data;
    bool have_dib = false;
    if (dib_available &&
        read_bytes(CF_DIB, kMaxClipboardDibSourceBytes, false, dib_data) &&
        valid_clipboard_dib(dib_data, nullptr)) {
        have_dib = true;
    }
    if (have_dib && !have_png) {
        std::vector<uint8_t> converted;
        if (!encode_dib_png(dib_data, converted, error)) {
            // Keep the codec's detailed error for the caller.
        } else if (converted.empty() || converted.size() > kMaxClipboardPngBytes) {
            if (error) *error = "the converted clipboard PNG has an invalid size";
        } else if (!valid_single(ClipboardFormat::Png, converted)) {
            if (error) *error = "the converted clipboard PNG failed validation";
        } else {
            png_data = std::move(converted);
            have_png = true;
        }
    }

    // Images are alternative representations. A broken PNG must not hide a
    // usable DIB, nor a large DIB that converted successfully. Append in
    // format-id order because the CCLB container requires ascending ids.
    if (have_dib && dib_data.size() <= kMaxClipboardImageBytes &&
        !accepts(content, ClipboardFormat::Dib, std::move(dib_data))) {
        have_dib = false;
    }
    if (have_png && !accepts(content, ClipboardFormat::Png, std::move(png_data))) {
        have_png = false;
    }
    // A partially rendered item is not a valid baseline either: otherwise a
    // format that becomes readable later would look like a new local copy.
    if (failed) {
        content = {};
        if (error) *error = "a clipboard format could not be read";
        return ClipboardReadState::Failed;
    }
    if (content.formats.empty()) {
        if (dib_available || png_available) {
            if (error && error->empty()) {
                *error = "the clipboard image could not be decoded";
            }
            return ClipboardReadState::Failed;
        }
        return ClipboardReadState::Empty;
    }
    if (!valid_clipboard_content(content, error)) {
        content = ClipboardContent{};
        return ClipboardReadState::Failed;
    }
    content.hash = clipboard_content_hash(content);
    return ClipboardReadState::Content;
}

bool read_clipboard(ClipboardContent& content, std::string* error) {
    return read_clipboard_state(content, error) == ClipboardReadState::Content;
}

bool write_clipboard(const ClipboardContent& content, std::string* error) {
    if (!valid_clipboard_content(content, error)) {
        return false;
    }
    struct Staged {
        UINT format = 0;
        HGLOBAL handle = nullptr;
    };
    std::vector<Staged> staged;
    staged.reserve(content.formats.size() * 2);
    // Prepare every handle before EmptyClipboard(). A preparation failure must
    // leave the clipboard alone; once it is emptied nothing may fail.
    // Text goes in last: a paste target that knows none of the richer
    // formats should still find the plain fallback.
    for (auto it = content.formats.rbegin(); it != content.formats.rend(); ++it) {
        // PNG is exposed under both the de-facto "PNG" name and the MIME name.
        // SetClipboardData transfers ownership, so every target format gets an
        // independent HGLOBAL rather than sharing one handle.
        for (UINT format : win32_formats(it->format)) {
            HGLOBAL handle = nullptr;
            if (format == CF_UNICODETEXT) {
                handle = make_text(it->data);
            } else if (format != 0) {
                handle = make_global(it->data);
            }
            if (handle != nullptr) {
                staged.push_back({format, handle});
            }
        }
    }
    auto release = [&staged] {
        for (const Staged& item : staged) {
            if (item.handle != nullptr) {
                GlobalFree(item.handle);
            }
        }
        staged.clear();
    };
    if (staged.empty()) {
        if (error) {
            *error = "the clipboard item could not be prepared";
        }
        return false;
    }
    if (!open_clipboard()) {
        release();
        if (error) {
            *error = "the clipboard is held by another process";
        }
        return false;
    }
    if (!EmptyClipboard()) {
        CloseClipboard();
        release();
        if (error) {
            *error = "the clipboard could not be cleared";
        }
        return false;
    }
    size_t written = 0;
    for (Staged& item : staged) {
        if (SetClipboardData(item.format, item.handle)) {
            item.handle = nullptr;  // the clipboard owns it now
            ++written;
        }
    }
    CloseClipboard();
    release();
    if (written == 0) {
        if (error) {
            *error = "the clipboard refused every format";
        }
        return false;
    }
    return true;
}

ClipboardReadState read_clipboard_files_state(std::vector<std::string>& paths,
                                             std::string* error) {
    paths.clear();
    if (error) error->clear();
    if (!open_clipboard()) {
        if (error) {
            *error = "the clipboard is held by another process";
        }
        return ClipboardReadState::Failed;
    }
    struct Closer {
        ~Closer() { CloseClipboard(); }
    } closer;
    // Do not force a remote virtual file to render while polling our own
    // clipboard, including from another connection in this process.
    if (is_virtual_clipboard()) return ClipboardReadState::Failed;
    if (!IsClipboardFormatAvailable(CF_HDROP)) return ClipboardReadState::Empty;
    const HANDLE handle = GetClipboardData(CF_HDROP);
    if (!handle) {
        if (error) {
            *error = "the clipboard holds no files";
        }
        return ClipboardReadState::Failed;
    }
    const HDROP drop = static_cast<HDROP>(handle);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
    if (count == 0) {
        if (error) {
            *error = "the clipboard file list is empty";
        }
        return ClipboardReadState::Empty;
    }
    if (count > kMaxClipboardFileEntries) {
        if (error) {
            *error = "the clipboard holds more files than a copy can carry";
        }
        return ClipboardReadState::Failed;
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
        return ClipboardReadState::Failed;
    }
    return ClipboardReadState::Content;
}

bool read_clipboard_files(std::vector<std::string>& paths, std::string* error) {
    return read_clipboard_files_state(paths, error) == ClipboardReadState::Content;
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
    // GlobalAlloc leaves stale bytes in the header; pt/fNC must be zero.
    *drop = DROPFILES{};
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
