#pragma once

// The chast wire protocol, version 9.
//
// One QUIC connection carries two things:
//
//   * a client-initiated bidirectional control stream (Hello / HelloAck,
//     key-frame requests, input events, closing statistics), and
//   * a server-initiated unidirectional video stream: a sequence of access
//     units, each prefixed by a fixed FrameHeader.
//
// QUIC streams give the video reliable, ordered delivery; the sequence number
// is still carried so the receiver can report gaps and so a future version can
// move frames to independent streams or datagrams without changing the header.
// Every payload carries a CRC32 because the transport is only part of the
// pipeline: it catches assembly or ordering mistakes in this protocol, not
// corruption on the wire.
//
// All integers are little-endian. Peers exchange the protocol version and the
// stream's actual codec in the handshake; a requested HEVC stream that fell
// back to AVC is announced as AVC.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "chast/encoder.h"

namespace chast {

inline constexpr uint16_t kProtocolVersion = 9;
// Complies with the ALPN private-use range. The ALPN names the protocol
// family, not its version: a mismatch is reported by the Hello exchange with
// the peer's number instead of as an opaque handshake failure.
inline constexpr char kAlpn[] = "chast/1";

inline constexpr size_t kFrameHeaderSize = 48;
inline constexpr size_t kControlHeaderSize = 12;
inline constexpr uint32_t kMaxFramePayload = 32u * 1024u * 1024u;
inline constexpr uint32_t kMaxControlPayload = 1024u * 1024u;

enum class ControlType : uint16_t {
    Hello = 1,
    HelloAck = 2,
    KeyframeRequest = 3,
    Stats = 4,
    Bye = 5,
    InputEvent = 6,
    // The viewer is giving up the keyboard and pointer (control toggled off,
    // the window lost focus, the app is closing). The server lifts everything
    // the injector still holds; no payload.
    InputRelease = 7,
    // The captured output's parameters changed mid-session (resolution,
    // rotation, origin, codec or rate control) after a duplication rebuild.
    // The payload is exactly the HelloAck payload, so the viewer can replace
    // everything it learned in the handshake. The video stream continues on
    // the same QUIC stream with the same sequence numbers; the sender forces
    // an IDR so a viewer that has to recreate its decoder can start there.
    StreamInfo = 8,
    // The server refused the Hello (bad or already used access token, or the
    // host declined the connection). Payload: AccessDenial code, reason
    // length, reason. Sent before the server closes the connection, so the
    // viewer can show why instead of a generic transport error.
    AccessDenied = 9,
    // Clock-sync probe. The viewer sends its steady-clock timestamp; the host
    // answers on the same control stream with LatencyReply. The four
    // timestamps give the viewer the round-trip time and the offset between
    // the two monotonic clocks, which is what turns FrameHeader::capture_ms
    // into an end-to-end frame age. Sent about once a second.
    LatencyProbe = 10,
    // Answer to LatencyProbe. Payload: client_us, host_recv_us, host_send_us
    // (u64 each). All are steady_clock readings in microseconds since that
    // clock's epoch; the viewer subtracts its own receive time to get RTT and
    // the clock offset without trusting either wall clock.
    LatencyReply = 11,
    // A change of the sender's text clipboard. Payload: seq (u64) followed by
    // the UTF-8 text. Both roles may send; QUIC's ordered stream already
    // guarantees delivery order, so seq is diagnostic rather than a dedup key.
    // Peers only send text that differs from the baseline they last saw, which
    // keeps the update from echoing back to its origin.
    ClipboardUpdate = 12,
};

// ----- input -----
//
// The client sends one InputEvent message per pointer or keyboard event on the
// control stream, in the order they happened. Coordinates are in the host's
// virtual-desktop space (pixels), so the server never needs to know which
// output the viewer is looking at and a later multi-display UI needs no wire
// change. Keys travel as USB HID usage ids: layout independent, and mapped to
// scan codes on the injecting side. Every event also carries the modifier
// keys held at that moment, which lets the injecting side repair a lost key
// transition instead of trusting that every Down has a matching Up.

enum class InputKind : uint8_t {
    Mouse = 1,
    Key = 2,
};

enum class MouseAction : uint8_t {
    Move = 0,             // absolute, virtual-desktop pixels
    MoveRelative = 1,
    ButtonDown = 2,
    ButtonUp = 3,
    Wheel = 4,            // y, in WHEEL_DELTA (120) units
    WheelHorizontal = 5,  // x, in WHEEL_DELTA units
};

// Bit positions in MouseEvent::buttons. Matches the order Windows uses for
// its X buttons so the injector can pass the value straight through.
enum MouseButtonBit : uint16_t {
    kMouseLeft = 1u << 0,
    kMouseRight = 1u << 1,
    kMouseMiddle = 1u << 2,
    kMouseBack = 1u << 3,
    kMouseForward = 1u << 4,
};

struct MouseEvent {
    MouseAction action = MouseAction::Move;
    // For ButtonDown/ButtonUp: exactly the button the action applies to, one
    // of MouseButtonBit. For Move: the buttons held at that moment, which the
    // injector does not need but a recording tool does. Zero for wheel events.
    uint16_t buttons = 0;
    // Absolute actions carry virtual-desktop pixels; relative moves carry the
    // delta; Wheel reads y and WheelHorizontal reads x, both in WHEEL_DELTA
    // (120) units with the desktop's sign convention.
    int32_t x = 0;
    int32_t y = 0;
};

enum class KeyAction : uint8_t {
    Down = 0,
    Up = 1,
};

struct KeyEvent {
    KeyAction action = KeyAction::Down;
    // USB HID usage id from usage page 0x07 (keyboard), e.g. 0x04 for A.
    uint16_t hid_usage = 0;
};

// Bit positions in InputEvent::modifiers. Bits 0..7 are the modifier keys on
// HID usage page 0x07 (0xE0..0xE7) in that order; every event, pointer or
// keyboard, carries the set the client saw held at that moment. The injector
// reconciles its own held set to this bitmap, so a lost press or release (a
// key-up that never arrives because the viewer lost focus, say) cannot leave
// the host keyboard out of sync.
//
// Bits 8..10 carry the client's lock-key state. Locks are toggles rather than
// held keys, so the injector compares them with the host's state and taps the
// lock key once when they disagree; that is what makes typed text match the
// viewer's expectations even when the two sides started with different Caps,
// Num or Scroll state.
enum ModifierBit : uint16_t {
    kModifierLeftCtrl = 1u << 0,
    kModifierLeftShift = 1u << 1,
    kModifierLeftAlt = 1u << 2,
    kModifierLeftGui = 1u << 3,
    kModifierRightCtrl = 1u << 4,
    kModifierRightShift = 1u << 5,
    kModifierRightAlt = 1u << 6,  // AltGr on layouts that have it
    kModifierRightGui = 1u << 7,
    kModifierMask = 0x00FFu,

    kLockCaps = 1u << 8,
    kLockNum = 1u << 9,
    kLockScroll = 1u << 10,
    kLockStateMask = 0x0700u,
};

// Everything InputEvent::modifiers may carry.
inline constexpr uint16_t kInputStateMask = 0x07FFu;

struct InputEvent {
    InputKind kind = InputKind::Mouse;
    MouseEvent mouse;
    KeyEvent key;
    // Modifier keys held and lock keys on, when the event happened; see
    // ModifierBit and LockStateBit.
    uint16_t modifiers = 0;
};

// ----- clipboard -----
//
// Only plain text travels, as UTF-8. The limit keeps one update from tying up
// the control stream; larger texts are simply not synced in this version.
inline constexpr size_t kMaxClipboardBytes = 256u * 1024u;

// True when `text` is a non-empty, well-formed UTF-8 string of at most
// kMaxClipboardBytes with no NUL. NUL is rejected because Windows'
// CF_UNICODETEXT is NUL-terminated and would silently truncate.
bool valid_clipboard_text(const std::string& text);

struct ClipboardUpdate {
    uint64_t seq = 0;
    std::string text;
};

struct FrameHeader {
    uint64_t seq = 0;
    // Presentation time relative to the host session's first stream. Ordering
    // and pacing only; it is not comparable across sessions or machines.
    uint64_t pts_ms = 0;
    uint32_t payload_size = 0;
    uint32_t payload_crc32 = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VideoCodec codec = VideoCodec::H264;
    bool keyframe = false;
    bool idr = false;
    // Host steady_clock milliseconds at capture, low 48 bits. Together with
    // the offset learned from LatencyReply it gives the viewer the age of the
    // picture without assuming the two clocks share an epoch. Zero when the
    // sender does not know (a synthetic frame).
    uint64_t capture_ms = 0;
};

// HelloAck flags. Bit 0 tells the viewer it may send InputEvents; bit 1 tells
// it that both sides may exchange ClipboardUpdate messages.
inline constexpr uint16_t kServerInfoInputEnabled = 0x0001;
inline constexpr uint16_t kServerInfoClipboardEnabled = 0x0002;
inline constexpr uint16_t kServerInfoKnownFlags =
    kServerInfoInputEnabled | kServerInfoClipboardEnabled;

// One attached output, as the server sees it. `rect` is the output's
// virtual-desktop rect (the orientation the user sees, so a rotated panel
// reports its rotated width and height). The list order is the server's
// display index, which is what Hello asks for when the viewer picks an output.
struct DisplayDescription {
    std::string name;  // "\\.\DISPLAY1", UTF-8
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
    uint16_t rotation_degrees = 0;  // 0/90/180/270
    bool primary = false;
};

// Hello display index meaning "whatever the server captures by default". The
// server's fallback is its own configuration (primary when unset).
inline constexpr uint16_t kDisplayAuto = 0xFFFF;
// Upper bound on a HelloAck display list; also the valid range of a Hello
// display index. A machine with more outputs than this is not something the
// viewer UI could present anyway, and the cap bounds the work a malformed
// HelloAck can cause.
inline constexpr uint16_t kMaxDisplayCount = 64;

// What the server announces in HelloAck. `encoder` is informational (the VPL
// implementation that won the tier ladder); `codec` is what the video stream
// actually contains. `origin_x`/`origin_y` are the captured output's top-left
// in virtual-desktop pixels, which the viewer adds to the frame-space pointer
// position before sending it back. `input_enabled` mirrors the server's gate:
// when false the viewer must not send InputEvents (they would be dropped).
// `displays` lists every attached output; the one being streamed is the entry
// whose rect starts at the origin and is `width` x `height` pixels.
// `clipboard_enabled` mirrors the server's clipboard gate: when false the
// viewer must not send ClipboardUpdates and will not receive any.
struct ServerInfo {
    VideoCodec codec = VideoCodec::H264;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t framerate = 30;
    uint32_t bitrate = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    bool input_enabled = false;
    bool clipboard_enabled = false;
    std::string encoder;
    std::vector<DisplayDescription> displays;
};

// What the receiver reports before it closes the connection.
struct ClientStats {
    uint64_t received = 0;
    uint64_t decoded = 0;
    uint64_t crc_errors = 0;
    uint64_t decode_errors = 0;
    uint64_t digest = 0;       // aggregate of the decoded frame hashes
    uint64_t elapsed_ms = 0;
};

// FNV-1a over the eight bytes of each decoded frame's content hash, in decode
// order. The receiver folds the hash of every picture it decodes; replaying
// the same access units through chast-decode produces the same aggregate, so
// the two paths' output can be compared even though they never share memory.
inline constexpr uint64_t kFrameDigestSeed = 0xcbf29ce484222325ull;
inline uint64_t mix_frame_hash(uint64_t digest, uint64_t frame_hash) {
    for (int i = 0; i < 8; ++i) {
        digest ^= (frame_hash >> (8 * i)) & 0xFFu;
        digest *= 0x100000001b3ull;
    }
    return digest;
}

uint32_t crc32(const uint8_t* data, size_t size);

void encode_frame_header(const FrameHeader& header, uint8_t out[kFrameHeaderSize]);
bool decode_frame_header(const uint8_t* data, size_t size, FrameHeader& header,
                         std::string* error);

// Hello asks the server to stream one display: `display` is an index into the
// HelloAck display list from a previous session, or kDisplayAuto. The server
// answers with the display it actually captured, which also fixes origin_x /
// origin_y for the viewer's pointer mapping.
//
// `access_token` is the passphrase the server was started with, empty when it
// accepts connections without one. It travels inside the TLS-authenticated
// QUIC connection, never as plaintext, and the server compares it in constant
// time. A one-time token is consumed by the first accepted handshake.
inline constexpr size_t kMaxAccessTokenLength = 128;
inline constexpr size_t kMaxAccessDenialReason = 512;

// Why the server refused a Hello.
enum class AccessDenial : uint8_t {
    InvalidToken = 1,  // no token, or one that does not match
    TokenUsed = 2,     // the one-time token was already spent
    Declined = 3,      // the host operator said no
};

std::vector<uint8_t> make_hello(uint16_t display = kDisplayAuto,
                                const std::string& access_token = {});
bool parse_hello(const std::vector<uint8_t>& payload, uint16_t& display,
                 std::string& access_token, std::string* error);
// The server's refusal of a Hello. The viewer shows `reason` to the user.
std::vector<uint8_t> make_access_denied(AccessDenial code, const std::string& reason);
bool parse_access_denied(const std::vector<uint8_t>& payload, AccessDenial& code,
                         std::string& reason, std::string* error);
std::vector<uint8_t> make_hello_ack(const ServerInfo& info);
bool parse_hello_ack(const std::vector<uint8_t>& payload, ServerInfo& info, std::string* error);
// A mid-session replacement for the HelloAck parameters. Same payload; the
// parser above reads it.
std::vector<uint8_t> make_stream_info(const ServerInfo& info);
std::vector<uint8_t> make_stats(const ClientStats& stats);
bool parse_stats(const std::vector<uint8_t>& payload, ClientStats& stats, std::string* error);
// Clock-sync exchange for RTT and the host-minus-client clock offset. All
// values are steady_clock microseconds; the host echoes the client's stamp so
// an out-of-order reply can still be paired with its probe.
std::vector<uint8_t> make_latency_probe(uint64_t client_us);
bool parse_latency_probe(const std::vector<uint8_t>& payload, uint64_t& client_us,
                         std::string* error);
std::vector<uint8_t> make_latency_reply(uint64_t client_us, uint64_t host_recv_us,
                                        uint64_t host_send_us);
bool parse_latency_reply(const std::vector<uint8_t>& payload, uint64_t& client_us,
                         uint64_t& host_recv_us, uint64_t& host_send_us, std::string* error);
std::vector<uint8_t> make_input_event(const InputEvent& event);
bool parse_input_event(const std::vector<uint8_t>& payload, InputEvent& event, std::string* error);
// Returns an empty vector when `text` fails valid_clipboard_text(); callers
// then skip the update instead of sending an invalid message.
std::vector<uint8_t> make_clipboard_update(uint64_t seq, const std::string& text);
bool parse_clipboard_update(const std::vector<uint8_t>& payload, ClipboardUpdate& update,
                            std::string* error);
std::vector<uint8_t> make_control(ControlType type, const std::vector<uint8_t>& payload);

// Reassembles control messages from arbitrarily sized stream chunks.
class ControlParser {
public:
    // Appends `size` bytes and appends every complete message to `messages`.
    // Returns false (with `error`) on a malformed header or an oversized
    // message; the parser is then unusable.
    bool push(const uint8_t* data, size_t size,
              std::vector<std::pair<ControlType, std::vector<uint8_t>>>& messages,
              std::string* error);
    void reset();

private:
    std::vector<uint8_t> buffer_;
    size_t offset_ = 0;
};

// Reassembles frame headers and payloads from video-stream chunks. The header
// may be split anywhere, and a stream chunk may end mid-payload.
class FrameParser {
public:
    enum class Status {
        Incomplete,
        Frame,
        Error,
    };

    // Copies the chunk in. Returns false (with `error`) when the pending buffer
    // grows past the protocol limit; the parser is then unusable.
    bool push(const uint8_t* data, size_t size, std::string* error);
    // Frame: a complete frame is at the front, and `payload` points into the
    // parser's buffer until consume() or push(). Incomplete: more bytes needed.
    // Error: the header is malformed or the payload is oversized.
    Status next(FrameHeader& header, const uint8_t** payload, std::string* error);
    void consume();
    size_t buffered() const { return buffer_.size() - offset_; }

private:
    std::vector<uint8_t> buffer_;
    size_t offset_ = 0;
    size_t current_payload_ = 0;
};

}  // namespace chast
