// Pure-logic checks for the wire format: no GPU, no network.

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "chast/input.h"
#include "chast/protocol.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// The parsers take the payload; the makers return the whole control message.
std::vector<uint8_t> payload_of(const std::vector<uint8_t>& message) {
    return std::vector<uint8_t>(message.begin() + chast::kControlHeaderSize, message.end());
}

void test_hello_ack() {
    chast::ServerInfo sent;
    sent.codec = chast::VideoCodec::Hevc;
    sent.width = 2560;
    sent.height = 1440;
    sent.framerate = 60;
    sent.bitrate = 12000000;
    sent.origin_x = -1920;
    sent.origin_y = -120;
    sent.input_enabled = true;
    sent.encoder = "mfxhw64 (VDENC HEVC)";

    chast::ServerInfo got;
    std::string error;
    const bool parsed = chast::parse_hello_ack(payload_of(chast::make_hello_ack(sent)), got, &error);
    check(parsed, "HelloAck round-trips");
    if (!parsed) {
        std::printf("     %s\n", error.c_str());
        return;
    }
    check(got.codec == sent.codec, "HelloAck keeps the codec");
    check(got.width == sent.width && got.height == sent.height, "HelloAck keeps the size");
    check(got.framerate == sent.framerate && got.bitrate == sent.bitrate,
          "HelloAck keeps the rate control");
    check(got.origin_x == sent.origin_x && got.origin_y == sent.origin_y,
          "HelloAck keeps negative virtual-desktop origins");
    check(got.input_enabled, "HelloAck keeps the input flag");
    check(got.encoder == sent.encoder, "HelloAck keeps the encoder name");

    sent.input_enabled = false;
    chast::ServerInfo disabled;
    check(chast::parse_hello_ack(payload_of(chast::make_hello_ack(sent)), disabled, &error) &&
              !disabled.input_enabled,
          "HelloAck reports input disabled");
}

void test_input_events() {
    std::string error;

    chast::InputEvent mouse;
    mouse.kind = chast::InputKind::Mouse;
    mouse.mouse.action = chast::MouseAction::ButtonDown;
    mouse.mouse.buttons = chast::kMouseBack;
    mouse.mouse.x = -32768;
    mouse.mouse.y = 65535;
    chast::InputEvent got;
    check(chast::parse_input_event(payload_of(chast::make_input_event(mouse)), got, &error),
          "mouse event round-trips");
    check(got.kind == chast::InputKind::Mouse && got.mouse.action == chast::MouseAction::ButtonDown,
          "mouse event keeps kind and action");
    check(got.mouse.buttons == chast::kMouseBack, "mouse event keeps the button");
    check(got.mouse.x == mouse.mouse.x && got.mouse.y == mouse.mouse.y,
          "mouse event keeps signed coordinates");

    chast::InputEvent key;
    key.kind = chast::InputKind::Key;
    key.key.action = chast::KeyAction::Up;
    key.key.hid_usage = 0xE6;
    check(chast::parse_input_event(payload_of(chast::make_input_event(key)), got, &error),
          "key event round-trips");
    check(got.kind == chast::InputKind::Key && got.key.action == chast::KeyAction::Up,
          "key event keeps kind and action");
    check(got.key.hid_usage == 0xE6, "key event keeps the HID usage");

    auto rejects = [&](std::vector<uint8_t> payload, const char* what) {
        chast::InputEvent dropped;
        check(!chast::parse_input_event(payload, dropped, &error), what);
    };
    rejects(std::vector<uint8_t>(8), "short InputEvent is rejected");

    const std::vector<uint8_t> body = payload_of(chast::make_input_event(mouse));
    std::vector<uint8_t> unknown_kind = body;
    unknown_kind[0] = 9;
    rejects(unknown_kind, "unknown input kind is rejected");
    std::vector<uint8_t> unknown_action = body;
    unknown_action[1] = 9;
    rejects(unknown_action, "unknown mouse action is rejected");
    std::vector<uint8_t> unknown_buttons = body;
    unknown_buttons[2] = 0x80;
    rejects(unknown_buttons, "unknown mouse buttons are rejected");
    std::vector<uint8_t> no_usage = body;
    no_usage[0] = static_cast<uint8_t>(chast::InputKind::Key);
    rejects(no_usage, "key event without a HID usage is rejected");
}

// The injector's whole path except SendInput itself, which is what the dry
// run exists for: a test must not move the real pointer.
void test_injector_dry_run() {
    std::string error;
    struct InjectorFixture {
        std::unique_ptr<chast::InputInjector> injector;
    };

    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        check(fixture.injector != nullptr, "the dry-run injector starts");
        if (fixture.injector) {
            chast::InputEvent move;
            move.kind = chast::InputKind::Mouse;
            move.mouse.action = chast::MouseAction::Move;
            move.mouse.x = 40;
            move.mouse.y = 20;
            fixture.injector->submit(move);

            chast::InputEvent wheel;
            wheel.kind = chast::InputKind::Mouse;
            wheel.mouse.action = chast::MouseAction::Wheel;
            wheel.mouse.y = -120;
            fixture.injector->submit(wheel);

            chast::InputEvent shift;
            shift.kind = chast::InputKind::Key;
            shift.key.hid_usage = 0xE1;
            fixture.injector->submit(shift);
            chast::InputEvent a;
            a.kind = chast::InputKind::Key;
            a.key.hid_usage = 0x04;
            fixture.injector->submit(a);
            a.key.action = chast::KeyAction::Up;
            fixture.injector->submit(a);
            shift.key.action = chast::KeyAction::Up;
            fixture.injector->submit(shift);

            fixture.injector->stop();
            const chast::InputStats stats = fixture.injector->stats();
            check(stats.applied == 6, "a balanced event batch is injected once each");
            check(stats.rejected == 0 && stats.failed == 0 && stats.dropped == 0,
                  "a balanced event batch has no rejects, failures or drops");
        }
    }

    InjectorFixture fixture{chast::InputInjector::create(true, &error)};
    if (!fixture.injector) {
        return;
    }
    chast::InputEvent held;
    held.kind = chast::InputKind::Key;
    held.key.hid_usage = 0xE0;  // Left Ctrl, never released by the peer
    fixture.injector->submit(held);

    chast::InputEvent unknown;
    unknown.kind = chast::InputKind::Key;
    unknown.key.hid_usage = 0x48;  // Pause: no set 1 scan code
    fixture.injector->submit(unknown);

    fixture.injector->stop();
    const chast::InputStats stats = fixture.injector->stats();
    check(stats.applied == 2, "stop() releases the key left down and counts it");
    check(stats.rejected == 1, "an unmappable HID usage is rejected");
}

void test_scancodes() {
    uint16_t scan = 0;
    bool extended = false;

    check(chast::hid_to_scancode(0x04, scan, extended) && scan == 0x1E && !extended,
          "HID A maps to set 1 0x1E");
    check(chast::hid_to_scancode(0x28, scan, extended) && scan == 0x1C && !extended,
          "HID Enter maps to set 1 0x1C");
    check(chast::hid_to_scancode(0x50, scan, extended) && scan == 0x4B && extended,
          "HID Left arrow maps to the extended 0x4B");
    check(chast::hid_to_scancode(0xE5, scan, extended) && scan == 0x36 && !extended,
          "HID Right Shift maps to 0x36");
    check(chast::hid_to_scancode(0xE6, scan, extended) && scan == 0x38 && extended,
          "HID Right Alt maps to the extended 0x38");
    check(chast::hid_to_scancode(0x58, scan, extended) && scan == 0x1C && extended,
          "HID keypad Enter maps to the extended 0x1C");
    check(!chast::hid_to_scancode(0x48, scan, extended), "HID Pause is rejected (needs E1)");
    check(!chast::hid_to_scancode(0x00, scan, extended), "unused HID id is rejected");
}

// Real SendInput, only when asked for on the command line: moves the pointer
// to a known virtual-desktop coordinate, reads it back, and puts it where it
// was. No clicks and no keys, so nothing on the desktop reacts.
bool test_live_pointer() {
    POINT saved{};
    if (!GetCursorPos(&saved)) {
        check(false, "live: cannot read the current pointer position");
        return false;
    }
    const int32_t left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int32_t top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int32_t width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int32_t height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0) {
        check(false, "live: no virtual screen metrics");
        return false;
    }

    struct Restore {
        POINT point;
        ~Restore() {
            SetCursorPos(point.x, point.y);
        }
    } restore{saved};

    std::string error;
    std::unique_ptr<chast::InputInjector> injector =
        chast::InputInjector::create(false, &error);
    if (!injector) {
        check(false, "live: the injector starts");
        return false;
    }

    // Aim at 3/4 across and 1/2 down, then read the result back.
    const int32_t target_x = left + width * 3 / 4;
    const int32_t target_y = top + height / 2;
    chast::InputEvent move;
    move.kind = chast::InputKind::Mouse;
    move.mouse.action = chast::MouseAction::Move;
    move.mouse.x = target_x;
    move.mouse.y = target_y;
    injector->submit(move);

    POINT reached{};
    bool landed = false;
    for (int attempt = 0; attempt < 50 && !landed; ++attempt) {
        Sleep(10);
        if (!GetCursorPos(&reached)) {
            break;
        }
        // Absolute injection quantizes to 1/65535 of the virtual screen, so a
        // pixel or two of error is expected, not a failure.
        landed = std::abs(reached.x - target_x) <= 2 && std::abs(reached.y - target_y) <= 2;
    }
    std::printf("     target=(%d,%d) reached=(%d,%d)\n", target_x, target_y, reached.x,
                reached.y);
    check(landed, "live: an absolute move lands on the requested pixel");

    injector->stop();
    const chast::InputStats stats = injector->stats();
    check(stats.applied == 1 && stats.failed == 0, "live: SendInput accepted the move");
    return landed;
}

}  // namespace

int main(int argc, char** argv) {
    test_hello_ack();
    test_input_events();
    test_scancodes();
    test_injector_dry_run();
    bool live = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--live-input") {
            live = true;
        }
    }
    if (live) {
        test_live_pointer();
    }
    if (failures != 0) {
        std::printf("protocol checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("protocol checks passed\n");
    return 0;
}
