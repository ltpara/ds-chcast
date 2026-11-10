#pragma once

// The chast wire protocol, version 63.
//
// One QUIC connection carries:
//
//   * a client-initiated bidirectional control stream (host auth challenge,
//     Hello / HelloAck, key-frame requests, input events, file offers,
//     closing statistics),
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
//     downloads to the viewer,
//   * one unidirectional `CPFW` stream per direction of a forwarded TCP
//     connection, and
//   * one `CTTY` uplink (input) plus one `CTTY` downlink (output) per remote
//     terminal.
//
// The streams are told apart by their packet magic, not by the order the
// streams were opened: QUIC does not promise that the first bytes of a later
// stream arrive after an earlier one. Video is `CFRM`, system audio is
// `CAUD`, the microphone is `CMIC`, clipboard content is `CCLB`, file data
// is `CFIL`, forwarded TCP bytes are `CPFW` and terminal bytes are `CTTY`.
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

#include "chast/access_ticket.h"
#include "chast/encoder.h"

namespace chast {

inline constexpr uint16_t kProtocolVersion = 68;
// Complies with the ALPN private-use range. The ALPN names the protocol
// family, not its version: a mismatch is reported by the Hello exchange with
// the peer's number instead of as an opaque handshake failure.
inline constexpr char kAlpn[] = "chast/1";

inline constexpr size_t kFrameHeaderSize = 52;
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
    // into an end-to-end frame age. The host also sends probes for application
    // RTT (v51); replies echo the origin stamp with the receiver's two stamps.
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
    // The host aggregates desktop viewers' preferences; the answer is
    // StreamOptionsResult. Changing preferences does not require input control.
    SetStreamOptions = 30,
    // Host -> viewer: the outcome of SetStreamOptions. Payload: code (u8),
    // effective codec (u8, 1 = H.264, 2 = HEVC, 3 = AV1), effective frame rate (u16),
    // effective quality (u8), reason length (u16), UTF-8 reason. The
    // effective fields describe the stream that is actually running, even
    // when the request was refused, so the viewer can restore its menu.
    StreamOptionsResult = 31,
    // Either role: one UTF-8 chat line. Payload: the text bytes (at most
    // kMaxChatBytes, no NUL). The control stream is ordered, so messages keep
    // their send order. No permission bit gates chat: an admitted session is
    // the authorization, and either side can ignore or close the chat UI.
    // Protocol v24.
    ChatMessage = 32,
    // Viewer -> host: register a port-forward target. Payload: forward id
    // (u32), target port (u16), host length (u16), UTF-8 host. The host
    // checks the permission and its settings and answers PortForwardResult.
    // Data for an accepted target travels on CPFW streams, never here.
    // Protocol v26.
    PortForwardRequest = 33,
    // Host -> viewer: the outcome of PortForwardRequest. Payload: forward id
    // (u32), code (u8), reason length (u16), UTF-8 reason. Ok means CPFW
    // streams naming that id are accepted. See PortForwardCode.
    PortForwardResult = 34,
    // Either role: stop one registered forward. Payload: forward id (u32),
    // code (u8), reason length (u16), UTF-8 reason. The sender closes every
    // connection it owns for the id; the receiver does the same on its side.
    PortForwardStop = 35,
    // Viewer -> host: open a remote terminal. Payload: terminal id (u32),
    // columns (u16), rows (u16). The host checks the permission and its
    // settings, spawns a console under ConPTY and answers TerminalResult.
    // Terminal bytes never travel here.
    // Protocol v27.
    TerminalRequest = 36,
    // Host -> viewer: the outcome of TerminalRequest. Payload: terminal id
    // (u32), code (u8), reason length (u16), UTF-8 reason. Ok means CTTY
    // streams naming that id are accepted. See TerminalCode.
    TerminalResult = 37,
    // Either role: stop one terminal. Payload: terminal id (u32), code (u8),
    // reason length (u16), UTF-8 reason. The host closes the console and the
    // child process; the viewer drops its streams.
    TerminalStop = 38,
    // Viewer -> host: change the console size of a running terminal. Payload:
    // terminal id (u32), columns (u16), rows (u16). The host applies it with
    // ResizePseudoConsole; too late or unknown ids are ignored.
    TerminalResize = 39,
    // Viewer -> host: ask which cameras the host can share. No payload. The
    // host checks kPermissionCamera and its settings and answers CameraList.
    // Protocol v29.
    ListCameras = 40,
    // Host -> viewer: the answer to ListCameras. Payload: code (u8), reason
    // length (u16), UTF-8 reason, count (u16), then per camera id (u16),
    // flags (u8, bit 0 = default device) and UTF-8 name (u16 length). Ids are
    // valid until the next list; they name an entry of the current
    // enumeration, not a persistent device handle. See CameraCode.
    CameraList = 41,
    // Viewer -> host: open one camera. Payload: camera id (u16). The host
    // checks the permission and its settings and answers CameraResult. On Ok
    // a server-initiated CCAM stream naming that id follows.
    StartCamera = 42,
    // Host -> viewer: the outcome of StartCamera. Payload: camera id (u16),
    // code (u8), reason length (u16), UTF-8 reason. Ok means the CCAM stream
    // is opening; every other code means no stream will follow.
    CameraResult = 43,
    // Viewer -> host: stop the running camera. Payload: camera id (u16). The
    // host finishes the CCAM stream; there is no answer, because the stream's
    // FIN is the verdict. An id that is not streaming is ignored.
    StopCamera = 44,
    // Viewer -> host: list the folder the host shares for browsing. Payload:
    // request id (u32), directory length (u16), UTF-8 directory relative to
    // the shared root with '/' separators (empty means the root). Requires
    // the file-transfer permission and the host's gate; the host answers
    // FileListResult. Since v56, a u16 length and UTF-8 continuation cursor
    // follow the directory. An empty cursor starts a fresh listing. Since
    // v65, a flags byte follows the cursor: bit 0 lists hidden entries too,
    // otherwise entries with FILE_ATTRIBUTE_HIDDEN are omitted.
    FileListRequest = 45,
    // Host -> viewer: the answer to FileListRequest. Payload: request id
    // (u32), code (u8, see FileCode), flags (u8, bit 0 = cut at the entry
    // limit), reason length (u16), UTF-8 reason, count (u16), then per entry
    // UTF-8 name (u16 length), size (u64), modification time (u64, Unix
    // milliseconds, 0 when unknown) and flags (u8, bit 0 = directory).
    // Entries are directories first, then by name; a non-Ok answer carries
    // no entries. Since v56, a u16 length and UTF-8 next cursor follow all
    // entries; the cursor is nonempty exactly when another page exists.
    FileListResult = 46,
    // Viewer -> host: ask for one file from the shared folder. Payload:
    // request id (u32), path length (u16), UTF-8 path relative to the shared
    // root with '/' separators. The host resolves it inside the root,
    // refuses reparse points and directories, and answers FileDownloadResult;
    // on Ok a FileOffer naming the transfer id follows.
    FileDownloadRequest = 47,
    // Host -> viewer: the outcome of FileDownloadRequest. Payload: request id
    // (u32), transfer id (u32), code (u8, see FileCode), reason length (u16),
    // UTF-8 reason. Ok means the FileOffer for that transfer follows, and the
    // viewer answers it like any host download.
    FileDownloadResult = 48,
    // Viewer -> host: create, rename or remove one entry below the shared
    // root. Payload: request id (u32), operation (u8, see FileOperation),
    // path length (u16), UTF-8 path relative to the root with '/' separators,
    // new name length (u16), UTF-8 new name. The new name is one component
    // and only a rename carries one. The host resolves the path inside the
    // root, refuses reparse points and answers FileOpResult. Protocol v31.
    FileOpRequest = 49,
    // Host -> viewer: the outcome of FileOpRequest. Payload: request id (u32),
    // code (u8, see FileCode), reason length (u16), UTF-8 reason. A refusal
    // leaves the file system untouched.
    FileOpResult = 50,
    // Viewer -> host: one block of text composed on the viewer, injected on
    // the host as Unicode. Payload: UTF-8 text (no length prefix, the control
    // payload is the text). The viewer's own input method composes the text,
    // so the host never needs the matching keyboard layout; the injector maps
    // each code point to KEYEVENTF_UNICODE, with '\n' and '\t' as Return and
    // Tab. Requires the input permission like InputEvent. Protocol v36.
    TextInput = 51,
    // Viewer -> host: start or stop a voice call, which streams the host's
    // microphone to the viewer's speakers. Payload: flags (u8, bit 0 =
    // connect) and request id (u64). Starting requires kPermissionMicrophone;
    // the host answers with VoiceCallResponse. A request with bit 0 clear
    // stops a running call; the host may send the same message to close one
    // from its side. Protocol v37.
    VoiceCallRequest = 52,
    // Host -> viewer: the answer to a start request. Payload: request id
    // (u64), code (u8, see VoiceCallCode), reason length (u16), UTF-8 reason,
    // and, when Ok, the microphone format: codec (u8), sample rate (u32),
    // channels (u16), frame samples (u16), extradata length (u16) and the
    // extradata. Ok means VoiceAudio messages follow; every other code leaves
    // the call closed.
    VoiceCallResponse = 53,
    // Host -> viewer: one encoded microphone access unit. Payload:
    // AudioHeader (48 bytes) followed by the Opus packet. Only sent while a
    // call is open; the viewer drops it otherwise. The control stream is
    // ordered, so packets arrive in capture order. Protocol v37.
    VoiceAudio = 54,
    // Viewer -> host: one host-control action. Payload: action (u8, see
    // HostAction) and value (u8, the requested block-input state for
    // BlockInput, zero otherwise). Restart needs kPermissionRestart, lock
    // screen needs kPermissionInput, and block input needs both
    // kPermissionInput and kPermissionBlockInput. The host answers every
    // request with HostActionResult. Protocol v38.
    HostActionRequest = 55,
    // Host -> viewer: the outcome of a HostActionRequest. Payload: action
    // (u8), block-input state after the action (u8, zero for other actions),
    // code (u8, see HostActionCode), reason length (u16), UTF-8 reason. The
    // host also sends this unsolicited with Denied when it releases block
    // input itself (permission revocation or session teardown).
    HostActionResult = 56,
    // Viewer -> host: the operating-system password to type on the logon or
    // lock screen, so a viewer can sign the host back in or unlock it. Payload:
    // the UTF-8 password with no length prefix. The host checks the screen
    // state right before typing, so the password never lands in an unlocked
    // desktop. Requires kPermissionInput and kPermissionOsLogin. Protocol v39.
    OsLoginRequest = 57,
    // Host -> viewer: the outcome of an OsLoginRequest. Payload: code (u8, see
    // OsLoginCode), reason length (u16), UTF-8 reason. Ok means the password
    // was handed to the input injector while the logon desktop was current;
    // whether the credentials were accepted is only visible in the desktop
    // state that follows.
    OsLoginResult = 58,
    // Viewer -> host: ask which Windows sessions this machine can hand the
    // stream to. No payload. Requires kPermissionSessionSelect; the host
    // answers SessionListResult. The list is asked for rather than pushed in
    // HelloAck so a pre-auth peer learns nothing about other sessions.
    // Protocol v40.
    SessionListRequest = 59,
    // Host -> viewer: the answer to SessionListRequest. Payload: code (u8, see
    // SessionListCode), reason length (u16), UTF-8 reason, count (u16), then
    // per session id (u32), flags (u8, bit 0 = active, bit 1 = the session the
    // stream currently runs in) and the UTF-8 user name (u16 length). Only
    // sessions that are active and have a signed-in user are listed: DXGI
    // cannot capture a disconnected desktop. A non-Ok answer carries no
    // entries.
    SessionListResult = 60,
    // Viewer -> host: move the stream to another Windows session. Payload:
    // session id (u32). Requires kPermissionSessionSelect and a fresh list;
    // the host answers SessionSelectResult and then ends the session so the
    // service can relaunch its worker in the chosen session. Protocol v40.
    SessionSelectRequest = 61,
    // Host -> viewer: the outcome of a SessionSelectRequest. Payload: session
    // id (u32), code (u8, see SessionSelectCode), reason length (u16), UTF-8
    // reason. Ok means the host accepted the handoff and is about to end this
    // session; every other code leaves it running.
    SessionSelectResult = 62,
    // Viewer -> host: which automatic display-follow modes this viewer wants.
    // Payload: flags (u8), bit 0 = follow the host pointer, bit 1 = follow the
    // window with foreground focus. The host moves its own capture with the
    // existing switch path, so an accepted follow needs input permission like
    // SwitchDisplay. The answer is DisplayFollowState. Protocol v44.
    SetDisplayFollow = 63,
    // Host -> viewer: the follow modes actually in effect. Payload: flags
    // (u8), code (u8, see DisplayFollowCode), reason length (u16), UTF-8
    // reason. Sent as the answer to SetDisplayFollow and unsolicited when the
    // host turns a mode off itself (input permission revoked, merged view,
    // session ending). See kDisplayFollowCursor/kDisplayFollowWindow.
    DisplayFollowState = 64,
    // Viewer -> host: read or change the host's persistent policy switches.
    // Payload: keys (u32), values (u32). Each set bit in `keys` names one
    // switch (see RemoteConfigKey) and carries the wanted state in the same
    // bit of `values`; a value bit without its key bit is malformed. keys=0
    // is a query that changes nothing. Requires kPermissionRemoteConfig; the
    // host only grants it when its allow_remote_config setting is on. The
    // answer is RemoteConfigState. Protocol v46.
    RemoteConfigRequest = 65,
    // Host -> viewer: the answer to a RemoteConfigRequest, and the state of
    // the exposed switches after the call. Payload: flags (u32, one bit per
    // RemoteConfigKey, the state now stored on the host), code (u8, see
    // RemoteConfigCode), reason length (u16), UTF-8 reason. A non-Ok answer
    // leaves every switch unchanged.
    RemoteConfigState = 66,
    // Viewer -> host: query or change this session's display topology.
    // Payload: action (u8, see DisplayControlAction), value (u8) and target
    // (u32, a host-issued session display id). Query, SetCount and Restore
    // change no target; SelectPrivacy carries the wanted
    // PrivacyImplementation in `value`; ToggleOutput carries the new active
    // state in `value` and the session display id in `target`. SetCount and
    // ToggleOutput require kPermissionPrivacyMode and kPermissionInput, the
    // other actions require kPermissionPrivacyMode, and Restore is also
    // accepted after a revocation so a viewer can undo its own change. The
    // host answers DisplayControlState. Protocol v47.
    DisplayControl = 67,
    // Host -> viewer: the display state after a DisplayControl request and
    // the current session display list. Payload: supported (u8), managed
    // (u8), count (u8), privacy (u8, see PrivacyImplementation), code (u8,
    // see DisplayControlCode), reason length (u16), UTF-8 reason, entry
    // count (u8), then per entry id (u32), active (u8). Count is the number
    // of attached desktop outputs, not allocated PnP monitors, and ids stay
    // stable for the session so a list index is never a wire identity.
    DisplayControlState = 68,
    // Viewer -> host: cumulative desktop access-unit receipt, after the
    // application takes the frame. Payload: sequence (u64). Protocol v52.
    VideoReceipt = 69,
    // Ordered desktop subscription mutations with RustDesk add/sub/set
    // semantics. Unlike SwitchDisplay, these must never be coalesced.
    CaptureDisplays = 70,
    CaptureDisplaysResult = 71,
    // One independently decoded desktop stream changed its format/geometry.
    DisplayStreamInfo = 72,
    // A keyframe request names one stream, leaving other decoders alone.
    DisplayKeyframeRequest = 73,
    // Source-relative cursor state for one subscribed desktop stream. v55.
    DisplayCursorUpdate = 74,
    // v59, either direction. A bounded range of one advertised clipboard file.
    ClipboardFileRead = 75,
    ClipboardFileData = 76,
    // v60: request local UAC approval. No OS credential travels on the wire.
    ElevationRequest = 77,
    ElevationState = 78,
    // Host -> viewer, the answer to AuthBegin (v62). Payload: 32 random
    // bytes. The viewer signs the access-ticket possession proof over this
    // challenge, the Hello nonce and the ticket, so a captured Hello cannot
    // be replayed on another connection. Sent before the Hello on every host,
    // managed or not, so both sides always follow the same order.
    AuthChallenge = 79,
    // Viewer -> host, the first message on the control stream (v62). Empty.
    // A client-initiated QUIC stream does not exist for the host until its
    // first byte arrives, so the viewer opens the exchange; the host answers
    // with AuthChallenge. Neither message is a credential.
    AuthBegin = 80,
    // Viewer -> host (v62): extend a ticket-admitted session with a freshly
    // issued ticket for the same device and client key. Payload: u16 ticket
    // length (1..1088), the signed ticket (envelope plus signature), u16 proof
    // length (must be 64), the possession proof. The proof covers this
    // session's Hello nonce and the host's challenge, so a renewal cannot be
    // transplanted to another connection or another ticket.
    TicketRenew = 81,
    // Host -> viewer (v62): the renewal answer. Payload: u8 code (0 accepted,
    // 1 refused), i64 expiry seconds (0 when refused), u32 permission mask in
    // force after the renewal, u16 reason length, UTF-8 reason. A refusal is
    // followed by the host closing the session.
    TicketRenewResult = 82,
    // Viewer -> host (v68): the viewer's pointer position for the "show my
    // cursor" hint. Payload: x (i32), y (i32) in virtual-desktop pixels,
    // flags (u8, bit 0 visible), 3 zero bytes. Draw-only: the host never
    // feeds these coordinates to the injector, so no kPermissionInput is
    // needed; admission already authorized the session. Sent at a throttled
    // rate while the viewer pointer moves.
    ShowCursor = 83,
};

// ----- input -----
//
// The client sends one InputEvent message per pointer, touch, pen or keyboard
// event on the control stream, in the order they happened. Coordinates are in
// the host's virtual-desktop space (pixels), so the server never needs to know
// which output the viewer is looking at and a later multi-display UI needs no
// wire change. Keys travel as USB HID usage ids: layout independent, and mapped
// to scan codes on the injecting side. Every event also carries the modifier
// keys held at that moment, which lets the injecting side repair a lost key
// transition instead of trusting that every Down has a matching Up.
//
// The payload is 24 bytes: kind (u8), action (u8), buttons (u16), x (u32),
// y (u32), hid usage (u16), modifiers (u16), pressure (u16), contact (u8),
// flags (u8), tilt x (i16), tilt y (i16). Fields a kind does not use are zero;
// a parser rejects non-zero spare fields so a mislabelled event is caught
// instead of silently ignored.

enum class InputKind : uint8_t {
    Mouse = 1,
    Key = 2,
    // Direct touch and pen digitizer contacts (protocol v43). Unlike mouse
    // events they carry a pressure value and, for touch, a contact slot, so a
    // viewer with a touchscreen can drive the host's real touch stack (press
    // and hold, two-finger scroll, pinch) instead of a synthesized mouse.
    Touch = 3,
    Pen = 4,
    // Precision-touchpad pan and pinch (protocol v48). Gestures are not touch
    // contacts: the viewer turns a trackpad's two-finger motion into bounded
    // wheel deltas, so the host scrolls or zooms exactly like a mouse wheel
    // without synthesizing a digitizer contact.
    Gesture = 5,
    // Windows virtual-key input (protocol v63), the Legacy keyboard mode.
    // The viewer sends the virtual-key code its own hook reported and the
    // host injects that named key, so the host layout and IME decide what it
    // produces. Key above sends a HID usage instead, which preserves the
    // physical position. Virtual keys only exist on Windows; other platforms
    // treat this kind as unsupported and keep using Key.
    LegacyKey = 6,
};

// Simultaneous touch contacts the protocol and the Windows injector accept.
// Windows allows up to 256; digitizers and the viewer's pointer tracker are
// the practical limits, and a small fixed array keeps the injector simple.
inline constexpr uint8_t kMaxTouchContacts = 10;

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

// Legacy keyboard mode. The code is a Windows virtual-key code (the value a
// WH_KEYBOARD_LL hook reports), never zero. It is carried in the same wire
// slot as KeyEvent::hid_usage, and the two kinds never share a meaning.
struct LegacyKeyEvent {
    KeyAction action = KeyAction::Down;
    uint16_t virtual_key = 0;
};

enum class TouchAction : uint8_t {
    Down = 0,
    Move = 1,
    Up = 2,
    // The viewer lost the contact (focus left the window, capture was
    // interrupted). The host lifts it at its last position instead of ending
    // a drag at a coordinate the user never touched.
    Cancel = 3,
};

struct TouchEvent {
    TouchAction action = TouchAction::Down;
    // Stable for the contact's lifetime, 0..kMaxTouchContacts-1. A slot can
    // be reused by a later contact once it lifts.
    uint8_t contact = 0;
    // Virtual-desktop pixels, like MouseEvent.
    int32_t x = 0;
    int32_t y = 0;
    // 0..1024 (Windows' range), 0 when the device reports no pressure.
    uint16_t pressure = 0;
};

// Bit positions in PenEvent::flags; the values match Windows' own PEN_FLAG_*
// so the injector can pass them through.
enum PenFlag : uint8_t {
    kPenBarrel = 1u << 0,
    kPenInverted = 1u << 1,
    kPenEraser = 1u << 2,
};

struct PenEvent {
    TouchAction action = TouchAction::Down;
    int32_t x = 0;
    int32_t y = 0;
    // 0..1024 (Windows' range), 0 when the device reports no pressure.
    uint16_t pressure = 0;
    uint8_t flags = 0;  // PenFlag bits
    // Degrees from the surface normal, -90..90. Positive tilts right (+x)
    // and toward the user (+y), which is the Windows POINTER_PEN_INFO
    // convention; the viewer converts its polar tilt/orientation to it.
    int16_t tilt_x = 0;
    int16_t tilt_y = 0;
};

// Gestures carry bounded wheel deltas (120 units per notch). Each Zoom is a
// complete Ctrl+wheel operation: no modifier survives an interrupted gesture.
enum class GestureAction : uint8_t {
    Pan = 0,
    Zoom = 1,
};
inline constexpr int32_t kMaxGestureDelta = 1200;
struct GestureEvent {
    GestureAction action = GestureAction::Pan;
    int32_t x = 0;  // positive scrolls right; zero for Zoom
    int32_t y = 0;  // positive scrolls up / zooms in
};
bool valid_gesture_event(const GestureEvent& event);

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
    LegacyKeyEvent legacy_key;
    TouchEvent touch;
    PenEvent pen;
    GestureEvent gesture;
    // Modifier keys held and lock keys on, when the event happened; see
    // ModifierBit and LockStateBit.
    uint16_t modifiers = 0;
};

// ----- show cursor (protocol v68) -----
//
// The viewer's pointer position for the host-side "show my cursor" hint.
// Coordinates use the same virtual-desktop pixel space as InputEvent::mouse,
// so the host overlay needs no per-display mapping. The hint is draw-only and
// carries no button state: it can never actuate the host.
struct ShowCursorHint {
    int32_t x = 0;
    int32_t y = 0;
    bool visible = false;
};
std::vector<uint8_t> make_show_cursor(const ShowCursorHint& hint);
bool parse_show_cursor(const std::vector<uint8_t>& payload, ShowCursorHint& hint,
                       std::string* error);

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

inline constexpr uint32_t kMaxClipboardFileRead = 64 * 1024;
struct ClipboardFileRead {
    uint32_t request_id = 0;
    uint64_t generation = 0;
    uint32_t index = 0;
    uint64_t offset = 0;
    uint32_t length = 0;
};
struct ClipboardFileData {
    uint32_t request_id = 0;
    uint64_t generation = 0;
    bool ok = false;
    std::vector<uint8_t> bytes;
};
std::vector<uint8_t> make_clipboard_file_read(const ClipboardFileRead& request);
bool parse_clipboard_file_read(const std::vector<uint8_t>& payload, ClipboardFileRead& request, std::string* error);
std::vector<uint8_t> make_clipboard_file_data(const ClipboardFileData& response);
bool parse_clipboard_file_data(const std::vector<uint8_t>& payload, ClipboardFileData& response, std::string* error);

// ----- file transfer -----
//
// A transfer starts with FileOffer on the control stream and, once the
// receiver answers Ok, a unidirectional CFIL stream from the sender carrying
// the bytes. Either side may start one: the viewer uploads to the host and the
// host downloads to the viewer. The control stream never carries file data.
inline constexpr char kFileStreamMagic[4] = {'C', 'F', 'I', 'L'};
// v1 carries raw file bytes after the header. v2 (protocol v66) keeps the same
// header and turns the body into blocks whose payload may be zstd data; the
// sender picks v2 only for files that are worth compressing.
inline constexpr uint8_t kFileStreamVersion = 1;
inline constexpr uint8_t kFileStreamVersionCompressed = 2;
// magic (4) + version (1) + reserved (3) + transfer id (4) + offset (8).
inline constexpr size_t kFileStreamHeaderSize = 20;
// v2 block: stored_size (4) + raw_size (4) + flags (1).
inline constexpr size_t kFileBlockHeaderSize = 9;
inline constexpr uint8_t kFileBlockFlagCompressed = 0x01;
inline constexpr uint8_t kFileBlockFlagsKnown = kFileBlockFlagCompressed;
inline constexpr uint32_t kFileBlockMaxRawBytes = 64u * 1024u;
// ZSTD_compressBound(65536) is a little above the raw size; the sender falls
// back to a raw block when compression does not shrink the chunk.
inline constexpr uint32_t kFileBlockMaxStoredBytes = kFileBlockMaxRawBytes + 4096u;
// Files smaller than this stay on the raw stream: the per-block headers cost
// more than zstd can win back on such short inputs.
inline constexpr uint64_t kFileCompressionMinBytes = 4096u;
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
// The upload belongs to the sender's clipboard (protocol v34): the receiver
// stages it for CF_HDROP instead of filing it into the receive directory, and
// the clipboard permission, not file transfer, gates it.
inline constexpr uint8_t kFileOfferFlagClipboard = 0x02;
inline constexpr uint8_t kFileOfferFlagsKnown =
    kFileOfferFlagResume | kFileOfferFlagClipboard;

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
    bool clipboard = false;  // a clipboard copy staged for CF_HDROP
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
// `offset` (v1), or v2 blocks that each carry their own raw/stored sizes.
struct FileStreamHeader {
    uint32_t transfer_id = 0;
    uint64_t offset = 0;
    uint8_t version = kFileStreamVersion;
};
void encode_file_stream_header(const FileStreamHeader& header,
                               uint8_t out[kFileStreamHeaderSize]);
bool decode_file_stream_header(const uint8_t* data, size_t size, FileStreamHeader& header,
                               std::string* error);

struct FileBlockHeader {
    uint32_t stored_size = 0;
    uint32_t raw_size = 0;
    bool compressed = false;
};
void encode_file_block_header(const FileBlockHeader& header,
                              uint8_t out[kFileBlockHeaderSize]);
bool decode_file_block_header(const uint8_t* data, size_t size, FileBlockHeader& header,
                              std::string* error);

// True when the sender should offer this file as a v2 stream: it is large
// enough to amortise the block headers, is not a clipboard staging copy, and
// its extension does not already denote a compressed container.
bool file_compression_worthwhile(const std::string& name, uint64_t size, bool clipboard);

// ----- shared-folder browsing -----
//
// The viewer may list the folder the host shares (its receive directory) and
// pull single files out of it. Paths are relative to that root and use '/'
// separators; absolute paths, drive letters, backslashes and empty, "." or
// ".." components are rejected on both sides, and the host additionally
// refuses reparse points, so a request can never leave the root. Browsing and
// downloading are gated by the same file-transfer permission and setting as
// uploads.
inline constexpr size_t kMaxFileListEntries = 1024;
inline constexpr size_t kMaxFileBrowserPathBytes = kMaxFileRelativePathBytes;
inline constexpr size_t kMaxFileBrowserReasonBytes = kMaxFileReasonBytes;
inline constexpr uint8_t kFileListFlagTruncated = 0x01;
inline constexpr uint8_t kFileListFlagShowHidden = 0x01;
inline constexpr uint8_t kFileEntryFlagDirectory = 0x01;

// True when `path` is empty or a '/'-separated relative path of at most
// kMaxFileBrowserPathBytes bytes and kMaxFileRelativeComponents components,
// with no empty, "." or ".." component, no backslash, no drive colon, no NUL
// and well-formed UTF-8. Each component may be at most kMaxFileNameBytes.
bool valid_file_browser_path(const std::string& path);

struct FileListEntry {
    std::string name;  // one component, UTF-8
    uint64_t size = 0;
    // Unix milliseconds; 0 when the file system does not report it.
    uint64_t modified_ms = 0;
    bool directory = false;
};

// Stateless continuation in directory-first, UTF-8 byte name order. A cursor
// is empty or "d:<name>" / "f:<name>"; it never supplies a filesystem path.
bool valid_file_list_cursor(const std::string& cursor);
inline std::string file_list_cursor(const FileListEntry& entry) {
    return std::string(entry.directory ? "d:" : "f:") + entry.name;
}
inline bool file_list_entry_less(const FileListEntry& left, const FileListEntry& right) {
    return left.directory != right.directory ? left.directory : left.name < right.name;
}

struct FileListRequest {
    uint32_t request_id = 0;
    std::string directory;
    std::string cursor;
    bool show_hidden = false;
};

struct FileListResult {
    uint32_t request_id = 0;
    FileCode code = FileCode::Ok;
    bool truncated = false;
    std::string reason;
    std::vector<FileListEntry> entries;
    std::string next_cursor;
};

struct FileDownloadResult {
    uint32_t request_id = 0;
    uint32_t transfer_id = 0;
    FileCode code = FileCode::Ok;
    std::string reason;
};

enum class FileOperation : uint8_t {
    MakeDirectory = 0,  // path names the new folder itself
    Rename = 1,         // path names the entry, new_name replaces its name
    Remove = 2,         // path names a file or an empty directory
    RemoveTree = 3,     // path names a directory whose contents are removed
};

struct FileOpResult {
    uint32_t request_id = 0;
    FileCode code = FileCode::Ok;
    std::string reason;
};

// True when `name` is one usable path component: non-empty, at most
// kMaxFileNameBytes, well-formed UTF-8, no NUL, '/', '\\' or ':', and not
// "." or "..".
bool valid_file_browser_name(const std::string& name);

// The makers reject a zero request id, an invalid path, a non-Ok code with
// entries (or a non-Ok download with a transfer id), over-long names or
// reasons and return an empty vector; the parsers reject the same cases plus
// trailing bytes.
std::vector<uint8_t> make_file_list_request(uint32_t request_id,
                                            const std::string& directory,
                                            const std::string& cursor = {},
                                            bool show_hidden = false);
bool parse_file_list_request(const std::vector<uint8_t>& payload, uint32_t& request_id,
                             std::string& directory, std::string* error,
                             std::string* cursor = nullptr,
                             bool* show_hidden = nullptr);
std::vector<uint8_t> make_file_list_result(const FileListResult& result);
bool parse_file_list_result(const std::vector<uint8_t>& payload, FileListResult& result,
                            std::string* error);
std::vector<uint8_t> make_file_download_request(uint32_t request_id,
                                                const std::string& path);
bool parse_file_download_request(const std::vector<uint8_t>& payload,
                                 uint32_t& request_id, std::string& path,
                                 std::string* error);
std::vector<uint8_t> make_file_download_result(const FileDownloadResult& result);
bool parse_file_download_result(const std::vector<uint8_t>& payload,
                                FileDownloadResult& result, std::string* error);

// The makers reject a zero request id, a non-empty path is required, a
// rename needs one valid new name while the other operations must leave it
// empty, and reasons are truncated like the other file messages. The
// parsers reject the same cases plus trailing bytes.
std::vector<uint8_t> make_file_op_request(uint32_t request_id, FileOperation op,
                                          const std::string& path,
                                          const std::string& new_name);
bool parse_file_op_request(const std::vector<uint8_t>& payload, uint32_t& request_id,
                           FileOperation& op, std::string& path, std::string& new_name,
                           std::string* error);
std::vector<uint8_t> make_file_op_result(const FileOpResult& result);
bool parse_file_op_result(const std::vector<uint8_t>& payload, FileOpResult& result,
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

struct DisplayCursorUpdate {
    uint32_t stream_id = 0;
    CursorUpdate cursor;
};

// Keep a not-yet-consumed shape when a later update only moves that shape.
void coalesce_cursor_update(CursorUpdate& pending, CursorUpdate update);

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
    // v54: 0 is the original desktop (and camera) stream. Additional desktop
    // subscriptions use connection-local IDs, never reused during a session.
    uint32_t stream_id = 0;
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
    VideoChroma chroma = VideoChroma::Yuv420;
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
// it that clipboard is granted within clipboard_direction; bit 2 tells it the host
// opened an audio stream and described the format in the audio block; bit 3
// tells it that file offers are accepted; bit 4 that the viewer may open a
// microphone stream; bit 5 that SetPrivacyMode is accepted; bit 7 that the
// viewer may ask for TCP port forwarding; bit 8 that remote terminals are
// accepted; bit 9 that the host consents to viewer-side recording; bit 10 that
// cameras may be listed and started. Bit 6 is
// not a permission: it says the picture is a merged layout of every attached
// output, with origin/source covering their union.
inline constexpr uint16_t kServerInfoInputEnabled = 0x0001;
inline constexpr uint16_t kServerInfoClipboardEnabled = 0x0002;
inline constexpr uint16_t kServerInfoAudioEnabled = 0x0004;
inline constexpr uint16_t kServerInfoFileTransferEnabled = 0x0008;
inline constexpr uint16_t kServerInfoMicrophoneEnabled = 0x0010;
inline constexpr uint16_t kServerInfoPrivacyModeEnabled = 0x0020;
inline constexpr uint16_t kServerInfoMergedLayout = 0x0040;
inline constexpr uint16_t kServerInfoPortForwardEnabled = 0x0080;
inline constexpr uint16_t kServerInfoTerminalEnabled = 0x0100;
inline constexpr uint16_t kServerInfoRecordEnabled = 0x0200;
inline constexpr uint16_t kServerInfoCameraEnabled = 0x0400;
// This session carries files only: no video or audio stream follows, and the
// picture and source sizes in the message are zero. The viewer never creates
// a decoder for such a session.
inline constexpr uint16_t kServerInfoFileTransferOnly = 0x0800;
// v35: system audio travels in QUIC DATAGRAM frames instead of the reliable
// audio stream. The viewer serves these packets before the stream parser; a
// lost datagram is a short gap, not a retransmission that stalls playback.
inline constexpr uint16_t kServerInfoAudioDatagram = 0x1000;
// v41: the session carries no desktop picture or audio. The mode byte appended
// to HelloAck/StreamInfo says what the session is for (Terminal, Camera or
// PortForward); FileTransfer keeps using the older FileTransferOnly flag. The
// viewer never creates a decoder for such a session.
inline constexpr uint16_t kServerInfoControlOnly = 0x2000;
// v42: the host keeps a persistent terminal service across viewer disconnects.
// The terminal permission must also be granted. Not a permission bit: it says
// the operator enabled terminal persistence (host.ini terminal_persist=).
inline constexpr uint16_t kServerInfoTerminalPersist = 0x4000;
inline constexpr uint16_t kServerInfoYuv444 = 0x8000;
// Flags that describe what the viewer may do. The permission mask appended to
// HelloAck is the authority for these bits, so the parser compares it with
// this mask rather than with every known flag. Restart and block input have no
// flag: their permission bits collide with the files-only and datagram-audio
// flags, so they travel in the mask alone.
inline constexpr uint16_t kServerInfoPermissionFlags = kServerInfoInputEnabled |
                                                       kServerInfoClipboardEnabled |
                                                       kServerInfoAudioEnabled |
                                                       kServerInfoFileTransferEnabled |
                                                       kServerInfoMicrophoneEnabled |
                                                       kServerInfoPrivacyModeEnabled |
                                                       kServerInfoPortForwardEnabled |
                                                       kServerInfoTerminalEnabled |
                                                       kServerInfoRecordEnabled |
                                                       kServerInfoCameraEnabled;
inline constexpr uint16_t kServerInfoKnownFlags = kServerInfoPermissionFlags |
                                                  kServerInfoMergedLayout |
                                                  kServerInfoFileTransferOnly |
                                                  kServerInfoAudioDatagram |
                                                  kServerInfoControlOnly |
                                                  kServerInfoTerminalPersist | kServerInfoYuv444;

// The permission mask. It is the v13 authority: HelloAck/StreamInfo append it
// after the source size (a u32 since v49), PermissionUpdate replaces it
// mid-session, and the
// ServerInfo booleans are its decoded form. Input covers pointer and
// keyboard injection; clipboard governs clipboard streams in both directions;
// audio is present only while an audio stream is open; microphone allows the
// viewer to open a `CMIC` uplink; file transfer gates FileOffer; privacy mode
// gates SetPrivacyMode; port forward gates PortForwardRequest and CPFW streams;
// terminal gates TerminalRequest and CTTY streams; record is the host's consent
// for the viewer to keep a local recording of the received stream (the host
// cannot enforce it, so it is an advisory permission like RustDesk's
// "enable-record-session"); camera gates ListCameras, StartCamera and CCAM
// streams; restart gates the RemoteRestart host action, and block input gates
// the BlockInput host action (which also needs input: blocking the local
// keyboard without remote control would lock the operator out). OS login gates
// OsLoginRequest and also needs input, because signing in means typing on the
// secure desktop. Session select gates SessionListRequest and
// SessionSelectRequest and needs the host's serving backend: the viewer moves
// the stream between Windows sessions, so the grant is refused when the service
// that owns the worker is not present.
// The permission mask travels as a u32 on the wire (v49). Every mask field in
// the API uses this alias, so widening the set again is a one-line change.
using PermissionMask = uint32_t;

enum PermissionBit : PermissionMask {
    kPermissionInput = 0x0001,
    kPermissionClipboard = 0x0002,
    kPermissionAudio = 0x0004,
    kPermissionFileTransfer = 0x0008,
    kPermissionMicrophone = 0x0010,
    kPermissionPrivacyMode = 0x0020,
    // 0x0040 stays unused. It was left out on the u16 wire so the mask lined
    // up field by field with the HelloAck flag word, where that bit is the
    // merged-layout marker. v49 widened the mask to u32, so this gap is now
    // only a historical reservation: new permissions start at 0x10000.
    kPermissionPortForward = 0x0080,
    kPermissionTerminal = 0x0100,
    kPermissionRecord = 0x0200,
    kPermissionCamera = 0x0400,
    kPermissionRestart = 0x0800,
    kPermissionBlockInput = 0x1000,
    // Like restart and block input, this bit travels in the mask alone: it has
    // no HelloAck flag of its own.
    kPermissionOsLogin = 0x2000,
    // v40: list and switch the Windows session the stream is served from. Only
    // the service backend can honour a switch, so its transport ceiling refuses
    // the bit when the machine is not running in service mode. Like the bits
    // above it travels in the mask alone.
    kPermissionSessionSelect = 0x4000,
    // v46: read and change the host's exposed policy switches
    // (RemoteConfigRequest). The host grants it only when allow_remote_config
    // is on; it travels in the mask alone.
    kPermissionRemoteConfig = 0x8000,
};
inline constexpr PermissionMask kPermissionAll = kPermissionInput | kPermissionClipboard |
                                                 kPermissionAudio | kPermissionFileTransfer |
                                                 kPermissionMicrophone | kPermissionPrivacyMode |
                                                 kPermissionPortForward | kPermissionTerminal |
                                                 kPermissionRecord | kPermissionCamera |
                                                 kPermissionRestart | kPermissionBlockInput |
                                                 kPermissionOsLogin | kPermissionSessionSelect |
                                                 kPermissionRemoteConfig;

// What the viewer wants to do on this connection. Desktop is the full remote
// desktop. The other modes ask for a control-only session that never starts a
// capturer or encoder: FileTransfer browses the shared directories and moves
// files, Terminal opens shells, Camera streams one camera, and PortForward
// relays TCP targets. The host may refuse the mode; its answer in HelloAck is
// authoritative.
enum class SessionMode : uint8_t {
    Desktop = 0,
    FileTransfer = 1,
    // v41: shells only (kPermissionTerminal).
    Terminal = 2,
    // v41: one camera stream only (kPermissionCamera).
    Camera = 3,
    // v41: viewer-registered TCP targets only (kPermissionPortForward).
    PortForward = 4,
};

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

// Which automatic display switches a viewer asked the host to perform.
inline constexpr uint8_t kDisplayFollowCursor = 1;
inline constexpr uint8_t kDisplayFollowWindow = 2;
inline constexpr uint8_t kDisplayFollowAll = kDisplayFollowCursor | kDisplayFollowWindow;

// Why a SetDisplayFollow was refused or cut short. Ok is the only code that
// leaves the requested flags in effect; the viewer shows its toggles from the
// flags the host reports either way.
enum class DisplayFollowCode : uint8_t {
    Ok = 0,
    Failed = 1,       // the host could not apply the request
    Unsupported = 2,  // a merged stream already shows every display
};
inline constexpr size_t kMaxDisplayFollowReason = 256;

// One DisplayFollowState answer: the modes now in effect, plus the outcome of
// the request that produced them.
struct DisplayFollowState {
    uint8_t flags = 0;
    DisplayFollowCode code = DisplayFollowCode::Ok;
    std::string reason;
};

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
// v57: clipboard policy is always expressed relative to the host. The
// permission bit and the viewer's pause switch remain independent gates.
enum class ClipboardDirection : uint8_t {
    Disabled = 0,
    HostToViewer = 1,
    ViewerToHost = 2,
    Both = 3,
};
inline constexpr bool clipboard_from_host(ClipboardDirection direction) {
    return (static_cast<uint8_t>(direction) & 1) != 0;
}
inline constexpr bool clipboard_to_host(ClipboardDirection direction) {
    return (static_cast<uint8_t>(direction) & 2) != 0;
}

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
    ClipboardDirection clipboard_direction = ClipboardDirection::Both;
    bool file_transfer_enabled = false;
    // The picture is the union of every attached output (kDisplayAll), not a
    // single captured output. `origin_x/y` and `source_width/height` describe
    // that union; `displays` lists the members.
    bool merged = false;
    // The session was opened for file transfer only: no video or audio stream
    // exists, `width`/`height` and `source_width`/`source_height` are zero and
    // the viewer must not expect frames. Only `file_transfer_enabled` can be
    // set alongside it.
    bool files_only = false;
    // v41: the session is control-only (kServerInfoControlOnly) and `mode`
    // says what it serves. FileTransfer keeps using files_only, so a viewer
    // that only understands v40 still lands in the file browser.
    bool control_only = false;
    SessionMode mode = SessionMode::Desktop;
    // The viewer may open a microphone stream. This is the permission bit's
    // decoded form; it is not a separate wire field.
    bool microphone_enabled = false;
    // The host accepts SetPrivacyMode. Decoded from the permission mask, like
    // the other booleans; the overlay's current state travels separately in
    // PrivacyModeState because a reconnect always starts with it off.
    bool privacy_mode_enabled = false;
    // The viewer may register TCP port-forward targets. Decoded from the
    // permission mask; each target is still checked when it is requested.
    bool port_forward_enabled = false;
    // The viewer may ask for remote terminals. Decoded from the permission
    // mask; every request is still checked against the host's settings.
    bool terminal_enabled = false;
    // v42: the host runs persistent terminal services, so a TerminalRequest
    // may ask for one and a later session may attach to it by service id.
    // Only set when terminal access is granted and the operator enabled
    // persistence.
    bool terminal_persist = false;
    // The host consents to the viewer keeping a local recording of the
    // received stream. Advisory: the stream is already on the viewer's side,
    // so the host cannot enforce it; the viewer hides its record button when
    // this is false.
    bool record_enabled = false;
    // The viewer may list and start cameras. Decoded from the permission
    // mask; the device is still checked when it is requested.
    bool camera_enabled = false;
    // v38: the viewer may ask for a remote restart or for local input to be
    // blocked. Decoded from the permission mask; each action is still checked
    // against the host's live settings when it arrives.
    bool restart_enabled = false;
    bool block_input_enabled = false;
    // v39: the viewer may ask the host to type the OS password at its logon or
    // lock screen. Decoded from the permission mask; the host still checks the
    // desktop state and its live setting for every request.
    bool os_login_enabled = false;
    // v40: the viewer may list the machine's Windows sessions and move the
    // stream to another one. Decoded from the permission mask; only a host
    // running in service mode sets it, and the host re-enumerates the sessions
    // when a request arrives.
    bool session_select_enabled = false;
    // v46: decoded from the existing remote-configuration permission bit.
    bool remote_config_enabled = false;
    // v35: audio packets follow on QUIC DATAGRAM frames, not the reliable
    // audio stream. Set only alongside audio.enabled.
    bool audio_datagram = false;
    std::string encoder;
    std::vector<DisplayDescription> displays;
    AudioInfo audio;
    VideoChroma chroma = VideoChroma::Yuv420;
};

struct CaptureDisplaysRequest {
    uint32_t request_id = 0;
    std::vector<uint16_t> add;
    // Keep only these already-subscribed displays; do not create new ones.
    std::vector<uint16_t> sub;
    std::vector<uint16_t> set;
};

struct DisplayStreamInfo {
    uint32_t stream_id = 0;
    uint16_t display_index = 0;
    ServerInfo info;
};

struct CaptureDisplaysResult {
    uint32_t request_id = 0;
    DisplaySwitchCode code = DisplaySwitchCode::Ok;
    std::string reason;
    // Complete resulting subscription set, including on refusal.
    std::vector<DisplayStreamInfo> streams;
};

std::vector<uint8_t> make_capture_displays(const CaptureDisplaysRequest& request);
bool parse_capture_displays(const std::vector<uint8_t>& payload,
                            CaptureDisplaysRequest& request, std::string* error);
std::vector<uint8_t> make_capture_displays_result(const CaptureDisplaysResult& result);
bool parse_capture_displays_result(const std::vector<uint8_t>& payload,
                                   CaptureDisplaysResult& result, std::string* error);
std::vector<uint8_t> make_display_stream_info(const DisplayStreamInfo& info);
bool parse_display_stream_info(const std::vector<uint8_t>& payload,
                               DisplayStreamInfo& info, std::string* error);
std::vector<uint8_t> make_display_keyframe_request(uint32_t stream_id);
bool parse_display_keyframe_request(const std::vector<uint8_t>& payload,
                                    uint32_t& stream_id, std::string* error);

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
// Because Hello rejects trailing bytes, the audit reference is part of the
// versioned layout: v25 appends it after totp, and v32 appends the session
// mode after the audit reference.
inline constexpr size_t kMaxTotpLength = 16;
inline constexpr size_t kMaxAuditRefLength = 64;
inline constexpr size_t kMaxSignatureLength = 128;
inline constexpr size_t kHelloNonceBytes = 16;

// auth_flags bits.
inline constexpr uint8_t kAuthFlagTrustDevice = 0x01;  // add this key to the trust list
inline constexpr uint8_t kAuthFlagSigned = 0x02;       // signature is present
inline constexpr uint8_t kAuthFlagsKnown = kAuthFlagTrustDevice | kAuthFlagSigned;

// ClientHello capabilities (v35). Bit 0 says the viewer can receive system
// audio as QUIC DATAGRAM frames and prefers them; the host falls back to the
// reliable audio stream when it is clear.
inline constexpr uint8_t kClientCapabilityAudioDatagram = 0x01;
inline constexpr uint8_t kClientCapabilitiesKnown = kClientCapabilityAudioDatagram;

enum class StreamCodecPreference : uint8_t;
struct ClientHello {
    uint16_t display = kDisplayAuto;
    std::string access_token;
    std::string password;
    // Bits from PermissionBit. The host intersects this with its defaults and
    // with what the operator approves.
    PermissionMask requested_permissions = kPermissionAll;
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
    // Canonical lowercase hex reference for one session. The host includes it
    // in the inbound audit records it forwards, which is how the server joins
    // the viewer's account report and the host's view of the same session.
    // Empty for CLI tools that do not report audit.
    std::string audit_ref;
    // v32: desktop by default; FileTransfer asks the host for a control-only
    // file session. v41 adds Terminal, Camera and PortForward. The host
    // answers with ServerInfo::mode (FileTransfer also sets files_only).
    SessionMode mode = SessionMode::Desktop;
    // v35: bits from the kClientCapability* set. Default empty; the transport
    // fills it from its configuration so a client can opt out of datagram
    // audio for A/B measurements.
    uint8_t capabilities = 0;
    // v50: formats this receiver can actually decode. Shared encoders use the
    // intersection of their subscribers. Non-video modes may advertise zero.
    VideoCodecMask decoding_codecs = kVideoCodecAll;
    StreamCodecPreference codec_preference = static_cast<StreamCodecPreference>(0);
    VideoCodecMask decoding_444 = 0;
    VideoChroma prefer_chroma = VideoChroma::Yuv420;
    // v62: a managed host's access ticket, envelope plus Ed25519 signature.
    // Empty on unmanaged hosts; a managed host refuses an empty ticket.
    std::vector<uint8_t> access_ticket;
    // Ed25519 proof by the ticket's client key over
    // access_ticket_proof_message(ticket host id, host fingerprint, the host's
    // AuthChallenge, `nonce`, `access_ticket`). Empty when no ticket is sent.
    std::vector<uint8_t> ticket_proof;
};

// The server accepts an empty reference or 8..64 lowercase hex digits. The
// length keeps the join key out of sequential-id territory.
inline bool valid_audit_ref(const std::string& audit_ref) {
    if (audit_ref.empty()) {
        return true;
    }
    if (audit_ref.size() < 8 || audit_ref.size() > kMaxAuditRefLength) {
        return false;
    }
    for (char c : audit_ref) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

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
// The host's per-connection challenge. Exactly 32 bytes; the viewer echoes
// neither the challenge nor its own nonce, because the proof covers both and
// the host already knows its challenge.
std::vector<uint8_t> make_auth_challenge(
    const std::array<uint8_t, kTicketProofHostNonceSize>& host_nonce);
bool parse_auth_challenge(const std::vector<uint8_t>& payload,
                          std::array<uint8_t, kTicketProofHostNonceSize>& host_nonce,
                          std::string* error);
// The server's refusal of a Hello. The viewer shows `reason` to the user.
std::vector<uint8_t> make_access_denied(AccessDenial code, const std::string& reason);
bool parse_access_denied(const std::vector<uint8_t>& payload, AccessDenial& code,
                         std::string& reason, std::string* error);

// The body of a v62 TicketRenew, without the control header.
struct TicketRenewRequest {
    std::vector<uint8_t> signed_ticket;
    std::vector<uint8_t> proof;
};

enum class TicketRenewCode : uint8_t {
    Accepted = 0,
    Refused = 1,
};

// The body of a v62 TicketRenewResult, without the control header. A refused
// answer carries expires_at=0 and permissions=0.
struct TicketRenewResult {
    TicketRenewCode code = TicketRenewCode::Refused;
    int64_t expires_at = 0;
    PermissionMask permissions = 0;
    std::string reason;
};

// Rejects an empty or oversized ticket, a proof that is not exactly 64 bytes,
// trailing bytes, an unknown code, a negative expiry or unknown permission
// bits. Both sides always carry fresh values, so strictness costs nothing.
std::vector<uint8_t> make_ticket_renew(const std::vector<uint8_t>& signed_ticket,
                                       const std::vector<uint8_t>& proof);
bool parse_ticket_renew(const std::vector<uint8_t>& payload, TicketRenewRequest& request,
                        std::string* error);
std::vector<uint8_t> make_ticket_renew_result(const TicketRenewResult& result);
bool parse_ticket_renew_result(const std::vector<uint8_t>& payload,
                               TicketRenewResult& result, std::string* error);

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
std::vector<uint8_t> make_video_receipt(uint64_t sequence);
bool parse_video_receipt(const std::vector<uint8_t>& payload, uint64_t& sequence,
                         std::string* error);
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
// Session chat (protocol v24). One non-empty UTF-8 line of at most
// kMaxChatBytes; NUL and invalid UTF-8 are rejected. The text is sent as-is,
// newlines included, so a peer may compose multi-line messages.
inline constexpr size_t kMaxChatBytes = 4096;
bool valid_chat_text(const std::string& text);
// Returns an empty vector when `text` fails valid_chat_text().
std::vector<uint8_t> make_chat_message(const std::string& text);
bool parse_chat_message(const std::vector<uint8_t>& payload, std::string& text,
                        std::string* error);

// Composed text injection (protocol v36). The payload is the raw UTF-8 text,
// between 1 and kMaxTextInputBytes; NUL and invalid UTF-8 are rejected. The
// injector types it on the host as Unicode, so a multi-line block and any
// script the host layout cannot produce still arrive intact.
inline constexpr size_t kMaxTextInputBytes = 4096;
bool valid_text_input(const std::string& text);
// Returns an empty vector when `text` fails valid_text_input().
std::vector<uint8_t> make_text_input(const std::string& text);
bool parse_text_input(const std::vector<uint8_t>& payload, std::string& text,
                      std::string* error);

// ----- port forwarding -----
//
// The viewer registers a target with PortForwardRequest; once the host answers
// Ok, every accepted local TCP connection becomes one client-initiated `CPFW`
// uplink stream. The host dials the target and, on success, opens a
// host-initiated `CPFW` downlink stream with the same pair of ids. Both
// streams carry raw TCP bytes after their 16-byte header; QUIC's per-stream
// flow control is the only backpressure needed. A stream FIN means the TCP
// side saw EOF in that direction; an aborted stream tears the connection down.
inline constexpr char kPortForwardStreamMagic[4] = {'C', 'P', 'F', 'W'};
inline constexpr uint8_t kPortForwardStreamVersion = 1;
inline constexpr size_t kPortForwardStreamHeaderSize = 16;
inline constexpr size_t kMaxPortForwardHostBytes = 255;
inline constexpr size_t kMaxPortForwardReasonBytes = 256;
// Targets a viewer may register in one session, and TCP connections the host
// carries at once across all of them.
inline constexpr size_t kMaxPortForwardTargets = 8;
inline constexpr uint32_t kMaxPortForwardConnections = 32;
inline constexpr size_t kMaxPortForwardChunkBytes = 64u * 1024u;
// The receive queue for one forwarded connection. Above the high-water mark
// the stream's QUIC receive is paused instead of dropping bytes, so a slow
// local reader slows the sender down rather than corrupting the connection.
inline constexpr size_t kPortForwardReceiveHighWaterBytes = 256u * 1024u;

enum class PortForwardCode : uint8_t {
    Ok = 0,
    Refused = 1,        // the host does not allow forwarding for this session
    InvalidTarget = 2,  // empty/oversized host, port 0, or an unusable id
    Busy = 3,           // the forward table or connection budget is full
    Cancelled = 4,      // either side stopped it (or the session is ending)
};

struct PortForwardRequest {
    uint32_t forward_id = 0;
    std::string host;
    uint16_t port = 0;
};

struct PortForwardResult {
    uint32_t forward_id = 0;
    PortForwardCode code = PortForwardCode::Ok;
    std::string reason;
};

struct PortForwardStop {
    uint32_t forward_id = 0;
    PortForwardCode code = PortForwardCode::Cancelled;
    std::string reason;
};

struct PortForwardStreamHeader {
    uint32_t forward_id = 0;
    uint32_t connection_id = 0;
};

// The makers truncate `reason` on a UTF-8 boundary; the parsers reject unknown
// codes, over-long or empty hosts, ports outside 1..65535, malformed UTF-8 and
// trailing bytes rather than repairing a broken message.
std::vector<uint8_t> make_port_forward_request(const PortForwardRequest& request);
bool parse_port_forward_request(const std::vector<uint8_t>& payload,
                                PortForwardRequest& request, std::string* error);
std::vector<uint8_t> make_port_forward_result(const PortForwardResult& result);
bool parse_port_forward_result(const std::vector<uint8_t>& payload,
                               PortForwardResult& result, std::string* error);
std::vector<uint8_t> make_port_forward_stop(const PortForwardStop& stop);
bool parse_port_forward_stop(const std::vector<uint8_t>& payload, PortForwardStop& stop,
                             std::string* error);
// Exactly kPortForwardStreamHeaderSize bytes. Zero ids are rejected because
// they would make an uninitialised structure look valid.
std::vector<uint8_t> make_port_forward_stream_header(uint32_t forward_id,
                                                      uint32_t connection_id);
bool decode_port_forward_stream_header(const uint8_t* data, size_t size,
                                       PortForwardStreamHeader& header, std::string* error);

// ----- remote terminal -----
//
// The viewer asks for a console with TerminalRequest; once the host answers
// Ok, the viewer opens a `CTTY` uplink stream for keyboard input and the host
// opens a `CTTY` downlink stream the first time the console produces output.
// Both carry raw console bytes after their 16-byte header, including the VT
// sequences ConPTY emits. A stream FIN ends that direction; aborting tears the
// terminal down. A plain terminal lives and dies with its session. v42 adds
// persistent services: the host keeps such a shell after the session ends,
// buffers the last 1 MiB of its output and lets a later session attach to it
// by service id, replaying the buffer into the new CTTY stream.
inline constexpr char kTerminalStreamMagic[4] = {'C', 'T', 'T', 'Y'};
inline constexpr uint8_t kTerminalStreamVersion = 1;
inline constexpr size_t kTerminalStreamHeaderSize = 16;
inline constexpr size_t kMaxTerminalReasonBytes = 256;
// v42: TerminalRequest flags. Persist asks the host to keep the new console
// after the session ends and to answer with a service id; Attach reconnects to
// a service from an earlier session. They are mutually exclusive, attach needs
// a service id in the request and persist needs an empty one.
inline constexpr uint8_t kTerminalRequestPersist = 0x01;
inline constexpr uint8_t kTerminalRequestAttach = 0x02;
// Persistent services one host keeps at once, how long a detached service
// survives without a viewer, and how much console output it replays.
inline constexpr size_t kMaxTerminalServices = 100;
inline constexpr uint32_t kTerminalServiceIdleSeconds = 3600;
inline constexpr size_t kTerminalScrollbackBytes = 1024u * 1024u;
// Consoles one session may open at once, and the accepted size range. Sizes
// outside it are refused instead of clamped: a viewer that asks for 0 columns
// has a bug, and silently "fixing" it would hide it.
inline constexpr size_t kMaxTerminals = 2;
inline constexpr uint16_t kMaxTerminalColumns = 500;
inline constexpr uint16_t kMaxTerminalRows = 200;
inline constexpr size_t kMaxTerminalChunkBytes = 64u * 1024u;
// Console output buffered for one terminal before the host stops reading the
// ConPTY pipe, and viewer input buffered by the host before it pauses the
// uplink stream.
inline constexpr size_t kTerminalReceiveHighWaterBytes = 256u * 1024u;
inline constexpr size_t kTerminalInputHighWaterBytes = 256u * 1024u;

enum class TerminalCode : uint8_t {
    Ok = 0,
    Refused = 1,  // the host does not allow terminals for this session
    Busy = 2,     // the per-session terminal budget is full
    Failed = 3,   // ConPTY or the child process could not be created
    Cancelled = 4,
    NotFound = 5,  // the attach names no live terminal service
};

struct TerminalRequest {
    // Session-scoped id for the CTTY streams. A persistent service keeps its
    // own `service_id` across sessions; on attach the new session picks a new
    // terminal id and names the service separately.
    uint32_t terminal_id = 0;
    uint16_t columns = 0;
    uint16_t rows = 0;
    uint8_t flags = 0;
    uint32_t service_id = 0;
};

struct TerminalResult {
    uint32_t terminal_id = 0;
    // Non-zero only for Ok on a persistent console: the id a later session
    // uses to attach.
    uint32_t service_id = 0;
    TerminalCode code = TerminalCode::Ok;
    std::string reason;
};

struct TerminalStop {
    uint32_t terminal_id = 0;
    TerminalCode code = TerminalCode::Cancelled;
    std::string reason;
};

struct TerminalSize {
    uint32_t terminal_id = 0;
    uint16_t columns = 0;
    uint16_t rows = 0;
};

struct TerminalStreamHeader {
    uint32_t terminal_id = 0;
};

bool valid_terminal_size(uint16_t columns, uint16_t rows);
// The makers reject a zero id, an out-of-range size or an inconsistent flag
// combination and return an empty vector; the parsers reject unknown codes and
// flags, malformed UTF-8 and trailing bytes rather than repairing a broken
// message.
std::vector<uint8_t> make_terminal_request(const TerminalRequest& request);
bool parse_terminal_request(const std::vector<uint8_t>& payload,
                            TerminalRequest& request, std::string* error);
std::vector<uint8_t> make_terminal_result(const TerminalResult& result);
bool parse_terminal_result(const std::vector<uint8_t>& payload,
                           TerminalResult& result, std::string* error);
std::vector<uint8_t> make_terminal_stop(const TerminalStop& stop);
bool parse_terminal_stop(const std::vector<uint8_t>& payload, TerminalStop& stop,
                         std::string* error);
std::vector<uint8_t> make_terminal_resize(uint32_t terminal_id, uint16_t columns,
                                          uint16_t rows);
bool parse_terminal_resize(const std::vector<uint8_t>& payload, TerminalSize& size,
                           std::string* error);
std::vector<uint8_t> make_terminal_stream_header(uint32_t terminal_id);
bool decode_terminal_stream_header(const uint8_t* data, size_t size,
                                   TerminalStreamHeader& header, std::string* error);

// ----- camera -----
//
// The viewer lists the host's cameras with ListCameras and asks for one with
// StartCamera; the host opens a server-initiated `CCAM` stream carrying the
// same encoded access units as the desktop picture (FrameHeader packets, its
// own encoder and timeline). The camera runs beside the desktop stream: it
// has its own resolution, sequence numbers and keyframe requests, and the
// stream's FIN is the camera switch. One camera per session.
inline constexpr char kCameraStreamMagic[4] = {'C', 'C', 'A', 'M'};
inline constexpr uint8_t kCameraStreamVersion = 1;
inline constexpr size_t kCameraStreamHeaderSize = 16;
inline constexpr size_t kMaxCameraReasonBytes = 256;
inline constexpr size_t kMaxCameraNameBytes = 128;
// Upper bound on one CameraList answer. A host with more cameras stops here.
inline constexpr uint16_t kMaxCameras = 8;

enum class CameraCode : uint8_t {
    Ok = 0,
    Refused = 1,   // the host does not allow cameras for this session
    NotFound = 2,  // the id is not in the latest list, or the device is gone
    Busy = 3,      // another camera is already streaming
    Failed = 4,    // the device or the encoder could not start
};

struct CameraDescription {
    uint16_t id = 0;
    std::string name;  // UTF-8 device name
    bool is_default = false;
};

struct CameraList {
    CameraCode code = CameraCode::Ok;
    std::string reason;
    std::vector<CameraDescription> cameras;
};

struct CameraResult {
    uint16_t camera_id = 0;
    CameraCode code = CameraCode::Ok;
    std::string reason;
};

struct CameraStreamHeader {
    uint16_t camera_id = 0;
};

// The makers reject a zero id, an unknown code, duplicate ids, an over-long
// or invalid UTF-8 name and return an empty vector; the parsers reject the
// same cases plus trailing bytes. A non-Ok list carries no cameras.
std::vector<uint8_t> make_camera_list_request();
bool parse_camera_list_request(const std::vector<uint8_t>& payload, std::string* error);
std::vector<uint8_t> make_camera_list(const CameraList& list);
bool parse_camera_list(const std::vector<uint8_t>& payload, CameraList& list,
                       std::string* error);
std::vector<uint8_t> make_camera_start(uint16_t camera_id);
bool parse_camera_start(const std::vector<uint8_t>& payload, uint16_t& camera_id,
                        std::string* error);
std::vector<uint8_t> make_camera_result(const CameraResult& result);
bool parse_camera_result(const std::vector<uint8_t>& payload, CameraResult& result,
                         std::string* error);
std::vector<uint8_t> make_camera_stop(uint16_t camera_id);
bool parse_camera_stop(const std::vector<uint8_t>& payload, uint16_t& camera_id,
                       std::string* error);
std::vector<uint8_t> make_camera_stream_header(uint16_t camera_id);
bool decode_camera_stream_header(const uint8_t* data, size_t size,
                                 CameraStreamHeader& header, std::string* error);

// ----- voice call (protocol v37) -----
//
// A voice call streams the host's microphone to the viewer. It leaves the
// two established audio directions alone: system sound keeps travelling on
// `CAUD` (or datagrams) and the viewer's microphone keeps travelling on
// `CMIC`. The call is negotiated with VoiceCallRequest/Response and the
// encoded microphone units ride on the ordered control stream as VoiceAudio,
// the same model RustDesk uses for its AudioFrame voice path. The host's
// microphone is gated by kPermissionMicrophone, which an operator grants at
// connection approval like the viewer's microphone.
inline constexpr size_t kMaxVoiceCallReasonBytes = 256;

enum class VoiceCallCode : uint8_t {
    Ok = 0,
    Refused = 1,  // microphone access is not granted for this session
    Busy = 2,     // a call is already running
    Failed = 3,   // the host microphone or its encoder could not start
};

struct VoiceCallRequest {
    uint64_t request_id = 0;
    bool connect = true;
};

struct VoiceCallResponse {
    uint64_t request_id = 0;
    VoiceCallCode code = VoiceCallCode::Ok;
    std::string reason;
    // The host microphone format when code is Ok; empty otherwise. Only Opus
    // is accepted, at 48 kHz with one or two channels and 20 ms frames.
    AudioInfo audio;
};

// The makers reject a zero request id, unknown codes and an Ok response
// without a usable microphone format; the parsers reject the same cases plus
// trailing bytes. A non-Ok response carries no format.
std::vector<uint8_t> make_voice_call_request(const VoiceCallRequest& request);
bool parse_voice_call_request(const std::vector<uint8_t>& payload,
                              VoiceCallRequest& request, std::string* error);
std::vector<uint8_t> make_voice_call_response(const VoiceCallResponse& response);
bool parse_voice_call_response(const std::vector<uint8_t>& payload,
                               VoiceCallResponse& response, std::string* error);
// AudioHeader + Opus payload. The maker fills in size and CRC and refuses an
// empty or oversized packet; the parser re-checks both and requires a
// protocol-valid Opus header.
std::vector<uint8_t> make_voice_audio(const AudioHeader& header,
                                      const uint8_t* payload, size_t size);
bool parse_voice_audio(const std::vector<uint8_t>& payload, AudioHeader& header,
                       const uint8_t** audio, size_t& audio_size, std::string* error);

// ----- host control (protocol v38) -----
//
// Actions that change the host machine itself rather than the remote picture:
// restart the operating system, lock the console, and block the local
// keyboard and pointer while the viewer keeps injecting input. They are
// modelled apart from InputEvent because they are edge-triggered commands,
// not key chords, and every one is destructive or security-sensitive enough
// to deserve its own permission bit and an explicit answer. Block input is a
// state, so its request carries the wanted state and the answer carries the
// state in effect after the call.
// request_id == 0 is a host announcement; requests require a nonzero id.
enum class ElevationCode : uint8_t {
    Unsupported = 0, Available = 1, Elevated = 2, Pending = 3,
    Restarting = 4, Cancelled = 5, Denied = 6, Busy = 7, Failed = 8,
};
struct ElevationState {
    uint32_t request_id = 0;
    ElevationCode code = ElevationCode::Unsupported;
    std::string reason;
};
std::vector<uint8_t> make_elevation_request(uint32_t request_id);
bool parse_elevation_request(const std::vector<uint8_t>& payload, uint32_t& request_id, std::string* error);
std::vector<uint8_t> make_elevation_state(const ElevationState& state);
bool parse_elevation_state(const std::vector<uint8_t>& payload, ElevationState& state, std::string* error);

inline constexpr size_t kMaxHostActionReason = 256;

enum class HostAction : uint8_t {
    Restart = 1,
    LockScreen = 2,
    BlockInput = 3,
};

enum class HostActionCode : uint8_t {
    Ok = 0,
    Denied = 1,       // the permission or the host's setting does not allow it
    Unsupported = 2,  // this host cannot perform the action
    Busy = 3,         // another viewer owns the shared desktop's control
    Failed = 4,       // the platform call failed; the reason says why
};

struct HostControlRequest {
    HostAction action = HostAction::Restart;
    bool block_input = false;
};

struct HostControlResult {
    HostAction action = HostAction::Restart;
    bool block_input = false;
    HostActionCode code = HostActionCode::Ok;
    std::string reason;
};

// The makers reject an unknown action and a value that does not belong to it;
// the parsers reject those cases plus trailing bytes. The result reason is
// truncated on a UTF-8 boundary by the maker and rejected when malformed or
// over-long by the parser.
std::vector<uint8_t> make_host_action_request(const HostControlRequest& request);
bool parse_host_action_request(const std::vector<uint8_t>& payload,
                               HostControlRequest& request, std::string* error);
std::vector<uint8_t> make_host_action_result(const HostControlResult& result);
bool parse_host_action_result(const std::vector<uint8_t>& payload,
                              HostControlResult& result, std::string* error);

// ----- OS login (protocol v39) -----
//
// Signing the host back in at the logon or lock screen is typing, not a host
// action: the password has to reach the secure desktop as keystrokes, and the
// host can only prove where those keystrokes land at the moment it sends them.
// The viewer therefore sends the password in its own message and pairs it with
// the desktop state the host already announces. OsLoginCode::Ok means the
// password was handed to the input injector while the logon desktop was
// current; a wrong password is not a protocol failure and simply leaves the
// host on the logon screen.
inline constexpr size_t kMaxOsLoginPasswordBytes = 256;
inline constexpr size_t kMaxOsLoginReason = 256;

enum class OsLoginCode : uint8_t {
    Ok = 0,
    Denied = 1,       // input or OS-login permission missing, or the setting is off
    Unsupported = 2,  // the host is not at the logon or lock screen, or it cannot type there
    Busy = 3,         // another viewer owns the shared desktop's control
    Failed = 4,       // the password could not be handed to the injector
};

// The maker rejects an empty password, one over kMaxOsLoginPasswordBytes, one
// that is not valid UTF-8, and one with a control character (the injector
// turns '\n' and '\t' into Return and Tab, which must not split a password).
// The parser rejects the same cases plus trailing bytes. The result reason is
// truncated on a UTF-8 boundary by the maker and rejected when malformed or
// over-long by the parser.
std::vector<uint8_t> make_os_login_request(const std::string& password);
bool parse_os_login_request(const std::vector<uint8_t>& payload,
                            std::string& password, std::string* error);
std::vector<uint8_t> make_os_login_result(OsLoginCode code,
                                          const std::string& reason = {});
bool parse_os_login_result(const std::vector<uint8_t>& payload, OsLoginCode& code,
                           std::string& reason, std::string* error);

// ----- remote sessions (protocol v40) -----
//
// A Windows machine can run several sessions at once (the console plus RDP
// sessions). The service backend always serves one of them; these messages let
// the viewer list the sessions that are active and signed in, and ask the
// service to move the stream to another one. The list is deliberately not part
// of HelloAck: it names other users, so it must not travel before the viewer is
// authenticated, and it has to be re-read when the viewer asks rather than at
// handshake time. Selecting a session ends the current connection; the viewer
// reconnects as usual and reaches the worker in the new session.
inline constexpr size_t kMaxRemoteSessions = 16;
inline constexpr size_t kMaxSessionUserBytes = 256;
inline constexpr size_t kMaxSessionReason = 256;

enum class SessionListCode : uint8_t {
    Ok = 0,
    Denied = 1,       // the permission or the host's setting does not allow it
    Unsupported = 2,  // this host does not run as the service with a switch backend
    Failed = 3,       // the platform enumeration failed; the reason says why
};

enum class SessionSelectCode : uint8_t {
    Ok = 0,           // accepted; this session is ending and the worker restarts
    NotFound = 1,     // the id is not an active, signed-in session any more
    Denied = 2,       // the permission or the host's setting does not allow it
    Unsupported = 3,  // this host does not run as the service with a switch backend
    Busy = 4,         // another viewer owns the shared desktop's control
    Failed = 5,       // the switch could not be requested; the reason says why
};

struct RemoteSession {
    uint32_t id = 0;
    // Sign-in name as the machine reports it; empty when Windows exposes none.
    std::string user;
    bool active = false;
    // The session the current stream is served from. Selecting it is a no-op
    // answered with Ok rather than a handoff.
    bool current = false;
};

struct SessionListResult {
    SessionListCode code = SessionListCode::Ok;
    std::string reason;
    std::vector<RemoteSession> sessions;
};

struct SessionSelectResult {
    uint32_t session_id = 0;
    SessionSelectCode code = SessionSelectCode::Ok;
    std::string reason;
};

// The makers reject zero and 0xFFFFFFFF session ids, unknown codes, an Ok list
// with more than kMaxRemoteSessions entries, duplicate ids, malformed or
// over-long user names, and a non-Ok list with entries. The parsers reject the
// same cases plus trailing bytes. Reasons are truncated on a UTF-8 boundary by
// the maker and rejected when malformed or over-long by the parser.
std::vector<uint8_t> make_session_list_request();
bool parse_session_list_request(const std::vector<uint8_t>& payload,
                                std::string* error);
std::vector<uint8_t> make_session_list_result(const SessionListResult& result);
bool parse_session_list_result(const std::vector<uint8_t>& payload,
                               SessionListResult& result, std::string* error);
std::vector<uint8_t> make_session_select_request(uint32_t session_id);
bool parse_session_select_request(const std::vector<uint8_t>& payload,
                                  uint32_t& session_id, std::string* error);
std::vector<uint8_t> make_session_select_result(const SessionSelectResult& result);
bool parse_session_select_result(const std::vector<uint8_t>& payload,
                                 SessionSelectResult& result, std::string* error);

// CursorUpdate payloads. The maker drops an attached sprite that does not
// match width*height*4 and clamps the size to kMaxCursorSize; callers that
// keep the sprite can send the same update again with an empty one.
std::vector<uint8_t> make_cursor_update(const CursorUpdate& update);
bool parse_cursor_update(const std::vector<uint8_t>& payload, CursorUpdate& update,
                         std::string* error);
std::vector<uint8_t> make_display_cursor_update(const DisplayCursorUpdate& update);
bool parse_display_cursor_update(const std::vector<uint8_t>& payload,
                                 DisplayCursorUpdate& update, std::string* error);
// Host -> viewer permission change. Unknown bits are rejected so a newer
// sender cannot silently grant something this build does not understand.
std::vector<uint8_t> make_permission_update(PermissionMask permissions);
bool parse_permission_update(const std::vector<uint8_t>& payload,
                             PermissionMask& permissions, std::string* error);

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
// Each connection contributes a codec vote, FPS ceiling and quality choice
// to the host-wide policy. Codec votes use the global capability intersection;
// FPS takes the lowest ceiling and quality uses the latest explicit choice.
// The host's configured bitrate remains a hard ceiling. Hello carries the
// initial codec vote; these options update it without changing subscriptions.

enum class StreamCodecPreference : uint8_t {
    Auto = 0,  // no vote: prefer usable HEVC, H.264, then AV1
    H264 = 1,  // vote for H.264 across all desktop connections
    Hevc = 2,  // vote for HEVC when every viewer and active display support it
    // Vote for hardware AV1. An unusable vote is retained but not counted;
    // there is no software fallback. Introduced in v45; voting since v53.
    Av1 = 3,
    Vp9 = 4,
};

enum class StreamQuality : uint8_t {
    High = 0,      // RustDesk Best ratio: 1.5
    Balanced = 1,  // RustDesk Balanced ratio: 0.67
    Smooth = 2,    // RustDesk Low ratio: 0.5
    // A custom numeric image quality. `quality_percent` carries 10..100, and
    // the ratio is percent / 50 (RustDesk's custom quality mapping).
    Custom = 3,
};
// RustDesk's custom image-quality range, in percent.
inline constexpr uint8_t kMinCustomQualityPercent = 10;
inline constexpr uint8_t kMaxCustomQualityPercent = 100;

// Ok accepts the vote; the returned codec is the globally negotiated result
// and may differ from this viewer's preference. Unusable votes are retained
// but excluded from counting. Other codes leave the previous vote in place.
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
    VideoChroma chroma = VideoChroma::Yuv420;
    // Custom quality in percent, 10..100, meaningful only with
    // StreamQuality::Custom; preset requests carry 0.
    uint8_t quality_percent = 0;
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
    VideoChroma chroma = VideoChroma::Yuv420;
    // The effective custom quality in percent when `quality` is Custom,
    // otherwise 0.
    uint8_t quality_percent = 0;
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
// Viewer -> host follow-mode request. `flags` is a combination of
// kDisplayFollowCursor and kDisplayFollowWindow; the host replaces its current
// modes with it (zero turns following off).
std::vector<uint8_t> make_set_display_follow(uint8_t flags);
bool parse_set_display_follow(const std::vector<uint8_t>& payload, uint8_t& flags,
                              std::string* error);
// Host -> viewer follow state. The maker truncates `reason` to
// kMaxDisplayFollowReason bytes on a UTF-8 boundary; the parser rejects an
// over-long reason or unknown flags instead of repairing them.
std::vector<uint8_t> make_display_follow_state(const DisplayFollowState& state);
bool parse_display_follow_state(const std::vector<uint8_t>& payload,
                                DisplayFollowState& state, std::string* error);

// ----- remote configuration (protocol v46) -----
//
// A viewer holding kPermissionRemoteConfig may read and change the policy
// switches the host exposes to it. The host grants the permission only when
// its allow_remote_config setting is on and the operator approved it. The
// exposed set is deliberately the switches a running host re-checks per
// request; listener settings that need a restart (bind address, port,
// carrier, viewer cap, codec) never travel here. Enabling a switch the
// transport refused at handshake still takes effect on the next connection,
// while disabling one applies immediately.
inline constexpr size_t kMaxRemoteConfigReason = 256;

enum RemoteConfigKey : uint32_t {
    // Lock the host's console when the last viewer of the session leaves.
    kRemoteConfigLockAfterSessionEnd = 1u << 0,
    kRemoteConfigClipboard = 1u << 1,
    kRemoteConfigFileTransfer = 1u << 2,
    kRemoteConfigAudio = 1u << 3,
    kRemoteConfigMicrophone = 1u << 4,
    kRemoteConfigPrivacyMode = 1u << 5,
    kRemoteConfigRestart = 1u << 6,
    kRemoteConfigBlockInput = 1u << 7,
    kRemoteConfigOsLogin = 1u << 8,
    kRemoteConfigSessionSelect = 1u << 9,
    kRemoteConfigTerminal = 1u << 10,
    kRemoteConfigPortForward = 1u << 11,
    kRemoteConfigCamera = 1u << 12,
    kRemoteConfigRecord = 1u << 13,
};
inline constexpr uint32_t kRemoteConfigAll =
    kRemoteConfigLockAfterSessionEnd | kRemoteConfigClipboard |
    kRemoteConfigFileTransfer | kRemoteConfigAudio | kRemoteConfigMicrophone |
    kRemoteConfigPrivacyMode | kRemoteConfigRestart | kRemoteConfigBlockInput |
    kRemoteConfigOsLogin | kRemoteConfigSessionSelect | kRemoteConfigTerminal |
    kRemoteConfigPortForward | kRemoteConfigCamera | kRemoteConfigRecord;

// Why a remote configuration request was refused. Ok is the only code that
// stores anything; every other code leaves the host settings untouched.
enum class RemoteConfigCode : uint8_t {
    Ok = 0,
    Denied = 1,       // the permission or the host's allow_remote_config is off
    Unsupported = 2,  // this host cannot change that switch
    Busy = 3,         // another viewer owns the shared desktop's control
    Failed = 4,       // the change could not be stored
};

struct RemoteConfigRequest {
    uint32_t keys = 0;
    uint32_t values = 0;
};

struct RemoteConfigState {
    uint32_t flags = 0;
    RemoteConfigCode code = RemoteConfigCode::Ok;
    std::string reason;
};

// The makers reject an unknown key or a value bit without its key; the
// parsers reject those cases plus trailing bytes. The state maker truncates
// `reason` on a UTF-8 boundary.
std::vector<uint8_t> make_remote_config_request(const RemoteConfigRequest& request);
bool parse_remote_config_request(const std::vector<uint8_t>& payload,
                                 RemoteConfigRequest& request, std::string* error);
std::vector<uint8_t> make_remote_config_state(const RemoteConfigState& state);
bool parse_remote_config_state(const std::vector<uint8_t>& payload,
                               RemoteConfigState& state, std::string* error);

// Session-scoped topology management. Restore is allowed after revocation.
enum class DisplayControlAction : uint8_t { Query, SetCount, Restore, SelectPrivacy, ToggleOutput };
enum class PrivacyImplementation : uint8_t { Overlay, VirtualDisplay };
enum class DisplayControlCode : uint8_t { Ok, Denied, Unsupported, Busy, Failed };
struct DisplayControlRequest {
    DisplayControlAction action = DisplayControlAction::Query;
    uint8_t value = 0;
    uint32_t target = 0; // host-issued session identity, never a display-list index
};
struct ControlledDisplay {
    uint32_t id = 0;
    bool active = false;
};
struct DisplayControlState {
    bool supported = false;
    bool managed = false;
    uint8_t count = 0;  // attached desktop outputs, not allocated PnP monitors
    PrivacyImplementation privacy = PrivacyImplementation::Overlay;
    DisplayControlCode code = DisplayControlCode::Ok;
    std::string reason;
    std::vector<ControlledDisplay> outputs;
};
std::vector<uint8_t> make_display_control(const DisplayControlRequest& request);
bool parse_display_control(const std::vector<uint8_t>& payload, DisplayControlRequest& request,
                           std::string* error = nullptr);
std::vector<uint8_t> make_display_control_state(const DisplayControlState& state);
bool parse_display_control_state(const std::vector<uint8_t>& payload, DisplayControlState& state,
                                 std::string* error = nullptr);

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
