#include "session_tab_host.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include <windowsx.h>

#include "runner_lifecycle_trace.h"
#include "session_tab_labels.h"

namespace {

constexpr int kTabsId = 4101;
constexpr int kNewButtonId = 4102;
constexpr int kTabCloseCommandId = 4103;
constexpr int kTabBarHeight = 48;
constexpr COLORREF kHeaderBackground = RGB(17, 19, 24);
constexpr COLORREF kHeaderBorder = RGB(39, 43, 51);
constexpr COLORREF kTabSelected = RGB(39, 45, 55);
constexpr COLORREF kTabHovered = RGB(29, 34, 42);
constexpr COLORREF kTabText = RGB(235, 238, 244);
constexpr COLORREF kTabTextMuted = RGB(154, 162, 176);
constexpr COLORREF kAccent = RGB(83, 158, 255);
constexpr COLORREF kNewButtonHovered = RGB(39, 45, 55);
constexpr COLORREF kCloseHovered = RGB(76, 41, 47);
constexpr COLORREF kTooltipBackground = RGB(31, 35, 42);
constexpr COLORREF kTooltipBorder = RGB(68, 75, 87);
constexpr COLORREF kTooltipText = RGB(235, 238, 244);
constexpr UINT_PTR kTabControlSubclassId = 1;
constexpr UINT_PTR kButtonSubclassId = 2;
constexpr size_t kMaxSessionTabs = 16;

void trace_tab_lifecycle(const char* event, HWND host, uint64_t id,
                         HWND child, size_t count, uint64_t selected);

int scale_for_window(HWND window, int value) {
  const UINT dpi = window ? GetDpiForWindow(window) : USER_DEFAULT_SCREEN_DPI;
  return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

HFONT create_header_font(HWND window, int weight = FW_NORMAL) {
  const int height = -MulDiv(10, static_cast<int>(GetDpiForWindow(window)), 72);
  return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                     L"Segoe UI");
}

void draw_rounded_rect(HDC dc, const RECT& bounds, COLORREF fill,
                       COLORREF border, int radius) {
  const int saved = SaveDC(dc);
  HBRUSH brush = CreateSolidBrush(fill);
  HPEN pen = CreatePen(PS_SOLID, 1, border);
  HGDIOBJ old_brush = SelectObject(dc, brush);
  HGDIOBJ old_pen = SelectObject(dc, pen);
  RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom,
            radius, radius);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  RestoreDC(dc, saved);
  DeleteObject(pen);
  DeleteObject(brush);
}

void draw_tab_item(const DRAWITEMSTRUCT& item, HWND tabs) {
  if (item.itemID >= static_cast<UINT>(TabCtrl_GetItemCount(tabs))) return;

  wchar_t label[1100]{};
  TCITEMW tab_item{};
  tab_item.mask = TCIF_TEXT | TCIF_PARAM;
  tab_item.pszText = label;
  tab_item.cchTextMax = static_cast<int>(std::size(label));
  if (!TabCtrl_GetItem(tabs, static_cast<int>(item.itemID), &tab_item)) return;

  POINT cursor{};
  GetCursorPos(&cursor);
  ScreenToClient(tabs, &cursor);
  TCHITTESTINFO hit{};
  hit.pt = cursor;
  const int hovered_index = TabCtrl_HitTest(tabs, &hit);
  const bool selected = TabCtrl_GetCurSel(tabs) == static_cast<int>(item.itemID);
  const bool hovered = hovered_index == static_cast<int>(item.itemID) && !selected;

  RECT pill = item.rcItem;
  InflateRect(&pill, -scale_for_window(tabs, 3), -scale_for_window(tabs, 3));
  RECT close_button{pill.right - scale_for_window(tabs, 25),
                    pill.top + (pill.bottom - pill.top - scale_for_window(tabs, 18)) / 2,
                    pill.right - scale_for_window(tabs, 7),
                    pill.top + (pill.bottom - pill.top + scale_for_window(tabs, 18)) / 2};
  const bool close_hovered = hovered_index == static_cast<int>(item.itemID) &&
                             PtInRect(&close_button, cursor) != FALSE;
  if (selected || hovered) {
    const COLORREF fill = selected ? kTabSelected : kTabHovered;
    draw_rounded_rect(item.hDC, pill, fill, fill,
                      scale_for_window(tabs, 7));
  }

  const int icon_x = pill.left + scale_for_window(tabs, 12);
  const int icon_y = pill.top + (pill.bottom - pill.top - scale_for_window(tabs, 12)) / 2;
  HPEN icon_pen = CreatePen(PS_SOLID, scale_for_window(tabs, 1),
                            selected ? kAccent : kTabTextMuted);
  HGDIOBJ icon_old_pen = SelectObject(item.hDC, icon_pen);
  HGDIOBJ icon_old_brush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
  RoundRect(item.hDC, icon_x, icon_y, icon_x + scale_for_window(tabs, 15),
            icon_y + scale_for_window(tabs, 10), scale_for_window(tabs, 2),
            scale_for_window(tabs, 2));
  MoveToEx(item.hDC, icon_x + scale_for_window(tabs, 7),
           icon_y + scale_for_window(tabs, 10), nullptr);
  LineTo(item.hDC, icon_x + scale_for_window(tabs, 7),
         icon_y + scale_for_window(tabs, 12));
  MoveToEx(item.hDC, icon_x + scale_for_window(tabs, 4),
           icon_y + scale_for_window(tabs, 12), nullptr);
  LineTo(item.hDC, icon_x + scale_for_window(tabs, 11),
         icon_y + scale_for_window(tabs, 12));
  SelectObject(item.hDC, icon_old_brush);
  SelectObject(item.hDC, icon_old_pen);
  DeleteObject(icon_pen);

  RECT text = pill;
  text.left = icon_x + scale_for_window(tabs, 24);
  text.right = close_button.left - scale_for_window(tabs, 4);
  SetBkMode(item.hDC, TRANSPARENT);
  SetTextColor(item.hDC, selected ? kTabText : kTabTextMuted);
  HFONT font = create_header_font(tabs, selected ? FW_SEMIBOLD : FW_NORMAL);
  HGDIOBJ old_font = SelectObject(item.hDC, font);
  constexpr UINT kTextFlags =
      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
  const auto length = static_cast<int>(std::char_traits<wchar_t>::length(label));
  const auto suffix_length = static_cast<int>(tab_item.lParam);
  int name_length = length;
  if (suffix_length > 0 && suffix_length + 3 <= length && text.right > text.left) {
    const wchar_t* suffix = label + length - suffix_length;
    SIZE measured{};
    if (GetTextExtentPoint32W(item.hDC, suffix, suffix_length, &measured)) {
      RECT suffix_rect = text;
      suffix_rect.left = (std::max)(text.left, text.right - measured.cx);
      SetTextColor(item.hDC, kTabTextMuted);
      DrawTextW(item.hDC, suffix, suffix_length, &suffix_rect,
                kTextFlags | DT_RIGHT);
      SetTextColor(item.hDC, selected ? kTabText : kTabTextMuted);
      text.right = suffix_rect.left - scale_for_window(tabs, 6);
      name_length -= suffix_length + 3;  // Skip the accessible-name separator.
    }
  }
  if (text.right > text.left)
    DrawTextW(item.hDC, label, name_length, &text, kTextFlags);
  SelectObject(item.hDC, old_font);
  DeleteObject(font);

  if (selected || hovered || close_hovered) {
    if (close_hovered) {
      HBRUSH close_background = CreateSolidBrush(kCloseHovered);
      HPEN close_border = CreatePen(PS_SOLID, 1, kCloseHovered);
      HGDIOBJ close_old_brush = SelectObject(item.hDC, close_background);
      HGDIOBJ close_old_pen = SelectObject(item.hDC, close_border);
      Ellipse(item.hDC, close_button.left, close_button.top,
              close_button.right, close_button.bottom);
      SelectObject(item.hDC, close_old_pen);
      SelectObject(item.hDC, close_old_brush);
      DeleteObject(close_border);
      DeleteObject(close_background);
    }
    const int cross_pad = scale_for_window(tabs, 6);
    HPEN cross = CreatePen(PS_SOLID, scale_for_window(tabs, 1),
                           close_hovered ? kTabText : kTabTextMuted);
    HGDIOBJ cross_old_pen = SelectObject(item.hDC, cross);
    MoveToEx(item.hDC, close_button.left + cross_pad,
             close_button.top + cross_pad, nullptr);
    LineTo(item.hDC, close_button.right - cross_pad,
           close_button.bottom - cross_pad);
    MoveToEx(item.hDC, close_button.right - cross_pad,
             close_button.top + cross_pad, nullptr);
    LineTo(item.hDC, close_button.left + cross_pad,
           close_button.bottom - cross_pad);
    SelectObject(item.hDC, cross_old_pen);
    DeleteObject(cross);
  }

  if (selected) {
    RECT indicator{pill.left + scale_for_window(tabs, 12),
                   pill.bottom - scale_for_window(tabs, 3),
                   pill.left + scale_for_window(tabs, 34),
                   pill.bottom - scale_for_window(tabs, 1)};
    HBRUSH accent = CreateSolidBrush(kAccent);
    FillRect(item.hDC, &indicator, accent);
    DeleteObject(accent);
  }
}

void draw_tab_control(HWND tabs, HDC dc) {
  RECT client{};
  GetClientRect(tabs, &client);
  HBRUSH background = CreateSolidBrush(kHeaderBackground);
  FillRect(dc, &client, background);
  DeleteObject(background);

  const int count = TabCtrl_GetItemCount(tabs);
  for (int index = 0; index < count; ++index) {
    DRAWITEMSTRUCT item{};
    item.CtlType = ODT_TAB;
    item.CtlID = kTabsId;
    item.itemID = static_cast<UINT>(index);
    item.hwndItem = tabs;
    item.hDC = dc;
    if (TabCtrl_GetItemRect(tabs, index, &item.rcItem))
      draw_tab_item(item, tabs);
  }
}

bool cursor_is_over(HWND window) {
  POINT cursor{};
  RECT screen_bounds{};
  if (!GetCursorPos(&cursor) || !GetWindowRect(window, &screen_bounds))
    return false;
  return PtInRect(&screen_bounds, cursor) != FALSE;
}

void hide_tab_tooltip(HWND tooltip) {
  if (tooltip && IsWindowVisible(tooltip)) ShowWindow(tooltip, SW_HIDE);
}

void show_tab_tooltip(HWND tooltip, HWND tabs, const std::wstring& caption,
                      POINT anchor) {
  if (!tooltip || caption.empty()) return;
  SetWindowTextW(tooltip, caption.c_str());

  HDC dc = GetDC(tabs);
  if (!dc) return;
  HFONT font = create_header_font(tabs, FW_NORMAL);
  HGDIOBJ old_font = SelectObject(dc, font);
  RECT measured{0, 0, scale_for_window(tabs, 560), 0};
  DrawTextW(dc, caption.c_str(), static_cast<int>(caption.size()), &measured,
            DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(dc, old_font);
  DeleteObject(font);
  ReleaseDC(tabs, dc);

  const int horizontal_padding = scale_for_window(tabs, 20);
  const int vertical_padding = scale_for_window(tabs, 20);
  const int width = (std::max)(scale_for_window(tabs, 80),
      static_cast<int>(measured.right) + horizontal_padding);
  const int height = (std::max)(scale_for_window(tabs, 36),
      static_cast<int>(measured.bottom) + vertical_padding);
  HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{sizeof(monitor_info)};
  if (monitor && GetMonitorInfoW(monitor, &monitor_info)) {
    const RECT& work = monitor_info.rcWork;
    anchor.x = (std::min)(anchor.x, work.right - width);
    anchor.y += scale_for_window(tabs, 6);
    if (anchor.y + height > work.bottom)
      anchor.y -= height + scale_for_window(tabs, 12);
    anchor.x = (std::max)(anchor.x, work.left);
    anchor.y = (std::max)(anchor.y, work.top);
  }
  SetWindowPos(tooltip, HWND_TOPMOST, anchor.x, anchor.y, width, height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  // Force the owner-drawn popup to paint before returning to the tab message
  // loop; otherwise a quick hover/capture can observe a visible HWND before
  // its text has reached the screen.
  RedrawWindow(tooltip, nullptr, nullptr,
               RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

void update_tab_tooltip(HWND tabs, HWND tooltip, LPARAM mouse_position) {
  if (!tooltip) return;
  POINT cursor{GET_X_LPARAM(mouse_position), GET_Y_LPARAM(mouse_position)};
  TCHITTESTINFO hit{};
  hit.pt = cursor;
  const int index = TabCtrl_HitTest(tabs, &hit);
  RECT item_rect{};
  if (index < 0 || !TabCtrl_GetItemRect(tabs, index, &item_rect)) {
    hide_tab_tooltip(tooltip);
    return;
  }

  wchar_t caption[1100]{};
  TCITEMW item{};
  item.mask = TCIF_TEXT;
  item.pszText = caption;
  item.cchTextMax = static_cast<int>(std::size(caption));
  if (!TabCtrl_GetItem(tabs, index, &item)) {
    hide_tab_tooltip(tooltip);
    return;
  }

  HDC dc = GetDC(tabs);
  if (!dc) return;
  HFONT font = create_header_font(tabs);
  HGDIOBJ old_font = SelectObject(dc, font);
  SIZE text_size{};
  const int text_length = static_cast<int>(std::char_traits<wchar_t>::length(caption));
  GetTextExtentPoint32W(dc, caption, text_length, &text_size);
  SelectObject(dc, old_font);
  DeleteObject(font);
  ReleaseDC(tabs, dc);

  const int available = item_rect.right - item_rect.left -
                        scale_for_window(tabs, 68);
  if (text_size.cx <= available) {
    hide_tab_tooltip(tooltip);
    return;
  }
  POINT anchor{item_rect.left, item_rect.bottom};
  ClientToScreen(tabs, &anchor);
  show_tab_tooltip(tooltip, tabs, caption, anchor);
}

LRESULT CALLBACK tab_tooltip_window_proc(HWND window, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_NCHITTEST) return HTTRANSPARENT;
  if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    HBRUSH background = CreateSolidBrush(kTooltipBackground);
    FillRect(dc, &client, background);
    DeleteObject(background);
    draw_rounded_rect(dc, client, kTooltipBackground, kTooltipBorder,
                      scale_for_window(window, 8));
    InflateRect(&client, -scale_for_window(window, 10),
                -scale_for_window(window, 7));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kTooltipText);
    HFONT font = create_header_font(window, FW_NORMAL);
    HGDIOBJ old_font = SelectObject(dc, font);
    wchar_t text[1100]{};
    GetWindowTextW(window, text, static_cast<int>(std::size(text)));
    DrawTextW(dc, text, -1, &client,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old_font);
    DeleteObject(font);
    EndPaint(window, &paint);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void draw_header_button(const DRAWITEMSTRUCT& item) {
  const bool hovered = (item.itemState & ODS_HOTLIGHT) != 0 ||
                       cursor_is_over(item.hwndItem);
  const bool pressed = (item.itemState & ODS_SELECTED) != 0;
  const bool disabled = (item.itemState & ODS_DISABLED) != 0;
  RECT pill = item.rcItem;
  InflateRect(&pill, -1, -2);
  HBRUSH background = CreateSolidBrush(kHeaderBackground);
  FillRect(item.hDC, &item.rcItem, background);
  DeleteObject(background);
  if (hovered || pressed) {
    const COLORREF fill = pressed ? kTabSelected : kNewButtonHovered;
    draw_rounded_rect(item.hDC, pill, fill, fill,
                      scale_for_window(item.hwndItem, 8));
  }

  const int center_x = (pill.left + pill.right) / 2;
  const int center_y = (pill.top + pill.bottom) / 2;
  const int arm = scale_for_window(item.hwndItem, 6);
  HPEN plus = CreatePen(PS_SOLID, scale_for_window(item.hwndItem, 2),
                        disabled ? kTabTextMuted : kTabText);
  HGDIOBJ old_pen = SelectObject(item.hDC, plus);
  MoveToEx(item.hDC, center_x - arm, center_y, nullptr);
  LineTo(item.hDC, center_x + arm + 1, center_y);
  MoveToEx(item.hDC, center_x, center_y - arm, nullptr);
  LineTo(item.hDC, center_x, center_y + arm + 1);
  SelectObject(item.hDC, old_pen);
  DeleteObject(plus);

  if ((item.itemState & ODS_FOCUS) != 0) {
    RECT focus = pill;
    InflateRect(&focus, -scale_for_window(item.hwndItem, 5),
                -scale_for_window(item.hwndItem, 5));
    DrawFocusRect(item.hDC, &focus);
  }
}

LRESULT CALLBACK header_control_subclass_proc(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam, UINT_PTR subclass_id, DWORD_PTR ref_data) {
  if (subclass_id == kTabControlSubclassId && message == WM_PAINT) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    draw_tab_control(window, dc);
    EndPaint(window, &paint);
    return 0;
  }
  if (subclass_id == kTabControlSubclassId &&
      (message == WM_PRINTCLIENT || message == WM_PRINT)) {
    draw_tab_control(window, reinterpret_cast<HDC>(wparam));
    return 0;
  }
  if (message == WM_ERASEBKGND && subclass_id == kTabControlSubclassId) {
    RECT client{};
    GetClientRect(window, &client);
    HBRUSH background = CreateSolidBrush(kHeaderBackground);
    FillRect(reinterpret_cast<HDC>(wparam), &client, background);
    DeleteObject(background);
    return 1;
  }
  if (message == WM_MOUSEMOVE) {
    if (subclass_id == kTabControlSubclassId) {
      update_tab_tooltip(window, reinterpret_cast<HWND>(ref_data), lparam);
    }
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
    TrackMouseEvent(&tracking);
    InvalidateRect(window, nullptr, FALSE);
  } else if (message == WM_MOUSELEAVE) {
    if (subclass_id == kTabControlSubclassId)
      hide_tab_tooltip(reinterpret_cast<HWND>(ref_data));
    InvalidateRect(window, nullptr, FALSE);
  } else if (subclass_id == kTabControlSubclassId &&
             (message == WM_LBUTTONUP || message == WM_MBUTTONUP)) {
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    TCHITTESTINFO hit{};
    hit.pt = point;
    const int index = TabCtrl_HitTest(window, &hit);
    RECT tab{};
    if (index >= 0 && TabCtrl_GetItemRect(window, index, &tab)) {
      RECT close_button{tab.right - scale_for_window(window, 25),
                        tab.top + (tab.bottom - tab.top - scale_for_window(window, 18)) / 2,
                        tab.right - scale_for_window(window, 7),
                        tab.top + (tab.bottom - tab.top + scale_for_window(window, 18)) / 2};
      if (message == WM_MBUTTONUP || PtInRect(&close_button, point)) {
        SendMessageW(GetParent(window), WM_COMMAND,
                     MAKEWPARAM(kTabCloseCommandId, 0),
                     static_cast<LPARAM>(index));
        return 0;
      }
    }
  } else if (message == WM_NCDESTROY) {
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    RemoveWindowSubclass(window, header_control_subclass_proc, subclass_id);
    return result;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

bool consume_test_close_post_failure() {
  static bool consumed = false;
  if (consumed) return false;

  char* testing = nullptr;
  size_t testing_size = 0;
  char* injection = nullptr;
  size_t injection_size = 0;
  const bool enabled =
      _dupenv_s(&testing, &testing_size, "CHAST_NATIVE_TESTING") == 0 &&
      testing != nullptr && std::string(testing) == "1" &&
      _dupenv_s(&injection, &injection_size,
                "CHAST_NATIVE_TEST_FAIL_TAB_CLOSE_POST_ONCE") == 0 &&
      injection != nullptr && std::string(injection) == "1";
  std::free(testing);
  std::free(injection);
  if (!enabled) return false;

  consumed = true;
  return true;
}

std::string test_tab_label_override() {
  char* testing = nullptr;
  size_t testing_size = 0;
  char* label = nullptr;
  size_t label_size = 0;
  const bool enabled =
      _dupenv_s(&testing, &testing_size, "CHAST_NATIVE_TESTING") == 0 &&
      testing != nullptr && std::string(testing) == "1" &&
      _dupenv_s(&label, &label_size, "CHAST_NATIVE_TEST_TAB_LABEL") == 0 &&
      label != nullptr;
  std::string result = enabled ? label : "";
  std::free(testing);
  std::free(label);
  return result;
}

void trace_tab_lifecycle(const char* event, HWND host, uint64_t id,
                         HWND child, size_t count, uint64_t selected) {
  char* testing = nullptr;
  size_t testing_size = 0;
  char* path = nullptr;
  size_t path_size = 0;
  const bool configured =
      _dupenv_s(&testing, &testing_size, "CHAST_NATIVE_TESTING") == 0 &&
      testing != nullptr && std::string(testing) == "1" &&
      _dupenv_s(&path, &path_size, "CHAST_NATIVE_TEST_TABS_TRACE") == 0 &&
      path != nullptr && *path != '\0';
  if (!configured) {
    std::free(testing);
    std::free(path);
    return;
  }
  std::ofstream output(path, std::ios::app);
  if (output) {
    output << event << '\t' << GetCurrentProcessId() << '\t'
           << GetCurrentThreadId() << '\t'
           << reinterpret_cast<uintptr_t>(host) << '\t' << id << '\t'
           << reinterpret_cast<uintptr_t>(child) << '\t' << count << '\t'
           << selected << '\t'
           << (host != nullptr && IsWindow(host) ? 1 : 0) << '\t'
           << (child != nullptr && IsWindow(child) ? 1 : 0) << '\t'
           << (child != nullptr && IsWindowVisible(child) ? 1 : 0) << '\n';
  }
  std::free(testing);
  std::free(path);
}

std::wstring to_wide(const std::string& value) {
  if (value.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        value.data(),
                                        static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(), size) != size)
    return {};
  return result;
}

}  // namespace

SessionTabHostWindow::SessionTabHostWindow(
    std::function<void()> show_home,
    std::shared_ptr<SessionTabVisibility> visibility)
    : show_home_(std::move(show_home)), visibility_(std::move(visibility)) {}

bool SessionTabHostWindow::OnCreate() {
  if (!Win32Window::OnCreate()) return false;
  INITCOMMONCONTROLSEX controls{sizeof(controls),
                                ICC_TAB_CLASSES | ICC_WIN95_CLASSES};
  if (!InitCommonControlsEx(&controls)) return false;
  const HWND parent = GetHandle();
  tabs_ = CreateWindowExW(0, WC_TABCONTROLW, L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_SINGLELINE | TCS_FOCUSNEVER |
          TCS_FIXEDWIDTH | TCS_HOTTRACK | TCS_OWNERDRAWFIXED,
      14, 7, 700, 34, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabsId)),
      GetModuleHandleW(nullptr), nullptr);
  new_button_ = CreateWindowExW(0, L"BUTTON", L"新建连接",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
      0, 0, 40, 36, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNewButtonId)),
      GetModuleHandleW(nullptr), nullptr);
  if (!tabs_ || !new_button_) return false;
  SetWindowSubclass(new_button_, header_control_subclass_proc,
                    kButtonSubclassId, 0);
  WNDCLASSEXW tooltip_class{};
  tooltip_class.cbSize = sizeof(tooltip_class);
  tooltip_class.lpfnWndProc = tab_tooltip_window_proc;
  tooltip_class.hInstance = GetModuleHandleW(nullptr);
  tooltip_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  tooltip_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
  tooltip_class.lpszClassName = L"ChastSessionTabTooltip";
  if (!RegisterClassExW(&tooltip_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return false;
  tab_tooltip_ = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      tooltip_class.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, parent,
      nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!tab_tooltip_) return false;
  SetWindowSubclass(tabs_, header_control_subclass_proc,
                    kTabControlSubclassId,
                    reinterpret_cast<DWORD_PTR>(tab_tooltip_));
  tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
      CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr,
      GetModuleHandleW(nullptr), nullptr);
  if (tooltip_) {
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = parent;
    tool.uId = reinterpret_cast<UINT_PTR>(new_button_);
    tool.lpszText = const_cast<LPWSTR>(L"新建连接");
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0,
                 reinterpret_cast<LPARAM>(&tool));

  }
  Layout();
  return true;
}

bool SessionTabHostWindow::CreateSession(
    const std::string& request, const std::string& label,
    const std::string& device_hint,
    OpenSessionWindow open_session,
    FlutterWindow::OpenDisplayWindow open_display) {
  trace_tab_lifecycle("create-begin", GetHandle(), 0, nullptr,
                      entries_.size(), model_.selected().value_or(0));
  if (closing_container_ || GetHandle() == nullptr ||
      entries_.size() >= kMaxSessionTabs || request.empty())
    return false;

  flutter::DartProject project(L"data");
  project.set_dart_entrypoint_arguments({"--session-window", request});
  auto child = std::make_unique<FlutterWindow>(
      project, std::move(open_session), std::move(open_display),
      std::shared_ptr<chast_viewer::SharedViewerSession>{}, std::nullopt, true);
  const RECT client = GetClientArea();
  const int bar_height = scale_for_window(GetHandle(), kTabBarHeight);
  const RECT content{0, bar_height, client.right, client.bottom};
  if (!child->CreateChild(L"Chast Session", GetHandle(), content)) return false;
  child->SetQuitOnClose(false);

  const auto id = model_.Add();
  if (!id) return false;
  const std::string test_label = test_tab_label_override();
  std::wstring full_label = session_tab_labels::Sanitize(
      to_wide(test_label.empty() ? label : test_label));
  std::wstring base_label =
      session_tab_labels::BaseLabel(full_label, *id);
  if (full_label.empty()) full_label = base_label;
  entries_.push_back({*id, std::move(base_label), std::move(full_label), {},
                      session_tab_labels::PublicDeviceHint(device_hint),
                      std::move(child)});
  trace_tab_lifecycle("create-done", GetHandle(), *id,
                      entries_.back().window->GetHandle(), entries_.size(),
                      model_.selected().value_or(0));
  RebuildTabs();
  SyncVisibility();
  Show();
  return true;
}

void SessionTabHostWindow::PruneClosed() {
  bool removed = false;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->window->GetHandle() != nullptr) {
      ++it;
      continue;
    }
    trace_tab_lifecycle("prune-child", GetHandle(), it->id, nullptr,
                        entries_.size(), model_.selected().value_or(0));
    model_.BeginClose(it->id);
    model_.Remove(it->id);
    it = entries_.erase(it);
    removed = true;
  }
  if (removed) {
    RebuildTabs();
    SyncVisibility();
  }
  if (entries_.empty() && GetHandle() != nullptr && !final_close_posted_) {
    final_close_posted_ = true;
    trace_tab_lifecycle("post-empty-host-close", GetHandle(), 0, nullptr,
                        entries_.size(), model_.selected().value_or(0));
    PostMessageW(GetHandle(), WM_CLOSE, 0, 0);
  }
}

bool SessionTabHostWindow::OwnsSession(const void* session) const {
  return session != nullptr && std::any_of(entries_.begin(), entries_.end(),
      [session](const Entry& entry) {
        return entry.window->session_identity() == session;
      });
}

void SessionTabHostWindow::Layout() {
  const RECT area = GetClientArea();
  const int width = static_cast<int>((std::max)(0L, area.right - area.left));
  const int pad = scale_for_window(GetHandle(), 14);
  const int gap = scale_for_window(GetHandle(), 8);
  const int control_y = scale_for_window(GetHandle(), 6);
  const int control_height = scale_for_window(GetHandle(), 36);
  const int new_width = scale_for_window(GetHandle(), 40);
  const int new_x = width - pad - new_width;
  const int tabs_width = (std::max)(0, new_x - gap - pad);
  if (tabs_) {
    MoveWindow(tabs_, pad, control_y, tabs_width, control_height, TRUE);
    const int min_tab_width = scale_for_window(GetHandle(), 156);
    const int max_tab_width = scale_for_window(GetHandle(), 232);
    const int visible_slots = (std::max)(1, tabs_width / min_tab_width);
    const int tab_count = (std::max)(1, static_cast<int>(entries_.size()));
    const int width_slots = (std::min)(visible_slots, tab_count);
    const int tab_width = std::clamp(tabs_width / width_slots,
                                     min_tab_width, max_tab_width);
    TabCtrl_SetItemSize(tabs_, tab_width,
                        scale_for_window(GetHandle(), 32));
  }
  if (new_button_)
    MoveWindow(new_button_, new_x, control_y, new_width, control_height, TRUE);
  const int content_top = scale_for_window(GetHandle(), kTabBarHeight);
  for (Entry& entry : entries_) {
    SetWindowPos(entry.window->GetHandle(), nullptr, 0, content_top, width,
                 (std::max)(0L, area.bottom - content_top),
                 SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

void SessionTabHostWindow::RebuildTabs() {
  if (!tabs_) return;
  hide_tab_tooltip(tab_tooltip_);
  updating_tabs_ = true;
  TabCtrl_DeleteAllItems(tabs_);
  std::vector<session_tab_labels::Tab> labels;
  labels.reserve(entries_.size());
  for (const Entry& entry : entries_)
    labels.push_back({entry.id, entry.label, entry.device_hint});
  for (size_t i = 0; i < entries_.size(); ++i) {
    const auto suffix = session_tab_labels::Disambiguator(labels[i], labels);
    Entry& entry = entries_[i];
    entry.caption =
        session_tab_labels::AccessibleCaption(entry.full_label, suffix);
    TCITEMW item{};
    item.mask = TCIF_TEXT | TCIF_PARAM;
    item.pszText = entry.caption.data();
    item.lParam = static_cast<LPARAM>(suffix.size());
    TabCtrl_InsertItem(tabs_, static_cast<int>(i), &item);
  }
  const auto selected = model_.selected();
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (selected == entries_[i].id) {
      TabCtrl_SetCurSel(tabs_, static_cast<int>(i));
      break;
    }
  }
  updating_tabs_ = false;
  Layout();
}

void SessionTabHostWindow::SyncVisibility() {
  const auto selected = model_.selected();
  const void* active_session = nullptr;
  for (Entry& entry : entries_) {
    const bool active = selected == entry.id &&
        std::none_of(model_.tabs().begin(), model_.tabs().end(),
            [&entry](const SessionTabModel::Tab& tab) {
              return tab.id == entry.id && tab.closing;
            });
    entry.window->SetTabVisible(active);
    if (active) active_session = entry.window->session_identity();
  }
  if (visibility_) visibility_->SetActive(active_session);
  if (selected) {
    if (Entry* entry = Find(*selected)) entry->window->FocusChildContent();
  }
}

void SessionTabHostWindow::SelectIndex(int index) {
  if (index < 0 || static_cast<size_t>(index) >= entries_.size()) return;
  if (!model_.Select(entries_[static_cast<size_t>(index)].id)) {
    RebuildTabs();
    return;
  }
  SyncVisibility();
}

void SessionTabHostWindow::CloseTab(SessionTabModel::Id id) {
  Entry* entry = Find(id);
  trace_tab_lifecycle("close-tab-request", GetHandle(), id,
                      entry ? entry->window->GetHandle() : nullptr,
                      entries_.size(), model_.selected().value_or(0));
  const auto selected_before = model_.selected();
  if (!entry || !model_.BeginClose(id)) return;
  entry->window->SetTabVisible(false);
  RebuildTabs();
  SyncVisibility();
  const HWND child = entry->window->GetHandle();
  trace_tab_lifecycle("close-tab-post", GetHandle(), id, child,
                      entries_.size(), model_.selected().value_or(0));
  HWND post_target = child;
  if (consume_test_close_post_failure()) {
    // Exercise the real Win32 failure path while leaving the live child HWND
    // untouched. This injection is unavailable outside native test runs.
    // Do not use -1: the test run showed it closing the host instead of failing
    // safely as an invalid child-window target.
    post_target = reinterpret_cast<HWND>(static_cast<intptr_t>(0xDEADBEEF));
    trace_tab_lifecycle("close-tab-test-inject-invalid-hwnd", GetHandle(), id,
                        child, entries_.size(),
                        model_.selected().value_or(0));
  }
  SetLastError(ERROR_SUCCESS);
  if (!PostMessageW(post_target, WM_CLOSE, 0, 0)) {
    const DWORD error = GetLastError();
    trace_tab_lifecycle("close-tab-post-failed", GetHandle(), id, child,
                        entries_.size(), model_.selected().value_or(0));
    TraceRunnerLifecycle("close-tab-post-failed", child, WM_CLOSE, error);
    if (model_.RollbackFailedClose(id, selected_before,
                                   IsWindow(child) != FALSE)) {
      TraceRunnerLifecycle("close-tab-post-rollback", child, WM_CLOSE, error);
      trace_tab_lifecycle("close-tab-post-rollback", GetHandle(), id, child,
                          entries_.size(), model_.selected().value_or(0));
      RebuildTabs();
      SyncVisibility();
    }
  }
}

void SessionTabHostWindow::CloseAll() {
  trace_tab_lifecycle("close-all", GetHandle(), 0, nullptr,
                      entries_.size(), model_.selected().value_or(0));
  closing_container_ = true;
  for (Entry& entry : entries_) {
    if (model_.BeginClose(entry.id)) {
      entry.window->SetTabVisible(false);
      PostMessageW(entry.window->GetHandle(), WM_CLOSE, 0, 0);
    }
  }
  RebuildTabs();
  SyncVisibility();
}

SessionTabHostWindow::Entry* SessionTabHostWindow::Find(
    SessionTabModel::Id id) {
  const auto it = std::find_if(entries_.begin(), entries_.end(),
      [id](const Entry& entry) { return entry.id == id; });
  return it == entries_.end() ? nullptr : &*it;
}

LRESULT SessionTabHostWindow::MessageHandler(HWND window, UINT const message,
    WPARAM const wparam, LPARAM const lparam) noexcept {
  if (message == WM_ERASEBKGND) {
    HDC dc = reinterpret_cast<HDC>(wparam);
    RECT client{};
    GetClientRect(window, &client);
    HBRUSH background = CreateSolidBrush(kHeaderBackground);
    FillRect(dc, &client, background);
    DeleteObject(background);
    RECT separator{client.left,
                   scale_for_window(window, kTabBarHeight) -
                       scale_for_window(window, 1),
                   client.right,
                   scale_for_window(window, kTabBarHeight)};
    HBRUSH border = CreateSolidBrush(kHeaderBorder);
    FillRect(dc, &separator, border);
    DeleteObject(border);
    return 1;
  }
  if (message == WM_DRAWITEM) {
    const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
    if (item && item->hwndItem == tabs_) {
      draw_tab_item(*item, tabs_);
      return TRUE;
    }
    if (item && item->hwndItem == new_button_) {
      draw_header_button(*item);
      return TRUE;
    }
  }
  if (message == WM_CLOSE || message == WM_DESTROY || message == WM_NCDESTROY) {
    trace_tab_lifecycle(message == WM_CLOSE ? "host-wm-close" :
                        message == WM_DESTROY ? "host-wm-destroy" :
                                                "host-wm-ncdestroy",
                        window, 0, nullptr, entries_.size(),
                        model_.selected().value_or(0));
  }
  if (message == WM_SIZE) {
    Layout();
  } else if (message == WM_NOTIFY && !updating_tabs_) {
    const auto* header = reinterpret_cast<const NMHDR*>(lparam);
    if (header && header->hwndFrom == tabs_ && header->code == TCN_SELCHANGING) {
      const auto selected = model_.selected();
      if (selected) {
        if (Entry* entry = Find(*selected)) entry->window->SetTabInputActive(false);
      }
    } else if (header && header->hwndFrom == tabs_ && header->code == TCN_SELCHANGE) {
      SelectIndex(TabCtrl_GetCurSel(tabs_));
      return 0;
    }
  } else if (message == WM_COMMAND) {
    const int id = LOWORD(wparam);
    if (id == kNewButtonId) {
      const auto selected = model_.selected();
      if (selected) {
        if (Entry* entry = Find(*selected)) entry->window->SetTabInputActive(false);
      }
      if (show_home_) show_home_();
      return 0;
    }
    if (id == kTabCloseCommandId) {
      const auto index = static_cast<size_t>(lparam);
      if (index < entries_.size()) CloseTab(entries_[index].id);
      return 0;
    }
  } else if (message == WM_ACTIVATE) {
    if (LOWORD(wparam) == WA_INACTIVE) {
      hide_tab_tooltip(tab_tooltip_);
      const HWND next_window = reinterpret_cast<HWND>(lparam);
      if (!visibility_ || !visibility_->OwnsActiveWindow(next_window)) {
        if (visibility_) visibility_->SetActive(nullptr);
        const auto selected = model_.selected();
        if (selected) {
          if (Entry* entry = Find(*selected)) entry->window->SetTabInputActive(false);
        }
      }
    } else {
      SyncVisibility();
    }
  } else if (message == WM_CLOSE) {
    if (!entries_.empty()) {
      if (!closing_container_) CloseAll();
      return 0;
    }
    if (visibility_) visibility_->SetActive(nullptr);
  }
  return Win32Window::MessageHandler(window, message, wparam, lparam);
}
