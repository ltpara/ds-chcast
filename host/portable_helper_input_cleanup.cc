#define NOMINMAX
#include "portable_helper_input_cleanup.h"

#include "chast/desktop.h"
#include "chast/input.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace chast::host {
namespace {

constexpr unsigned kMaximumApiAttempts = 3;

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

bool state_empty(const PortableHelperInputJournalState& state) {
  return state.keys.empty() && state.mouse_buttons == 0 &&
      state.touches.empty() && !state.pen_down;
}

bool nonzero(const PortableHelperInstanceId& id) {
  return std::any_of(id.begin(), id.end(),
                     [](uint8_t value) { return value != 0; });
}

bool same_scope(const PortableHelperInputJournalScope& left,
                const PortableHelperInputJournalScope& right) {
  return left.instance_id == right.instance_id &&
      left.connection_id == right.connection_id &&
      left.windows_session_id == right.windows_session_id &&
      left.generation == right.generation;
}

bool is_modifier(const PortableHelperJournalKey& key) {
  switch (key.virtual_key) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
      return true;
    default:
      break;
  }
  if ((key.flags & KEYEVENTF_SCANCODE) == 0) return false;
  const bool extended = (key.flags & KEYEVENTF_EXTENDEDKEY) != 0;
  switch (key.scan_code) {
    case 0x2A: case 0x36: case 0x38:
      return true;
    case 0x1D:
      return true;  // Left/right Control share the set-1 make code.
    case 0x5B: case 0x5C:
      return extended;
    default:
      return false;
  }
}

void append_button_release(uint16_t buttons, uint16_t bit, DWORD flags,
                           DWORD data,
                           std::vector<INPUT>* inputs) {
  if ((buttons & bit) == 0) return;
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = flags;
  input.mi.mouseData = data;
  inputs->push_back(input);
}

bool current_desktop_matches(const std::string& expected,
                             std::string* error) {
  if (expected.empty())
    return fail(error, "input cleanup has no recorded desktop identity");
  const chast::InputDesktopStatus status = chast::input_desktop_status();
  if (status.kind == chast::InputDesktopKind::Unavailable ||
      status.name != expected || !chast::on_input_desktop())
    return fail(error, "input desktop changed before cleanup could be confirmed");
  return true;
}

bool send_all(const std::vector<INPUT>& inputs,
              const std::string& expected_desktop, const char* action,
              std::string* error) {
  if (inputs.empty()) return true;
  for (unsigned attempt = 0; attempt < kMaximumApiAttempts; ++attempt) {
    if (!current_desktop_matches(expected_desktop, error))
      return false;
    SetLastError(ERROR_SUCCESS);
    const UINT inserted = SendInput(static_cast<UINT>(inputs.size()),
                                    const_cast<INPUT*>(inputs.data()),
                                    sizeof(INPUT));
    const DWORD code = inserted == inputs.size() ? ERROR_SUCCESS : GetLastError();
    if (inserted == inputs.size()) return true;
    if (code != ERROR_SUCCESS && code != ERROR_NOT_READY &&
        code != ERROR_ACCESS_DENIED && code != ERROR_INVALID_HANDLE) {
      if (error != nullptr)
        *error = std::string(action) + " failed (Win32 error " +
            std::to_string(code) + ")";
      return false;
    }
    if (attempt + 1 != kMaximumApiAttempts) Sleep(1);
  }
  if (error != nullptr)
    *error = std::string(action) + " did not release the complete recorded set";
  return false;
}

using CreatePointerDeviceFn =
    void* (WINAPI*)(POINTER_INPUT_TYPE, ULONG, POINTER_FEEDBACK_MODE);
using InjectPointerInputFn =
    BOOL(WINAPI*)(void*, const POINTER_TYPE_INFO*, UINT32);
using DestroyPointerDeviceFn = void(WINAPI*)(void*);

class PointerDevice final {
 public:
  PointerDevice() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) return;
    create_ = reinterpret_cast<CreatePointerDeviceFn>(
        GetProcAddress(user32, "CreateSyntheticPointerDevice"));
    inject_ = reinterpret_cast<InjectPointerInputFn>(
        GetProcAddress(user32, "InjectSyntheticPointerInput"));
    destroy_ = reinterpret_cast<DestroyPointerDeviceFn>(
        GetProcAddress(user32, "DestroySyntheticPointerDevice"));
    if (create_ == nullptr || inject_ == nullptr || destroy_ == nullptr) return;
    device_ = create_(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
  }
  ~PointerDevice() {
    if (device_ != nullptr && destroy_ != nullptr) destroy_(device_);
  }
  PointerDevice(const PointerDevice&) = delete;
  PointerDevice& operator=(const PointerDevice&) = delete;

  bool Release(const POINTER_TYPE_INFO& input,
               const std::string& expected_desktop, std::string* error) {
    if (device_ == nullptr || inject_ == nullptr)
      return fail(error, "synthetic pen cleanup API is unavailable");
    if (!current_desktop_matches(expected_desktop, error)) return false;
    SetLastError(ERROR_SUCCESS);
    if (inject_(device_, &input, 1)) return true;
    if (error != nullptr)
      *error = "synthetic pen release failed (Win32 error " +
          std::to_string(GetLastError()) + ")";
    return false;
  }

 private:
  CreatePointerDeviceFn create_ = nullptr;
  InjectPointerInputFn inject_ = nullptr;
  DestroyPointerDeviceFn destroy_ = nullptr;
  void* device_ = nullptr;
};

bool inject_touch_releases(const std::vector<POINTER_TOUCH_INFO>& contacts,
                           const std::string& expected_desktop,
                           std::string* error) {
  if (contacts.empty()) return true;
  if (!InitializeTouchInjection(chast::kMaxTouchContacts,
                                TOUCH_FEEDBACK_NONE))
    return fail(error, "cannot initialize touch cleanup in the target session");
  for (unsigned attempt = 0; attempt < kMaximumApiAttempts; ++attempt) {
    if (contacts.size() > chast::kMaxTouchContacts)
      return fail(error, "touch cleanup plan exceeds the supported contact count");
    if (!current_desktop_matches(expected_desktop, error)) return false;
    SetLastError(ERROR_SUCCESS);
    if (InjectTouchInput(static_cast<UINT32>(contacts.size()),
                         const_cast<POINTER_TOUCH_INFO*>(contacts.data())))
      return true;
    const DWORD code = GetLastError();
    if (code != ERROR_NOT_READY || attempt + 1 == kMaximumApiAttempts) {
      if (error != nullptr)
        *error = "touch contact release failed (Win32 error " +
            std::to_string(code) + ")";
      return false;
    }
    Sleep(1);
  }
  return fail(error, "touch contact release did not complete");
}

bool validate_release_plan(const PortableHelperInputCleanupPlan& plan,
                           std::string* error) {
  const bool has_actions = !plan.mouse_buttons_up.empty() ||
      !plan.touch_contacts_up.empty() || plan.pen_up ||
      !plan.keys_up.empty();
  if (has_actions && plan.input_desktop_name.empty())
    return fail(error, "nonempty input cleanup plan has no desktop identity");
  if (plan.touch_contacts_up.size() > chast::kMaxTouchContacts ||
      plan.keys_up.size() > 512 || plan.mouse_buttons_up.size() > 5)
    return fail(error, "input cleanup plan exceeds a supported bound");

  uint16_t mouse_buttons = 0;
  for (const INPUT& input : plan.mouse_buttons_up) {
    if (input.type != INPUT_MOUSE || input.mi.dx != 0 || input.mi.dy != 0 ||
        input.mi.time != 0 || input.mi.dwExtraInfo != 0)
      return fail(error, "input cleanup mouse entry has the wrong event type");
    uint16_t button = 0;
    switch (input.mi.dwFlags) {
      case MOUSEEVENTF_LEFTUP:
        if (input.mi.mouseData != 0)
          return fail(error, "left-button cleanup has unexpected mouse data");
        button = chast::kMouseLeft;
        break;
      case MOUSEEVENTF_RIGHTUP:
        if (input.mi.mouseData != 0)
          return fail(error, "right-button cleanup has unexpected mouse data");
        button = chast::kMouseRight;
        break;
      case MOUSEEVENTF_MIDDLEUP:
        if (input.mi.mouseData != 0)
          return fail(error, "middle-button cleanup has unexpected mouse data");
        button = chast::kMouseMiddle;
        break;
      case MOUSEEVENTF_XUP:
        if (input.mi.mouseData == XBUTTON1)
          button = chast::kMouseBack;
        else if (input.mi.mouseData == XBUTTON2)
          button = chast::kMouseForward;
        else
          return fail(error, "extra-button cleanup has an unknown button id");
        break;
      default:
        return fail(error, "input cleanup mouse entry is not a button-up event");
    }
    if ((mouse_buttons & button) != 0)
      return fail(error, "input cleanup plan repeats a mouse button release");
    mouse_buttons = static_cast<uint16_t>(mouse_buttons | button);
  }

  constexpr DWORD kAllowedKeyFlags = KEYEVENTF_EXTENDEDKEY |
      KEYEVENTF_KEYUP | KEYEVENTF_SCANCODE;
  for (size_t index = 0; index < plan.keys_up.size(); ++index) {
    const INPUT& input = plan.keys_up[index];
    if (input.type != INPUT_KEYBOARD ||
        (input.ki.dwFlags & KEYEVENTF_KEYUP) == 0 ||
        (input.ki.dwFlags & ~kAllowedKeyFlags) != 0 ||
        (input.ki.wVk == 0 && input.ki.wScan == 0) ||
        input.ki.dwExtraInfo !=
            static_cast<ULONG_PTR>(chast::kInputInjectionTag))
      return fail(error, "input cleanup keyboard entry is not a tagged key-up event");
    for (size_t prior = 0; prior < index; ++prior) {
      const INPUT& other = plan.keys_up[prior];
      if (other.ki.wVk == input.ki.wVk &&
          other.ki.wScan == input.ki.wScan &&
          (other.ki.dwFlags & ~KEYEVENTF_KEYUP) ==
              (input.ki.dwFlags & ~KEYEVENTF_KEYUP))
        return fail(error, "input cleanup plan repeats a keyboard release");
    }
  }

  for (size_t index = 0; index < plan.touch_contacts_up.size(); ++index) {
    const POINTER_TOUCH_INFO& touch = plan.touch_contacts_up[index];
    if (touch.pointerInfo.pointerType != PT_TOUCH ||
        touch.pointerInfo.pointerId >= chast::kMaxTouchContacts ||
        touch.pointerInfo.pointerFlags !=
            (POINTER_FLAG_UP | POINTER_FLAG_CANCELED) ||
        touch.pointerInfo.frameId != 0 ||
        touch.pointerInfo.sourceDevice != nullptr ||
        touch.pointerInfo.hwndTarget != nullptr ||
        touch.pointerInfo.ptHimetricLocation.x != 0 ||
        touch.pointerInfo.ptHimetricLocation.y != 0 ||
        touch.pointerInfo.ptPixelLocationRaw.x != 0 ||
        touch.pointerInfo.ptPixelLocationRaw.y != 0 ||
        touch.pointerInfo.ptHimetricLocationRaw.x != 0 ||
        touch.pointerInfo.ptHimetricLocationRaw.y != 0 ||
        touch.pointerInfo.dwTime != 0 ||
        touch.pointerInfo.historyCount != 0 ||
        touch.pointerInfo.InputData != 0 ||
        touch.pointerInfo.dwKeyStates != 0 ||
        touch.pointerInfo.PerformanceCount != 0 ||
        touch.pointerInfo.ButtonChangeType != POINTER_CHANGE_NONE ||
        touch.touchMask != 0 || touch.rcContact.left != 0 ||
        touch.rcContact.top != 0 || touch.rcContact.right != 0 ||
        touch.rcContact.bottom != 0 || touch.orientation != 0 ||
        touch.pressure != 0)
      return fail(error, "input cleanup touch entry is not a canceled contact-up event");
    for (size_t prior = 0; prior < index; ++prior) {
      if (plan.touch_contacts_up[prior].pointerInfo.pointerId ==
          touch.pointerInfo.pointerId)
        return fail(error, "input cleanup plan repeats a touch contact release");
    }
  }

  if (plan.pen_up) {
    constexpr uint8_t kAllowedPenFlags = PEN_FLAG_BARREL | PEN_FLAG_INVERTED |
        PEN_FLAG_ERASER;
    if (plan.pen.pressure > 1024 ||
        (plan.pen.flags & ~kAllowedPenFlags) != 0 ||
        plan.pen.tilt_x < -90 || plan.pen.tilt_x > 90 ||
        plan.pen.tilt_y < -90 || plan.pen.tilt_y > 90)
      return fail(error, "input cleanup pen state is outside supported release bounds");
  } else if (plan.pen.x != 0 || plan.pen.y != 0 ||
             plan.pen.pressure != 0 || plan.pen.flags != 0 ||
             plan.pen.tilt_x != 0 || plan.pen.tilt_y != 0) {
    return fail(error, "input cleanup plan has pen state without a pen release");
  }
  return true;
}

}  // namespace

bool make_portable_helper_input_cleanup_plan(
    const PortableHelperInputJournalScope& expected_scope,
    const PortableHelperInputJournalImage& image,
    PortableHelperInputCleanupPlan* plan, std::string* error) {
  if (error != nullptr) error->clear();
  if (plan == nullptr) return fail(error, "missing input cleanup plan output");
  *plan = {};
  std::vector<uint8_t> validation;
  if (!nonzero(expected_scope.instance_id) ||
      !nonzero(expected_scope.connection_id) ||
      expected_scope.windows_session_id == 0 || expected_scope.generation == 0 ||
      !same_scope(expected_scope, image.scope) ||
      !encode_portable_helper_input_journal(image, &validation, error))
    return fail(error, "input cleanup image is invalid or belongs to another scope");
  if (image.released || state_empty(image.possible)) return true;
  if (image.input_desktop_name.empty())
    return fail(error, "input cleanup image has no owning desktop");

  plan->input_desktop_name = image.input_desktop_name;
  const uint16_t buttons = image.possible.mouse_buttons;
  append_button_release(buttons, chast::kMouseLeft, MOUSEEVENTF_LEFTUP, 0,
                        &plan->mouse_buttons_up);
  append_button_release(buttons, chast::kMouseRight, MOUSEEVENTF_RIGHTUP, 0,
                        &plan->mouse_buttons_up);
  append_button_release(buttons, chast::kMouseMiddle, MOUSEEVENTF_MIDDLEUP, 0,
                        &plan->mouse_buttons_up);
  append_button_release(buttons, chast::kMouseBack, MOUSEEVENTF_XUP, XBUTTON1,
                        &plan->mouse_buttons_up);
  append_button_release(buttons, chast::kMouseForward, MOUSEEVENTF_XUP, XBUTTON2,
                        &plan->mouse_buttons_up);

  plan->touch_contacts_up.reserve(image.possible.touches.size());
  for (const auto& touch : image.possible.touches) {
    POINTER_TOUCH_INFO info{};
    info.pointerInfo.pointerType = PT_TOUCH;
    info.pointerInfo.pointerId = touch.pointer_id;
    info.pointerInfo.pointerFlags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
    info.pointerInfo.ptPixelLocation = POINT{touch.x, touch.y};
    plan->touch_contacts_up.push_back(info);
  }

  plan->pen_up = image.possible.pen_down;
  if (plan->pen_up) plan->pen = image.possible.pen;

  std::vector<PortableHelperJournalKey> keys = image.possible.keys;
  std::sort(keys.begin(), keys.end(), [](const auto& left, const auto& right) {
    if (is_modifier(left) != is_modifier(right)) return !is_modifier(left);
    if (left.virtual_key != right.virtual_key)
      return left.virtual_key > right.virtual_key;
    return left.scan_code > right.scan_code;
  });
  plan->keys_up.reserve(keys.size());
  for (const auto& key : keys) {
    if (key.extra_info != chast::kInputInjectionTag)
      return fail(error, "journal key is not tagged as a host-injected input");
    if (key.extra_info > std::numeric_limits<ULONG_PTR>::max())
      return fail(error, "journal key tag exceeds this process pointer width");
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key.virtual_key;
    input.ki.wScan = key.scan_code;
    input.ki.dwFlags = key.flags | KEYEVENTF_KEYUP;
    input.ki.time = key.time;
    input.ki.dwExtraInfo = static_cast<ULONG_PTR>(key.extra_info);
    plan->keys_up.push_back(input);
  }
  return true;
}

bool execute_portable_helper_input_cleanup_plan(
    const PortableHelperInputCleanupPlan& plan, std::string* error) {
  if (error != nullptr) error->clear();
  if (!validate_release_plan(plan, error)) return false;
  if (plan.mouse_buttons_up.empty() && plan.touch_contacts_up.empty() &&
      !plan.pen_up && plan.keys_up.empty())
    return true;

  chast::enable_physical_desktop_coordinates();
  std::string desktop_error;
  bool switched = false;
  if (!chast::follow_input_desktop(&desktop_error, &switched) ||
      !current_desktop_matches(plan.input_desktop_name, error)) {
    if (error != nullptr && error->empty()) *error = desktop_error;
    return false;
  }

  if (!send_all(plan.mouse_buttons_up, plan.input_desktop_name,
                "mouse button release", error)) return false;

  if (!plan.touch_contacts_up.empty()) {
    if (!current_desktop_matches(plan.input_desktop_name, error) ||
        !inject_touch_releases(plan.touch_contacts_up,
                               plan.input_desktop_name, error))
      return false;
  }

  if (plan.pen_up) {
    if (!current_desktop_matches(plan.input_desktop_name, error)) return false;
    const int32_t left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int32_t top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    POINTER_TYPE_INFO input{};
    input.type = PT_PEN;
    input.penInfo.pointerInfo.pointerType = PT_PEN;
    input.penInfo.pointerInfo.pointerId = 0;
    input.penInfo.pointerInfo.pointerFlags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
    input.penInfo.pointerInfo.ptPixelLocation.x = plan.pen.x - left;
    input.penInfo.pointerInfo.ptPixelLocation.y = plan.pen.y - top;
    input.penInfo.penFlags = plan.pen.flags;
    if (plan.pen.pressure != 0) {
      input.penInfo.penMask |= PEN_MASK_PRESSURE;
      input.penInfo.pressure = plan.pen.pressure;
    }
    if (plan.pen.tilt_x != 0) {
      input.penInfo.penMask |= PEN_MASK_TILT_X;
      input.penInfo.tiltX = plan.pen.tilt_x;
    }
    if (plan.pen.tilt_y != 0) {
      input.penInfo.penMask |= PEN_MASK_TILT_Y;
      input.penInfo.tiltY = plan.pen.tilt_y;
    }
    PointerDevice device;
    if (!device.Release(input, plan.input_desktop_name, error)) return false;
  }

  return send_all(plan.keys_up, plan.input_desktop_name,
                  "keyboard key release", error);
}

bool recover_portable_helper_input_journal(
    const PortableHelperInputJournalScope& expected_scope,
    PortableHelperInputJournalFileStore& store,
    const PortableHelperAuthenticatedInputCleanupExecutor& executor,
    std::string* error) {
  if (error != nullptr) error->clear();
  PortableHelperInputJournalImage image;
  bool found = false;
  if (!store.Load(&image, &found, error) || !found ||
      !same_scope(expected_scope, image.scope))
    return fail(error, "input cleanup requires the exact existing journal scope");
  if (image.released) return true;

  PortableHelperInputCleanupPlan plan;
  if (!make_portable_helper_input_cleanup_plan(expected_scope, image, &plan,
                                               error))
    return false;
  const bool has_effects = !plan.mouse_buttons_up.empty() ||
      !plan.touch_contacts_up.empty() || plan.pen_up || !plan.keys_up.empty();
  if (has_effects && !executor)
    return fail(error, "input cleanup has no authenticated target-session executor");
  if (has_effects) {
    try {
      if (!executor(expected_scope, image, error)) {
        if (error != nullptr && error->empty())
          *error = "authenticated input cleanup executor did not confirm release";
        return false;
      }
    } catch (...) {
      return fail(error, "authenticated input cleanup executor raised an exception");
    }
  }

  PortableHelperInputJournal recovered(expected_scope,
      [&store](const PortableHelperInputJournalImage& next,
               std::string* persist_error) {
        return store.Persist(next, persist_error);
      });
  if (!recovered.Restore(image, error) ||
      !store.ResumeLoadedRecord(recovered, error) ||
      !recovered.MarkReleased(error))
    return false;
  return true;
}

}  // namespace chast::host
