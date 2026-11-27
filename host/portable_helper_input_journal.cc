#define NOMINMAX
#include <windows.h>

#include "portable_helper_input_journal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace chast::host {
namespace {

constexpr std::array<uint8_t, 8> kMagic = {'C', 'H', 'A', 'S', 'I', 'N', 'P', '1'};
constexpr uint16_t kVersion = 3;
constexpr size_t kMaximumKeys = 512;
constexpr size_t kMaximumBytes = 64 * 1024;
constexpr size_t kMaximumDesktopNameBytes = 64;
constexpr uint16_t kJournalMouseMask = chast::kMouseLeft | chast::kMouseRight |
    chast::kMouseMiddle | chast::kMouseBack | chast::kMouseForward;

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

bool nonzero(const PortableHelperInstanceId& id) {
  return std::any_of(id.begin(), id.end(), [](uint8_t byte) { return byte != 0; });
}

bool valid_scope(const PortableHelperInputJournalScope& scope) {
  return nonzero(scope.instance_id) && nonzero(scope.connection_id) &&
         scope.windows_session_id != 0 && scope.generation != 0;
}

bool valid_desktop_name(const std::string& name, bool allow_empty) {
  if (name.empty()) return allow_empty;
  if (name.size() > kMaximumDesktopNameBytes) return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char value) {
    return (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9') || value == '_' || value == '-';
  });
}

bool key_less(const PortableHelperJournalKey& left,
              const PortableHelperJournalKey& right) {
  if (left.virtual_key != right.virtual_key)
    return left.virtual_key < right.virtual_key;
  if (left.scan_code != right.scan_code) return left.scan_code < right.scan_code;
  if (left.flags != right.flags) return left.flags < right.flags;
  if (left.time != right.time) return left.time < right.time;
  return left.extra_info < right.extra_info;
}

bool same_key(const PortableHelperJournalKey& left,
              const PortableHelperJournalKey& right) {
  return left.virtual_key == right.virtual_key &&
         left.scan_code == right.scan_code && left.flags == right.flags &&
         left.time == right.time && left.extra_info == right.extra_info;
}

bool same_key_identity(const PortableHelperJournalKey& left,
                       const PortableHelperJournalKey& right) {
  return left.virtual_key == right.virtual_key &&
         left.scan_code == right.scan_code && left.flags == right.flags &&
         left.extra_info == right.extra_info;
}

bool touch_less(const PortableHelperJournalTouch& left,
                const PortableHelperJournalTouch& right) {
  return left.pointer_id < right.pointer_id;
}

bool same_touch(const PortableHelperJournalTouch& left,
                const PortableHelperJournalTouch& right) {
  return left.pointer_id == right.pointer_id && left.x == right.x &&
         left.y == right.y && left.pressure == right.pressure;
}

bool same_pen(const PortableHelperJournalPen& left,
              const PortableHelperJournalPen& right) {
  return left.x == right.x && left.y == right.y &&
         left.pressure == right.pressure && left.flags == right.flags &&
         left.tilt_x == right.tilt_x && left.tilt_y == right.tilt_y;
}

bool state_equal(const PortableHelperInputJournalState& left,
                 const PortableHelperInputJournalState& right) {
  if (left.mouse_buttons != right.mouse_buttons ||
      left.pen_down != right.pen_down || left.keys.size() != right.keys.size() ||
      left.touches.size() != right.touches.size())
    return false;
  for (size_t i = 0; i < left.keys.size(); ++i)
    if (!same_key(left.keys[i], right.keys[i])) return false;
  for (size_t i = 0; i < left.touches.size(); ++i)
    if (!same_touch(left.touches[i], right.touches[i])) return false;
  return !left.pen_down || same_pen(left.pen, right.pen);
}

bool state_empty(const PortableHelperInputJournalState& state) {
  return state.keys.empty() && state.mouse_buttons == 0 &&
      state.touches.empty() && !state.pen_down;
}

void normalize(PortableHelperInputJournalState* state) {
  std::sort(state->keys.begin(), state->keys.end(), key_less);
  std::sort(state->touches.begin(), state->touches.end(), touch_less);
  if (!state->pen_down) state->pen = {};
}

bool valid_state(const PortableHelperInputJournalState& state,
                 bool require_canonical, std::string* error) {
  if (state.keys.size() > kMaximumKeys ||
      state.touches.size() > chast::kMaxTouchContacts ||
      (state.mouse_buttons & ~kJournalMouseMask) != 0)
    return fail(error, "input journal state exceeds a supported bound");
  for (size_t i = 0; i < state.keys.size(); ++i) {
    const auto& key = state.keys[i];
    const uint32_t allowed_flags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_SCANCODE;
    if (key.virtual_key > 0xFF || (key.flags & ~allowed_flags) != 0 ||
        (key.virtual_key == 0 && (key.flags & KEYEVENTF_SCANCODE) == 0) ||
        (i > 0 && (same_key_identity(state.keys[i - 1], key) ||
                   (require_canonical && key_less(key, state.keys[i - 1])))))
      return fail(error, "input journal contains an invalid or duplicate key");
  }
  for (size_t i = 0; i < state.touches.size(); ++i) {
    if (state.touches[i].pointer_id >= chast::kMaxTouchContacts ||
        state.touches[i].pressure > 1024 ||
        (i > 0 && (state.touches[i - 1].pointer_id == state.touches[i].pointer_id ||
                   (require_canonical && touch_less(state.touches[i],
                                                     state.touches[i - 1])))))
      return fail(error, "input journal contains an invalid or duplicate touch");
  }
  if (!state.pen_down &&
      (state.pen.x != 0 || state.pen.y != 0 || state.pen.pressure != 0 ||
       state.pen.flags != 0 || state.pen.tilt_x != 0 || state.pen.tilt_y != 0))
    return fail(error, "inactive pen state carries data");
  if (state.pen_down &&
      (state.pen.pressure > 1024 ||
       (state.pen.flags & ~(PEN_FLAG_BARREL | PEN_FLAG_INVERTED |
                            PEN_FLAG_ERASER)) != 0))
    return fail(error, "input journal pen pressure is out of range");
  return true;
}

PortableHelperInputJournalState union_state(
    const PortableHelperInputJournalState& before,
    const PortableHelperInputJournalState& after) {
  PortableHelperInputJournalState result = before;
  result.mouse_buttons |= after.mouse_buttons;
  for (const auto& key : after.keys) {
    const auto found = std::find_if(result.keys.begin(), result.keys.end(),
        [&key](const auto& existing) { return same_key_identity(existing, key); });
    if (found == result.keys.end()) result.keys.push_back(key);
  }
  for (const auto& touch : after.touches) {
    const auto found = std::find_if(result.touches.begin(), result.touches.end(),
        [&touch](const auto& existing) {
          return existing.pointer_id == touch.pointer_id;
        });
    if (found == result.touches.end()) result.touches.push_back(touch);
    else *found = touch;  // Prefer the latest possibly accepted position.
  }
  if (after.pen_down) result.pen = after.pen;
  result.pen_down = before.pen_down || after.pen_down;
  normalize(&result);
  return result;
}

bool state_is_superset(const PortableHelperInputJournalState& possible,
                       const PortableHelperInputJournalState& committed) {
  if ((possible.mouse_buttons & committed.mouse_buttons) !=
      committed.mouse_buttons)
    return false;
  for (const auto& key : committed.keys) {
    if (std::none_of(possible.keys.begin(), possible.keys.end(),
                     [&key](const auto& candidate) {
                       return same_key_identity(candidate, key);
                     }))
      return false;
  }
  for (const auto& touch : committed.touches) {
    if (std::none_of(possible.touches.begin(), possible.touches.end(),
                     [&touch](const auto& candidate) {
                       return candidate.pointer_id == touch.pointer_id;
                     }))
      return false;
  }
  return !committed.pen_down || possible.pen_down;
}

bool image_valid(const PortableHelperInputJournalImage& image,
                 std::string* error) {
  if (!valid_scope(image.scope) ||
      !valid_desktop_name(image.input_desktop_name, true))
    return fail(error, "input journal scope is invalid");
  if (!valid_state(image.committed, true, error) ||
      !valid_state(image.possible, true, error))
    return false;
  if (image.pending) {
    if (image.pending_sequence == 0 ||
        image.pending_sequence <= image.last_sequence ||
        static_cast<uint8_t>(image.pending_api) <
            static_cast<uint8_t>(chast::InputSideEffectApi::SendInput) ||
        static_cast<uint8_t>(image.pending_api) >
            static_cast<uint8_t>(chast::InputSideEffectApi::InjectSyntheticPointerInput) ||
        !state_is_superset(image.possible, image.committed))
      return fail(error, "pending input journal image is inconsistent");
  } else if (image.pending_sequence != 0 ||
             image.pending_api != chast::InputSideEffectApi::SendInput ||
             !state_equal(image.committed, image.possible)) {
    return fail(error, "committed input journal image is inconsistent");
  }
  if (image.released &&
      (image.pending || !image.committed.keys.empty() ||
       image.committed.mouse_buttons != 0 || !image.committed.touches.empty() ||
       image.committed.pen_down || !image.possible.keys.empty() ||
       image.possible.mouse_buttons != 0 || !image.possible.touches.empty() ||
       image.possible.pen_down || !image.input_desktop_name.empty()))
    return fail(error, "released input journal still contains possible held state");
  if ((!state_empty(image.committed) || !state_empty(image.possible)) &&
      image.input_desktop_name.empty())
    return fail(error, "held input journal state has no desktop identity");
  return true;
}

void put16(std::vector<uint8_t>& bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void put32(std::vector<uint8_t>& bytes, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void put64(std::vector<uint8_t>& bytes, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

class Reader final {
 public:
  explicit Reader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
  bool take(void* output, size_t count) {
    if (output == nullptr || count > bytes_.size() - offset_) return false;
    std::memcpy(output, bytes_.data() + offset_, count);
    offset_ += count;
    return true;
  }
  bool get8(uint8_t* value) { return take(value, sizeof(*value)); }
  bool get16(uint16_t* value) {
    uint8_t raw[2]{};
    if (!take(raw, sizeof(raw))) return false;
    *value = static_cast<uint16_t>(raw[0]) |
             static_cast<uint16_t>(static_cast<uint16_t>(raw[1]) << 8);
    return true;
  }
  bool get32(uint32_t* value) {
    uint8_t raw[4]{};
    if (!take(raw, sizeof(raw))) return false;
    *value = 0;
    for (unsigned i = 0; i < 4; ++i)
      *value |= static_cast<uint32_t>(raw[i]) << (8 * i);
    return true;
  }
  bool get64(uint64_t* value) {
    uint8_t raw[8]{};
    if (!take(raw, sizeof(raw))) return false;
    *value = 0;
    for (unsigned i = 0; i < 8; ++i)
      *value |= static_cast<uint64_t>(raw[i]) << (8 * i);
    return true;
  }
  bool done() const { return offset_ == bytes_.size(); }

 private:
  const std::vector<uint8_t>& bytes_;
  size_t offset_ = 0;
};

void encode_state(const PortableHelperInputJournalState& state,
                  std::vector<uint8_t>* bytes) {
  put16(*bytes, static_cast<uint16_t>(state.keys.size()));
  put16(*bytes, state.mouse_buttons);
  put16(*bytes, static_cast<uint16_t>(state.touches.size()));
  bytes->push_back(state.pen_down ? 1 : 0);
  bytes->push_back(0);
  for (const auto& key : state.keys) {
    put16(*bytes, key.virtual_key);
    put16(*bytes, key.scan_code);
    put32(*bytes, key.flags);
    put32(*bytes, key.time);
    put64(*bytes, key.extra_info);
  }
  for (const auto& touch : state.touches) {
    put32(*bytes, touch.pointer_id);
    put32(*bytes, static_cast<uint32_t>(touch.x));
    put32(*bytes, static_cast<uint32_t>(touch.y));
    put16(*bytes, touch.pressure);
    put16(*bytes, 0);
  }
  if (state.pen_down) {
    put32(*bytes, static_cast<uint32_t>(state.pen.x));
    put32(*bytes, static_cast<uint32_t>(state.pen.y));
    put16(*bytes, state.pen.pressure);
    bytes->push_back(state.pen.flags);
    put16(*bytes, static_cast<uint16_t>(state.pen.tilt_x));
    put16(*bytes, static_cast<uint16_t>(state.pen.tilt_y));
  }
}

bool decode_state(Reader* reader, PortableHelperInputJournalState* state) {
  uint16_t key_count = 0, touch_count = 0;
  uint8_t pen_down = 0, reserved = 0;
  if (!reader->get16(&key_count) || key_count > kMaximumKeys ||
      !reader->get16(&state->mouse_buttons) ||
      !reader->get16(&touch_count) ||
      touch_count > chast::kMaxTouchContacts ||
      !reader->get8(&pen_down) || pen_down > 1 ||
      !reader->get8(&reserved) || reserved != 0)
    return false;
  state->pen_down = pen_down != 0;
  state->keys.resize(key_count);
  for (auto& key : state->keys) {
    if (!reader->get16(&key.virtual_key) || !reader->get16(&key.scan_code) ||
        !reader->get32(&key.flags) || !reader->get32(&key.time) ||
        !reader->get64(&key.extra_info))
      return false;
  }
  state->touches.resize(touch_count);
  for (auto& touch : state->touches) {
    uint32_t x = 0, y = 0;
    uint16_t reserved_touch = 0;
    if (!reader->get32(&touch.pointer_id) || !reader->get32(&x) ||
        !reader->get32(&y) || !reader->get16(&touch.pressure) ||
        !reader->get16(&reserved_touch) || reserved_touch != 0)
      return false;
    touch.x = static_cast<int32_t>(x);
    touch.y = static_cast<int32_t>(y);
  }
  if (state->pen_down) {
    uint32_t x = 0, y = 0;
    uint16_t tilt_x = 0, tilt_y = 0;
    if (!reader->get32(&x) || !reader->get32(&y) ||
        !reader->get16(&state->pen.pressure) ||
        !reader->get8(&state->pen.flags) || !reader->get16(&tilt_x) ||
        !reader->get16(&tilt_y))
      return false;
    state->pen.x = static_cast<int32_t>(x);
    state->pen.y = static_cast<int32_t>(y);
    state->pen.tilt_x = static_cast<int16_t>(tilt_x);
    state->pen.tilt_y = static_cast<int16_t>(tilt_y);
  }
  return true;
}

bool apply_send_input(const PortableHelperInputJournalState& base,
                      const INPUT& input,
                      PortableHelperInputJournalState* candidate,
                      std::string* error) {
  if (input.type == INPUT_KEYBOARD) {
    const KEYBDINPUT& key = input.ki;
    const DWORD allowed = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP |
        KEYEVENTF_SCANCODE | KEYEVENTF_UNICODE;
    if ((key.dwFlags & ~allowed) != 0 || key.wVk > 0xFF)
      return fail(error, "input journal received unsupported keyboard flags");
    if ((key.dwFlags & KEYEVENTF_UNICODE) != 0) {
      if (key.wVk != 0 || key.wScan == 0 ||
          (key.dwFlags & (KEYEVENTF_EXTENDEDKEY | KEYEVENTF_SCANCODE)) != 0)
        return fail(error, "input journal received a malformed Unicode input");
      *candidate = base;  // Unicode text does not leave a held key.
      return true;
    }
    if (key.wVk == 0 && (key.dwFlags & KEYEVENTF_SCANCODE) == 0)
      return fail(error, "input journal key has neither VK nor scan-code form");

    PortableHelperJournalKey stored;
    stored.virtual_key = key.wVk;
    stored.scan_code = key.wScan;
    stored.flags = key.dwFlags & ~KEYEVENTF_KEYUP;
    stored.time = key.time;
    stored.extra_info = static_cast<uint64_t>(key.dwExtraInfo);
    *candidate = base;
    const auto found = std::find_if(candidate->keys.begin(), candidate->keys.end(),
        [&stored](const auto& current) {
          return same_key_identity(current, stored);
        });
    if ((key.dwFlags & KEYEVENTF_KEYUP) != 0) {
      if (found != candidate->keys.end()) candidate->keys.erase(found);
    } else if (found == candidate->keys.end()) {
      if (candidate->keys.size() >= kMaximumKeys)
        return fail(error, "input journal key-state limit reached");
      candidate->keys.push_back(stored);
    }
    normalize(candidate);
    return true;
  }

  if (input.type != INPUT_MOUSE)
    return fail(error, "input journal received an unsupported SendInput type");

  const DWORD flags = input.mi.dwFlags;
  const DWORD allowed = MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN |
      MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTDOWN | MOUSEEVENTF_RIGHTUP |
      MOUSEEVENTF_MIDDLEDOWN | MOUSEEVENTF_MIDDLEUP | MOUSEEVENTF_XDOWN |
      MOUSEEVENTF_XUP | MOUSEEVENTF_WHEEL | MOUSEEVENTF_HWHEEL |
      MOUSEEVENTF_MOVE_NOCOALESCE | MOUSEEVENTF_VIRTUALDESK |
      MOUSEEVENTF_ABSOLUTE;
  if ((flags & ~allowed) != 0)
    return fail(error, "input journal received unsupported mouse flags");
  if ((flags & MOUSEEVENTF_XDOWN) != 0 &&
      (flags & MOUSEEVENTF_XUP) != 0)
    return fail(error, "input journal mouse event mixes X-button directions");
  if (((flags & MOUSEEVENTF_LEFTDOWN) != 0 &&
       (flags & MOUSEEVENTF_LEFTUP) != 0) ||
      ((flags & MOUSEEVENTF_RIGHTDOWN) != 0 &&
       (flags & MOUSEEVENTF_RIGHTUP) != 0) ||
      ((flags & MOUSEEVENTF_MIDDLEDOWN) != 0 &&
       (flags & MOUSEEVENTF_MIDDLEUP) != 0))
    return fail(error, "input journal mouse event mixes button directions");
  if ((flags & (MOUSEEVENTF_XDOWN | MOUSEEVENTF_XUP)) != 0 &&
      input.mi.mouseData != XBUTTON1 && input.mi.mouseData != XBUTTON2)
    return fail(error, "input journal received an invalid X-button id");

  *candidate = base;
  const auto apply_button = [&](DWORD down_flag, DWORD up_flag,
                                uint16_t button) {
    if ((flags & down_flag) != 0) candidate->mouse_buttons |= button;
    if ((flags & up_flag) != 0)
      candidate->mouse_buttons &= static_cast<uint16_t>(~button);
  };
  apply_button(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, chast::kMouseLeft);
  apply_button(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, chast::kMouseRight);
  apply_button(MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, chast::kMouseMiddle);
  if ((flags & (MOUSEEVENTF_XDOWN | MOUSEEVENTF_XUP)) != 0) {
    const uint16_t button = input.mi.mouseData == XBUTTON1
        ? chast::kMouseBack : chast::kMouseForward;
    if ((flags & MOUSEEVENTF_XDOWN) != 0) candidate->mouse_buttons |= button;
    if ((flags & MOUSEEVENTF_XUP) != 0)
      candidate->mouse_buttons &= static_cast<uint16_t>(~button);
  }
  return true;
}

bool apply_touch_frame(const PortableHelperInputJournalState& base,
                       const std::vector<POINTER_TOUCH_INFO>& contacts,
                       PortableHelperInputJournalState* candidate,
                       std::string* error) {
  if (contacts.empty() || contacts.size() > chast::kMaxTouchContacts)
    return fail(error, "input journal touch frame has an invalid contact count");
  std::array<bool, chast::kMaxTouchContacts> present{};
  for (const auto& info : contacts) {
    const auto& pointer = info.pointerInfo;
    if (pointer.pointerId >= present.size() || present[pointer.pointerId])
      return fail(error, "input journal touch frame has a duplicate or invalid id");
    present[pointer.pointerId] = true;
  }
  for (const auto& touch : base.touches) {
    if (!present[touch.pointer_id])
      return fail(error, "input journal touch frame omits an active contact");
  }
  *candidate = base;
  for (const auto& info : contacts) {
    const auto& pointer = info.pointerInfo;
    const DWORD action = pointer.pointerFlags &
        (POINTER_FLAG_DOWN | POINTER_FLAG_UPDATE | POINTER_FLAG_UP);
    const DWORD allowed = POINTER_FLAG_DOWN | POINTER_FLAG_UPDATE |
        POINTER_FLAG_UP | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT |
        POINTER_FLAG_CANCELED;
    const bool in_range = (pointer.pointerFlags & POINTER_FLAG_INRANGE) != 0;
    const bool in_contact = (pointer.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
    const bool action_flags_valid =
        ((action == POINTER_FLAG_DOWN || action == POINTER_FLAG_UPDATE) &&
         in_range && in_contact) ||
        (action == POINTER_FLAG_UP && !in_range && !in_contact);
    if (pointer.pointerType != PT_TOUCH ||
        pointer.pointerId >= chast::kMaxTouchContacts ||
        (pointer.pointerFlags & ~allowed) != 0 ||
        (action != POINTER_FLAG_DOWN && action != POINTER_FLAG_UPDATE &&
         action != POINTER_FLAG_UP) ||
        !action_flags_valid ||
        ((pointer.pointerFlags & POINTER_FLAG_CANCELED) != 0 &&
         action != POINTER_FLAG_UP) || info.pressure > 1024 ||
        (info.touchMask & ~TOUCH_MASK_PRESSURE) != 0)
      return fail(error, "input journal touch frame contains an invalid contact");

    auto found = std::find_if(candidate->touches.begin(), candidate->touches.end(),
        [&pointer](const auto& touch) {
          return touch.pointer_id == pointer.pointerId;
        });
    const auto before = std::find_if(base.touches.begin(), base.touches.end(),
        [&pointer](const auto& touch) {
          return touch.pointer_id == pointer.pointerId;
        });
    if ((action == POINTER_FLAG_DOWN && before != base.touches.end()) ||
        (action != POINTER_FLAG_DOWN && before == base.touches.end()))
      return fail(error, "input journal touch transition does not match active state");
    if (action == POINTER_FLAG_UP) {
      if (pointer.ptPixelLocation.x != before->x ||
          pointer.ptPixelLocation.y != before->y)
        return fail(error, "input journal touch up changes the previous contact location");
      candidate->touches.erase(found);
      continue;
    }
    PortableHelperJournalTouch touch{pointer.pointerId,
        pointer.ptPixelLocation.x, pointer.ptPixelLocation.y,
        (info.touchMask & TOUCH_MASK_PRESSURE) != 0 ?
            static_cast<uint16_t>(info.pressure) : uint16_t{0}};
    if (found == candidate->touches.end()) {
      if (candidate->touches.size() >= chast::kMaxTouchContacts)
        return fail(error, "input journal touch-state limit reached");
      candidate->touches.push_back(touch);
    } else {
      *found = touch;
    }
  }
  normalize(candidate);
  return true;
}

bool apply_pen_frame(const PortableHelperInputJournalState& base,
                     const std::vector<POINTER_TYPE_INFO>& pen_inputs,
                     PortableHelperInputJournalState* candidate,
                     std::string* error) {
  if (pen_inputs.size() != 1 || pen_inputs[0].type != PT_PEN)
    return fail(error, "input journal pen frame has an invalid shape");
  const auto& info = pen_inputs[0].penInfo;
  const DWORD action = info.pointerInfo.pointerFlags &
      (POINTER_FLAG_DOWN | POINTER_FLAG_UPDATE | POINTER_FLAG_UP);
  const DWORD allowed = POINTER_FLAG_DOWN | POINTER_FLAG_UPDATE | POINTER_FLAG_UP |
      POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_CANCELED;
  const bool in_range =
      (info.pointerInfo.pointerFlags & POINTER_FLAG_INRANGE) != 0;
  const bool in_contact =
      (info.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
  const bool action_flags_valid =
      ((action == POINTER_FLAG_DOWN || action == POINTER_FLAG_UPDATE) &&
       in_range && in_contact) ||
      (action == POINTER_FLAG_UP && !in_range && !in_contact);
  if (info.pointerInfo.pointerId != 0 ||
      (info.pointerInfo.pointerFlags & ~allowed) != 0 ||
      (action != POINTER_FLAG_DOWN && action != POINTER_FLAG_UPDATE &&
       action != POINTER_FLAG_UP) ||
      !action_flags_valid ||
      ((info.pointerInfo.pointerFlags & POINTER_FLAG_CANCELED) != 0 &&
       action != POINTER_FLAG_UP) || info.pressure > 1024 ||
      (info.penFlags & ~(PEN_FLAG_BARREL | PEN_FLAG_INVERTED |
                         PEN_FLAG_ERASER)) != 0 ||
      (info.penMask & ~(PEN_MASK_PRESSURE | PEN_MASK_TILT_X |
                        PEN_MASK_TILT_Y)) != 0)
    return fail(error, "input journal pen frame contains invalid flags or values");
  const bool was_down = base.pen_down;
  if ((action == POINTER_FLAG_DOWN && was_down) ||
      (action != POINTER_FLAG_DOWN && !was_down))
    return fail(error, "input journal pen transition does not match active state");
  if (((info.penMask & PEN_MASK_TILT_X) != 0 &&
       (info.tiltX < std::numeric_limits<int16_t>::min() ||
        info.tiltX > std::numeric_limits<int16_t>::max())) ||
      ((info.penMask & PEN_MASK_TILT_Y) != 0 &&
       (info.tiltY < std::numeric_limits<int16_t>::min() ||
        info.tiltY > std::numeric_limits<int16_t>::max())))
    return fail(error, "input journal pen tilt exceeds its serialized range");
  if (action == POINTER_FLAG_UP &&
      (info.pointerInfo.ptPixelLocation.x != base.pen.x ||
       info.pointerInfo.ptPixelLocation.y != base.pen.y))
    return fail(error, "input journal pen up changes the previous pointer location");
  *candidate = base;
  if (action == POINTER_FLAG_UP) {
    candidate->pen_down = false;
    candidate->pen = {};
  } else {
    candidate->pen_down = true;
    candidate->pen.x = info.pointerInfo.ptPixelLocation.x;
    candidate->pen.y = info.pointerInfo.ptPixelLocation.y;
    candidate->pen.pressure = (info.penMask & PEN_MASK_PRESSURE) != 0
        ? static_cast<uint16_t>(info.pressure) : 0;
    candidate->pen.flags = static_cast<uint8_t>(info.penFlags &
        (PEN_FLAG_BARREL | PEN_FLAG_INVERTED | PEN_FLAG_ERASER));
    candidate->pen.tilt_x = (info.penMask & PEN_MASK_TILT_X) != 0
        ? static_cast<int16_t>(info.tiltX) : int16_t{0};
    candidate->pen.tilt_y = (info.penMask & PEN_MASK_TILT_Y) != 0
        ? static_cast<int16_t>(info.tiltY) : int16_t{0};
  }
  return true;
}

bool apply_intent(const PortableHelperInputJournalState& base,
                  const chast::InputSideEffectIntent& intent,
                  PortableHelperInputJournalState* candidate,
                  std::string* error) {
  switch (intent.api) {
    case chast::InputSideEffectApi::SendInput:
      if (intent.inputs.size() != 1 || !intent.touch_contacts.empty() ||
          !intent.pointer_inputs.empty())
        return fail(error, "input journal SendInput intent is malformed");
      return apply_send_input(base, intent.inputs.front(), candidate, error);
    case chast::InputSideEffectApi::InjectTouchInput:
      if (!intent.inputs.empty() || !intent.pointer_inputs.empty())
        return fail(error, "input journal touch intent contains foreign payloads");
      return apply_touch_frame(base, intent.touch_contacts, candidate, error);
    case chast::InputSideEffectApi::InjectSyntheticPointerInput:
      if (!intent.inputs.empty() || !intent.touch_contacts.empty())
        return fail(error, "input journal pen intent contains foreign payloads");
      return apply_pen_frame(base, intent.pointer_inputs, candidate, error);
  }
  return fail(error, "input journal intent names an unknown API");
}

}  // namespace

bool encode_portable_helper_input_journal(
    const PortableHelperInputJournalImage& image,
    std::vector<uint8_t>* encoded, std::string* error) {
  if (error != nullptr) error->clear();
  if (encoded == nullptr) return fail(error, "missing input journal output");
  encoded->clear();
  if (!image_valid(image, error)) return false;
  encoded->insert(encoded->end(), kMagic.begin(), kMagic.end());
  put16(*encoded, kVersion);
  put16(*encoded, (image.pending ? 1u : 0u) | (image.released ? 2u : 0u));
  encoded->insert(encoded->end(), image.scope.instance_id.begin(),
                  image.scope.instance_id.end());
  encoded->insert(encoded->end(), image.scope.connection_id.begin(),
                  image.scope.connection_id.end());
  put32(*encoded, image.scope.windows_session_id);
  put64(*encoded, image.scope.generation);
  put16(*encoded, static_cast<uint16_t>(image.input_desktop_name.size()));
  encoded->insert(encoded->end(), image.input_desktop_name.begin(),
                  image.input_desktop_name.end());
  put64(*encoded, image.last_sequence);
  put64(*encoded, image.pending_sequence);
  encoded->push_back(image.pending
      ? static_cast<uint8_t>(image.pending_api) : 0);
  encoded->insert(encoded->end(), 7, 0);
  encode_state(image.committed, encoded);
  encode_state(image.possible, encoded);
  if (encoded->size() > kMaximumBytes) {
    encoded->clear();
    return fail(error, "input journal encoding exceeds its size limit");
  }
  return true;
}

bool decode_portable_helper_input_journal(
    const std::vector<uint8_t>& encoded,
    PortableHelperInputJournalImage* image, std::string* error) {
  if (error != nullptr) error->clear();
  if (image == nullptr) return fail(error, "missing input journal output");
  *image = {};
  if (encoded.size() < 8 + 2 + 2 + 16 + 16 + 4 + 8 + 2 + 8 + 8 + 8 + 8 ||
      encoded.size() > kMaximumBytes)
    return fail(error, "input journal size is invalid");
  Reader reader(encoded);
  std::array<uint8_t, 8> magic{};
  uint16_t version = 0, flags = 0;
  uint16_t desktop_name_bytes = 0;
  uint8_t pending_api = 0;
  std::array<uint8_t, 7> reserved{};
  auto& candidate = *image;
  if (!reader.take(magic.data(), magic.size()) || magic != kMagic ||
      !reader.get16(&version) || version != kVersion ||
      !reader.get16(&flags) || (flags & ~3u) != 0 ||
      !reader.take(candidate.scope.instance_id.data(),
                   candidate.scope.instance_id.size()) ||
      !reader.take(candidate.scope.connection_id.data(),
                   candidate.scope.connection_id.size()) ||
      !reader.get32(&candidate.scope.windows_session_id) ||
      !reader.get64(&candidate.scope.generation) ||
      !reader.get16(&desktop_name_bytes) ||
      desktop_name_bytes > kMaximumDesktopNameBytes) {
    *image = {};
    return fail(error, "input journal desktop identity is malformed");
  }
  candidate.input_desktop_name.resize(desktop_name_bytes);
  if (!reader.take(candidate.input_desktop_name.data(), desktop_name_bytes) ||
      !reader.get64(&candidate.last_sequence) ||
      !reader.get64(&candidate.pending_sequence) ||
      !reader.get8(&pending_api) || !reader.take(reserved.data(), reserved.size()) ||
      std::any_of(reserved.begin(), reserved.end(),
                  [](uint8_t value) { return value != 0; }) ||
      !decode_state(&reader, &candidate.committed) ||
      !decode_state(&reader, &candidate.possible) || !reader.done()) {
    *image = {};
    return fail(error, "input journal is malformed or unsupported");
  }
  candidate.pending = (flags & 1u) != 0;
  candidate.released = (flags & 2u) != 0;
  if (candidate.pending) {
    if (pending_api < static_cast<uint8_t>(chast::InputSideEffectApi::SendInput) ||
        pending_api > static_cast<uint8_t>(
            chast::InputSideEffectApi::InjectSyntheticPointerInput)) {
      *image = {};
      return fail(error, "input journal pending API is invalid");
    }
    candidate.pending_api = static_cast<chast::InputSideEffectApi>(pending_api);
  } else if (pending_api != 0) {
    *image = {};
    return fail(error, "committed input journal has a pending API value");
  }
  if (!image_valid(candidate, error)) {
    *image = {};
    return false;
  }
  return true;
}

PortableHelperInputJournal::PortableHelperInputJournal(
    PortableHelperInputJournalScope scope, Persist persist)
    : scope_(std::move(scope)), persist_(std::move(persist)) {
  image_.scope = scope_;
}

bool PortableHelperInputJournal::Initialize(std::string* error) {
  if (error != nullptr) error->clear();
  if (initialized_ || terminal_ || !valid_scope(scope_) || !persist_)
    return fail(error, "input journal cannot initialize without a unique scope and durable store");
  PortableHelperInputJournalImage initial;
  initial.scope = scope_;
  std::string persist_error;
  try {
    if (!persist_(initial, &persist_error)) {
      if (error != nullptr) *error = persist_error.empty()
          ? "input journal initial image was not made durable" : persist_error;
      return false;
    }
  } catch (...) {
    return fail(error, "input journal persistence callback threw during initialization");
  }
  image_ = std::move(initial);
  initialized_ = true;
  return true;
}

bool PortableHelperInputJournal::Restore(
    const PortableHelperInputJournalImage& image, std::string* error) {
  if (error != nullptr) error->clear();
  if (initialized_ || terminal_ || !persist_ || !image_valid(image, error) ||
      image.scope.instance_id != scope_.instance_id ||
      image.scope.connection_id != scope_.connection_id ||
      image.scope.windows_session_id != scope_.windows_session_id ||
      image.scope.generation != scope_.generation) {
    return fail(error, "input journal restore scope or state is invalid");
  }
  image_ = image;
  initialized_ = true;
  recovery_blocked_ = true;
  terminal_ = image.released;
  return true;
}

bool PortableHelperInputJournal::Prepare(
    const chast::InputSideEffectIntent& intent, std::string* error) {
  if (error != nullptr) error->clear();
  if (!initialized_ || terminal_ || recovery_blocked_ || image_.released ||
      image_.pending ||
      pending_.sequence != 0 ||
      intent.sequence == 0 || intent.sequence <= image_.last_sequence)
    return fail(error, "input journal prepare is out of scope, replayed, or already pending");

  PortableHelperInputJournalState candidate;
  if (!apply_intent(image_.committed, intent, &candidate, error) ||
      !valid_state(candidate, false, error))
    return false;
  normalize(&candidate);
  const bool stateful = !state_equal(image_.committed, candidate);
  PortableHelperInputJournalImage prepared = image_;
  if (stateful) {
    if (!valid_desktop_name(intent.input_desktop_name, false))
      return fail(error, "stateful input intent has no valid desktop identity");
    if (!state_empty(image_.committed) &&
        image_.input_desktop_name != intent.input_desktop_name)
      return fail(error, "input desktop changed while the journal still owns held state");
    if (state_empty(image_.committed))
      prepared.input_desktop_name = intent.input_desktop_name;
    prepared.pending = true;
    prepared.pending_sequence = intent.sequence;
    prepared.pending_api = intent.api;
    prepared.possible = union_state(image_.committed, candidate);
    if (!image_valid(prepared, error)) return false;
    std::string persist_error;
    try {
      if (!persist_(prepared, &persist_error)) {
        if (error != nullptr) *error = persist_error.empty()
            ? "input journal write-ahead image was not made durable" : persist_error;
        return false;
      }
    } catch (...) {
      return fail(error, "input journal persistence callback threw during prepare");
    }
    image_ = std::move(prepared);
  }
  pending_.sequence = intent.sequence;
  pending_.api = intent.api;
  pending_.stateful = stateful;
  pending_.candidate = std::move(candidate);
  return true;
}

bool PortableHelperInputJournal::Complete(
    const chast::InputSideEffectResult& result, std::string* error) {
  if (error != nullptr) error->clear();
  if (!initialized_ || pending_.sequence == 0 ||
      result.sequence != pending_.sequence || result.api != pending_.api ||
      result.return_value > 1 ||
      result.api_succeeded != (result.return_value == 1))
    return fail(error, "input journal result does not match the pending API attempt");

  if (pending_.stateful) {
    PortableHelperInputJournalImage committed = image_;
    if (result.api_succeeded) {
      committed.pending = false;
      committed.pending_sequence = 0;
      committed.pending_api = chast::InputSideEffectApi::SendInput;
      committed.last_sequence = result.sequence;
      committed.committed = pending_.candidate;
      if (state_empty(committed.committed)) committed.input_desktop_name.clear();
    } else {
      // A failed API result does not prove that the desktop state was
      // unchanged. Keep the durable prepare record unresolved so only the
      // independent cleanup owner can clear the possible held state.
      pending_ = {};
      return fail(error, "input API failed with uncertain desktop state; cleanup is required");
    }
    committed.possible = committed.committed;
    if (!image_valid(committed, error)) return false;
    std::string persist_error;
    try {
      if (!persist_(committed, &persist_error)) {
        if (error != nullptr) *error = persist_error.empty()
            ? "input journal result image was not made durable" : persist_error;
        return false;
      }
    } catch (...) {
      return fail(error, "input journal persistence callback threw during result commit");
    }
    image_ = std::move(committed);
  } else {
    image_.last_sequence = result.sequence;
  }
  pending_ = {};
  return true;
}

bool PortableHelperInputJournal::MarkReleased(std::string* error) {
  if (error != nullptr) error->clear();
  if (!initialized_)
    return fail(error, "input journal is not active for release confirmation");
  if (image_.released) {
    terminal_ = true;
    return true;
  }
  if (terminal_) return fail(error, "input journal generation is already terminal");
  PortableHelperInputJournalImage released = image_;
  released.pending = false;
  released.released = true;
  released.pending_sequence = 0;
  released.pending_api = chast::InputSideEffectApi::SendInput;
  released.input_desktop_name.clear();
  released.committed = {};
  released.possible = {};
  if (!image_valid(released, error)) return false;
  std::string persist_error;
  try {
    if (!persist_(released, &persist_error)) {
      if (error != nullptr) *error = persist_error.empty()
          ? "input journal release acknowledgement was not made durable"
          : persist_error;
      return false;
    }
  } catch (...) {
    return fail(error, "input journal persistence callback threw while marking released");
  }
  image_ = std::move(released);
  pending_ = {};
  initialized_ = false;
  terminal_ = true;
  return true;
}

PortableHelperInputJournalImage PortableHelperInputJournal::image() const {
  return image_;
}

bool PortableHelperInputJournal::has_pending_operation() const {
  return image_.pending || pending_.sequence != 0;
}

bool PortableHelperInputJournal::recovery_only() const {
  return recovery_blocked_;
}

}  // namespace chast::host
