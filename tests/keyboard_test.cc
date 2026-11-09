#include "keyboard_capture.h"
#include "local_text_input.h"
#include <chrono>
#include <future>
#include <iostream>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

chast::InputEvent input_key(uint16_t usage, bool down, uint16_t modifiers = 0) {
    chast::InputEvent event;
    event.kind = chast::InputKind::Key;
    event.key = {down ? chast::KeyAction::Down : chast::KeyAction::Up, usage};
    event.modifiers = modifiers;
    return event;
}

bool is_key(const INPUT& input, WORD scan, bool down, bool extended = false) {
    return input.type == INPUT_KEYBOARD && input.ki.wScan == scan &&
        ((input.ki.dwFlags & KEYEVENTF_KEYUP) == 0) == down &&
        ((input.ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0) == extended;
}

bool is_virtual_key(const INPUT& input, WORD vk, bool down, bool extended = false) {
    return input.type == INPUT_KEYBOARD && input.ki.wVk == vk &&
        (input.ki.dwFlags & KEYEVENTF_SCANCODE) == 0 &&
        ((input.ki.dwFlags & KEYEVENTF_KEYUP) == 0) == down &&
        ((input.ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0) == extended &&
        input.ki.dwExtraInfo == chast::kInputInjectionTag;
}

chast::InputEvent legacy_key(uint16_t vk, bool down, uint16_t modifiers = 0) {
    chast::InputEvent event;
    event.kind = chast::InputKind::LegacyKey;
    event.legacy_key = {down ? chast::KeyAction::Down : chast::KeyAction::Up, vk};
    event.modifiers = modifiers;
    return event;
}
}

int main() {
    chast_viewer::KeyboardState state;
    chast::InputEvent event;
    bool escape = false;
    auto key = [&](DWORD vk, DWORD scan, bool down, DWORD flags = 0, bool relative = false) {
        KBDLLHOOKSTRUCT data{};
        data.vkCode = vk;
        data.scanCode = scan;
        data.flags = flags;
        return state.Handle(data, down, 0, relative, event, escape);
    };
    check(key('A', 0x1E, true) && event.key.hid_usage == 4, "A maps physically");
    check(key('A', 0x1E, true) && event.kind == chast::InputKind::Key &&
          event.key.action == chast::KeyAction::Down, "ordinary repeats are forwarded");
    check(key('A', 0x1E, false) && event.key.action == chast::KeyAction::Up,
          "repeat sequence ends with one release");

    state.Reset();
    check(key(VK_LCONTROL, 0x21D, true) && event.kind != chast::InputKind::Key &&
          state.modifiers() == 0, "synthetic AltGr Ctrl is filtered by provenance");
    check(key(VK_RMENU, 0x38, true, LLKHF_EXTENDED) &&
          event.modifiers == chast::kModifierRightAlt, "AltGr carries only RightAlt");
    key(VK_RMENU, 0x38, false, LLKHF_EXTENDED);
    key(VK_LCONTROL, 0x21D, false);
    check(state.modifiers() == 0, "AltGr release leaves no Ctrl");
    key(VK_LCONTROL, 0x1D, true);
    key(VK_RMENU, 0x38, true, LLKHF_EXTENDED);
    check(event.modifiers == (chast::kModifierLeftCtrl | chast::kModifierRightAlt),
          "real Ctrl+RightAlt is preserved");
    check(key(VK_RMENU, 0x38, true, LLKHF_EXTENDED) &&
          event.kind != chast::InputKind::Key, "modifier repeats are filtered");
    key(VK_LCONTROL, 0x21D, true);
    key(VK_LCONTROL, 0x21D, false);
    check(state.modifiers() == (chast::kModifierLeftCtrl | chast::kModifierRightAlt),
          "synthetic Ctrl edges cannot release a physically held Ctrl");
    state.Reset(chast::kModifierLeftShift);
    key('Q', 0x1E, true);
    check(event.key.hid_usage == 0x04 && event.modifiers == chast::kModifierLeftShift,
          "focus resume uses physical position and tracked modifiers");
    key(VK_LSHIFT, 0x2A, false);
    check(state.modifiers() == 0, "modifier held before capture can be released");

    state.Reset();
    check(!key(VK_CAPITAL, 0x3A, true) && !key(VK_NUMLOCK, 0x45, true),
          "Caps and Num remain local for lock-state tracking");
    state.SetLegacy(true);
    check(!key(VK_CAPITAL, 0x3A, true), "Caps stays local in legacy mode too");
    check(key('A', 0x1E, true) && event.kind == chast::InputKind::LegacyKey &&
          event.legacy_key.virtual_key == 'A' &&
          event.legacy_key.action == chast::KeyAction::Down,
          "legacy mode sends the hook's virtual key");
    check(key('A', 0x1E, true) && event.kind == chast::InputKind::LegacyKey,
          "legacy repeats keep the virtual key");
    check(key('A', 0x1E, false) && event.legacy_key.action == chast::KeyAction::Up,
          "legacy release keeps the virtual key");
    check(key(VK_VOLUME_MUTE, 0x20, true, LLKHF_EXTENDED) &&
          event.kind == chast::InputKind::LegacyKey &&
          event.legacy_key.virtual_key == VK_VOLUME_MUTE,
          "legacy forwards named keys without a HID usage");
    check(key(VK_LSHIFT, 0x2A, true) && event.kind == chast::InputKind::LegacyKey &&
          event.legacy_key.virtual_key == VK_LSHIFT &&
          state.modifiers() == chast::kModifierLeftShift,
          "legacy modifiers update the input-state bitmap");
    check(key(VK_LSHIFT, 0x2A, true) && event.kind != chast::InputKind::LegacyKey,
          "legacy modifier repeats are filtered");
    key(VK_LSHIFT, 0x2A, false);
    check(state.modifiers() == 0, "legacy modifier release clears the bitmap");
    state.SetLegacy(false);
    state.Reset();
    check(!key(VK_VOLUME_MUTE, 0x20, true, LLKHF_EXTENDED),
          "map mode still drops keys without a HID usage");
    check(key('A', 0x1E, true) && event.kind == chast::InputKind::Key,
          "leaving legacy mode restores HID events");
    key(VK_LWIN, 0x5B, true, LLKHF_EXTENDED);
    check(event.key.hid_usage == 0xE3 && event.modifiers == chast::kModifierLeftGui,
          "Win is captured");
    state.Reset();
    key(VK_LMENU, 0x38, true);
    key(VK_TAB, 0x0F, true);
    check(event.key.hid_usage == 0x2B && event.modifiers == chast::kModifierLeftAlt,
          "Alt+Tab retains Alt");
    key(VK_LCONTROL, 0x1D, true);
    key(VK_LSHIFT, 0x2A, true);
    key('R', 0x13, true, 0, true);
    check(escape && event.kind != chast::InputKind::Key, "relative escape is local");
    key('R', 0x13, true);
    check(!escape && event.kind != chast::InputKind::Key, "escape repeats stay local");
    key('R', 0x13, false);
    check(event.kind != chast::InputKind::Key, "escape release stays local");
    key('R', 0x13, true);
    check(event.key.hid_usage == 0x15, "next independent R is forwarded");

    INPUT input{};
    {
        chast_viewer::KeyboardState translated;
        auto local_key = [&](DWORD vk, DWORD scan, bool down, bool composing = false, DWORD flags = 0) {
            KBDLLHOOKSTRUCT data{};
            data.vkCode = vk; data.scanCode = scan; data.flags = flags;
            return translated.Handle(data, down, 0, false, event, escape, true, composing);
        };
        check(!local_key('A', 0x1e, true) && event.kind != chast::InputKind::Key,
              "local layout owns printable key down");
        check(!local_key('A', 0x1e, false) && event.kind != chast::InputKind::Key,
              "text key up cannot leak a physical key");
        check(local_key(VK_RETURN, 0x1c, true) && event.key.hid_usage == 0x28,
              "Enter outside composition is remote");
        local_key(VK_RETURN, 0x1c, false);
        check(!local_key(VK_RETURN, 0x1c, true, true), "IME owns candidate confirmation");
        check(!local_key(VK_RETURN, 0x1c, false, false), "IME confirmation up stays local after composition ends");
        check(local_key(VK_BACK, 0x0e, true), "Backspace outside composition is remote");
        local_key(VK_BACK, 0x0e, false);
        check(!local_key(VK_BACK, 0x0e, true, true), "IME owns editing the preedit");
        local_key(VK_BACK, 0x0e, false);
        check(!local_key(VK_LCONTROL, 0x1d, true), "Ctrl remains visible to the local IME");
        check(local_key('C', 0x2e, true) && event.modifiers == chast::kModifierLeftCtrl,
              "Ctrl+C is still a remote shortcut");
        check(!local_key(VK_LCONTROL, 0x1d, false) && event.kind == chast::InputKind::Key,
              "remote shortcut modifier is released while local Ctrl also lifts");
        check(local_key('C', 0x2e, false) && event.key.action == chast::KeyAction::Up,
              "shortcut up retains routing after Ctrl lifts");
        local_key(VK_LCONTROL, 0x1d, true);
        check(!local_key(VK_SPACE, 0x39, true), "Ctrl+Space can toggle local IME");
        local_key(VK_SPACE, 0x39, false); local_key(VK_LCONTROL, 0x1d, false);
        check(!local_key(VK_LCONTROL, 0x21d, true), "synthetic AltGr Ctrl reaches the local layout");
        local_key(VK_RMENU, 0x38, true, false, LLKHF_EXTENDED);
        check(!local_key('Q', 0x10, true), "AltGr character remains local text");
    }
    check(chast::make_keyboard_input(0x55, true, input) && input.ki.wScan == 0x37 &&
          (input.ki.dwFlags & KEYEVENTF_EXTENDEDKEY) == 0, "multiply is not PrintScreen");
    check(chast::make_keyboard_input(0x46, true, input) &&
          (input.ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0, "PrintScreen stays extended");
    check(chast::make_keyboard_input(0x48, true, input) && input.ki.wVk == VK_PAUSE &&
          (input.ki.dwFlags & KEYEVENTF_SCANCODE) == 0, "Pause uses virtual-key fallback");
    check(chast::make_keyboard_input(0x48, false, input) &&
          (input.ki.dwFlags & KEYEVENTF_KEYUP) != 0, "Pause releases correctly");
    check(chast::make_legacy_keyboard_input('A', true, input) &&
          is_virtual_key(input, 'A', true) && !(input.ki.dwFlags & KEYEVENTF_EXTENDEDKEY),
          "legacy letters inject by virtual key");
    check(chast::make_legacy_keyboard_input(VK_LEFT, false, input) &&
          is_virtual_key(input, VK_LEFT, false, true),
          "legacy arrows keep the extended flag");
    check(!chast::make_legacy_keyboard_input(VK_PACKET, true, input),
          "legacy mode rejects VK_PACKET");
    check(!chast::make_legacy_keyboard_input(0, true, input),
          "legacy mode rejects an empty virtual key");
    for (uint16_t usage = 0x68; usage <= 0x73; ++usage) {
        check(chast::make_keyboard_input(usage, true, input) &&
              chast::windows_key_to_hid(VK_F13 + usage - 0x68, input.ki.wScan, false) == usage,
              "F13-F24 round-trip");
    }

    std::string error;
    {
        std::vector<INPUT> injected;
        auto injector = chast::InputInjector::create(true, &error,
            [&](const INPUT& value) { injected.push_back(value); });
        check(injector != nullptr, "sequence observer starts");
        if (injector) {
            const uint16_t ctrl = chast::kModifierLeftCtrl;
            const uint16_t chord = ctrl | chast::kModifierRightAlt;
            injector->submit(input_key(0xE0, true, ctrl));
            injector->submit(input_key(0xE6, true, chord));
            injector->submit(input_key(0x04, true, chord));
            injector->submit(input_key(0x04, false, chord));
            injector->submit(input_key(0xE6, false, ctrl));
            injector->submit(input_key(0xE0, false));
            injector->stop();
            check(injected.size() == 6 &&
                is_key(injected[0], 0x1D, true) &&
                is_key(injected[1], 0x38, true, true) &&
                is_key(injected[2], 0x1E, true) &&
                is_key(injected[3], 0x1E, false) &&
                is_key(injected[4], 0x38, false, true) &&
                is_key(injected[5], 0x1D, false),
                "real Ctrl stays held throughout RightAlt+A, not folded into AltGr");
        }
    }
    {
        std::vector<INPUT> injected;
        auto injector = chast::InputInjector::create(true, &error,
            [&](const INPUT& value) { injected.push_back(value); });
        check(injector != nullptr, "legacy sequence observer starts");
        if (injector) {
            const uint16_t ctrl = chast::kModifierLeftCtrl;
            injector->submit(legacy_key(VK_LCONTROL, true, ctrl));
            injector->submit(legacy_key('C', true, ctrl));
            injector->submit(legacy_key('C', false, ctrl));
            injector->submit(legacy_key(VK_LCONTROL, false));
            injector->stop();
            check(injected.size() == 4 &&
                is_virtual_key(injected[0], VK_LCONTROL, true) &&
                is_virtual_key(injected[1], 'C', true) &&
                is_virtual_key(injected[2], 'C', false) &&
                is_virtual_key(injected[3], VK_LCONTROL, false),
                "legacy Ctrl+C keeps its chord through the shared modifier bitmap");
        }
    }
    {
        std::vector<INPUT> injected;
        auto injector = chast::InputInjector::create(true, &error,
            [&](const INPUT& value) { injected.push_back(value); });
        check(injector != nullptr, "lock sequence observer starts");
        if (injector) {
            injector->submit(input_key(0x1E, true, chast::kLockCaps));
            injector->submit(input_key(0x1E, false, chast::kLockCaps));
            injector->stop();
            check(injected.size() == 10 &&
                is_key(injected[0], 0x3A, true) && is_key(injected[1], 0x3A, false) &&
                is_key(injected[2], 0x02, true) &&
                is_key(injected[3], 0x3A, true) && is_key(injected[4], 0x3A, false) &&
                is_key(injected[7], 0x02, false),
                "Caps is temporarily synchronized for layout-dependent digit keys");
        }
    }
    {
        std::vector<INPUT> injected;
        auto injector = chast::InputInjector::create(true, &error,
            [&](const INPUT& value) { injected.push_back(value); });
        check(injector != nullptr, "text observer starts");
        if (injector) {
            injector->submit_text("A\n\t");
            injector->stop();
            bool tagged = injected.size() == 6;
            for (const auto& value : injected) {
                tagged = tagged && value.ki.dwExtraInfo == chast::kInputInjectionTag;
            }
            check(tagged, "Unicode and virtual-key text injection cannot echo into capture");
        }
    }
    {
        std::promise<void> entered;
        auto entered_future = entered.get_future();
        std::promise<void> resume;
        auto resume_future = resume.get_future();
        std::vector<INPUT> injected;
        auto injector = chast::InputInjector::create(true, &error,
            [&](const INPUT& value) {
                injected.push_back(value);
                if (injected.size() == 1) {
                    entered.set_value();
                    resume_future.wait();
                }
            });
        check(injector != nullptr, "saturation observer starts");
        if (injector) {
            injector->submit(input_key(0x04, true));
            const bool ready = entered_future.wait_for(std::chrono::seconds(2)) ==
                std::future_status::ready;
            check(ready, "first down reaches injection before queue saturation");
            if (ready) {
                for (int i = 0; i < 1024; ++i) injector->submit(input_key(0x05, true));
                injector->release_held();
            }
            resume.set_value();
            injector->stop();
            check(ready && injected.size() == 2 &&
                is_key(injected[0], 0x1E, true) && is_key(injected[1], 0x1E, false) &&
                injector->stats().dropped == 1024,
                "a full queue still releases the key already held on the host");
        }
    }
    {
        std::vector<INPUT> injected;
        auto injector = chast::InputInjector::create(true, &error,
            [&](const INPUT& value) { injected.push_back(value); });
        check(injector != nullptr, "legacy lock collision observer starts");
        if (injector) {
            // 0x39 is both VK_9 and the Caps Lock HID usage. Hold the legacy
            // key, then force a Caps tap through a second legacy key; the tap
            // must not consume or untrack the held VK_9.
            injector->submit(legacy_key(0x39, true));
            injector->submit(legacy_key('A', true, chast::kLockCaps));
            injector->submit(legacy_key('A', false, chast::kLockCaps));
            injector->release_held();
            injector->stop();
            check(!injected.empty() &&
                  is_virtual_key(injected.back(), 0x39, false),
                  "lock synchronization leaves the same-numbered legacy key intact");
        }
    }
    std::cout << "keyboard failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
