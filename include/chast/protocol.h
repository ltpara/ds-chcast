#pragma once

// The chast wire protocol, version 1.
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

inline constexpr uint16_t kProtocolVersion = 2;
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
};

// ----- input -----
//
// The client sends one InputEvent message per pointer or keyboard event on the
// control stream, in the order they happened. Coordinates are in the host's
// virtual-desktop space (pixels), so the server never needs to know which
// output the viewer is looking at and a later multi-display UI needs no wire
// change. Keys travel as USB HID usage ids: layout independent, and mapped to
// scan codes on the injecting side.

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

struct InputEvent {
    InputKind kind = InputKind::Mouse;
    MouseEvent mouse;
    KeyEvent key;
};

struct FrameHeader {
    uint64_t seq = 0;
    uint64_t pts_ms = 0;
    uint32_t payload_size = 0;
    uint32_t payload_crc32 = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VideoCodec codec = VideoCodec::H264;
    bool keyframe = false;
    bool idr = false;
};

// HelloAck flags. Bit 0 tells the viewer it may send InputEvents.
inline constexpr uint16_t kServerInfoInputEnabled = 0x0001;

// What the server announces in HelloAck. `encoder` is informational (the VPL
// implementation that won the tier ladder); `codec` is what the video stream
// actually contains. `origin_x`/`origin_y` are the captured output's top-left
// in virtual-desktop pixels, which the viewer adds to the frame-space pointer
// position before sending it back. `input_enabled` mirrors the server's gate:
// when false the viewer must not send InputEvents (they would be dropped).
struct ServerInfo {
    VideoCodec codec = VideoCodec::H264;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t framerate = 30;
    uint32_t bitrate = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    bool input_enabled = false;
    std::string encoder;
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

std::vector<uint8_t> make_hello();
bool parse_hello(const std::vector<uint8_t>& payload, std::string* error);
std::vector<uint8_t> make_hello_ack(const ServerInfo& info);
bool parse_hello_ack(const std::vector<uint8_t>& payload, ServerInfo& info, std::string* error);
std::vector<uint8_t> make_stats(const ClientStats& stats);
bool parse_stats(const std::vector<uint8_t>& payload, ClientStats& stats, std::string* error);
std::vector<uint8_t> make_input_event(const InputEvent& event);
bool parse_input_event(const std::vector<uint8_t>& payload, InputEvent& event, std::string* error);
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
