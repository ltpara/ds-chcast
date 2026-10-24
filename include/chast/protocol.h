#pragma once

// The chast wire protocol, version 22.
//
// One QUIC connection carries:
//
//   * a client-initiated bidirectional control stream (Hello / HelloAck,
//     key-frame requests, input events, file offers, closing statistics),
//   * a server-initiated unidirectional video stream: a sequence of access
//     units, each prefixed by a fixed FrameHeader,
//   * when the host shares system audio, a second server-initiated
//     unidirectional stream of Opus access units, each prefixed by an
//     AudioHeader,
//   * when the viewer's microphone is granted, a viewer-initiated
//     unidirectional microphone stream (`CMIC`): a format header followed by
//     Opus access units,
//   * one unidirectional stream per clipboard change (`CCLB`), opened by the
//     side that copied, and
//   * one unidirectional stream per file transfer (`CFIL`), opened by the
//     side that offered the file: the viewer uploads to the host and the host
//     downloads to the viewer.
//
// The streams are told apart by their packet magic, not by the order the
// streams were opened: QUIC does not promise that the first bytes of a later
// stream arrive after an earlier one. Video is `CFRM`, system audio is
// `CAUD`, the microphone is `CMIC`, clipboard content is `CCLB` and file data
// is `CFIL`.
//
// QUIC streams give the media reliable, ordered delivery; the sequence number
// is still carried so the receiver can report gaps and so a future version can
// move packets to independent streams or datagrams without changing the header.
// Every payload carries a CRC32 because the transport is only part of the
// pipeline: it catches assembly or ordering mistakes in this protocol, not
// corruption on the wire.
//
// All integers are little-endian. Peers exchange the protocol version and the
// stream's actual codec in the handshake; a requested HEVC stream that fell
// back to AVC is announced as AVC.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "chast/encoder.h"

namespace chast {

inline constexpr uint16_t kProtocolVersion = 23;
// Complies with the ALPN private-use range. The ALPN names the protocol
// family, not its version: a mismatch is reported by the Hello exchange with
// the peer's number instead of as an opaque handshake failure.
inline constexpr char kAlpn[] = "chast/1";

inline constexpr size_t kFrameHeaderSize = 48;
inline constexpr size_t kAudioHeaderSize = 48;
inline constexpr size_t kControlHeaderSize = 12;
inline constexpr uint32_t kMaxFramePayload = 32u * 1024u * 1024u;
inline constexpr uint32_t kMaxAudioPayload = 64u * 1024u;
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
    // The host's pointer as the viewer should draw it. The sprite travels once
    // per shape; positions that reuse the same shape are 24 bytes. Sent only
    // when the shape, the position or the visibility actually changed, so an
    // idle pointer costs nothing. See CursorUpdate for the payload.
    CursorUpdate = 13,
    // The host's grants changed mid-session (the operator ticked a checkbox
    // in the session window, or a policy revoked a permission). Payload: the
    // u16 permission mask. The viewer applies it immediately and stops
    // sending input or clipboard it no longer holds; audio teardown follows
    // the stream state in the next StreamInfo.
    PermissionUpdate = 14,
    // Mid-session display switch. Payload: the u16 display index (an index
    // into the HelloAck display list, or kDisplayAuto for the host's default).
    // The server answers with DisplaySwitchResult on the same control stream;
    // the video stream stays up throughout. Repeating the current display is a
    // no-op answered with Ok.
    SwitchDisplay = 15,
    // Answer to SwitchDisplay. Payload: requested index (u16), code (u8),
    // reason length (u16), UTF-8 reason. Ok means a StreamInfo carrying the new
    // display follows (or that nothing had to change); any other code leaves
    // the previous stream untouched. See DisplaySwitchCode.
    DisplaySwitchResult = 16,
    // Asks for the modes the output reports. Payload: display index (u16).
    // The host answers with DisplayModes. Listing is allowed without input
    // permission; only SetDisplayMode is gated.
    ListDisplayModes = 17,
    // Answer to ListDisplayModes. Payload: display index (u16), code (u8),
    // reason length (u16), UTF-8 reason, mode count (u16), then per mode
    // width/height/refresh (u16 each) and flags (u16, bit 0 = current). Sizes
    // are desktop-space: a 90/270 panel reports the swapped pair.
    DisplayModes = 18,
    // Changes one output's mode for this session. Payload: display index,
    // width, height and refresh (u16 each). The display must be the one being
    // streamed, and the mode must appear in the latest DisplayModes answer.
    // Requires input permission. The answer is DisplayModeResult; on Ok a
    // StreamInfo describing the new source size follows.
    SetDisplayMode = 19,
    // Answer to SetDisplayMode. Payload: display index, width, height,
    // refresh (u16 each), code (u8), reason length (u16), UTF-8 reason. Ok
    // means the mode is in effect and a StreamInfo follows; every other code
    // leaves the previous mode and stream in place. See DisplayModeCode.
    DisplayModeResult = 20,
    // Sender -> receiver, one file transfer (a viewer upload or a host
    // download). Payload: transfer id (u32), size (u64), flags (u8), SHA-256
    // of the whole file (32 B), name length (u16) and UTF-8 name. The
    // receiver answers with FileAnswer; accepted bytes follow on a CFIL
    // stream opened by the sender.
    FileOffer = 21,
    // Receiver -> sender answer to FileOffer. Payload: transfer id (u32),
    // code (u8), resume offset (u64), reason length (u16), UTF-8 reason. Ok
    // means the sender should open the CFIL stream at resume_offset.
    FileAnswer = 22,
    // Receiver -> sender final result. Payload: transfer id (u32), code (u8),
    // bytes written (u64), reason length (u16), UTF-8 reason. Sent once the
    // CFIL stream ended and the digest was checked, or when the transfer was
    // aborted.
    FileResult = 23,
    // Either side cancels a transfer. Payload: transfer id (u32), code (u8),
    // reason length (u16), UTF-8 reason. The receiver closes and removes the
    // partial file.
    FileCancel = 24,
    // Viewer -> host: hide or restore the host's physical display. Payload:
    // enabled (u8). Requires the privacy-mode permission; the host answers
    // with PrivacyModeState. Enabling is refused rather than negotiated when
    // the host's settings or the platform do not allow it.
    SetPrivacyMode = 25,
    // Host -> viewer: the privacy overlay's state. Payload: enabled (u8),
    // code (u8), reason length (u16), UTF-8 reason. Sent as the answer to
    // SetPrivacyMode and unsolicited when the host turns the overlay off
    // itself (permission revoked, session ending). See PrivacyModeCode.
    PrivacyModeState = 26,
    // Host -> viewer: which desktop the stream is showing. Payload: kind
    // (u8). Sent when it changes and once after HelloAck. See DesktopKind.
    DesktopState = 27,
    // Viewer -> host: inject the Secure Attention Sequence (Ctrl+Alt+Del).
    // No payload. Windows only accepts it through SendSAS() from LocalSystem
    // or a policy-enabled process, so it cannot be synthesized as a key
    // chord. Requires input permission; the host answers with SasResult.
    SendSas = 28,
    // Host -> viewer: the outcome of SendSas. Payload: code (u8), reason
    // length (u16), UTF-8 reason. See SasCode.
    SasResult = 29,
    // Viewer -> host: the video preferences for this session. Payload:
    // codec (u8, see StreamCodecPreference), maximum frame rate (u16,
    // 0 = the host's configured rate), and quality (u8, see StreamQuality).
    // The host can only lower its own budget, never raise it; the answer is
    // StreamOptionsResult. Changing preferences does not require input
    // permission because it only affects this viewer's picture.
    SetStreamOptions = 30,
    // Host -> viewer: the outcome of SetStreamOptions. Payload: code (u8),
    // effective codec (u8, 1 = H.264, 2 = HEVC), effective frame rate (u16),
    // effective quality (u8), reason length (u16), UTF-8 reason. The
    // effective fields describe the stream that is actually running, even
    // when the request was refused, so the viewer can restore its menu.
    StreamOptionsResult = 31,
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
// Rich clipboard content (text, HTML, RTF and DIB) travels on its own
// unidirectional CCLB stream, one stream per change. The container and its
// limits live in chast/clipboard.h; the legacy ClipboardUpdate control message
// below stays defined for older tools but this build only sends CCLB.
inline constexpr size_t kMaxClipboardBytes = 256u * 1024u;

// True when `text` is a non-empty, well-formed UTF-8 string of at most
// kMaxClipboardBytes with no NUL. NUL is rejected because Windows'
// CF_UNICODETEXT is NUL-terminated and would silently truncate.
bool valid_clipboard_text(const std::string& text);
// Well-formed UTF-8 without NUL, no size limit. Shared by clipboard and
// control-message validation.
bool valid_utf8(const std::string& text);

struct ClipboardUpdate {
    uint64_t seq = 0;
    std::string text;
};

// ----- file transfer -----
//
// A transfer starts with FileOffer on the control stream and, once the
// receiver answers Ok, a unidirectional CFIL stream from the sender carrying
// the bytes. Either side may start one: the viewer uploads to the host and the
// host downloads to the viewer. The control stream never carries file data.
inline constexpr char kFileStreamMagic[4] = {'C', 'F', 'I', 'L'};
inline constexpr uint8_t kFileStreamVersion = 1;
// magic (4) + version (1) + reserved (3) + transfer id (4) + offset (8).
inline constexpr size_t kFileStreamHeaderSize = 20;
inline constexpr size_t kFileChunkBytes = 64u * 1024u;
// Protocol ceiling; the host applies a smaller, configurable limit.
inline constexpr uint64_t kMaxFileTransferBytes = 16ull * 1024ull * 1024ull * 1024ull;
inline constexpr size_t kMaxFileNameBytes = 512;
// Directory part of an offer, relative to the receiver's root and '/'
// separated. Empty means the root itself.
inline constexpr size_t kMaxFileRelativePathBytes = 1024;
inline constexpr size_t kMaxFileRelativeComponents = 32;
inline constexpr size_t kMaxFileReasonBytes = 256;
inline constexpr uint8_t kFileOfferFlagResume = 0x01;
inline constexpr uint8_t kFileOfferFlagsKnown = kFileOfferFlagResume;

enum class FileCode : uint8_t {
    Ok = 0,
    Refused = 1,         // policy, size, disk space, or unusable name
    Busy = 2,            // another transfer owns the receive slot
    NotFound = 3,        // the transfer id is not the active offer
    DigestMismatch = 4,  // the received bytes do not hash to the offer
    IoError = 5,         // the host could not write or finalize the file
    Cancelled = 6,       // the viewer (or a permission change) cancelled
};

struct FileOffer {
    uint32_t transfer_id = 0;
    uint64_t size = 0;
    bool resume = false;
    std::array<uint8_t, 32> sha256{};
    std::string name;
    // Directory below the receiver's root, '/'-separated and without the
    // file name. Empty keeps the file at the root. The receiver sanitizes
    // each component before it creates anything.
    std::string relative_path;
};

struct FileAnswer {
    uint32_t transfer_id = 0;
    FileCode code = FileCode::Ok;
    uint64_t resume_offset = 0;
    std::string reason;
};

struct FileResult {
    uint32_t transfer_id = 0;
    FileCode code = FileCode::Ok;
    uint64_t received = 0;
    std::string reason;
};

struct FileCancel {
    uint32_t transfer_id = 0;
    FileCode code = FileCode::Cancelled;
    std::string reason;
};

// CFIL stream prefix. The bytes after it are raw file content starting at
// `offset`; the stream ends with a QUIC FIN.
struct FileStreamHeader {
    uint32_t transfer_id = 0;
    uint64_t offset = 0;
};
void encode_file_stream_header(const FileStreamHeader& header,
                               uint8_t out[kFileStreamHeaderSize]);
bool decode_file_stream_header(const uint8_t* data, size_t size, FileStreamHeader& header,
                               std::string* error);

// ----- pointer -----
//
// The host draws its pointer itself (Desktop Duplication leaves it out of the
// desktop image), so the viewer has to be told both the sprite and where it
// is. The sprite only rides along when its id changes; a position-only update
// is 26 bytes.

inline constexpr uint16_t kMaxCursorSize = 256;

struct CursorUpdate {
    // The host's pointer is on the captured output and should be drawn. False
    // while the host hides it (a full-screen video, an idle timer) or when it
    // is over another output.
    bool visible = false;
    // `x`/`y` are in source pixels: the captured desktop's coordinate space,
    // which is also the space the viewer's own pointer is mapped to and what
    // ServerInfo::source_width/source_height describe. Independent of the
    // encoded picture size, so adaptive downscaling does not move the pointer.
    // The sprite's top-left lands on them.
    int32_t x = 0;
    int32_t y = 0;
    // The pixel inside the sprite that sits on (x, y).
    uint16_t hotspot_x = 0;
    uint16_t hotspot_y = 0;
    // Identity of `sprite`; stable across revisits, so an id the viewer
    // already has needs no image. Zero when no sprite is attached.
    uint64_t shape_id = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    // Premultiplied BGRA, width*height*4 bytes. Empty when the update only
    // moves a sprite the viewer already holds; `width`/`height` still
    // describe it so the viewer can place a shape it does not have yet.
    std::vector<uint8_t> sprite;
};

// ----- audio -----
//
// System sound travels one way on a server-initiated `CAUD` stream; the
// viewer's microphone travels the other way on a viewer-initiated `CMIC`
// stream. Both carry raw Opus access units in production. The decoder config
// for `CAUD` rides in AudioInfo (HelloAck / StreamInfo); `CMIC` self-describes
// with a header before its first packet, because the viewer chooses the
// device and therefore the format. AAC-LC remains decodable for older
// recordings and the codec tests, but no session creates an AAC stream.
enum class AudioCodec : uint8_t {
    AAC = 1,   // AAC-LC, raw access units
    Opus = 2,  // raw Opus packets, 20 ms frames
};

inline constexpr size_t kMaxAudioExtradata = 64;
inline constexpr uint32_t kAudioFrameSamples = 1024;
inline constexpr uint32_t kOpusFrameSamples = 960;  // 20 ms at 48 kHz

struct AudioInfo {
    bool enabled = false;
    AudioCodec codec = AudioCodec::AAC;
    uint32_t sample_rate = 48000;
    uint16_t channels = 2;
    uint32_t bitrate = 0;  // bits per second, informational
    uint16_t frame_samples = kAudioFrameSamples;
    // Codec config: OpusHead for Opus, AudioSpecificConfig for AAC. Empty
    // when audio is disabled; the decoder needs it before the first packet.
    std::vector<uint8_t> extradata;
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

// One compressed audio access unit. A media stream never mixes audio and
// video packets: this header starts with its own magic so the receiver can
// classify a server-initiated stream from its first bytes.
struct AudioHeader {
    uint64_t seq = 0;
    // Presentation timestamp on the host session's timeline, same origin as
    // FrameHeader::pts_ms. The viewer schedules PCM against it after
    // correcting with the clock offset from LatencyReply.
    uint64_t pts_ms = 0;
    // Host steady_clock milliseconds at capture, low 48 bits, same clock as
    // FrameHeader::capture_ms.
    uint64_t capture_ms = 0;
    uint32_t payload_size = 0;
    uint32_t payload_crc32 = 0;
    uint32_t sample_rate = 0;
    uint16_t frame_samples = 0;  // decoded samples per channel
    uint16_t channels = 0;
    AudioCodec codec = AudioCodec::Opus;
};

// ----- microphone uplink -----
//
// The viewer opens one client-initiated unidirectional stream per microphone
// session. Its first bytes describe the device format; the rest are
// AudioHeader + payload packets exactly like `CAUD`. Opening the stream turns
// the microphone on and closing it turns it off, so no control message is
// needed and a denied or revoked permission is simply a refused stream.
inline constexpr char kMicrophoneStreamMagic[4] = {'C', 'M', 'I', 'C'};
inline constexpr size_t kMicrophoneStreamHeaderSize = 20;
inline constexpr uint8_t kMicrophoneStreamVersion = 1;

// Fixed 20-byte header followed by AudioInfo::extradata. The header carries
// the codec, rate, channel count, frame length and extradata size; trim
// nothing, the returned vector is exactly what goes on the stream.
std::vector<uint8_t> make_microphone_stream_header(const AudioInfo& info);
// Full header length (20 + extradata) once the fixed part has arrived, or 0
// while it is incomplete or names an unusable extradata size. A receiver uses
// it to wait for a split header without rejecting a legitimately truncated
// first packet.
size_t microphone_stream_header_length(const uint8_t* data, size_t size);
// Parses the fixed part and the extradata. `consumed` is 20 + extradata size,
// which is where the first AudioHeader starts. Rejects a truncated header,
// unknown version/codec, unsupported rate, mono/stereo-plus channels, an
// extradata overrun, and any non-zero reserved field.
bool decode_microphone_stream_header(const uint8_t* data, size_t size, AudioInfo& info,
                                     size_t& consumed, std::string* error);

// HelloAck flags. Bit 0 tells the viewer it may send InputEvents; bit 1 tells
// it that both sides may exchange clipboard content; bit 2 tells it the host
// opened an audio stream and described the format in the audio block; bit 3
// tells it that file offers are accepted; bit 4 that the viewer may open a
// microphone stream; bit 5 that SetPrivacyMode is accepted. Bit 6 is not a
// permission: it says the picture is a merged layout of every attached output,
// with origin/source covering their union.
inline constexpr uint16_t kServerInfoInputEnabled = 0x0001;
inline constexpr uint16_t kServerInfoClipboardEnabled = 0x0002;
inline constexpr uint16_t kServerInfoAudioEnabled = 0x0004;
inline constexpr uint16_t kServerInfoFileTransferEnabled = 0x0008;
inline constexpr uint16_t kServerInfoMicrophoneEnabled = 0x0010;
inline constexpr uint16_t kServerInfoPrivacyModeEnabled = 0x0020;
inline constexpr uint16_t kServerInfoMergedLayout = 0x0040;
// Flags that describe what the viewer may do. The permission mask appended to
// HelloAck is the authority for these bits, so the parser compares it with
// this mask rather than with every known flag.
inline constexpr uint16_t kServerInfoPermissionFlags = kServerInfoInputEnabled |
                                                       kServerInfoClipboardEnabled |
                                                       kServerInfoAudioEnabled |
                                                       kServerInfoFileTransferEnabled |
                                                       kServerInfoMicrophoneEnabled |
                                                       kServerInfoPrivacyModeEnabled;
inline constexpr uint16_t kServerInfoKnownFlags = kServerInfoPermissionFlags |
                                                  kServerInfoMergedLayout;

// The permission mask. It is the v13 authority: HelloAck/StreamInfo append it
// after the source size, PermissionUpdate replaces it mid-session, and the
// ServerInfo booleans are its decoded form. Input covers pointer and
// keyboard injection; clipboard governs clipboard streams in both directions;
// audio is present only while an audio stream is open; microphone allows the
// viewer to open a `CMIC` uplink; file transfer gates FileOffer; privacy mode
// gates SetPrivacyMode.
enum PermissionBit : uint16_t {
    kPermissionInput = 0x0001,
    kPermissionClipboard = 0x0002,
    kPermissionAudio = 0x0004,
    kPermissionFileTransfer = 0x0008,
    kPermissionMicrophone = 0x0010,
    kPermissionPrivacyMode = 0x0020,
};
inline constexpr uint16_t kPermissionAll = kPermissionInput | kPermissionClipboard |
                                           kPermissionAudio | kPermissionFileTransfer |
                                           kPermissionMicrophone | kPermissionPrivacyMode;

// Informational platform tag in the trust list.
enum class DevicePlatform : uint8_t {
    Unknown = 0,
    Windows = 1,
};

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
// Hello display index meaning "every attached output, merged into one picture
// in virtual-desktop layout". The host may refuse (no displays, adapters that
// cannot share a device, an oversized union) and then answers with its default
// single display; HelloAck's merged flag says which happened.
inline constexpr uint16_t kDisplayAll = 0xFFFE;
// Upper bound on a HelloAck display list; also the valid range of a Hello
// display index. A machine with more outputs than this is not something the
// viewer UI could present anyway, and the cap bounds the work a malformed
// HelloAck can cause.
inline constexpr uint16_t kMaxDisplayCount = 64;

// Why a SwitchDisplay was refused. Ok is the only code that changes the
// stream; the viewer keeps its current picture on every other code and shows
// `reason` (empty when the server has nothing to add).
enum class DisplaySwitchCode : uint8_t {
    Ok = 0,
    NotFound = 1,  // the index is outside the server's current display list
    Failed = 2,    // the target output exists but could not be captured
    Busy = 3,      // a previous switch is still settling; retry after a moment
};
// Bound on the UTF-8 reason; the payload stays small enough to be a control
// message and a longer explanation belongs in the host log, not the wire.
inline constexpr size_t kMaxDisplaySwitchReason = 256;

// Why a SetDisplayMode was refused. Ok is the only code that switches the
// output; every other code leaves the current mode (and the stream) in place.
enum class DisplayModeCode : uint8_t {
    Ok = 0,
    NotFound = 1,  // the display is gone, or it does not report that mode
    Failed = 2,    // the driver refused the mode or the stream could not rebuild
    Busy = 3,      // a mode change is still settling; retry after a moment
};
inline constexpr size_t kMaxDisplayModeReason = 256;
// Upper bound on one DisplayModes list. The host enumeration stops here too.
inline constexpr uint16_t kMaxDisplayModes = 256;

struct DisplayModeInfo {
    // Desktop-space size (rotation already applied), matching the viewer's
    // picture orientation.
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t refresh_hz = 0;
    bool current = false;
};

// One DisplayModes answer: the list for `display`, or a refusal with `reason`
// and no modes.
struct DisplayModeList {
    uint16_t display = 0;
    DisplayModeCode code = DisplayModeCode::Ok;
    std::string reason;
    std::vector<DisplayModeInfo> modes;
};

// What the server announces in HelloAck. `encoder` is informational (the VPL
// implementation that won the tier ladder); `codec` is what the video stream
// actually contains. `width`/`height` are the encoded picture size, which
// adaptive resolution may scale below the captured frame; `source_width`/
// `source_height` are the captured desktop's size in pixels, the space pointer
// positions, cursor sprites and input coordinates use. The two are equal when
// the stream is not scaled. `origin_x`/`origin_y` are the captured output's
// top-left in virtual-desktop pixels, which the viewer adds to the source-space
// pointer position before sending it back. `input_enabled` mirrors the server's
// gate: when false the viewer must not send InputEvents (they would be
// dropped). `displays` lists every attached output; the one being streamed is
// the entry whose rect starts at the origin and is `source_width` x
// `source_height` pixels; a merged stream has no such entry, `merged` is true,
// and the lists are its members. `clipboard_enabled` mirrors the server's
// clipboard gate: when false the viewer must not send clipboard content and
// will not receive any. `file_transfer_enabled` gates FileOffer. `audio.enabled` says
// whether an audio stream follows; when true, `audio` carries the codec, rate,
// channel count and AudioSpecificConfig needed to decode it. Audio that fails
// mid-session is turned off with a StreamInfo.
struct ServerInfo {
    VideoCodec codec = VideoCodec::H264;
    uint16_t width = 0;         // encoded picture size
    uint16_t height = 0;
    uint16_t source_width = 0;  // captured desktop size (pointer/input space)
    uint16_t source_height = 0;
    uint16_t framerate = 30;
    uint32_t bitrate = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    bool input_enabled = false;
    bool clipboard_enabled = false;
    bool file_transfer_enabled = false;
    // The picture is the union of every attached output (kDisplayAll), not a
    // single captured output. `origin_x/y` and `source_width/height` describe
    // that union; `displays` lists the members.
    bool merged = false;
    // The viewer may open a microphone stream. This is the permission bit's
    // decoded form; it is not a separate wire field.
    bool microphone_enabled = false;
    // The host accepts SetPrivacyMode. Decoded from the permission mask, like
    // the other booleans; the overlay's current state travels separately in
    // PrivacyModeState because a reconnect always starts with it off.
    bool privacy_mode_enabled = false;
    std::string encoder;
    std::vector<DisplayDescription> displays;
    AudioInfo audio;
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
void encode_audio_header(const AudioHeader& header, uint8_t out[kAudioHeaderSize]);
bool decode_audio_header(const uint8_t* data, size_t size, AudioHeader& header,
                         std::string* error);

// Hello asks the server to stream one display: `display` is an index into the
// HelloAck display list from a previous session, kDisplayAuto, or kDisplayAll
// for the merged view. The server answers with what it actually captured,
// either one display or the merged union, and that answer fixes origin_x /
// origin_y for the viewer's pointer mapping.
//
// Authentication travels in the same message. `access_token` is the legacy
// passphrase for CLI and automation (empty unless the host enabled it);
// `password` is what the interactive viewer collects from the user. A signed
// Hello proves the device key belongs to this request: the signature covers
// the host fingerprint, `nonce` and `unix_seconds`, so a replayed Hello fails
// once the nonce cache expires or the clock window closes.
inline constexpr size_t kMaxAccessTokenLength = 128;
inline constexpr size_t kMaxAccessDenialReason = 512;
inline constexpr size_t kMaxPasswordLength = 128;
inline constexpr size_t kMaxDeviceKeyLength = 128;
inline constexpr size_t kMaxDeviceNameLength = 64;
inline constexpr size_t kMaxTotpLength = 16;
inline constexpr size_t kMaxSignatureLength = 128;
inline constexpr size_t kHelloNonceBytes = 16;

// auth_flags bits.
inline constexpr uint8_t kAuthFlagTrustDevice = 0x01;  // add this key to the trust list
inline constexpr uint8_t kAuthFlagSigned = 0x02;       // signature is present
inline constexpr uint8_t kAuthFlagsKnown = kAuthFlagTrustDevice | kAuthFlagSigned;

struct ClientHello {
    uint16_t display = kDisplayAuto;
    std::string access_token;
    std::string password;
    // Bits from PermissionBit. The host intersects this with its defaults and
    // with what the operator approves.
    uint16_t requested_permissions = kPermissionAll;
    // ECDSA P-256 public key blob. Empty when the client has no identity (the
    // CLI tools); a signed flag requires it.
    std::vector<uint8_t> device_key;
    uint8_t device_platform = static_cast<uint8_t>(DevicePlatform::Unknown);
    std::string device_name;
    std::array<uint8_t, kHelloNonceBytes> nonce{};
    uint64_t unix_seconds = 0;
    uint8_t auth_flags = 0;
    // ECDSA-SHA256 over host_fingerprint || nonce || unix_seconds (little
    // endian). Empty when unsigned.
    std::vector<uint8_t> signature;
    // Six ASCII digits, empty when the client has nothing to prove.
    std::string totp;
};

// The bytes a trusted device signs: the host's canonical (64 uppercase hex
// digit) certificate fingerprint, the nonce, and unix_seconds as a
// little-endian u64. Binding the fingerprint keeps a captured proof from
// working against another host; the nonce and timestamp keep it from working
// again later.
inline std::vector<uint8_t> hello_signature_message(const std::string& host_fingerprint,
                                                    const ClientHello& hello) {
    std::vector<uint8_t> message(host_fingerprint.begin(), host_fingerprint.end());
    message.insert(message.end(), hello.nonce.begin(), hello.nonce.end());
    for (int i = 0; i < 8; ++i) {
        message.push_back(static_cast<uint8_t>(hello.unix_seconds >> (8 * i)));
    }
    return message;
}

// Why the server refused a Hello.
enum class AccessDenial : uint8_t {
    InvalidToken = 1,      // no token, or one that does not match
    TokenUsed = 2,         // the one-time token was already spent
    Declined = 3,          // the host operator said no
    WrongPassword = 4,     // password auth failed; the viewer may retry
    PasswordRequired = 5,  // the mode requires a password and none was given
    TwoFactorRequired = 6, // the password was right; reconnect with a TOTP code
    WrongTwoFactor = 7,    // the TOTP code was wrong or expired
    LockedOut = 8,         // too many failures from this address
    NotWhitelisted = 9,    // the source address is outside the IP whitelist
    AuthTimeout = 10,      // a challenge was not answered in time
};

std::vector<uint8_t> make_hello(const ClientHello& hello);
bool parse_hello(const std::vector<uint8_t>& payload, ClientHello& hello,
                 std::string* error);
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
// CursorUpdate payloads. The maker drops an attached sprite that does not
// match width*height*4 and clamps the size to kMaxCursorSize; callers that
// keep the sprite can send the same update again with an empty one.
std::vector<uint8_t> make_cursor_update(const CursorUpdate& update);
bool parse_cursor_update(const std::vector<uint8_t>& payload, CursorUpdate& update,
                         std::string* error);
// Host -> viewer permission change. Unknown bits are rejected so a newer
// sender cannot silently grant something this build does not understand.
std::vector<uint8_t> make_permission_update(uint16_t permissions);
bool parse_permission_update(const std::vector<uint8_t>& payload,
                             uint16_t& permissions, std::string* error);

// ----- privacy mode and desktop state -----
//
// Privacy mode hides the host's physical display behind a topmost overlay
// while leaving the captured picture untouched, so the remote viewer keeps
// seeing the desktop. It is requested by the viewer and answered with the
// resulting state; the host may also send a state message on its own when it
// turns the overlay off (permission revoked, session ending).

// Why a privacy-mode request was refused. Ok means the resulting state matches
// the request; every other code leaves the overlay off.
enum class PrivacyModeCode : uint8_t {
    Ok = 0,
    Denied = 1,       // no permission, or the host settings do not allow it
    Unsupported = 2,  // this Windows build or session cannot hide the display
    Failed = 3,       // the overlay could not be shown or removed
};
inline constexpr size_t kMaxPrivacyModeReason = 256;

// Which desktop the video stream is capturing. A secure desktop (the lock
// screen, UAC, a credential prompt) is a different Windows desktop; the host
// announces the change so the viewer can explain a paused or changed picture.
enum class DesktopKind : uint8_t {
    Default = 0,      // the interactive user's desktop
    Secure = 1,       // a secure desktop: lock screen, UAC, credential prompt
    Unavailable = 2,  // no desktop is accessible (sign-out, session switch)
};

std::vector<uint8_t> make_set_privacy_mode(bool enabled);
bool parse_set_privacy_mode(const std::vector<uint8_t>& payload, bool& enabled,
                            std::string* error);
// The maker truncates `reason` on a UTF-8 boundary; the parser rejects an
// over-long or malformed reason instead of truncating it.
std::vector<uint8_t> make_privacy_mode_state(bool enabled, PrivacyModeCode code,
                                             const std::string& reason);
bool parse_privacy_mode_state(const std::vector<uint8_t>& payload, bool& enabled,
                              PrivacyModeCode& code, std::string& reason,
                              std::string* error);
std::vector<uint8_t> make_desktop_state(DesktopKind kind);
bool parse_desktop_state(const std::vector<uint8_t>& payload, DesktopKind& kind,
                         std::string* error);

// ----- secure attention sequence -----
//
// Ctrl+Alt+Del is not an ordinary key chord: Windows only accepts it from
// SendSAS(), callable by LocalSystem services (the service worker) or a
// process covered by the SoftwareSASGeneration policy. The viewer therefore
// asks for it explicitly instead of injecting E0/E2/E4.
//
// The host checks input permission itself and answers every request, so a
// viewer that lost control learns why instead of getting silence. The tray
// host reports Unsupported because it cannot call SendSAS.

// Why a Secure Attention Sequence request did not reach the secure desktop.
enum class SasCode : uint8_t {
    Ok = 0,
    Denied = 1,       // input control is not granted to this viewer
    Unsupported = 2,  // this host cannot send SAS (tray host, older Windows)
    Failed = 3,       // sas.dll is missing or the call could not be made
};
inline constexpr size_t kMaxSasReason = 256;

std::vector<uint8_t> make_send_sas();
bool parse_send_sas(const std::vector<uint8_t>& payload, std::string* error);
// The maker truncates `reason` on a UTF-8 boundary; the parser rejects an
// over-long or malformed reason instead of truncating it.
std::vector<uint8_t> make_sas_result(SasCode code, const std::string& reason);
bool parse_sas_result(const std::vector<uint8_t>& payload, SasCode& code,
                      std::string& reason, std::string* error);

// ----- stream options -----
//
// The viewer chooses how its own picture is produced: encoder family, a
// frame-rate ceiling, and a quality preset that scales the host's video
// budget. The host keeps its configured bitrate as a hard ceiling and never
// follows a preference beyond it, so a viewer can only ask for less. The
// choice lives for the session; a reconnect starts from the host's defaults
// until the viewer sends it again.

enum class StreamCodecPreference : uint8_t {
    Auto = 0,  // follow the host's configured encoder
    H264 = 1,  // require H.264
    Hevc = 2,  // require HEVC; refused when the host cannot produce it
};

enum class StreamQuality : uint8_t {
    High = 0,      // the full video budget
    Balanced = 1,  // 75% of it
    Smooth = 2,    // 50% of it
};

// Why a preference change did not fully take effect. Ok means the stream now
// matches the request (or already did); every other code leaves the running
// stream untouched.
enum class StreamOptionsCode : uint8_t {
    Ok = 0,
    Unsupported = 1,  // the host cannot produce the requested encoder
    Failed = 2,       // the encoder could not be rebuilt or reprogrammed
    Busy = 3,         // a codec change just happened; retry after a moment
};
// The host setting's own range; a viewer cap outside it is malformed.
inline constexpr uint16_t kMaxStreamFps = 240;
inline constexpr size_t kMaxStreamOptionReason = 256;

struct StreamOptions {
    StreamCodecPreference codec = StreamCodecPreference::Auto;
    // 0 means the host's configured rate, otherwise 1..kMaxStreamFps. This is
    // a ceiling: the adaptive ladder may still go below it.
    uint16_t max_fps = 0;
    StreamQuality quality = StreamQuality::High;
};

struct StreamOptionsResult {
    StreamOptionsCode code = StreamOptionsCode::Ok;
    // What the running encoder actually produces, and the frame-rate ceiling
    // and quality preset now in effect. On a refusal these describe the
    // unchanged stream.
    VideoCodec codec = VideoCodec::H264;
    uint16_t fps = 0;
    StreamQuality quality = StreamQuality::High;
    std::string reason;
};

std::vector<uint8_t> make_set_stream_options(const StreamOptions& options);
bool parse_set_stream_options(const std::vector<uint8_t>& payload, StreamOptions& options,
                              std::string* error);
// The maker truncates `reason` on a UTF-8 boundary; the parser rejects an
// over-long or malformed reason and an unknown code instead of repairing it.
std::vector<uint8_t> make_stream_options_result(const StreamOptionsResult& result);
bool parse_stream_options_result(const std::vector<uint8_t>& payload,
                                 StreamOptionsResult& result, std::string* error);

// Viewer -> host display switch. `display_index` is any u16; the host decides
// whether it names one of its outputs (kDisplayAuto always means its default).
std::vector<uint8_t> make_switch_display(uint16_t display_index);
bool parse_switch_display(const std::vector<uint8_t>& payload, uint16_t& display_index,
                          std::string* error);
// Host -> viewer result. The maker truncates `reason` to
// kMaxDisplaySwitchReason bytes without splitting a UTF-8 sequence; the parser
// rejects an over-long reason instead of truncating it.
std::vector<uint8_t> make_display_switch_result(uint16_t display_index, DisplaySwitchCode code,
                                                const std::string& reason);
bool parse_display_switch_result(const std::vector<uint8_t>& payload, uint16_t& display_index,
                                 DisplaySwitchCode& code, std::string& reason,
                                 std::string* error);
// Viewer -> host mode list request for one announced display.
std::vector<uint8_t> make_list_display_modes(uint16_t display_index);
bool parse_list_display_modes(const std::vector<uint8_t>& payload, uint16_t& display_index,
                              std::string* error);
// Host -> viewer mode list. The maker truncates `reason` on a UTF-8 boundary
// and drops entries past kMaxDisplayModes; the parser rejects a list that
// breaks the wire rules (unknown flags, more than one current mode, zero
// sizes) instead of repairing it.
std::vector<uint8_t> make_display_modes(const DisplayModeList& list);
bool parse_display_modes(const std::vector<uint8_t>& payload, DisplayModeList& list,
                         std::string* error);
// Viewer -> host mode change for the streamed display.
std::vector<uint8_t> make_set_display_mode(uint16_t display_index, uint16_t width,
                                           uint16_t height, uint16_t refresh_hz);
bool parse_set_display_mode(const std::vector<uint8_t>& payload, uint16_t& display_index,
                            uint16_t& width, uint16_t& height, uint16_t& refresh_hz,
                            std::string* error);
// Host -> viewer answer, echoing the requested mode so an out-of-order answer
// can still be paired with its request.
std::vector<uint8_t> make_display_mode_result(uint16_t display_index, uint16_t width,
                                              uint16_t height, uint16_t refresh_hz,
                                              DisplayModeCode code, const std::string& reason);
bool parse_display_mode_result(const std::vector<uint8_t>& payload, uint16_t& display_index,
                               uint16_t& width, uint16_t& height, uint16_t& refresh_hz,
                               DisplayModeCode& code, std::string& reason, std::string* error);
// Joins the top-level upload folder and the native path below it into the
// '/'-separated directory FileOffer carries. Empty or "." relatives mean the
// top folder itself; a leading separator is ignored.
std::string make_offer_relative_path(const std::string& top, const std::string& relative);
// File transfer control messages. The makers truncate reasons/names on a
// UTF-8 boundary; the parsers reject unknown codes, flags, empty or oversized
// names and trailing bytes rather than repairing them.
std::vector<uint8_t> make_file_offer(const FileOffer& offer);
bool parse_file_offer(const std::vector<uint8_t>& payload, FileOffer& offer, std::string* error);
std::vector<uint8_t> make_file_answer(const FileAnswer& answer);
bool parse_file_answer(const std::vector<uint8_t>& payload, FileAnswer& answer,
                       std::string* error);
std::vector<uint8_t> make_file_result(const FileResult& result);
bool parse_file_result(const std::vector<uint8_t>& payload, FileResult& result,
                       std::string* error);
std::vector<uint8_t> make_file_cancel(const FileCancel& cancel);
bool parse_file_cancel(const std::vector<uint8_t>& payload, FileCancel& cancel,
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
    void reset();
    size_t buffered() const { return buffer_.size() - offset_; }

private:
    std::vector<uint8_t> buffer_;
    size_t offset_ = 0;
    size_t current_payload_ = 0;
};

// Same reassembly rules as FrameParser, for the audio stream. Kept separate so
// a malformed audio packet can be dropped without disturbing video framing.
class AudioParser {
public:
    enum class Status {
        Incomplete,
        Packet,
        Error,
    };

    bool push(const uint8_t* data, size_t size, std::string* error);
    Status next(AudioHeader& header, const uint8_t** payload, std::string* error);
    void consume();
    void reset();
    size_t buffered() const { return buffer_.size() - offset_; }

private:
    std::vector<uint8_t> buffer_;
    size_t offset_ = 0;
    size_t current_payload_ = 0;
};

}  // namespace chast
