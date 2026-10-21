// Pure-logic checks for the wire format: no GPU, no network.

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "chast/identity.h"
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

void test_hello() {
    std::string error;
    auto parses_hello = [&](const std::vector<uint8_t>& payload, uint16_t& display,
                            std::string& token) {
        return chast::parse_hello(payload, display, token, &error);
    };
    auto rejects_hello = [&](std::vector<uint8_t> payload, const char* what) {
        uint16_t display = chast::kDisplayAuto;
        std::string token;
        check(!parses_hello(payload, display, token), what);
    };

    uint16_t display = 0;
    std::string token;
    check(parses_hello(payload_of(chast::make_hello()), display, token), "Hello round-trips");
    check(display == chast::kDisplayAuto, "Hello defaults to the server's display");
    check(token.empty(), "Hello without a token carries an empty one");
    check(parses_hello(payload_of(chast::make_hello(3)), display, token) && display == 3,
          "Hello keeps an explicit display index");

    const std::string secret = "correct horse battery staple";
    check(parses_hello(payload_of(chast::make_hello(chast::kDisplayAuto, secret)), display,
                       token) &&
              token == secret,
          "Hello round-trips an access token");
    check(parses_hello(payload_of(chast::make_hello(1, std::string(128, 'x'))), display,
                       token) &&
              token.size() == 128,
          "Hello accepts a token at the length limit");

    rejects_hello(std::vector<uint8_t>(4), "short Hello is rejected");
    std::vector<uint8_t> bad_display = payload_of(chast::make_hello());
    bad_display[2] = static_cast<uint8_t>(chast::kMaxDisplayCount);
    bad_display[3] = 0;
    rejects_hello(bad_display, "Hello with a display index past the limit is rejected");

    std::vector<uint8_t> old_version = payload_of(chast::make_hello());
    old_version[0] = static_cast<uint8_t>(chast::kProtocolVersion - 1);
    old_version[1] = 0;
    rejects_hello(old_version, "Hello from another protocol version is rejected");

    std::vector<uint8_t> long_token = payload_of(chast::make_hello());
    long_token[4] = static_cast<uint8_t>(chast::kMaxAccessTokenLength + 1);
    long_token[5] = 0;
    rejects_hello(long_token, "Hello with an oversized token length is rejected");

    std::vector<uint8_t> truncated_token = payload_of(chast::make_hello());
    truncated_token[4] = 3;  // claims three bytes that are not there
    truncated_token[5] = 0;
    rejects_hello(truncated_token, "Hello whose token length disagrees with the payload is rejected");

    std::vector<uint8_t> trailing = payload_of(chast::make_hello());
    trailing.push_back(0xFF);
    rejects_hello(trailing, "Hello with trailing bytes is rejected");
}

void test_access_denied() {
    chast::AccessDenial code = chast::AccessDenial::InvalidToken;
    std::string reason;
    std::string error;
    check(chast::parse_access_denied(
              payload_of(chast::make_access_denied(chast::AccessDenial::TokenUsed,
                                                   "the token was already used")),
              code, reason, &error) &&
              code == chast::AccessDenial::TokenUsed &&
              reason == "the token was already used",
          "AccessDenied round-trips its code and reason");

    check(chast::parse_access_denied(
              payload_of(chast::make_access_denied(chast::AccessDenial::Declined, "")), code,
              reason, &error) &&
              code == chast::AccessDenial::Declined && reason.empty(),
          "AccessDenied may carry an empty reason");

    std::vector<uint8_t> unknown_code =
        payload_of(chast::make_access_denied(chast::AccessDenial::Declined, "no"));
    unknown_code[0] = 0;
    check(!chast::parse_access_denied(unknown_code, code, reason, &error),
          "AccessDenied with an unknown code is rejected");

    std::vector<uint8_t> long_reason = payload_of(
        chast::make_access_denied(chast::AccessDenial::Declined, "short"));
    long_reason[1] = static_cast<uint8_t>(chast::kMaxAccessDenialReason + 1);
    long_reason[2] = static_cast<uint8_t>((chast::kMaxAccessDenialReason + 1) >> 8);
    check(!chast::parse_access_denied(long_reason, code, reason, &error),
          "AccessDenied whose reason length disagrees with the payload is rejected");

    check(!chast::parse_access_denied(std::vector<uint8_t>(2), code, reason, &error),
          "short AccessDenied is rejected");
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
    sent.clipboard_enabled = true;
    sent.encoder = "mfxhw64 (VDENC HEVC)";
    chast::DisplayDescription primary;
    primary.name = "\\\\.\\DISPLAY1";
    primary.right = 2560;
    primary.bottom = 1440;
    primary.primary = true;
    chast::DisplayDescription portrait;
    portrait.name = "\\\\.\\DISPLAY2";
    portrait.left = -1080;
    portrait.bottom = 1920;
    portrait.rotation_degrees = 90;
    sent.displays = {primary, portrait};

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
    check(got.clipboard_enabled, "HelloAck keeps the clipboard flag");
    check(got.encoder == sent.encoder, "HelloAck keeps the encoder name");
    check(got.displays.size() == 2, "HelloAck keeps the display count");
    if (got.displays.size() == 2) {
        check(got.displays[0].name == primary.name && got.displays[0].primary &&
                  got.displays[0].right == 2560 && got.displays[0].rotation_degrees == 0,
              "HelloAck keeps the primary display");
        check(got.displays[1].left == -1080 && got.displays[1].top == 0 &&
                  got.displays[1].rotation_degrees == 90 && !got.displays[1].primary,
              "HelloAck keeps a rotated display with a negative origin");
    }

    auto ack_payload = [&] { return payload_of(chast::make_hello_ack(sent)); };
    const size_t display_count_at = 24 + sent.encoder.size();
    const size_t first_display_at = display_count_at + 2;
    auto rejects_ack = [&](std::vector<uint8_t> payload, const char* what) {
        chast::ServerInfo dropped;
        check(!chast::parse_hello_ack(payload, dropped, &error), what);
    };
    std::vector<uint8_t> too_many = ack_payload();
    too_many[display_count_at] = 65;
    too_many[display_count_at + 1] = 0;
    rejects_ack(too_many, "a HelloAck with too many displays is rejected");
    std::vector<uint8_t> bad_rotation = ack_payload();
    bad_rotation[first_display_at + 2 + primary.name.size() + 16] = 45;
    rejects_ack(bad_rotation, "a display with an unknown rotation is rejected");
    std::vector<uint8_t> empty_rect = ack_payload();
    const size_t right_at = first_display_at + 2 + primary.name.size() + 8;
    empty_rect[right_at] = 0;
    empty_rect[right_at + 1] = 0;
    empty_rect[right_at + 2] = 0;
    empty_rect[right_at + 3] = 0;
    rejects_ack(empty_rect, "a display with an empty rect is rejected");
    std::vector<uint8_t> unknown_flags = ack_payload();
    // codec, reserved, size, rate, bitrate, origin, then the flags word.
    constexpr size_t kFlagsAt = 20;
    unknown_flags[kFlagsAt] = 0x04;
    unknown_flags[kFlagsAt + 1] = 0;
    rejects_ack(std::move(unknown_flags), "a HelloAck with unknown flags is rejected");

    sent.input_enabled = false;
    sent.clipboard_enabled = false;
    chast::ServerInfo disabled;
    check(chast::parse_hello_ack(payload_of(chast::make_hello_ack(sent)), disabled, &error) &&
              !disabled.input_enabled && !disabled.clipboard_enabled,
          "HelloAck reports input and clipboard disabled");
}

void test_frame_header() {
    chast::FrameHeader sent;
    sent.seq = 42;
    sent.pts_ms = 1234;
    sent.payload_size = 1000;
    sent.payload_crc32 = 0xA5A5F00D;
    sent.width = 1920;
    sent.height = 1080;
    sent.codec = chast::VideoCodec::Hevc;
    sent.keyframe = true;
    sent.idr = true;
    sent.capture_ms = 0x0000ABCDEF123456ull;

    uint8_t bytes[chast::kFrameHeaderSize]{};
    chast::encode_frame_header(sent, bytes);
    chast::FrameHeader got;
    std::string error;
    const bool decoded =
        chast::decode_frame_header(bytes, sizeof(bytes), got, &error);
    check(decoded, "FrameHeader round-trips");
    if (decoded) {
        check(got.seq == sent.seq && got.pts_ms == sent.pts_ms &&
                  got.payload_size == sent.payload_size &&
                  got.payload_crc32 == sent.payload_crc32,
              "FrameHeader keeps its counters");
        check(got.width == sent.width && got.height == sent.height &&
                  got.codec == sent.codec && got.keyframe && got.idr,
              "FrameHeader keeps the picture description");
        check(got.capture_ms == sent.capture_ms,
              "FrameHeader round-trips the 48-bit capture time");
    }
    check(sizeof(bytes) == 48, "FrameHeader is still 48 bytes");

    sent.capture_ms = 0xFFFFFFFFFFFFull;
    chast::encode_frame_header(sent, bytes);
    check(chast::decode_frame_header(bytes, sizeof(bytes), got, &error) &&
              got.capture_ms == 0xFFFFFFFFFFFFull,
          "FrameHeader carries the largest 48-bit capture time");

    std::vector<uint8_t> old_version(bytes, bytes + sizeof(bytes));
    old_version[4] = static_cast<uint8_t>(chast::kProtocolVersion - 1);
    old_version[5] = 0;
    check(!chast::decode_frame_header(old_version.data(), old_version.size(), got, &error),
          "FrameHeader from another protocol version is rejected");
    check(!chast::decode_frame_header(bytes, chast::kFrameHeaderSize - 1, got, &error),
          "a short FrameHeader is rejected");
}

void test_latency_probe() {
    std::string error;
    uint64_t client_us = 0;
    check(chast::parse_latency_probe(
              payload_of(chast::make_latency_probe(0x1122334455667788ull)), client_us,
              &error) &&
              client_us == 0x1122334455667788ull,
          "LatencyProbe round-trips its client timestamp");

    check(!chast::parse_latency_probe(std::vector<uint8_t>(7), client_us, &error),
          "a short LatencyProbe is rejected");
    std::vector<uint8_t> trailing =
        payload_of(chast::make_latency_probe(1));
    trailing.push_back(0);
    check(!chast::parse_latency_probe(trailing, client_us, &error),
          "LatencyProbe with trailing bytes is rejected");

    uint64_t host_recv_us = 0;
    uint64_t host_send_us = 0;
    const std::vector<uint8_t> reply =
        chast::make_latency_reply(10, 20, 25);
    check(chast::parse_latency_reply(payload_of(reply), client_us, host_recv_us,
                                     host_send_us, &error) &&
              client_us == 10 && host_recv_us == 20 && host_send_us == 25,
          "LatencyReply round-trips all three timestamps");
    check(reply.size() == chast::kControlHeaderSize + 24 &&
              reply[6] == static_cast<uint8_t>(chast::ControlType::LatencyReply),
          "LatencyReply names its control type and size");
    check(!chast::parse_latency_reply(payload_of(chast::make_latency_reply(10, 25, 20)),
                                      client_us, host_recv_us, host_send_us, &error),
          "LatencyReply that sends before receiving is rejected");
}

void test_clipboard() {
    std::string error;
    const std::string text = "hello 剪贴板 😀";
    const std::vector<uint8_t> message = chast::make_clipboard_update(0x1122334455667788ull, text);
    check(message.size() == chast::kControlHeaderSize + 8 + text.size(),
          "ClipboardUpdate carries the sequence and the text");
    check(message.size() > 7 &&
              message[6] == static_cast<uint8_t>(chast::ControlType::ClipboardUpdate),
          "ClipboardUpdate names its control type");
    chast::ClipboardUpdate got;
    check(chast::parse_clipboard_update(payload_of(message), got, &error) &&
              got.seq == 0x1122334455667788ull && got.text == text,
          "ClipboardUpdate round-trips UTF-8 text");

    const std::string largest(chast::kMaxClipboardBytes, 'a');
    check(chast::valid_clipboard_text(largest), "the clipboard length limit is accepted");
    check(chast::parse_clipboard_update(
              payload_of(chast::make_clipboard_update(7, largest)), got, &error) &&
              got.seq == 7 && got.text == largest,
          "a clipboard text at the length limit round-trips");
    check(chast::make_clipboard_update(1, largest + "a").empty(),
          "a text past the length limit is not encoded");

    check(!chast::valid_clipboard_text(""), "an empty clipboard text is rejected");
    check(!chast::valid_clipboard_text(std::string("a\0b", 3)),
          "a clipboard text with NUL is rejected");
    check(!chast::valid_clipboard_text("\x80"), "a lone continuation byte is rejected");
    check(!chast::valid_clipboard_text("\xC0\x80"), "an overlong NUL is rejected");
    check(!chast::valid_clipboard_text("\xE0\x80\x80"), "an overlong three-byte form is rejected");
    check(!chast::valid_clipboard_text("\xED\xA0\x80"), "a UTF-16 surrogate is rejected");
    check(!chast::valid_clipboard_text("\xF4\x90\x80\x80"), "a code point past U+10FFFF is rejected");
    check(!chast::valid_clipboard_text("\xE4\xB8"), "a truncated sequence is rejected");

    check(!chast::parse_clipboard_update(std::vector<uint8_t>(8), got, &error),
          "a ClipboardUpdate without text is rejected");
    std::vector<uint8_t> too_long(8 + chast::kMaxClipboardBytes + 1, 'a');
    check(!chast::parse_clipboard_update(too_long, got, &error),
          "an oversized ClipboardUpdate is rejected");
    std::vector<uint8_t> invalid_utf8{0, 0, 0, 0, 0, 0, 0, 0, 0xED, 0xA0, 0x80};
    check(!chast::parse_clipboard_update(invalid_utf8, got, &error),
          "a ClipboardUpdate with invalid UTF-8 is rejected");
    std::vector<uint8_t> short_message(7, 0);
    check(!chast::parse_clipboard_update(short_message, got, &error),
          "a truncated ClipboardUpdate is rejected");
}

void test_stream_info() {
    chast::ServerInfo sent;
    sent.codec = chast::VideoCodec::H264;
    sent.width = 1280;
    sent.height = 720;
    sent.framerate = 30;
    sent.bitrate = 4000000;
    sent.origin_x = 1920;
    sent.encoder = "mfxhw64 (VDENC AVC)";
    chast::DisplayDescription display;
    display.name = "\\\\.\\DISPLAY2";
    display.left = 1920;
    display.right = 3200;
    display.bottom = 720;
    sent.displays = {display};

    const std::vector<uint8_t> hello_ack = chast::make_hello_ack(sent);
    const std::vector<uint8_t> stream_info = chast::make_stream_info(sent);
    check(payload_of(hello_ack) == payload_of(stream_info),
          "StreamInfo carries the HelloAck payload");
    check(stream_info.size() >= chast::kControlHeaderSize &&
              stream_info[6] == static_cast<uint8_t>(chast::ControlType::StreamInfo),
          "StreamInfo names its control type");

    chast::ServerInfo got;
    std::string error;
    const bool parsed = chast::parse_hello_ack(payload_of(stream_info), got, &error);
    check(parsed, "StreamInfo parses with the HelloAck parser");
    if (parsed) {
        check(got.width == sent.width && got.height == sent.height && got.codec == sent.codec,
              "StreamInfo carries the new stream parameters");
        check(got.origin_x == sent.origin_x && got.displays.size() == 1 &&
                  got.displays[0].left == 1920,
              "StreamInfo carries the new origin and display list");
    }
}

void test_identity() {
    const std::string hex = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
    check(chast::normalize_fingerprint(hex) == hex, "a canonical fingerprint is unchanged");
    check(chast::normalize_fingerprint("01:23:45:67:89:ab:cd:ef"
                                        "01:23:45:67:89:ab:cd:ef"
                                        "01:23:45:67:89:ab:cd:ef"
                                        "01:23:45:67:89:ab:cd:ef") == hex,
          "colons, lower case and spacing normalize to upper hex");
    check(chast::normalize_fingerprint(" 0123 4567 89ab cdef 0123 4567 89ab cdef"
                                       " 0123 4567 89ab cdef 0123 4567 89ab cdef ") == hex,
          "spaces between byte groups are accepted");
    check(chast::normalize_fingerprint(std::string(63, 'A')).empty(),
          "a 63-digit fingerprint is rejected");
    check(chast::normalize_fingerprint(std::string(65, 'A')).empty(),
          "a 65-digit fingerprint is rejected");
    check(chast::normalize_fingerprint(std::string(64, 'G')).empty(),
          "a non-hex fingerprint is rejected");
    check(chast::normalize_fingerprint("").empty(), "an empty fingerprint is rejected");

    check(chast::format_fingerprint(hex) ==
              "01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:"
              "01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF",
          "formatting inserts a colon between bytes");
    check(chast::format_fingerprint("").empty(), "an empty fingerprint formats to nothing");

    check(chast::fingerprint_matches(hex, hex), "an identical fingerprint matches");
    check(chast::fingerprint_matches(chast::format_fingerprint(hex), hex),
          "a formatted pin matches the bare fingerprint");
    std::string other = hex;
    other.back() = '0';
    check(!chast::fingerprint_matches(other, hex), "a 1-bit difference is not a match");
    check(!chast::fingerprint_matches("not-a-fingerprint", hex),
          "an invalid pin never matches");
}

void test_input_events() {
    std::string error;

    chast::InputEvent mouse;
    mouse.kind = chast::InputKind::Mouse;
    mouse.mouse.action = chast::MouseAction::ButtonDown;
    mouse.mouse.buttons = chast::kMouseBack;
    mouse.mouse.x = -32768;
    mouse.mouse.y = 65535;
    mouse.modifiers = chast::kModifierLeftCtrl | chast::kModifierRightShift | chast::kLockCaps |
                      chast::kLockScroll;
    chast::InputEvent got;
    check(chast::parse_input_event(payload_of(chast::make_input_event(mouse)), got, &error),
          "mouse event round-trips");
    check(got.kind == chast::InputKind::Mouse && got.mouse.action == chast::MouseAction::ButtonDown,
          "mouse event keeps kind and action");
    check(got.mouse.buttons == chast::kMouseBack, "mouse event keeps the button");
    check(got.mouse.x == mouse.mouse.x && got.mouse.y == mouse.mouse.y,
          "mouse event keeps signed coordinates");
    check(got.modifiers == mouse.modifiers, "mouse event keeps the modifier and lock state");

    chast::InputEvent key;
    key.kind = chast::InputKind::Key;
    key.key.action = chast::KeyAction::Up;
    key.key.hid_usage = 0xE6;
    key.modifiers = chast::kModifierRightAlt;
    check(chast::parse_input_event(payload_of(chast::make_input_event(key)), got, &error),
          "key event round-trips");
    check(got.kind == chast::InputKind::Key && got.key.action == chast::KeyAction::Up,
          "key event keeps kind and action");
    check(got.key.hid_usage == 0xE6, "key event keeps the HID usage");
    check(got.modifiers == chast::kModifierRightAlt, "key event keeps the modifier bitmap");

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
    std::vector<uint8_t> unknown_modifiers = body;
    unknown_modifiers[15] = 0x80;
    rejects(unknown_modifiers, "unknown input state bits are rejected");
}

// The injector's whole path except SendInput itself, which is what the dry
// run exists for: a test must not move the real pointer.
void test_injector_dry_run() {
    std::string error;
    struct InjectorFixture {
        std::unique_ptr<chast::InputInjector> injector;
    };

    auto key = [](uint16_t usage, chast::KeyAction action, uint16_t modifiers) {
        chast::InputEvent event;
        event.kind = chast::InputKind::Key;
        event.key.hid_usage = usage;
        event.key.action = action;
        event.modifiers = modifiers;
        return event;
    };

    // A coherent chord needs exactly one injection per event.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        check(fixture.injector != nullptr, "the dry-run injector starts");
        if (fixture.injector) {
            chast::InputEvent move;
            move.kind = chast::InputKind::Mouse;
            move.mouse.action = chast::MouseAction::Move;
            move.mouse.x = 40;
            move.mouse.y = 20;
            move.modifiers = chast::kModifierLeftShift;
            fixture.injector->submit(key(0xE1, chast::KeyAction::Down,
                                         chast::kModifierLeftShift));
            fixture.injector->submit(move);
            fixture.injector->submit(key(0x04, chast::KeyAction::Down,
                                         chast::kModifierLeftShift));
            fixture.injector->submit(key(0x04, chast::KeyAction::Up,
                                         chast::kModifierLeftShift));
            fixture.injector->submit(key(0xE1, chast::KeyAction::Up, 0));

            fixture.injector->stop();
            const chast::InputStats stats = fixture.injector->stats();
            check(stats.applied == 5, "a coherent chord is injected once per event");
            check(stats.rejected == 0 && stats.failed == 0 && stats.dropped == 0,
                  "a coherent chord has no rejects, failures or drops");
        }
    }

    // A Shift Down that never arrived is synthesized from the next bitmap.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            fixture.injector->submit(key(0x04, chast::KeyAction::Down,
                                         chast::kModifierLeftShift));
            fixture.injector->submit(key(0x04, chast::KeyAction::Up,
                                         chast::kModifierLeftShift));
            fixture.injector->stop();  // lifts the synthesized Shift
            const chast::InputStats stats = fixture.injector->stats();
            check(stats.applied == 4, "a missing modifier press is synthesized once");
        }
    }

    // A Shift Up that never arrived is lifted by the next empty bitmap.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            fixture.injector->submit(key(0xE1, chast::KeyAction::Down,
                                         chast::kModifierLeftShift));
            fixture.injector->submit(key(0x04, chast::KeyAction::Down, 0));
            fixture.injector->submit(key(0x04, chast::KeyAction::Up, 0));
            fixture.injector->stop();  // nothing is left held
            const chast::InputStats stats = fixture.injector->stats();
            check(stats.applied == 4, "a missing modifier release is repaired once");
        }
    }

    // AltGr arrives as Windows' synthetic left Ctrl plus Right Alt; the pair is
    // folded into Right Alt, including the Ctrl release at the end.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            const uint16_t altgr = chast::kModifierLeftCtrl | chast::kModifierRightAlt;
            fixture.injector->submit(key(0xE0, chast::KeyAction::Down,
                                         chast::kModifierLeftCtrl));
            fixture.injector->submit(key(0xE6, chast::KeyAction::Down, altgr));
            fixture.injector->submit(key(0xE6, chast::KeyAction::Up,
                                         chast::kModifierLeftCtrl));
            fixture.injector->submit(key(0xE0, chast::KeyAction::Up, 0));
            fixture.injector->stop();
            const chast::InputStats stats = fixture.injector->stats();
            check(stats.applied == 4, "AltGr's synthetic Ctrl is folded away");
            check(stats.rejected == 0 && stats.failed == 0 && stats.dropped == 0,
                  "AltGr has no rejects, failures or drops");
        }
    }

    // Lock keys are states, not held keys: the first event reports Caps on
    // and the injector taps Caps once; later events carrying the same state
    // do nothing, and switching back taps it again.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            fixture.injector->submit(key(0x04, chast::KeyAction::Down, chast::kLockCaps));
            fixture.injector->submit(key(0x04, chast::KeyAction::Up, chast::kLockCaps));
            fixture.injector->submit(key(0x04, chast::KeyAction::Down, 0));
            fixture.injector->submit(key(0x04, chast::KeyAction::Up, 0));
            fixture.injector->stop();
            const chast::InputStats stats = fixture.injector->stats();
            check(stats.lock_toggles == 2, "a lock-state change taps the lock key once");
            check(stats.applied == 8, "each lock tap is a press and a release");
            check(stats.rejected == 0 && stats.failed == 0,
                  "lock synchronization injects cleanly");
        }
    }

    // A host already in the reported lock state is left alone.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            fixture.injector->submit(key(0xE0, chast::KeyAction::Down, 0));
            fixture.injector->stop();
            check(fixture.injector->stats().lock_toggles == 0,
                  "an already matching lock state is left alone");
        }
    }

    // InputRelease lifts what is held, in order, without stopping the thread.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            fixture.injector->submit(key(0xE0, chast::KeyAction::Down,
                                         chast::kModifierLeftCtrl));
            fixture.injector->release_held();
            auto applied = [&] { return fixture.injector->stats().applied; };
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (applied() < 2 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            check(applied() == 2, "a release request lifts the held modifier");
        }
    }

    // Fast toggling of a held modifier stays balanced.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            for (int i = 0; i < 10; ++i) {
                fixture.injector->submit(key(0xE1, chast::KeyAction::Down,
                                             chast::kModifierLeftShift));
                fixture.injector->submit(key(0xE1, chast::KeyAction::Up, 0));
            }
            fixture.injector->stop();
            const chast::InputStats stats = fixture.injector->stats();
            check(stats.applied == 20, "rapid modifier toggling injects every transition once");
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
    test_hello();
    test_access_denied();
    test_hello_ack();
    test_frame_header();
    test_latency_probe();
    test_clipboard();
    test_stream_info();
    test_input_events();
    test_identity();
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
