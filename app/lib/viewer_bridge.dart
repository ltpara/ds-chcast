import 'package:flutter/services.dart';

enum ViewerState { idle, connecting, handshaking, streaming, ended, failed }

/// One output the server announced in HelloAck. [index] is its position in
/// the list, which is what a reconnect sends back to pick that output.
class ViewerDisplay {
  const ViewerDisplay({
    required this.index,
    required this.name,
    required this.left,
    required this.top,
    required this.right,
    required this.bottom,
    required this.rotation,
    required this.primary,
  });

  final int index;
  final String name;
  final int left;
  final int top;
  final int right;
  final int bottom;
  final int rotation;
  final bool primary;

  int get width => right - left;
  int get height => bottom - top;

  /// "\\.\DISPLAY1" as "DISPLAY1" for the picker.
  String get label {
    final int slash = name.lastIndexOf('\\');
    return slash >= 0 && slash + 1 < name.length ? name.substring(slash + 1) : name;
  }

  static ViewerDisplay? fromMap(Map<Object?, Object?> map, int index) {
    final int left = (map['left'] as num?)?.toInt() ?? 0;
    final int top = (map['top'] as num?)?.toInt() ?? 0;
    final int right = (map['right'] as num?)?.toInt() ?? 0;
    final int bottom = (map['bottom'] as num?)?.toInt() ?? 0;
    if (right <= left || bottom <= top) {
      return null;
    }
    return ViewerDisplay(
      index: index,
      name: map['name']?.toString() ?? '',
      left: left,
      top: top,
      right: right,
      bottom: bottom,
      rotation: (map['rotation'] as num?)?.toInt() ?? 0,
      primary: map['primary'] == true,
    );
  }
}

class ViewerStatus {
  const ViewerStatus({
    this.state = ViewerState.idle,
    this.error = '',
    this.host = '',
    this.port = 0,
    this.codec = '',
    this.width = 0,
    this.height = 0,
    this.sourceWidth = 0,
    this.sourceHeight = 0,
    this.framerate = 0,
    this.serverBitrate = 0,
    this.decoder = '',
    this.serverEncoder = '',
    this.serverFingerprint = '',
    this.denial = 0,
    this.received = 0,
    this.decoded = 0,
    this.keyframes = 0,
    this.published = 0,
    this.crcErrors = 0,
    this.decodeErrors = 0,
    this.protocolErrors = 0,
    this.gaps = 0,
    this.bytes = 0,
    this.fps = 0,
    this.kbps = 0,
    this.rttMs = 0,
    this.latencyMs = 0,
    this.elapsedMs = 0,
    this.videoFinished = false,
    this.fullscreen = false,
    this.inputEnabled = false,
    this.permissions = 0,
    this.permissionUpdates = 0,
    this.originX = 0,
    this.originY = 0,
    this.inputSent = 0,
    this.inputFailed = 0,
    this.clipboardEnabled = false,
    this.clipboardSent = 0,
    this.clipboardReceived = 0,
    this.clipboardFailed = 0,
    this.cursorUpdates = 0,
    this.cursorPending = false,
    this.audioAnnounced = false,
    this.audioEnabled = false,
    this.audioMuted = false,
    this.audioSampleRate = 0,
    this.audioChannels = 0,
    this.audioBitrate = 0,
    this.audioReceived = 0,
    this.audioDecoded = 0,
    this.audioErrors = 0,
    this.audioUnderruns = 0,
    this.audioRecoveries = 0,
    this.audioRecovering = false,
    this.audioBufferMs = 0,
    this.audioError = '',
    this.displaySwitches = 0,
    this.displaySwitchFailures = 0,
    this.displaySwitchError = '',
    this.displays = const <ViewerDisplay>[],
  });

  final ViewerState state;
  final String error;
  final String host;
  final int port;
  final String codec;

  /// The encoded picture size, which is what the decoder produces and the
  /// texture holds. The host may send a smaller picture than the desktop it
  /// captures.
  final int width;
  final int height;

  /// The captured desktop size. Pointer positions from the host and the
  /// absolute moves sent back travel in this space, so the page maps them
  /// through the source size, not the encoded one.
  final int sourceWidth;
  final int sourceHeight;
  final int framerate;
  final int serverBitrate;
  final String decoder;
  final String serverEncoder;

  /// SHA-256 of the certificate the server presented, 64 uppercase hex digits.
  /// Empty until one arrived; also filled when the identity did not match the
  /// pin, so the viewer can show the server's new fingerprint.
  final String serverFingerprint;

  /// The server's refusal code when it answered Hello with AccessDenied:
  /// [ViewerDenial] values, or 0 when it never refused. [error] carries the
  /// server's own reason.
  final int denial;
  final int received;
  final int decoded;
  final int keyframes;
  final int published;
  final int crcErrors;
  final int decodeErrors;
  final int protocolErrors;
  final int gaps;
  final int bytes;
  final double fps;
  final double kbps;

  /// Probe round trip to the host, in milliseconds; 0 until the first
  /// LatencyReply.
  final double rttMs;

  /// Age of the most recent frame from host capture to local receipt, in
  /// milliseconds; 0 until a reply and a timestamped frame arrived.
  final double latencyMs;
  final int elapsedMs;
  final bool videoFinished;
  final bool fullscreen;
  final bool inputEnabled;

  /// The permission mask the host currently grants this session
  /// (chast::Permission in chast/protocol.h), and how many times the host
  /// announced a change to it. The first announcement counts too, so a
  /// non-zero value after a drop means a live update arrived.
  final int permissions;
  final int permissionUpdates;
  final int originX;
  final int originY;
  final int inputSent;
  final int inputFailed;

  /// True when the server announced that it mirrors clipboard text for this
  /// session. Only then does the page read the local clipboard, and the
  /// native side accepts [ViewerBridge.sendClipboard].
  final bool clipboardEnabled;
  final int clipboardSent;
  final int clipboardReceived;
  final int clipboardFailed;

  /// Pointer updates the host sent. The sprite itself never reaches Dart: the
  /// native side installs it as the Windows cursor, which is where it has to
  /// live to be composited by the OS at the pointer's own rate.
  final int cursorUpdates;

  /// True while a pointer update is waiting for the native side to pick up.
  final bool cursorPending;

  /// True when the host announced system sound in HelloAck/StreamInfo.
  /// [audioEnabled] additionally requires the local decoder and renderer to be
  /// running; [audioError] explains a local pipeline that failed while video
  /// kept going.
  final bool audioAnnounced;
  final bool audioEnabled;
  final bool audioMuted;
  final int audioSampleRate;
  final int audioChannels;
  final int audioBitrate;
  final int audioReceived;
  final int audioDecoded;
  final int audioErrors;
  final int audioUnderruns;

  /// Render-endpoint rebuilds caused by a lost or changed default output.
  /// [audioRecovering] is true while the renderer sits between attempts; the
  /// audio stream stays up, so this is not an error.
  final int audioRecoveries;
  final bool audioRecovering;
  final int audioBufferMs;
  final String audioError;

  /// Display switches the host accepted this session, and the refusals the
  /// engine saw (including requests it could not send), with the host's reason
  /// for the most recent refusal (cleared by an accepted switch). A refused
  /// switch leaves the current stream up; the page explains it and puts the
  /// picker back on [streamedDisplayIndex]. The page compares both counters
  /// with the previous poll so two identical reasons still read as two events.
  final int displaySwitches;
  final int displaySwitchFailures;
  final String displaySwitchError;
  final List<ViewerDisplay> displays;

  bool get hasFrame => published > 0 && width > 0 && height > 0;
  bool get isLive => state == ViewerState.streaming;
  bool get isOver =>
      state == ViewerState.ended || state == ViewerState.failed || error.isNotEmpty;
  // Pointer and keyboard events can only reach the host while the stream is
  // live and the server announced its input gate as open.
  bool get acceptsInput => inputEnabled && isLive;
  bool get acceptsClipboard => clipboardEnabled && isLive;
  // The speaker button and the overlay's audio row need a live local pipeline.
  bool get acceptsAudio => audioEnabled && isLive;
  String get codecLabel => codec == 'hevc' ? 'HEVC' : 'H.264';

  /// The sample rate as "48 kHz", the way the overlay shows it.
  String get audioSampleRateLabel {
    if (audioSampleRate <= 0) {
      return '';
    }
    final double khz = audioSampleRate / 1000.0;
    return '${khz == khz.roundToDouble() ? khz.toStringAsFixed(0) : khz.toStringAsFixed(1)} kHz';
  }

  String get audioChannelLabel => audioChannels >= 2 ? '立体声' : '单声道';

  /// The announced display whose rect is the stream's origin and source size,
  /// or null while no list has arrived.
  int? get streamedDisplayIndex {
    for (final ViewerDisplay display in displays) {
      if (display.left == originX &&
          display.top == originY &&
          display.width == sourceWidth &&
          display.height == sourceHeight) {
        return display.index;
      }
    }
    return null;
  }

  static ViewerStatus fromMap(Map<Object?, Object?> map) {
    int asInt(Object? value) => (value as num?)?.toInt() ?? 0;
    double asDouble(Object? value) => (value as num?)?.toDouble() ?? 0.0;
    String asString(Object? value) => value?.toString() ?? '';
    final int width = asInt(map['width']);
    final int height = asInt(map['height']);
    // Maps from before the source size existed only carry the encoded size,
    // which was also the desktop size then.
    final int sourceWidth = asInt(map['sourceWidth']) > 0 ? asInt(map['sourceWidth']) : width;
    final int sourceHeight =
        asInt(map['sourceHeight']) > 0 ? asInt(map['sourceHeight']) : height;
    final List<ViewerDisplay> displays = <ViewerDisplay>[];
    final Object? rawDisplays = map['displays'];
    if (rawDisplays is List) {
      for (int i = 0; i < rawDisplays.length; ++i) {
        final Object? entry = rawDisplays[i];
        if (entry is Map) {
          final ViewerDisplay? display = ViewerDisplay.fromMap(entry, i);
          if (display != null) {
            displays.add(display);
          }
        }
      }
    }
    return ViewerStatus(
      state: _parseState(map['state']),
      error: asString(map['error']),
      host: asString(map['host']),
      port: asInt(map['port']),
      codec: asString(map['codec']),
      width: width,
      height: height,
      sourceWidth: sourceWidth,
      sourceHeight: sourceHeight,
      framerate: asInt(map['framerate']),
      serverBitrate: asInt(map['serverBitrate']),
      decoder: asString(map['decoder']),
      serverEncoder: asString(map['serverEncoder']),
      serverFingerprint: asString(map['serverFingerprint']),
      denial: asInt(map['denial']),
      received: asInt(map['received']),
      decoded: asInt(map['decoded']),
      keyframes: asInt(map['keyframes']),
      published: asInt(map['published']),
      crcErrors: asInt(map['crcErrors']),
      decodeErrors: asInt(map['decodeErrors']),
      protocolErrors: asInt(map['protocolErrors']),
      gaps: asInt(map['gaps']),
      bytes: asInt(map['bytes']),
      fps: asDouble(map['fps']),
      kbps: asDouble(map['kbps']),
      rttMs: asDouble(map['rttMs']),
      latencyMs: asDouble(map['latencyMs']),
      elapsedMs: asInt(map['elapsedMs']),
      videoFinished: map['videoFinished'] == true,
      fullscreen: map['fullscreen'] == true,
      inputEnabled: map['inputEnabled'] == true,
      permissions: asInt(map['permissions']),
      permissionUpdates: asInt(map['permissionUpdates']),
      originX: asInt(map['originX']),
      originY: asInt(map['originY']),
      inputSent: asInt(map['inputSent']),
      inputFailed: asInt(map['inputFailed']),
      clipboardEnabled: map['clipboardEnabled'] == true,
      clipboardSent: asInt(map['clipboardSent']),
      clipboardReceived: asInt(map['clipboardReceived']),
      clipboardFailed: asInt(map['clipboardFailed']),
      cursorUpdates: asInt(map['cursorUpdates']),
      cursorPending: map['cursorPending'] == true,
      audioAnnounced: map['audioAnnounced'] == true,
      audioEnabled: map['audioEnabled'] == true,
      audioMuted: map['audioMuted'] == true,
      audioSampleRate: asInt(map['audioSampleRate']),
      audioChannels: asInt(map['audioChannels']),
      audioBitrate: asInt(map['audioBitrate']),
      audioReceived: asInt(map['audioReceived']),
      audioDecoded: asInt(map['audioDecoded']),
      audioErrors: asInt(map['audioErrors']),
      audioUnderruns: asInt(map['audioUnderruns']),
      audioRecoveries: asInt(map['audioRecoveries']),
      audioRecovering: map['audioRecovering'] == true,
      audioBufferMs: asInt(map['audioBufferMs']),
      audioError: asString(map['audioError']),
      displaySwitches: asInt(map['displaySwitches']),
      displaySwitchFailures: asInt(map['displaySwitchFailures']),
      displaySwitchError: asString(map['displaySwitchError']),
      displays: List<ViewerDisplay>.unmodifiable(displays),
    );
  }

  static ViewerState _parseState(Object? value) {
    switch (value) {
      case 'connecting':
        return ViewerState.connecting;
      case 'handshaking':
        return ViewerState.handshaking;
      case 'streaming':
        return ViewerState.streaming;
      case 'ended':
        return ViewerState.ended;
      case 'failed':
        return ViewerState.failed;
      default:
        return ViewerState.idle;
    }
  }
}

class ViewerBridge {
  static const MethodChannel _channel = MethodChannel('chast/viewer');

  /// [gpuOutput] is the opaque pointer from
  /// `FlutterGpuTextureRenderer.output(textureId)`; the native side copies
  /// every decoded frame into the texture registered with Flutter.
  ///
  /// [fingerprint] is the server identity this viewer pins, 64 hex digits
  /// (colons, spaces and lower case are accepted). Without one the native side
  /// refuses to connect unless [allowUnverified] is set; that is only for a
  /// first contact whose fingerprint the caller reads from [status] and stores.
  ///
  /// [accessToken] is the server's access token, sent in Hello over the
  /// encrypted connection. Empty for servers started without one.
  ///
  /// [password] is the session password the host asked for. [totp] is the
  /// six-digit code for hosts that also require two-factor. [trustDevice]
  /// asks the host to remember this viewer's identity after a successful
  /// sign-in, so later connections skip the password.
  static Future<void> start({
    required String host,
    required int port,
    required int gpuOutput,
    int display = kViewerDisplayAuto,
    String fingerprint = '',
    bool allowUnverified = false,
    String accessToken = '',
    String password = '',
    String totp = '',
    bool trustDevice = false,
  }) async {
    await _channel.invokeMethod<void>(
      'start',
      <String, Object>{
        'host': host,
        'port': port,
        'gpuOutput': gpuOutput,
        'display': display,
        'fingerprint': fingerprint,
        'allowUnverified': allowUnverified,
        'accessToken': accessToken,
        'password': password,
        'totp': totp,
        'trustDevice': trustDevice,
      },
    );
  }

  /// Reads the password saved for [trustKey] from this Windows account's
  /// DPAPI-protected store, or null when none is saved.
  static Future<String?> loadPassword(String trustKey) {
    return _channel.invokeMethod<String>('loadPassword', trustKey);
  }

  /// Saves [password] for [trustKey]. The native side encrypts it with DPAPI
  /// under the current user; an empty password removes the entry.
  static Future<void> savePassword(String trustKey, String password) async {
    await _channel.invokeMethod<void>('savePassword', <String, Object>{
      'key': trustKey,
      'password': password,
    });
  }

  static Future<void> stop() => _channel.invokeMethod<void>('stop');

  static Future<ViewerStatus> status() async {
    final Object? result = await _channel.invokeMethod<Object?>('status');
    if (result is Map) {
      return ViewerStatus.fromMap(result);
    }
    return const ViewerStatus();
  }

  static Future<bool> setFullscreen(bool enable) async {
    final bool? actual = await _channel.invokeMethod<bool>('setFullscreen', enable);
    return actual ?? false;
  }

  /// Sends one pointer event. [x]/[y] are virtual-desktop pixels for absolute
  /// moves, deltas for relative moves, and WHEEL_DELTA units for the wheel
  /// actions. [modifiers] is the input-state bitmap (see [ViewerModifier] and
  /// [ViewerLock]) captured at this moment. Returns false when the native side
  /// refused to queue it (input disabled, session ending, or a full queue);
  /// malformed arguments throw a [PlatformException].
  static Future<bool> sendMouse({
    required int action,
    int buttons = 0,
    int x = 0,
    int y = 0,
    int modifiers = 0,
  }) async {
    final bool? queued = await _channel.invokeMethod<bool>('sendInput', <String, Object>{
      'kind': 1,
      'action': action,
      'buttons': buttons,
      'x': x,
      'y': y,
      'modifiers': modifiers,
    });
    return queued ?? false;
  }

  /// Sends one key transition by USB HID usage id (usage page 0x07), which is
  /// what `PhysicalKeyboardKey.usbHidUsage` already is. [modifiers] is the
  /// input-state bitmap at this moment; the host reconciles its own state to
  /// it.
  static Future<bool> sendKey({
    required int usage,
    required bool down,
    int modifiers = 0,
  }) async {
    final bool? queued = await _channel.invokeMethod<bool>('sendInput', <String, Object>{
      'kind': 2,
      'action': down ? 0 : 1,
      'buttons': 0,
      'x': 0,
      'y': 0,
      'hidUsage': usage,
      'modifiers': modifiers,
    });
    return queued ?? false;
  }

  /// Asks the host to lift every key and button its injector still holds.
  /// Sent when the viewer gives up control or the window loses focus, so a
  /// release that never made it out cannot leave the host stuck. Queued in
  /// order behind the events already sent.
  static Future<bool> releaseInput() async {
    final bool? queued = await _channel.invokeMethod<bool>('releaseInput');
    return queued ?? false;
  }

  /// Asks the running session to capture another announced display. False
  /// means the request was not queued (no live session, view-only access, or
  /// an index outside the announced list); the host's answer then arrives in
  /// [ViewerStatus.displaySwitches] / [ViewerStatus.displaySwitchError].
  static Future<bool> switchDisplay(int display) async {
    final bool? queued = await _channel.invokeMethod<bool>('switchDisplay', display);
    return queued ?? false;
  }

  /// While remote control is on, an active local IME must not turn physical
  /// keys into VK_PROCESSKEY (the engine drops those before Dart sees them).
  /// Turning this off restores the IME for local text fields.
  static Future<bool> setKeyCapture(bool enable) async {
    final bool? active = await _channel.invokeMethod<bool>('setKeyCapture', enable);
    return active ?? false;
  }

  /// Queues one clipboard text for the host. Invalid UTF-8, text over 256 KiB
  /// and sessions without the server's clipboard gate are refused; the caller
  /// should keep the local copy as the source of truth either way.
  static Future<bool> sendClipboard(String text) async {
    final bool? queued = await _channel.invokeMethod<bool>('sendClipboard', text);
    return queued ?? false;
  }

  /// Takes the next clipboard text the host sent, or null when none is
  /// waiting. Updates are only queued while the server's gate is open.
  static Future<String?> takeClipboard() {
    return _channel.invokeMethod<String>('takeClipboard');
  }

  /// Picks up whatever pointer state the host sent and installs it as this
  /// window's cursor while [enabled]. [scale] is the size the video is drawn
  /// at on this screen (logical video pixels to physical cursor pixels), so a
  /// scaled-down stream gets a proportionally smaller pointer.
  ///
  /// The sprite never crosses this boundary: the native side registers it
  /// straight from the session and only the two numbers come back from Dart.
  /// Returns true while a remote cursor is installed, i.e. while the local one
  /// should stay hidden.
  static Future<bool> refreshRemoteCursor({
    required bool enabled,
    required double scale,
  }) async {
    final bool? installed = await _channel.invokeMethod<bool>(
      'refreshRemoteCursor',
      <String, Object>{
        'enabled': enabled,
        'scaleMilli': (scale * 1000).round(),
      },
    );
    return installed ?? false;
  }

  /// Drops every cached pointer shape and hands the window back to the system
  /// arrow. Called when control ends, so the page's own cursor returns.
  static Future<void> clearRemoteCursor() async {
    await _channel.invokeMethod<void>('clearRemoteCursor');
  }

  /// Mutes or unmutes system sound on this viewer. Decoding and buffering keep
  /// running while muted. Returns false when no audio pipeline is running, so
  /// the caller knows the tap had no effect.
  static Future<bool> setAudioMuted(bool mute) async {
    final bool? muted = await _channel.invokeMethod<bool>('setAudioMuted', mute);
    return muted ?? false;
  }
}

/// Wire values shared with chast/protocol.h.
abstract final class ViewerMouse {
  static const int move = 0;
  static const int moveRelative = 1;
  static const int buttonDown = 2;
  static const int buttonUp = 3;
  static const int wheel = 4;
  static const int wheelHorizontal = 5;

  // Same bit layout as Flutter's PointerEvent.buttons for the five buttons the
  // protocol carries, so the raw value passes through unchanged.
  static const int left = 1;
  static const int right = 2;
  static const int middle = 4;
  static const int back = 8;
  static const int forward = 16;
}

/// Modifier bitmap carried by every input event. One bit per HID usage id
/// 0xE0..0xE7, in that order; the host reconciles its held modifiers to it.
abstract final class ViewerModifier {
  static const int leftCtrl = 1 << 0;
  static const int leftShift = 1 << 1;
  static const int leftAlt = 1 << 2;
  static const int leftGui = 1 << 3;
  static const int rightCtrl = 1 << 4;
  static const int rightShift = 1 << 5;
  static const int rightAlt = 1 << 6;
  static const int rightGui = 1 << 7;
  static const int mask = 0xFF;
}

/// Lock-key state carried in the same bitmap as the modifiers. These are
/// states, not held keys: the host taps a lock key once when its local state
/// differs, so typing matches what the viewer sees.
abstract final class ViewerLock {
  static const int caps = 1 << 8;
  static const int num = 1 << 9;
  static const int scroll = 1 << 10;
  static const int mask = 0x700;
}

/// Modifier bits for the given physical keys; anything that is not one of the
/// eight modifier usages is ignored. [PhysicalKeyboardKey.usbHidUsage] carries
/// the usage page in its upper half (0x000700E0 for left Ctrl), so only page
/// 0x07 keys qualify. Windows reports AltGr as a synthetic left Ctrl plus
/// Right Alt, so the bitmap stays physical and the host folds the pair into
/// Right Alt.
int modifierBitsFor(Iterable<PhysicalKeyboardKey> keys) {
  int bits = 0;
  for (final PhysicalKeyboardKey key in keys) {
    final int usage = key.usbHidUsage & 0x0000FFFF;
    if ((key.usbHidUsage >> 16) == 0x07 && usage >= 0xE0 && usage <= 0xE7) {
      bits |= 1 << (usage - 0xE0);
    }
  }
  return bits;
}

/// Lock-state bits for the keys whose lock is on. The host compares these
/// with its own state on every event that changes them.
int lockBitsFor(Set<KeyboardLockMode> locks) {
  int bits = 0;
  if (locks.contains(KeyboardLockMode.capsLock)) {
    bits |= ViewerLock.caps;
  }
  if (locks.contains(KeyboardLockMode.numLock)) {
    bits |= ViewerLock.num;
  }
  if (locks.contains(KeyboardLockMode.scrollLock)) {
    bits |= ViewerLock.scroll;
  }
  return bits;
}

/// One Windows wheel notch in WHEEL_DELTA units.
const int kViewerWheelDelta = 120;

/// Hello display index meaning "let the server choose the captured output"
/// (chast::kDisplayAuto in chast/protocol.h).
const int kViewerDisplayAuto = 0xFFFF;

/// The permission mask a host announces (chast::Permission in
/// chast/protocol.h). Input covers pointer and keyboard injection; clipboard
/// covers both directions; audio is granted only while an audio stream is open.
abstract final class ViewerPermission {
  static const int input = 0x0001;
  static const int clipboard = 0x0002;
  static const int audio = 0x0004;
}

/// The AccessDenied codes a server answers a refused Hello with
/// (chast::AccessDenial in chast/protocol.h).
abstract final class ViewerDenial {
  static const int invalidToken = 1;
  static const int tokenUsed = 2;
  static const int declined = 3;
  static const int wrongPassword = 4;

  /// The host wants a session password that this connection did not send.
  static const int passwordRequired = 5;
  static const int twoFactorRequired = 6;
  static const int wrongTwoFactor = 7;
  static const int lockedOut = 8;
  static const int notWhitelisted = 9;
  static const int authTimeout = 10;
}

/// What to tell the user about a refusal; empty when the server did not
/// mention a reason the viewer knows.
String accessDenialText(int denial) {
  switch (denial) {
    case ViewerDenial.invalidToken:
      return '访问令牌不正确。请向主机确认当前令牌后重试。';
    case ViewerDenial.tokenUsed:
      return '该一次性令牌已被使用，请让主机重新生成一个。';
    case ViewerDenial.declined:
      return '主机拒绝了本次连接请求，请让主机确认后重试。';
    case ViewerDenial.wrongPassword:
      return '密码不正确，请重试。';
    case ViewerDenial.passwordRequired:
      return '主机要求输入连接密码。';
    case ViewerDenial.twoFactorRequired:
      return '主机要求两步验证码。';
    case ViewerDenial.wrongTwoFactor:
      return '两步验证码不正确或已过期，请重新输入。';
    case ViewerDenial.lockedOut:
      return '尝试次数过多，主机已暂时拒绝此地址。请稍后再试。';
    case ViewerDenial.notWhitelisted:
      return '本机地址不在主机的允许列表中。';
    case ViewerDenial.authTimeout:
      return '主机未在限定时间内完成验证，请重试。';
    default:
      return '';
  }
}

/// Flutter's Windows embedder turns one wheel notch (WHEEL_DELTA) into this
/// many logical pixels of PointerScrollEvent.scrollDelta. The viewer converts
/// back with this constant so a notch on the client is a notch on the host.
const double kViewerScrollPixelsPerNotch = 53.0;
