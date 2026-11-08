#include "local_text_input.h"

#include <algorithm>
#include <utility>
#include "chast/protocol.h"
#include "chast/input.h"

namespace chast_viewer {

LocalTextInput::LocalTextInput(HWND view, std::function<bool(const std::string&)> send,
    std::function<void(bool)> composing)
    : view_(view), send_(std::move(send)), composition_changed_(std::move(composing)) {}

LocalTextInput::~LocalTextInput() {
    Enable(false);
    if (context_) ImmDestroyContext(context_);
}

bool LocalTextInput::Enable(bool enabled) {
    if (enabled_ == enabled) return enabled_;
    if (!enabled) {
        // Invalidate before cancellation, which may deliver result messages.
        enabled_ = false;
        Cancel();
        if (IsWindow(view_)) ImmAssociateContext(view_, previous_);
        previous_ = nullptr;
        return false;
    }
    if (!IsWindow(view_)) return false;
    if (!context_) {
        context_ = ImmCreateContext();
        if (!context_) return false;
        const HIMC existing = ImmGetContext(view_);
        if (existing) {
            DWORD conversion = 0, sentence = 0;
            ImmGetConversionStatus(existing, &conversion, &sentence);
            ImmSetConversionStatus(context_, conversion, sentence);
            ImmSetOpenStatus(context_, ImmGetOpenStatus(existing));
            ImmReleaseContext(view_, existing);
        }
    }
    previous_ = ImmAssociateContext(view_, context_);
    enabled_ = true;
    Position();
    return true;
}

void LocalTextInput::Cancel() {
    if (cancelling_) return;
    cancelling_ = true;
    surrogate_ = 0;
    injected_key_ = false;
    composing_ = false;
    composition_changed_(false);
    if (context_) ImmNotifyIME(context_, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
    cancelling_ = false;
}

void LocalTextInput::Position() {
    if (!context_) return;
    POINT point{};
    GetCursorPos(&point);
    ScreenToClient(view_, &point);
    RECT client{};
    GetClientRect(view_, &client);
    point.x = std::clamp(point.x, 0L, (std::max)(0L, client.right - 160));
    point.y = std::clamp(point.y, 32L, (std::max)(32L, client.bottom - 60));
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = point;
    ImmSetCompositionWindow(context_, &composition);
    CANDIDATEFORM candidate{};
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = point;
    candidate.ptCurrentPos.y += 24;
    ImmSetCandidateWindow(context_, &candidate);
}

bool LocalTextInput::Commit(const std::wstring& text) {
    if (!enabled_ || cancelling_ || text.empty() || GetForegroundWindow() != GetAncestor(view_, GA_ROOT)) return false;
    if (text.size() > chast::kMaxTextInputBytes) return false;
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0 || size > static_cast<int>(chast::kMaxTextInputBytes)) return false;
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
    return chast::valid_text_input(utf8) && send_(utf8);
}

bool LocalTextInput::Handle(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) {
    if (!enabled_) return false;
    result = 0;
    switch (message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            // TranslateMessage does not reliably copy extra-info to WM_CHAR.
            // Remember provenance from its preceding native key message.
            injected_key_ = static_cast<ULONG_PTR>(GetMessageExtraInfo()) == chast::kInputInjectionTag;
            return false;
        case WM_KILLFOCUS:
            Cancel();
            return false;
        case WM_INPUTLANGCHANGE:
            Cancel();
            return false;
        case WM_IME_SETCONTEXT:
            // The native composition/candidate UI owns local IME preedit.
            // Flutter's text plugin has no text field for this remote canvas.
            result = DefWindowProcW(view_, message, wparam, lparam);
            return true;
        case WM_IME_NOTIFY:
        case WM_IME_REQUEST:
        case WM_IME_CONTROL:
        case WM_IME_SELECT:
            result = DefWindowProcW(view_, message, wparam, lparam);
            return true;
        case WM_IME_STARTCOMPOSITION:
            composing_ = true;
            composition_changed_(true);
            surrogate_ = 0;
            Position();
            result = DefWindowProcW(view_, message, wparam, lparam);
            return true;
        case WM_IME_COMPOSITION: {
            if ((lparam & GCS_RESULTSTR) != 0) {
                const LONG bytes = ImmGetCompositionStringW(context_, GCS_RESULTSTR, nullptr, 0);
                if (bytes > 0 && bytes <= static_cast<LONG>(chast::kMaxTextInputBytes * sizeof(wchar_t)) &&
                    bytes % sizeof(wchar_t) == 0) {
                    std::wstring text(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
                    if (ImmGetCompositionStringW(context_, GCS_RESULTSTR, text.data(), bytes) == bytes)
                        Commit(text);
                }
            }
            // Do not let DefWindowProc emit WM_IME_CHAR for the committed
            // result a second time. Other flags keep the native preedit visible.
            const LPARAM preedit = lparam & ~static_cast<LPARAM>(GCS_RESULTSTR);
            if (preedit) result = DefWindowProcW(view_, message, wparam, preedit);
            return true;
        }
        case WM_IME_ENDCOMPOSITION:
            composing_ = false;
            composition_changed_(false);
            result = DefWindowProcW(view_, message, wparam, lparam);
            return true;
        case WM_IME_CHAR:
        case WM_SYSCHAR:
            return true;
        case WM_CHAR: {
            if (injected_key_ || static_cast<ULONG_PTR>(GetMessageExtraInfo()) == chast::kInputInjectionTag) return true;
            // Enter, Backspace and Tab are already forwarded as physical keys
            // outside composition; while composing they belong to the IME.
            const wchar_t unit = static_cast<wchar_t>(wparam);
            if (composing_ || unit < 0x20 || unit == 0x7f) return true;
            if (unit >= 0xd800 && unit <= 0xdbff) { surrogate_ = unit; return true; }
            std::wstring text;
            if (unit >= 0xdc00 && unit <= 0xdfff) {
                if (!surrogate_) return true;
                text.push_back(surrogate_);
            }
            surrogate_ = 0;
            text.push_back(unit);
            Commit(text);
            return true;
        }
        case WM_UNICHAR:
            if (wparam == UNICODE_NOCHAR) { result = TRUE; return true; }
            if (injected_key_ || static_cast<ULONG_PTR>(GetMessageExtraInfo()) == chast::kInputInjectionTag) return true;
            if (wparam >= 0x20 && wparam <= 0x10ffff && !composing_) {
                std::wstring text;
                if (wparam > 0xffff) {
                    text.push_back(static_cast<wchar_t>(0xd800 + ((wparam - 0x10000) >> 10)));
                    text.push_back(static_cast<wchar_t>(0xdc00 + ((wparam - 0x10000) & 0x3ff)));
                } else text.push_back(static_cast<wchar_t>(wparam));
                Commit(text);
            }
            return true;
        default:
            return false;
    }
}

}  // namespace chast_viewer
