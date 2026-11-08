import 'package:flutter/services.dart';
import 'package:chast_viewer/l10n.dart';

enum ViewerState { idle, connecting, handshaking, streaming, ended, failed }
enum ViewerKeyboardMode { physical, local }

/// One remote console's life on the viewer (chast_viewer::TerminalPhase).
/// Requesting covers the round trip to the host's answer; ended means the host
/// stopped the console, and any bytes it printed arrive through
/// [ViewerBridge.takeTerminalOutput] before the panel shows that ending.
enum ViewerTerminalPhase {
  closed(0),
  requesting(1),
  open(2),
  ended(3),
  failed(4);

  const ViewerTerminalPhase(this.code);
  final int code;

  static ViewerTerminalPhase fromCode(int code) {
    for (final ViewerTerminalPhase phase in values) {
      if (phase.code == code) {
        return phase;
      }
    }
    return closed;
  }
}

/// Consoles the viewer and the protocol host both accept
/// (chast::kMaxTerminals). Each slot is an independent shell.
const int kViewerMaxTerminals = 2;

/// One console slot, as the session loop and the panel see it. [serviceId] is
/// non-zero while a persistent console is alive; it names what a later
/// session can attach to and clears when the console ends or its open is
/// refused. [bytes] counts what arrived and [updates] pairs a phase change
/// with the poll that saw it.
class ViewerTerminalStatus {
  const ViewerTerminalStatus({
    this.phase = ViewerTerminalPhase.closed,
    this.code = 0,
    this.reason = '',
    this.error = '',
    this.columns = 0,
    this.rows = 0,
    this.bytes = 0,
    this.updates = 0,
    this.serviceId = 0,
  });

  final ViewerTerminalPhase phase;
  final int code;
  final String reason;
  final String error;
  final int columns;
  final int rows;
  final int bytes;
  final int updates;
  final int serviceId;

  factory ViewerTerminalStatus.fromMap(Map<Object?, Object?> map) {
    int number(String name) => (map[name] as num?)?.toInt() ?? 0;
    return ViewerTerminalStatus(
      phase: ViewerTerminalPhase.fromCode(number('phase')),
      code: number('code'),
      reason: map['reason'] as String? ?? '',
      error: map['error'] as String? ?? '',
      columns: number('columns'),
      rows: number('rows'),
      bytes: number('bytes'),
      updates: number('updates'),
      serviceId: number('serviceId'),
    );
  }
}

/// One piece of console output. [ended] marks the last piece the console ever
/// produced; the panel keeps the bytes and then shows the ending.
class ViewerTerminalOutput {
  const ViewerTerminalOutput({required this.data, required this.ended});

  final Uint8List data;
  final bool ended;
}

/// One shared camera's life on the viewer (chast_viewer::CameraPhase).
/// [listing] covers the round trip of a list request, [starting] covers
/// StartCamera until the host's answer, and [streaming] means pictures are
/// arriving. [stopped] is terminal: the viewer asked to stop or the host
/// ended the stream. [failed] carries the host's CameraCode and reason, or a
/// local error.
enum ViewerCameraPhase {
  idle(0),
  listing(1),
  listed(2),
  starting(3),
  streaming(4),
  stopped(5),
  failed(6);

  const ViewerCameraPhase(this.code);
  final int code;

  static ViewerCameraPhase fromCode(int code) {
    for (final ViewerCameraPhase phase in values) {
      if (phase.code == code) {
        return phase;
      }
    }
    return idle;
  }
}

/// One camera the host announced. [id] is only valid for the newest list; a
/// later list may reuse or drop it.
class ViewerCamera {
  const ViewerCamera({
    required this.id,
    required this.name,
    this.isDefault = false,
  });

  final int id;
  final String name;
  final bool isDefault;

  String get label => name.isEmpty ? tr('摄像头') : name;

  static ViewerCamera? fromMap(Map<Object?, Object?> map) {
    final int id = (map['id'] as num?)?.toInt() ?? 0;
    if (id <= 0) {
      return null;
    }
    return ViewerCamera(
      id: id,
      name: map['name']?.toString() ?? '',
      isDefault: map['default'] == true,
    );
  }
}

/// One entry of the host's shared folder. [name] is a single component;
/// [modifiedMs] is Unix milliseconds and 0 when the file system did not
/// report it. Directories carry [size] 0.
class ViewerFileEntry {
  const ViewerFileEntry({
    required this.name,
    required this.size,
    required this.modifiedMs,
    required this.directory,
  });

  final String name;
  final int size;
  final int modifiedMs;
  final bool directory;

  static ViewerFileEntry? fromMap(Map<Object?, Object?> map) {
    final String name = map['name']?.toString() ?? '';
    if (name.isEmpty) {
      return null;
    }
    return ViewerFileEntry(
      name: name,
      size: (map['size'] as num?)?.toInt() ?? 0,
      modifiedMs: (map['modifiedMs'] as num?)?.toInt() ?? 0,
      directory: map['directory'] == true,
    );
  }
}

/// The file operations the host applies below its shared root
/// (`chast::FileOperation`). Only [rename] carries a second name; the others
/// act on [path] alone.
enum ViewerFileOperation {
  makeDirectory(0),
  rename(1),
  remove(2);

  const ViewerFileOperation(this.wireValue);

  final int wireValue;
}

/// One directory listing the host answered (`chast::FileListResult`).
/// [code] is a `chast::FileCode` value: 0 Ok, 1 Refused, 2 Busy, 3 NotFound,
/// 4 DigestMismatch, 5 IoError, 6 Cancelled. [directory] is the relative
/// request path the answer belongs to (empty is the shared root).
class ViewerFileListing {
  const ViewerFileListing({
    required this.directory,
    required this.code,
    required this.reason,
    required this.truncated,
    required this.entries,
    this.cursor = '',
    this.nextCursor = '',
  });

  final String directory;
  final int code;
  final String reason;
  final bool truncated;
  final List<ViewerFileEntry> entries;
  final String cursor;
  final String nextCursor;

  bool get ok => code == 0;
}

/// One entry of a batch download: [path] is the '/'-separated relative path
/// below the shared root and [directory] selects the recursive walk.
class ViewerFileSelection {
  const ViewerFileSelection({required this.path, required this.directory});

  final String path;
  final bool directory;
}

/// One finished transfer the native session queued for the audit. [upload] is
/// true for a viewer-to-host transfer and false for a host download; [detail]
/// explains a failure and is empty on success. [occurredAtMs] is Unix
/// milliseconds taken when the transfer finished. Clipboard staging is not
/// queued, matching what the host audits.
class ViewerCompletedFile {
  const ViewerCompletedFile({
    required this.upload,
    required this.name,
    required this.size,
    required this.success,
    required this.detail,
    required this.occurredAtMs,
  });

  final bool upload;
  final String name;
  final int size;
  final bool success;
  final String detail;
  final int occurredAtMs;

  static ViewerCompletedFile? fromMap(Map<Object?, Object?> map) {
    final String name = map['name']?.toString() ?? '';
    if (name.isEmpty) {
      return null;
    }
    return ViewerCompletedFile(
      upload: map['upload'] == true,
      name: name,
      size: (map['size'] as num?)?.toInt() ?? 0,
      success: map['success'] == true,
      detail: map['detail']?.toString() ?? '',
      occurredAtMs: (map['occurredAtMs'] as num?)?.toInt() ?? 0,
    );
  }
}

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
    return slash >= 0 && slash + 1 < name.length
        ? name.substring(slash + 1)
        : name;
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

class ViewerDisplayMode {
  const ViewerDisplayMode({
    required this.width,
    required this.height,
    required this.refresh,
    this.current = false,
  });

  final int width;
  final int height;
  final int refresh;

  /// True for the mode the host's output runs right now.
  final bool current;

  /// "1920 × 1080 · 60 Hz", the way the resolution menu lists a mode.
  String get label => '$width × $height · $refresh Hz';

  static ViewerDisplayMode? fromMap(Map<Object?, Object?> map) {
    final int width = (map['width'] as num?)?.toInt() ?? 0;
    final int height = (map['height'] as num?)?.toInt() ?? 0;
    final int refresh = (map['refresh'] as num?)?.toInt() ?? 0;
    if (width <= 0 || height <= 0 || refresh <= 0) {
      return null;
    }
    return ViewerDisplayMode(
      width: width,
      height: height,
      refresh: refresh,
      current: map['current'] == true,
    );
  }
}

/// One viewer-side port-forward rule: the local listener and the host target
/// it relays to. Up to eight rules run in the same session; [active] is true
/// once the host accepted the target, and [error] explains a refusal or a
/// failed listener until the rule is removed.
class ViewerPortForwardRule {
  const ViewerPortForwardRule({
    required this.ruleId,
    this.active = false,
    this.localPort = 0,
    this.remoteHost = '',
    this.remotePort = 0,
    this.connections = 0,
    this.failures = 0,
    this.bytesUp = 0,
    this.bytesDown = 0,
    this.error = '',
  });

  final int ruleId;
  final bool active;
  final int localPort;
  final String remoteHost;
  final int remotePort;
  final int connections;
  final int failures;
  final int bytesUp;
  final int bytesDown;
  final String error;

  static ViewerPortForwardRule? fromMap(Map<Object?, Object?> map) {
    final int ruleId = (map['ruleId'] as num?)?.toInt() ?? 0;
    if (ruleId <= 0) {
      return null;
    }
    return ViewerPortForwardRule(
      ruleId: ruleId,
      active: map['active'] == true,
      localPort: (map['localPort'] as num?)?.toInt() ?? 0,
      remoteHost: map['remoteHost']?.toString() ?? '',
      remotePort: (map['remotePort'] as num?)?.toInt() ?? 0,
      connections: (map['connections'] as num?)?.toInt() ?? 0,
      failures: (map['failures'] as num?)?.toInt() ?? 0,
      bytesUp: (map['bytesUp'] as num?)?.toInt() ?? 0,
      bytesDown: (map['bytesDown'] as num?)?.toInt() ?? 0,
      error: map['error']?.toString() ?? '',
    );
  }
}

/// One Windows session the host can hand the stream to (protocol v40).
/// [id] is the WTS session id the switch request sends back; only active
/// sessions with a logged-on user are listed, and [current] marks the session
/// that is being streamed now.
class ViewerRemoteSession {
  const ViewerRemoteSession({
    required this.id,
    required this.user,
    this.active = true,
    this.current = false,
  });

  final int id;
  final String user;
  final bool active;
  final bool current;

  String get label => user.isEmpty ? 'Session $id' : '$user · Session $id';

  static ViewerRemoteSession? fromMap(Map<Object?, Object?> map) {
    final int id = (map['id'] as num?)?.toInt() ?? 0;
    if (id <= 0 || id >= 0xFFFFFFFF) {
      return null;
    }
    return ViewerRemoteSession(
      id: id,
      user: (map['user'] as String?) ?? '',
      active: map['active'] == true,
      current: map['current'] == true,
    );
  }
}

/// One active capture endpoint the host machine exposes as a microphone
/// source. [id] is the WASAPI endpoint id the picker sends back.
class ViewerMicrophoneDevice {
  const ViewerMicrophoneDevice({
    required this.id,
    required this.name,
    this.isDefault = false,
  });

  final String id;
  final String name;
  final bool isDefault;

  String get label => name.isEmpty ? tr('麦克风') : name;

  static ViewerMicrophoneDevice? fromMap(Map<Object?, Object?> map) {
    final String id = map['id']?.toString() ?? '';
    if (id.isEmpty) {
      return null;
    }
    return ViewerMicrophoneDevice(
      id: id,
      name: map['name']?.toString() ?? '',
      isDefault: map['default'] == true,
    );
  }
}

/// One active playback endpoint the viewer can render system audio to. [id]
/// is the WASAPI endpoint id the picker sends back; empty selects the system
/// default instead.
class ViewerAudioOutput {
  const ViewerAudioOutput({
    required this.id,
    required this.name,
    this.isDefault = false,
  });

  final String id;
  final String name;
  final bool isDefault;

  String get label => name.isEmpty ? tr('音频输出') : name;

  static ViewerAudioOutput? fromMap(Map<Object?, Object?> map) {
    final String id = map['id']?.toString() ?? '';
    if (id.isEmpty) {
      return null;
    }
    return ViewerAudioOutput(
      id: id,
      name: map['name']?.toString() ?? '',
      isDefault: map['default'] == true,
    );
  }
}

/// One virtual desktop output the host can toggle. [id] is a session-stable
/// identity, never a display-list index, so a reordered list cannot address
/// the wrong output.
class ViewerControlledDisplay {
  const ViewerControlledDisplay({required this.id, required this.active});

  final int id;
  final bool active;

  static List<ViewerControlledDisplay> fromList(Object? value) {
    if (value is! List) {
      return const <ViewerControlledDisplay>[];
    }
    return <ViewerControlledDisplay>[
      for (final item in value)
        if (item is Map && item['id'] is int && (item['id'] as int) > 0)
          ViewerControlledDisplay(
            id: item['id'] as int,
            active: item['active'] == true,
          ),
    ];
  }
}

class ViewerStatus {
  const ViewerStatus({
    this.state = ViewerState.idle,
    this.error = '',
    this.host = '',
    this.port = 0,
    this.transport = 'quic',
    this.transportError = '',
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
    this.auditRef = '',
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
    this.filesOnly = false,
    this.controlOnly = false,
    this.sessionMode = 'desktop',
    this.fullscreen = false,
    this.inputEnabled = false,
    this.permissions = 0,
    this.permissionUpdates = 0,
    this.originX = 0,
    this.originY = 0,
    this.inputSent = 0,
    this.inputFailed = 0,
    this.clipboardEnabled = false,
    this.clipboardDirection = 3,
    this.clipboardSent = 0,
    this.clipboardReceived = 0,
    this.clipboardFailed = 0,
    this.clipboardSentFormats = 0,
    this.clipboardReceivedFormats = 0,
    this.filePhase = ViewerFilePhase.idle,
    this.fileName = '',
    this.fileRelativePath = '',
    this.fileQueue = 0,
    this.fileSize = 0,
    this.fileSent = 0,
    this.fileError = '',
    this.lastFileName = '',
    this.lastFileError = '',
    this.filesSent = 0,
    this.fileFailures = 0,
    this.fileCancels = 0,
    this.downloadPhase = ViewerFilePhase.idle,
    this.downloadName = '',
    this.downloadPath = '',
    this.downloadSize = 0,
    this.downloadReceived = 0,
    this.downloadError = '',
    this.filesReceived = 0,
    this.fileReceiveFailures = 0,
    this.downloadCancels = 0,
    this.downloadCancellable = false,
    this.fileListAnswers = 0,
    this.fileListDirectory = '',
    this.fileListEntries = 0,
    this.fileListTruncated = false,
    this.fileListError = '',
    this.fileDownloadAnswers = 0,
    this.fileDownloadCode = 0,
    this.fileDownloadTransferId = 0,
    this.fileDownloadPath = '',
    this.fileDownloadError = '',
    this.fileOpAnswers = 0,
    this.fileOpCode = 0,
    this.fileOpPath = '',
    this.fileOpError = '',
    this.folderDownloadActive = false,
    this.folderDownloadName = '',
    this.folderDownloadDestination = '',
    this.folderDownloadFilesTotal = 0,
    this.folderDownloadFilesDone = 0,
    this.folderDownloadCurrent = '',
    this.folderDownloadRoots = 0,
    this.folderDownloadBatch = false,
    this.folderDownloadAnswers = 0,
    this.folderDownloadCode = 0,
    this.folderDownloadError = '',
    this.cursorUpdates = 0,
    this.cursorPending = false,
    this.audioAnnounced = false,
    this.audioEnabled = false,
    this.audioMuted = false,
    this.recording = false,
    this.recordPath = '',
    this.recordError = '',
    this.recordSamples = 0,
    this.recordKeyframes = 0,
    this.recordSkipped = 0,
    this.recordSegments = 0,
    this.screenshotUpdates = 0,
    this.screenshotPath = '',
    this.screenshotError = '',
    this.audioSampleRate = 0,
    this.audioChannels = 0,
    this.audioBitrate = 0,
    this.audioReceived = 0,
    this.audioDecoded = 0,
    this.audioErrors = 0,
    this.audioUnderruns = 0,
    this.audioStarvedMs = 0,
    this.audioDropped = 0,
    this.audioRecoveries = 0,
    this.audioRecovering = false,
    this.audioBufferMs = 0,
    this.audioLatencyMs = -1,
    this.avSkewMs = -1,
    this.audioError = '',
    this.audioDevice = '',
    this.audioDeviceName = '',
    this.audioAec = true,
    this.audioNoiseSuppression = true,
    this.microphoneAecFrames = 0,
    this.microphoneNsFrames = 0,
    this.microphoneAuthorized = false,
    this.microphoneEnabled = false,
    this.microphoneDevice = '',
    this.microphoneName = '',
    this.microphonePackets = 0,
    this.microphoneDropped = 0,
    this.microphoneErrors = 0,
    this.microphoneRecoveries = 0,
    this.microphoneRecovering = false,
    this.microphoneError = '',
    this.voiceAuthorized = false,
    this.voiceEnabled = false,
    this.voicePending = false,
    this.voicePackets = 0,
    this.voiceDropped = 0,
    this.voiceErrors = 0,
    this.voiceError = '',
    this.privacySupported = false,
    this.privacyEnabled = false,
    this.privacyCode = 0,
    this.privacyError = '',
    this.privacyUpdates = 0,
    this.displayControlSupported = false,
    this.displayControlManaged = false,
    this.displayControlCount = 0,
    this.displayControlPrivacy = 0,
    this.displayControlCode = 0,
    this.displayControlError = '',
    this.displayControlUpdates = 0,
    this.displayControlBusy = false,
    this.displayControlOutputs = const <ViewerControlledDisplay>[],
    this.desktop = ViewerDesktopKind.defaultDesktop,
    this.desktopUpdates = 0,
    this.sasCode = 0,
    this.sasError = '',
    this.sasUpdates = 0,
    this.restartSupported = false,
    this.blockInputSupported = false,
    this.blockInputActive = false,
    this.hostActionAction = 0,
    this.hostActionCode = 0,
    this.hostActionError = '',
    this.hostActionUpdates = 0,
    this.osLoginSupported = false,
    this.osLoginCode = 0,
    this.osLoginError = '',
    this.osLoginUpdates = 0,
    this.elevationCode = 0,
    this.elevationRequestId = 0,
    this.elevationReason = '',
    this.elevationUpdates = 0,
    this.sessionSelectSupported = false,
    this.sessions = const <ViewerRemoteSession>[],
    this.sessionListCode = 0,
    this.sessionListError = '',
    this.sessionListUpdates = 0,
    this.sessionSelectId = 0,
    this.sessionSelectCode = 0,
    this.sessionSelectError = '',
    this.sessionSelectUpdates = 0,
    this.chatSent = 0,
    this.chatReceived = 0,
    this.chatFailures = 0,
    this.chatError = '',
    this.displaySwitches = 0,
    this.displaySwitchFailures = 0,
    this.displaySwitchError = '',
    this.displayModes = const <ViewerDisplayMode>[],
    this.displayModesDisplay = 0,
    this.displayModesCode = 0,
    this.displayModesError = '',
    this.displayModeLists = 0,
    this.displayModeChanges = 0,
    this.displayModeFailures = 0,
    this.displayModeError = '',
    this.displayFollowFlags = 0,
    this.displayFollowCode = 0,
    this.displayFollowError = '',
    this.displayFollowResults = 0,
    this.streamQuality = 0,
    this.chroma = 0,
    this.streamOptionCode = 0,
    this.streamOptionError = '',
    this.streamOptionResults = 0,
    this.remoteConfigSupported = false,
    this.remoteConfigFlags = 0,
    this.remoteConfigCode = 0,
    this.remoteConfigError = '',
    this.remoteConfigUpdates = 0,
    this.portForwardEnabled = false,
    this.portForwardRules = const <ViewerPortForwardRule>[],
    this.terminalSupported = false,
    this.terminalPersistSupported = false,
    this.terminalServiceId = 0,
    this.terminalPhase = ViewerTerminalPhase.closed,
    this.terminalCode = 0,
    this.terminalReason = '',
    this.terminalError = '',
    this.terminalColumns = 0,
    this.terminalRows = 0,
    this.terminalBytes = 0,
    this.terminalUpdates = 0,
    this.terminals = const <ViewerTerminalStatus>[],
    this.cameraSupported = false,
    this.cameraPhase = ViewerCameraPhase.idle,
    this.cameraCode = 0,
    this.cameraReason = '',
    this.cameraError = '',
    this.cameraId = 0,
    this.cameras = const <ViewerCamera>[],
    this.cameraListUpdates = 0,
    this.cameraWidth = 0,
    this.cameraHeight = 0,
    this.cameraFrames = 0,
    this.cameraDecoder = '',
    this.cameraUpdates = 0,
    this.displays = const <ViewerDisplay>[],
    this.mergedLayout = false,
  });

  final ViewerState state;
  final String error;
  final String host;
  final int port;

  /// The path the session settled on: `quic` (UDP), `tcp` or `ws` (the TCP
  /// and WebSocket carriers). While connecting it is the requested mode, so
  /// `auto` shows as `quic` until the first path answers.
  final String transport;

  /// Why the paths before [transport] were abandoned, joined with `; `.
  /// Empty when the first path worked; kept after a fallback so the overlay
  /// can explain why the session is not on UDP.
  final String transportError;
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

  /// Lowercase hex reference generated for this session and sent in Hello.
  /// The host copies it into its own audit records; reporting it here lets the
  /// account server merge both sides into one row.
  final String auditRef;

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

  /// True when the host answered a file-transfer-only request: this session
  /// has no picture, sound, input or pointer stream, and the page shows the
  /// file browser instead of the remote screen. The state still reaches
  /// [ViewerState.streaming] once the host accepted the Hello.
  final bool filesOnly;

  /// True when the host answered a terminal, camera or port-forward request:
  /// the session has no picture and no remote-screen stack. The state still
  /// reaches [ViewerState.streaming] once the host accepted the Hello.
  final bool controlOnly;

  /// What the host agreed to serve: `desktop`, `file_transfer`, `terminal`,
  /// `camera` or `port_forward` (protocol v41). Anything but `desktop` has no
  /// screen stream, so the page shows the matching panel instead of a picture.
  final String sessionMode;
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

  /// True when the server announced that it mirrors the clipboard for this
  /// session. The native side then reads and writes the local clipboard
  /// itself; the page only pauses or resumes it through
  /// [ViewerBridge.setClipboardSync].
  final bool clipboardEnabled;
  // Host-relative policy: 0 disabled, 1 host to viewer, 2 viewer to host, 3 both.
  final int clipboardDirection;
  final int clipboardSent;
  final int clipboardReceived;
  final int clipboardFailed;

  /// Bitmask of [ViewerClipboardFormat] in the most recent item sent to the
  /// host and received from it (0 until one crossed).
  final int clipboardSentFormats;
  final int clipboardReceivedFormats;

  /// The current or most recent viewer-to-host upload. [filePhase] is a
  /// [ViewerFilePhase] value; [fileSent] counts bytes streamed, resume offset
  /// included. A failure leaves the local file untouched and explains itself
  /// in [fileError]. [fileRelativePath] is the folder below the host's receive
  /// root (empty at the root) and [fileQueue] counts the entries not yet
  /// finished, including this one. [filesSent] and [fileFailures] cover the
  /// session.
  final int filePhase;
  final String fileName;
  final String fileRelativePath;
  final int fileQueue;
  final int fileSize;
  final int fileSent;
  final String fileError;

  /// The last upload that left the queue. [filesSent] and [fileFailures] are
  /// advanced together with this name and reason, so a poll that sees a new
  /// counter also has the name it belongs to (the active entry may already be
  /// the next file).
  final String lastFileName;
  final String lastFileError;
  final int filesSent;
  final int fileFailures;

  /// Uploads the viewer dropped after the user asked to cancel them. They are
  /// kept apart from [fileFailures] so a page can word them as cancelled.
  final int fileCancels;

  /// The most recent host-to-viewer download. [downloadPhase] is a
  /// [ViewerFilePhase] value; [downloadReceived] counts bytes on disk, so a
  /// resumed transfer starts above zero. [filesReceived] and
  /// [fileReceiveFailures] cover the session.
  final int downloadPhase;
  final String downloadName;

  /// Where the finished download was renamed to on this viewer; empty until
  /// [downloadPhase] is done.
  final String downloadPath;
  final int downloadSize;
  final int downloadReceived;
  final String downloadError;
  final int filesReceived;
  final int fileReceiveFailures;

  /// Downloads the viewer stopped through [ViewerBridge.cancelDownload];
  /// counted apart from [fileReceiveFailures] so a user cancel can be worded
  /// as cancelled.
  final int downloadCancels;

  /// Whether the active download is an ordinary single-file request. Folder
  /// walks have their own cancel and clipboard transfers follow the clipboard
  /// permission, so the page offers no cancel action for those.
  final bool downloadCancellable;

  /// Shared-folder browsing. [fileListAnswers] changes with every listing the
  /// host sent (the entries themselves come from [ViewerBridge.takeFileList]);
  /// [fileListDirectory] echoes the request, [fileListTruncated] says the host
  /// stopped at its entry cap, and [fileListError] explains a refusal.
  final int fileListAnswers;
  final String fileListDirectory;
  final int fileListEntries;
  final bool fileListTruncated;
  final String fileListError;

  /// The newest viewer-initiated download request. A zero [fileDownloadCode]
  /// (Ok) means the host queued it and its progress shows up in the download
  /// fields above; [fileDownloadError] carries the host's refusal otherwise.
  final int fileDownloadAnswers;
  final int fileDownloadCode;
  final int fileDownloadTransferId;
  final String fileDownloadPath;
  final String fileDownloadError;

  /// The newest file-operation answer (new folder, rename or remove).
  /// [fileOpAnswers] changes with every answer, [fileOpCode] is a
  /// `chast::FileCode` value (0 Ok), [fileOpPath] echoes the entry and
  /// [fileOpError] explains a refusal.
  final int fileOpAnswers;
  final int fileOpCode;
  final String fileOpPath;
  final String fileOpError;

  /// A folder download walks the browsed tree and mirrors it below
  /// [folderDownloadDestination]. [folderDownloadFilesTotal] grows while
  /// folders are listed; [folderDownloadCurrent] names the file in flight.
  /// [folderDownloadAnswers] moves once when the walk ends and
  /// [folderDownloadCode] then says how: 0 done, 1 failed, 2 cancelled, with
  /// [folderDownloadError] explaining the other two.
  final bool folderDownloadActive;
  final String folderDownloadName;
  final String folderDownloadDestination;
  final int folderDownloadFilesTotal;
  final int folderDownloadFilesDone;
  final String folderDownloadCurrent;

  /// The selected entries the running walk was asked for: 1 is the folder
  /// row's recursive download, higher counts are a batch.
  final int folderDownloadRoots;

  /// Whether the walk came from the multi-select bar. A batch can hold one
  /// file, which [folderDownloadRoots] alone cannot tell from a folder row.
  final bool folderDownloadBatch;
  final int folderDownloadAnswers;
  final int folderDownloadCode;
  final String folderDownloadError;

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

  /// Viewer-side recording: true while the engine is muxing the received
  /// stream to a local MP4. Nothing crosses the wire and nothing is uploaded.
  /// [recordPath] is the newest file, [recordError] explains a failure, and
  /// the counters mirror the command-line tool (a picture-size change rolls
  /// the recording into a new segment, counted by [recordSegments]).
  final bool recording;
  final String recordPath;
  final String recordError;
  final int recordSamples;
  final int recordKeyframes;
  final int recordSkipped;
  final int recordSegments;

  /// Viewer-side screenshots: the engine reads the newest decoded picture back
  /// from the GPU and writes a PNG on this machine, so nothing crosses the
  /// wire. [screenshotUpdates] counts finished attempts, [screenshotPath] is
  /// the file the last success wrote and [screenshotError] explains the last
  /// failure.
  final int screenshotUpdates;
  final String screenshotPath;
  final String screenshotError;
  final int audioSampleRate;
  final int audioChannels;
  final int audioBitrate;
  final int audioReceived;
  final int audioDecoded;
  final int audioErrors;
  final int audioUnderruns;

  /// Scheduled silence on the playout timeline (not just underrun events)
  /// and blocks discarded after a missed slot or queue overflow, mirroring
  /// the CLI playback line. These are the counters the dual-machine audio
  /// acceptance compares, especially while blocks are being dropped.
  final int audioStarvedMs;
  final int audioDropped;

  /// Render-endpoint rebuilds caused by a lost or changed default output.
  /// [audioRecovering] is true while the renderer sits between attempts; the
  /// audio stream stays up, so this is not an error.
  final int audioRecoveries;
  final bool audioRecovering;
  final int audioBufferMs;

  /// Host-clock audio playout latency (capture to scheduled render) and its
  /// skew against [latencyMs]: positive means audio reaches the viewer later
  /// than video captured at the same moment. Negative until measured.
  final double audioLatencyMs;
  final double avSkewMs;
  final String audioError;

  /// The playback endpoint the engine is using: empty follows the Windows
  /// default. [audioDeviceName] is its friendly name (empty while the default
  /// or an unavailable id is in use); a fallback to the default after a failed
  /// open is explained in [audioError]. [audioAec] and
  /// [audioNoiseSuppression] are the microphone processing switches;
  /// [microphoneAecFrames]/[microphoneNsFrames] count processed near-end
  /// frames under each switch this session.
  final String audioDevice;
  final String audioDeviceName;
  final bool audioAec;
  final bool audioNoiseSuppression;
  final int microphoneAecFrames;
  final int microphoneNsFrames;

  /// Microphone uplink. [microphoneAuthorized] mirrors the host permission;
  /// [microphoneEnabled] means the local capture and the CMIC stream are
  /// running. [microphoneDevice] is the pinned endpoint id (empty follows the
  /// default communications device) and [microphoneName] what the picker
  /// shows. A failure never touches video or system sound and explains itself
  /// in [microphoneError].
  final bool microphoneAuthorized;
  final bool microphoneEnabled;
  final String microphoneDevice;
  final String microphoneName;
  final int microphonePackets;
  final int microphoneDropped;
  final int microphoneErrors;
  final int microphoneRecoveries;
  final bool microphoneRecovering;
  final String microphoneError;

  /// Voice call with the host microphone. [voiceAuthorized] mirrors the host
  /// permission; [voiceEnabled] means the call is up and the host's Opus
  /// packets are being played; [voicePending] covers the request round trip.
  /// A refusal or a host-initiated close explains itself in [voiceError].
  final bool voiceAuthorized;
  final bool voiceEnabled;
  final bool voicePending;
  final int voicePackets;
  final int voiceDropped;
  final int voiceErrors;
  final String voiceError;

  /// Privacy mode hides the host's own screen while the stream continues.
  /// [privacySupported] mirrors the host's permission; [privacyEnabled] is the
  /// overlay state from the host's most recent answer, and every session starts
  /// off. [privacyCode] is the host's PrivacyModeCode (0 = Ok) and
  /// [privacyError] its reason for a refusal or a host-initiated disable.
  /// [privacyUpdates] counts answers so the page can pair one with its poll.
  final bool privacySupported;
  final bool privacyEnabled;
  final int privacyCode;
  final String privacyError;
  final int privacyUpdates;

  /// Virtual display management (protocol v47). [displayControlSupported] is
  /// the host's driver probe, [displayControlManaged] means this session owns
  /// a temporary layout, and [displayControlCount] is the number of attached
  /// virtual outputs. [displayControlOutputs] carries the session-stable ids
  /// the toolbar toggles; [displayControlPrivacy] is the selected privacy
  /// implementation (0 overlay, 1 virtual display). [displayControlCode] is
  /// the host's DisplayControlCode (0 = Ok), [displayControlError] its reason,
  /// and [displayControlBusy] covers the request/answer round trip.
  final bool displayControlSupported;
  final bool displayControlManaged;
  final int displayControlCount;
  final int displayControlPrivacy;
  final int displayControlCode;
  final String displayControlError;
  final int displayControlUpdates;
  final bool displayControlBusy;
  final List<ViewerControlledDisplay> displayControlOutputs;

  /// Which desktop the host's video is capturing: the user desktop, a secure
  /// desktop (lock screen, UAC, credential prompt) or none. The picture pauses
  /// while the host cannot follow it and resumes when the user desktop returns.
  final ViewerDesktopKind desktop;
  final int desktopUpdates;

  /// Ctrl+Alt+Del answers. [sasCode] is the host's [ViewerSasCode] (0 = Ok),
  /// [sasError] its explanation, and [sasUpdates] counts answers so the page
  /// can pair each one with its poll. Only the host's service mode can send
  /// the sequence; the tray reports that it is unsupported.
  final int sasCode;
  final String sasError;
  final int sasUpdates;

  /// Host-control actions (remote restart, console lock, local input block).
  /// [restartSupported] and [blockInputSupported] mirror the host's
  /// permissions; [blockInputActive] is the host's block state as last
  /// reported. [hostActionAction] names the answered action
  /// ([ViewerHostAction.code]), [hostActionCode] is its
  /// [ViewerHostActionCode] (0 = Ok), [hostActionError] the reason for a
  /// refusal, and [hostActionUpdates] counts answers so the page can pair one
  /// with its poll. The host also sends an unsolicited BlockInput denial when
  /// it releases the block itself.
  final bool restartSupported;
  final bool blockInputSupported;
  final bool blockInputActive;
  final int hostActionAction;
  final int hostActionCode;
  final String hostActionError;
  final int hostActionUpdates;

  /// OS login (typing the host's password at its logon or lock screen).
  /// [osLoginSupported] requires both the input and OS-login permissions.
  /// [osLoginCode] is the host's [ViewerHostActionCode]-style answer (0 = Ok),
  /// [osLoginError] the reason for a refusal, and [osLoginUpdates] counts
  /// answers so the page can pair one with its poll. The password never
  /// crosses this status: it is sent once and never echoed back.
  final bool osLoginSupported;
  final int osLoginCode;
  final String osLoginError;
  final int osLoginUpdates;

  /// Windows session select (protocol v40). [sessionSelectSupported] mirrors
  /// the host's mask-only permission, which only its service backend grants.
  /// [sessions] is the newest list answer: [sessionListCode] is its
  /// [ViewerSessionListCode] (0 = Ok), [sessionListError] its reason and
  /// [sessionListUpdates] pairs an answer with one poll. A switch is answered
  /// for [sessionSelectId] with [sessionSelectCode] (0 = Ok, a
  /// [ViewerSessionSelectCode]) and its reason; an accepted switch ends the
  /// connection, so the page reconnects and lands in the target session.
  final int elevationCode;
  final int elevationRequestId;
  final String elevationReason;
  final int elevationUpdates;
  bool get elevationPending => elevationCode == 3 || elevationCode == 4;
  bool get elevationAvailable =>
      acceptsInput && const <int>[1, 5, 6, 7, 8].contains(elevationCode);
  final bool sessionSelectSupported;
  final List<ViewerRemoteSession> sessions;
  final int sessionListCode;
  final String sessionListError;
  final int sessionListUpdates;
  final int sessionSelectId;
  final int sessionSelectCode;
  final String sessionSelectError;
  final int sessionSelectUpdates;

  /// Session chat counters. Both directions are ungated once the host admits
  /// the viewer. [chatFailures] counts queued lines the transport failed to
  /// send, with [chatError] explaining the most recent one. Inbound text is
  /// taken separately through [ViewerBridge.takeChat], so a status poll never
  /// carries conversation content.
  final int chatSent;
  final int chatReceived;
  final int chatFailures;
  final String chatError;

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

  /// True when the host is streaming the union of [displays] as one picture.
  /// No single entry then describes the streamed surface, and the host refuses
  /// per-display mode changes.
  final bool mergedLayout;

  /// The modes the host last reported for one output, the index that answer
  /// describes, and the host's DisplayModeCode (0 = Ok) with its reason. A new
  /// answer replaces the previous list even when it is a refusal, so the page
  /// checks [displayModesDisplay] against [streamedDisplayIndex] before using
  /// it. [displayModeLists] counts answers, so the page can wait for its own
  /// request.
  final List<ViewerDisplayMode> displayModes;
  final int displayModesDisplay;
  final int displayModesCode;
  final String displayModesError;
  final int displayModeLists;

  /// Mode changes the host accepted, and the refusals the engine saw
  /// (including requests it could not send), with the host's reason for the
  /// most recent refusal (cleared by an accepted change). A refusal leaves the
  /// picture as it was. The page compares both counters with the previous poll
  /// so two identical reasons still read as two events.
  final int displayModeChanges;
  final int displayModeFailures;
  final String displayModeError;

  /// Automatic display following. Bit 0 follows the host's pointer and bit 1
  /// its foreground window ([ViewerDisplayFollow]). The flags are what the
  /// host reports in effect, so a host-initiated disable clears them too.
  /// [displayFollowCode] is the host's [ViewerDisplayFollowCode] (0 = Ok) for
  /// the newest answer with its reason in [displayFollowError];
  /// [displayFollowResults] counts answers, including local send failures.
  final int displayFollowFlags;
  final int displayFollowCode;
  final String displayFollowError;
  final int displayFollowResults;

  /// Stream preferences the host actually runs. [streamQuality] is a
  /// [ViewerStreamQuality] value; the effective encoder and frame rate are
  /// [codec] and [framerate], which the host's StreamInfo also updates.
  /// [streamOptionCode] is the host's [ViewerStreamOptionsCode] (0 = Ok) for
  /// the most recent answer and [streamOptionError] its refusal reason
  /// (cleared by an accepted change). [streamOptionResults] counts answers,
  /// including local send failures, so the page can pair one with its poll.
  final int streamQuality;
  final int chroma;
  final int streamOptionCode;
  final String streamOptionError;
  final int streamOptionResults;

  /// Remote configuration (protocol v46). [remoteConfigSupported] mirrors the
  /// host's announced [ViewerPermission.remoteConfig]; [remoteConfigFlags] is
  /// the switch snapshot the host last reported (bits from
  /// [ViewerRemoteConfigSwitch]). [remoteConfigCode] is the host's
  /// [ViewerRemoteConfigCode] (0 = Ok) for the newest answer, with its reason
  /// in [remoteConfigError]; [remoteConfigUpdates] counts answers, including
  /// local send failures, so the page can pair one with its poll.
  final bool remoteConfigSupported;
  final int remoteConfigFlags;
  final int remoteConfigCode;
  final String remoteConfigError;
  final int remoteConfigUpdates;

  /// Viewer-side port forwarding. [portForwardEnabled] mirrors the host's
  /// permission; every entry in [portForwardRules] owns a local listener on
  /// 127.0.0.1 and one host target. A rule is active once the host accepted
  /// its target; while the request is in flight the listener is already bound
  /// but [ViewerPortForwardRule.active] stays false. The counters are per
  /// rule, and a refused rule stays listed with its error.
  final bool portForwardEnabled;
  final List<ViewerPortForwardRule> portForwardRules;

  /// Remote terminals. [terminalSupported] mirrors the host's permission;
  /// [terminalPersistSupported] says the host keeps consoles across
  /// disconnects (protocol v42) and each slot's service id is the kept
  /// console's id, 0 for a session console or before the host answers.
  /// [terminals] is the per-slot truth; the scalar fields below always
  /// describe slot 1 for callers that predate the tabs. A slot's [phase] is
  /// its console state, [code] and [reason] carry the host's TerminalCode and
  /// wording for a refusal or an ending, and [error] explains a local failure
  /// such as a send error or a revoked permission. [columns]/[rows] are the
  /// size the host last acknowledged. Output never travels in this struct:
  /// [ViewerBridge.takeTerminalOutput] hands it over and [bytes] counts what
  /// arrived. [updates] counts phase transitions so the panel pairs each
  /// ending with the poll that saw it.
  final bool terminalSupported;
  final bool terminalPersistSupported;
  final int terminalServiceId;
  final ViewerTerminalPhase terminalPhase;
  final int terminalCode;
  final String terminalReason;
  final String terminalError;
  final int terminalColumns;
  final int terminalRows;
  final int terminalBytes;
  final int terminalUpdates;
  final List<ViewerTerminalStatus> terminals;

  /// The status of console slot [id] (1-based). Out-of-range ids read as a
  /// closed console; slot 1 falls back to the legacy scalar fields when the
  /// native side did not send a list.
  ViewerTerminalStatus terminal(int id) {
    if (id >= 1 && id <= terminals.length) {
      return terminals[id - 1];
    }
    if (id != 1) {
      return const ViewerTerminalStatus();
    }
    return ViewerTerminalStatus(
      phase: terminalPhase,
      code: terminalCode,
      reason: terminalReason,
      error: terminalError,
      columns: terminalColumns,
      rows: terminalRows,
      bytes: terminalBytes,
      updates: terminalUpdates,
      serviceId: terminalServiceId,
    );
  }

  /// Camera sharing (protocol v29). [cameraSupported] mirrors the host's
  /// permission; the viewer watches at most one camera. [cameraPhase] is its
  /// life, [cameraCode]/[cameraReason] carry the host's CameraCode and wording
  /// for a refusal or an ending, and [cameraError] explains a local failure.
  /// [cameraId] names the camera the phase describes; [cameras] is the newest
  /// list and [cameraListUpdates] counts answers so the panel can pair one
  /// list with a poll. The picture arrives on its own Flutter texture; its
  /// encoded size is [cameraWidth] x [cameraHeight] and [cameraFrames] counts
  /// decoded pictures.
  final bool cameraSupported;
  final ViewerCameraPhase cameraPhase;
  final int cameraCode;
  final String cameraReason;
  final String cameraError;
  final int cameraId;
  final List<ViewerCamera> cameras;
  final int cameraListUpdates;
  final int cameraWidth;
  final int cameraHeight;
  final int cameraFrames;
  final String cameraDecoder;
  final int cameraUpdates;

  bool get hasFrame => published > 0 && width > 0 && height > 0;
  bool get hasCameraFrame =>
      cameraFrames > 0 && cameraWidth > 0 && cameraHeight > 0;
  // The camera panel needs a live session and the host's camera bit.
  bool get acceptsCamera => isLive && cameraSupported;
  bool get isLive => state == ViewerState.streaming;
  bool get isOver =>
      state == ViewerState.ended ||
      state == ViewerState.failed ||
      error.isNotEmpty;
  // Pointer and keyboard events can only reach the host while the stream is
  // live and the server announced its input gate as open.
  bool get acceptsInput => inputEnabled && isLive;
  bool get acceptsClipboard =>
      clipboardEnabled && clipboardDirection != 0 && isLive;
  // The upload button needs a live session and the host's file-transfer bit.
  bool get acceptsFiles =>
      isLive && (permissions & ViewerPermission.fileTransfer) != 0;
  // True while an upload is being hashed, offered or streamed. Done and
  // failed are terminal and leave the button usable for another file; a
  // closed session clears the overlay even if the phase went stale.
  bool get fileActive =>
      isLive &&
      (filePhase == ViewerFilePhase.hashing ||
          filePhase == ViewerFilePhase.offered ||
          filePhase == ViewerFilePhase.sending);
  // Streamed fraction of [fileSize], 0 when there is nothing to show.
  double get fileProgress =>
      fileSize <= 0 ? 0.0 : (fileSent / fileSize).clamp(0.0, 1.0);
  // True while the host's file is being received. The receive directory is
  // written as the bytes arrive; a closed session clears the overlay.
  bool get downloadActive => isLive && downloadPhase == ViewerFilePhase.sending;
  double get downloadProgress => downloadSize <= 0
      ? 0.0
      : (downloadReceived / downloadSize).clamp(0.0, 1.0);
  // The speaker button and the overlay's audio row need a live local pipeline.
  bool get acceptsAudio => audioEnabled && isLive;
  // The microphone button needs a live session and the host's microphone bit.
  bool get acceptsMicrophone =>
      isLive && (permissions & ViewerPermission.microphone) != 0;
  // The voice-call button shares the microphone bit: an admitted session with
  // that grant may play the host's microphone.
  bool get acceptsVoiceCall =>
      isLive && (permissions & ViewerPermission.microphone) != 0;
  // The privacy button needs a live session and the host's privacy bit.
  bool get acceptsPrivacy =>
      isLive && (permissions & ViewerPermission.privacyMode) != 0;
  // Host-control menu entries: restart has its own bit, block input needs the
  // explicit bit (the host also requires input), and locking follows input.
  bool get acceptsRestart =>
      isLive && (permissions & ViewerPermission.restart) != 0;
  bool get acceptsBlockInput =>
      isLive && (permissions & ViewerPermission.blockInput) != 0;
  bool get acceptsLockScreen => acceptsInput;
  // OS login needs both the explicit bit and input; the host refuses it
  // outside a logon or lock screen even when the menu is enabled.
  bool get acceptsOsLogin => isLive && osLoginSupported;
  // Session select is mask-only: only the host's service backend grants the
  // bit, so a tray host never shows the switch menu.
  bool get acceptsSessionSelect => isLive && sessionSelectSupported;
  // The port-forward dialog needs a live session and the host's forward bit.
  bool get acceptsPortForward =>
      isLive && (permissions & ViewerPermission.portForward) != 0;
  // The terminal panel needs a live session and the host's terminal bit.
  bool get acceptsTerminal => isLive && terminalSupported;
  // Recording needs a live session and the host's consent bit. A recording
  // already in progress keeps running if the permission is revoked.
  bool get acceptsRecording =>
      isLive && (permissions & ViewerPermission.record) != 0;
  // True while the host captures a secure desktop (lock screen, UAC) or no
  // desktop at all; the picture is frozen and input is not delivered.
  bool get secureDesktop =>
      desktop == ViewerDesktopKind.secure ||
      desktop == ViewerDesktopKind.unavailable;
  // Where the uplink captures from, for the tooltip and the overlay.
  String get microphoneLabel => microphoneName.isNotEmpty
      ? microphoneName
      : (microphoneDevice.isEmpty ? tr('系统默认麦克风') : tr('麦克风'));
  // Where system audio goes, for the volume menu's menu entry.
  String get audioOutputLabel => audioDeviceName.isNotEmpty
      ? audioDeviceName
      : (audioDevice.isEmpty ? tr('系统默认输出') : tr('输出设备'));
  String get codecLabel =>
      codec == 'vp9' ? 'VP9' : codec == 'hevc' ? 'HEVC' : (codec == 'av1' ? 'AV1' : 'H.264');

  /// "UDP", "TCP 回退" or "WebSocket 回退" for the stats overlay.
  String get transportLabel {
    switch (transport) {
      case 'tcp':
        return tr('TCP 回退');
      case 'ws':
        return tr('WebSocket 回退');
      default:
        return 'UDP';
    }
  }

  /// The sample rate as "48 kHz", the way the overlay shows it.
  String get audioSampleRateLabel {
    if (audioSampleRate <= 0) {
      return '';
    }
    final double khz = audioSampleRate / 1000.0;
    return '${khz == khz.roundToDouble() ? khz.toStringAsFixed(0) : khz.toStringAsFixed(1)} kHz';
  }

  String get audioChannelLabel => audioChannels >= 2 ? tr('立体声') : tr('单声道');

  /// The announced display whose rect is the stream's origin and source size,
  /// or null while no list has arrived or when the picture is a merged union.
  int? get streamedDisplayIndex {
    if (mergedLayout) {
      return null;
    }
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
    final int sourceWidth = asInt(map['sourceWidth']) > 0
        ? asInt(map['sourceWidth'])
        : width;
    final int sourceHeight = asInt(map['sourceHeight']) > 0
        ? asInt(map['sourceHeight'])
        : height;
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
    final List<ViewerCamera> cameras = <ViewerCamera>[];
    final Object? rawCameras = map['cameras'];
    if (rawCameras is List) {
      for (final Object? entry in rawCameras) {
        if (entry is Map) {
          final ViewerCamera? camera = ViewerCamera.fromMap(entry);
          if (camera != null) {
            cameras.add(camera);
          }
        }
      }
    }
    final List<ViewerDisplayMode> displayModes = <ViewerDisplayMode>[];
    final Object? rawModes = map['displayModes'];
    if (rawModes is List) {
      for (final Object? entry in rawModes) {
        if (entry is Map) {
          final ViewerDisplayMode? mode = ViewerDisplayMode.fromMap(entry);
          if (mode != null) {
            displayModes.add(mode);
          }
        }
      }
    }
    final List<ViewerPortForwardRule> forwardRules = <ViewerPortForwardRule>[];
    final Object? rawForwardRules = map['portForwardRules'];
    if (rawForwardRules is List) {
      for (final Object? entry in rawForwardRules) {
        if (entry is Map) {
          final ViewerPortForwardRule? rule = ViewerPortForwardRule.fromMap(
            entry,
          );
          if (rule != null) {
            forwardRules.add(rule);
          }
        }
      }
    }
    final List<ViewerRemoteSession> sessions = <ViewerRemoteSession>[];
    final Object? rawSessions = map['sessions'];
    if (rawSessions is List) {
      for (final Object? entry in rawSessions) {
        if (entry is Map) {
          final ViewerRemoteSession? session = ViewerRemoteSession.fromMap(
            entry,
          );
          if (session != null) {
            sessions.add(session);
          }
        }
      }
    }
    return ViewerStatus(
      state: _parseState(map['state']),
      error: asString(map['error']),
      host: asString(map['host']),
      port: asInt(map['port']),
      transport: asString(map['transport']).isEmpty
          ? 'quic'
          : asString(map['transport']),
      transportError: asString(map['transportError']),
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
      auditRef: asString(map['auditRef']),
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
      filesOnly: map['filesOnly'] == true,
      controlOnly: map['controlOnly'] == true,
      sessionMode: asString(map['sessionMode']).isEmpty
          ? 'desktop'
          : asString(map['sessionMode']),
      fullscreen: map['fullscreen'] == true,
      inputEnabled: map['inputEnabled'] == true,
      permissions: asInt(map['permissions']),
      permissionUpdates: asInt(map['permissionUpdates']),
      originX: asInt(map['originX']),
      originY: asInt(map['originY']),
      inputSent: asInt(map['inputSent']),
      inputFailed: asInt(map['inputFailed']),
      clipboardEnabled: map['clipboardEnabled'] == true,
      clipboardDirection: map['clipboardDirection'] is num
          ? asInt(map['clipboardDirection'])
          : 3,
      clipboardSent: asInt(map['clipboardSent']),
      clipboardReceived: asInt(map['clipboardReceived']),
      clipboardFailed: asInt(map['clipboardFailed']),
      clipboardSentFormats: asInt(map['clipboardSentFormats']),
      clipboardReceivedFormats: asInt(map['clipboardReceivedFormats']),
      filePhase: asInt(map['filePhase']),
      fileName: asString(map['fileName']),
      fileRelativePath: asString(map['fileRelativePath']),
      fileQueue: asInt(map['fileQueue']),
      fileSize: asInt(map['fileSize']),
      fileSent: asInt(map['fileSent']),
      fileError: asString(map['fileError']),
      lastFileName: asString(map['lastFileName']),
      lastFileError: asString(map['lastFileError']),
      filesSent: asInt(map['filesSent']),
      fileFailures: asInt(map['fileFailures']),
      fileCancels: asInt(map['fileCancels']),
      downloadPhase: asInt(map['downloadPhase']),
      downloadName: asString(map['downloadName']),
      downloadPath: asString(map['downloadPath']),
      downloadSize: asInt(map['downloadSize']),
      downloadReceived: asInt(map['downloadReceived']),
      downloadError: asString(map['downloadError']),
      filesReceived: asInt(map['filesReceived']),
      fileReceiveFailures: asInt(map['fileReceiveFailures']),
      downloadCancels: asInt(map['downloadCancels']),
      downloadCancellable: map['downloadCancellable'] == true,
      fileListAnswers: asInt(map['fileListAnswers']),
      fileListDirectory: asString(map['fileListDirectory']),
      fileListEntries: asInt(map['fileListEntries']),
      fileListTruncated: map['fileListTruncated'] == true,
      fileListError: asString(map['fileListError']),
      fileDownloadAnswers: asInt(map['fileDownloadAnswers']),
      fileDownloadCode: asInt(map['fileDownloadCode']),
      fileDownloadTransferId: asInt(map['fileDownloadTransferId']),
      fileDownloadPath: asString(map['fileDownloadPath']),
      fileDownloadError: asString(map['fileDownloadError']),
      fileOpAnswers: asInt(map['fileOpAnswers']),
      fileOpCode: asInt(map['fileOpCode']),
      fileOpPath: asString(map['fileOpPath']),
      fileOpError: asString(map['fileOpError']),
      folderDownloadActive: map['folderDownloadActive'] == true,
      folderDownloadName: asString(map['folderDownloadName']),
      folderDownloadDestination: asString(map['folderDownloadDestination']),
      folderDownloadFilesTotal: asInt(map['folderDownloadFilesTotal']),
      folderDownloadFilesDone: asInt(map['folderDownloadFilesDone']),
      folderDownloadCurrent: asString(map['folderDownloadCurrent']),
      folderDownloadRoots: asInt(map['folderDownloadRoots']),
      folderDownloadBatch: map['folderDownloadBatch'] == true,
      folderDownloadAnswers: asInt(map['folderDownloadAnswers']),
      folderDownloadCode: asInt(map['folderDownloadCode']),
      folderDownloadError: asString(map['folderDownloadError']),
      cursorUpdates: asInt(map['cursorUpdates']),
      cursorPending: map['cursorPending'] == true,
      audioAnnounced: map['audioAnnounced'] == true,
      audioEnabled: map['audioEnabled'] == true,
      audioMuted: map['audioMuted'] == true,
      recording: map['recording'] == true,
      recordPath: asString(map['recordPath']),
      recordError: asString(map['recordError']),
      recordSamples: asInt(map['recordSamples']),
      recordKeyframes: asInt(map['recordKeyframes']),
      recordSkipped: asInt(map['recordSkipped']),
      recordSegments: asInt(map['recordSegments']),
      screenshotUpdates: asInt(map['screenshotUpdates']),
      screenshotPath: asString(map['screenshotPath']),
      screenshotError: asString(map['screenshotError']),
      audioSampleRate: asInt(map['audioSampleRate']),
      audioChannels: asInt(map['audioChannels']),
      audioBitrate: asInt(map['audioBitrate']),
      audioReceived: asInt(map['audioReceived']),
      audioDecoded: asInt(map['audioDecoded']),
      audioErrors: asInt(map['audioErrors']),
      audioUnderruns: asInt(map['audioUnderruns']),
      audioStarvedMs: asInt(map['audioStarvedMs']),
      audioDropped: asInt(map['audioDropped']),
      audioRecoveries: asInt(map['audioRecoveries']),
      audioRecovering: map['audioRecovering'] == true,
      audioBufferMs: asInt(map['audioBufferMs']),
      audioLatencyMs: (map['audioLatencyMs'] as num?)?.toDouble() ?? -1.0,
      avSkewMs: (map['avSkewMs'] as num?)?.toDouble() ?? -1.0,
      audioError: asString(map['audioError']),
      audioDevice: asString(map['audioDevice']),
      audioDeviceName: asString(map['audioDeviceName']),
      audioAec: map['audioAec'] != false,
      audioNoiseSuppression: map['audioNoiseSuppression'] != false,
      microphoneAecFrames: asInt(map['microphoneAecFrames']),
      microphoneNsFrames: asInt(map['microphoneNsFrames']),
      microphoneAuthorized: map['microphoneAuthorized'] == true,
      microphoneEnabled: map['microphoneEnabled'] == true,
      microphoneDevice: asString(map['microphoneDevice']),
      microphoneName: asString(map['microphoneName']),
      microphonePackets: asInt(map['microphonePackets']),
      microphoneDropped: asInt(map['microphoneDropped']),
      microphoneErrors: asInt(map['microphoneErrors']),
      microphoneRecoveries: asInt(map['microphoneRecoveries']),
      microphoneRecovering: map['microphoneRecovering'] == true,
      microphoneError: asString(map['microphoneError']),
      voiceAuthorized: map['voiceAuthorized'] == true,
      voiceEnabled: map['voiceEnabled'] == true,
      voicePending: map['voicePending'] == true,
      voicePackets: asInt(map['voicePackets']),
      voiceDropped: asInt(map['voiceDropped']),
      voiceErrors: asInt(map['voiceErrors']),
      voiceError: asString(map['voiceError']),
      privacySupported: map['privacySupported'] == true,
      privacyEnabled: map['privacyEnabled'] == true,
      privacyCode: asInt(map['privacyCode']),
      privacyError: asString(map['privacyError']),
      privacyUpdates: asInt(map['privacyUpdates']),
      displayControlSupported: map['displayControlSupported'] == true,
      displayControlManaged: map['displayControlManaged'] == true,
      displayControlCount: asInt(map['displayControlCount']),
      displayControlPrivacy: asInt(map['displayControlPrivacy']),
      displayControlCode: asInt(map['displayControlCode']),
      displayControlError: asString(map['displayControlError']),
      displayControlUpdates: asInt(map['displayControlUpdates']),
      displayControlBusy: map['displayControlBusy'] == true,
      displayControlOutputs: ViewerControlledDisplay.fromList(
        map['displayControlOutputs'],
      ),
      desktop: ViewerDesktopKind.fromCode(asInt(map['desktop'])),
      desktopUpdates: asInt(map['desktopUpdates']),
      sasCode: asInt(map['sasCode']),
      sasError: asString(map['sasError']),
      sasUpdates: asInt(map['sasUpdates']),
      restartSupported: map['restartSupported'] == true,
      blockInputSupported: map['blockInputSupported'] == true,
      blockInputActive: map['blockInputActive'] == true,
      hostActionAction: asInt(map['hostActionAction']),
      hostActionCode: asInt(map['hostActionCode']),
      hostActionError: asString(map['hostActionError']),
      hostActionUpdates: asInt(map['hostActionUpdates']),
      osLoginSupported: map['osLoginSupported'] == true,
      osLoginCode: asInt(map['osLoginCode']),
      osLoginError: asString(map['osLoginError']),
      osLoginUpdates: asInt(map['osLoginUpdates']),
      elevationCode: asInt(map['elevationCode']),
      elevationRequestId: asInt(map['elevationRequestId']),
      elevationReason: asString(map['elevationReason']),
      elevationUpdates: asInt(map['elevationUpdates']),
      sessionSelectSupported: map['sessionSelectSupported'] == true,
      sessions: List<ViewerRemoteSession>.unmodifiable(sessions),
      sessionListCode: asInt(map['sessionListCode']),
      sessionListError: asString(map['sessionListError']),
      sessionListUpdates: asInt(map['sessionListUpdates']),
      sessionSelectId: asInt(map['sessionSelectId']),
      sessionSelectCode: asInt(map['sessionSelectCode']),
      sessionSelectError: asString(map['sessionSelectError']),
      sessionSelectUpdates: asInt(map['sessionSelectUpdates']),
      chatSent: asInt(map['chatSent']),
      chatReceived: asInt(map['chatReceived']),
      chatFailures: asInt(map['chatFailures']),
      chatError: asString(map['chatError']),
      displaySwitches: asInt(map['displaySwitches']),
      displaySwitchFailures: asInt(map['displaySwitchFailures']),
      displaySwitchError: asString(map['displaySwitchError']),
      displayModes: List<ViewerDisplayMode>.unmodifiable(displayModes),
      displayModesDisplay: asInt(map['displayModesDisplay']),
      displayModesCode: asInt(map['displayModesCode']),
      displayModesError: asString(map['displayModesError']),
      displayModeLists: asInt(map['displayModeLists']),
      displayModeChanges: asInt(map['displayModeChanges']),
      displayModeFailures: asInt(map['displayModeFailures']),
      displayModeError: asString(map['displayModeError']),
      displayFollowFlags: asInt(map['displayFollowFlags']),
      displayFollowCode: asInt(map['displayFollowCode']),
      displayFollowError: asString(map['displayFollowError']),
      displayFollowResults: asInt(map['displayFollowResults']),
      streamQuality: asInt(map['streamQuality']),
      chroma: asInt(map['chroma']),
      streamOptionCode: asInt(map['streamOptionCode']),
      streamOptionError: asString(map['streamOptionError']),
      streamOptionResults: asInt(map['streamOptionResults']),
      remoteConfigSupported: map['remoteConfigSupported'] == true,
      remoteConfigFlags: asInt(map['remoteConfigFlags']),
      remoteConfigCode: asInt(map['remoteConfigCode']),
      remoteConfigError: asString(map['remoteConfigError']),
      remoteConfigUpdates: asInt(map['remoteConfigUpdates']),
      portForwardEnabled: map['portForwardEnabled'] == true,
      portForwardRules: List<ViewerPortForwardRule>.unmodifiable(forwardRules),
      terminalSupported: map['terminalSupported'] == true,
      terminalPersistSupported: map['terminalPersistSupported'] == true,
      terminalServiceId: asInt(map['terminalServiceId']),
      terminalPhase: ViewerTerminalPhase.fromCode(asInt(map['terminalPhase'])),
      terminalCode: asInt(map['terminalCode']),
      terminalReason: asString(map['terminalReason']),
      terminalError: asString(map['terminalError']),
      terminalColumns: asInt(map['terminalColumns']),
      terminalRows: asInt(map['terminalRows']),
      terminalBytes: asInt(map['terminalBytes']),
      terminalUpdates: asInt(map['terminalUpdates']),
      terminals: <ViewerTerminalStatus>[
        for (final Object? item
            in (map['terminals'] as List<Object?>? ?? const <Object?>[]))
          if (item is Map<Object?, Object?>) ViewerTerminalStatus.fromMap(item),
      ],
      cameraSupported: map['cameraSupported'] == true,
      cameraPhase: ViewerCameraPhase.fromCode(asInt(map['cameraPhase'])),
      cameraCode: asInt(map['cameraCode']),
      cameraReason: asString(map['cameraReason']),
      cameraError: asString(map['cameraError']),
      cameraId: asInt(map['cameraId']),
      cameras: List<ViewerCamera>.unmodifiable(cameras),
      cameraListUpdates: asInt(map['cameraListUpdates']),
      cameraWidth: asInt(map['cameraWidth']),
      cameraHeight: asInt(map['cameraHeight']),
      cameraFrames: asInt(map['cameraFrames']),
      cameraDecoder: asString(map['cameraDecoder']),
      cameraUpdates: asInt(map['cameraUpdates']),
      displays: List<ViewerDisplay>.unmodifiable(displays),
      mergedLayout: map['mergedLayout'] == true,
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
  ///
  /// [transport] is `auto` (UDP first, then the TCP and WebSocket carriers),
  /// `quic`, `tcp` or `ws`. [carrierHost]/[carrierPort] name the carrier
  /// endpoint: a direct connection uses host:port+1, a tunnelled one uses the
  /// relay or shared carrier the tunnel agent reported. Empty host means
  /// [host] and port 0 means [port] + 1. [carrierHostId]/[carrierToken] select
  /// this host on a shared carrier; leave them empty for a per-host port.
  ///
  /// [fileTransferOnly] asks the host for a session without a screen stream
  /// (protocol v32); [gpuOutput] is then unused and may be 0. A host that does
  /// not allow file transfer refuses the Hello. [sessionMode] (v41) selects
  /// `terminal`, `camera` or `port_forward` instead; those also need no
  /// [gpuOutput].
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
    String transport = 'auto',
    String carrierHost = '',
    int carrierPort = 0,
    String carrierHostId = '',
    String carrierToken = '',
    bool fileTransferOnly = false,
    String sessionMode = 'desktop',
  }) async {
    await _channel.invokeMethod<void>('start', <String, Object>{
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
      'transport': transport,
      'carrierHost': carrierHost,
      'carrierPort': carrierPort,
      'carrierHostId': carrierHostId,
      'carrierToken': carrierToken,
      'fileTransferOnly': fileTransferOnly,
      'sessionMode': sessionMode,
    });
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

  /// Encrypts [plaintext] with DPAPI under the current Windows user and
  /// returns the base64 blob. Throws [PlatformException] when DPAPI fails.
  static Future<String> protectSecret(String plaintext) async {
    final String? blob = await _channel.invokeMethod<String>(
      'protectSecret',
      plaintext,
    );
    if (blob == null || blob.isEmpty) {
      throw PlatformException(
        code: 'dpapi',
        message: 'the secret was not protected',
      );
    }
    return blob;
  }

  /// Decrypts a [protectSecret] blob. Throws [PlatformException] when the
  /// blob does not belong to this Windows account or was corrupted.
  static Future<String> unprotectSecret(String blob) async {
    final String? plaintext = await _channel.invokeMethod<String>(
      'unprotectSecret',
      blob,
    );
    if (plaintext == null) {
      throw PlatformException(
        code: 'dpapi',
        message: 'the secret could not be read',
      );
    }
    return plaintext;
  }

  /// Opens an http(s) URL in the user's default browser. Used by the SSO
  /// login flow; the native side rejects other schemes.
  static Future<void> openExternalUrl(String url) {
    return _channel.invokeMethod<void>('openExternalUrl', url);
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
    final bool? actual = await _channel.invokeMethod<bool>(
      'setFullscreen',
      enable,
    );
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
    final bool? queued = await _channel
        .invokeMethod<bool>('sendInput', <String, Object>{
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
    bool physical = false,
  }) async {
    final bool? queued = await _channel
        .invokeMethod<bool>('sendInput', <String, Object>{
          'kind': 2,
          'action': down ? 0 : 1,
          'buttons': 0,
          'x': 0,
          'y': 0,
          'hidUsage': usage,
          'physical': physical,
          'modifiers': modifiers,
        });
    return queued ?? false;
  }

  /// Sends one touch contact transition. [contact] is a slot in
  /// 0..[kViewerMaxTouchContacts]-1 and stays the same for the contact's
  /// lifetime; [x]/[y] are virtual-desktop pixels and [pressure] is 0..1024.
  /// The host injects the whole active contact set on every frame, so a
  /// viewer must send Down before Move/Up for the same slot.
  static Future<bool> sendTouch({
    required int action,
    required int contact,
    required int x,
    required int y,
    int pressure = 0,
    int modifiers = 0,
  }) async {
    final bool? queued = await _channel
        .invokeMethod<bool>('sendInput', <String, Object>{
          'kind': 3,
          'action': action,
          'buttons': 0,
          'x': x,
          'y': y,
          'contact': contact,
          'pressure': pressure,
          'modifiers': modifiers,
        });
    return queued ?? false;
  }

  /// Sends a trackpad pan or a complete zoom chord, gated by remote input
  /// permission. Pan deltas are wheel units (120 per notch) on both axes;
  /// zoom uses only [y], and its sign matches the wheel (positive zooms in).
  /// Each call is at most ten notches, which the native side enforces.
  static Future<bool> sendGesture({
    required bool zoom,
    int x = 0,
    int y = 0,
    int modifiers = 0,
  }) async {
    final bool? queued = await _channel
        .invokeMethod<bool>('sendInput', <String, Object>{
          'kind': 5,
          'action': zoom ? 1 : 0,
          'buttons': 0,
          'x': x,
          'y': y,
          'modifiers': modifiers,
        });
    return queued ?? false;
  }

  /// Sends one pen transition. [tiltX]/[tiltY] are degrees from the surface
  /// normal (-90..90, positive right / toward the user), and [flags] is a
  /// combination of [ViewerPenFlag]. Pressure and tilt are stored by the host
  /// until the lift, so Move may carry only the position.
  static Future<bool> sendPen({
    required int action,
    required int x,
    required int y,
    int pressure = 0,
    int flags = 0,
    int tiltX = 0,
    int tiltY = 0,
    int modifiers = 0,
  }) async {
    final bool? queued = await _channel
        .invokeMethod<bool>('sendInput', <String, Object>{
          'kind': 4,
          'action': action,
          'buttons': 0,
          'x': x,
          'y': y,
          'pressure': pressure,
          'flags': flags,
          'tiltX': tiltX,
          'tiltY': tiltY,
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
    final bool? queued = await _channel.invokeMethod<bool>(
      'switchDisplay',
      display,
    );
    return queued ?? false;
  }

  /// Asks the host for [display]'s modes. False means the request was not
  /// queued (no live session or an index outside the announced list). The
  /// answer replaces [ViewerStatus.displayModes] and is counted by
  /// [ViewerStatus.displayModeLists]. Listing needs no input permission.
  static Future<bool> requestDisplayModes(int display) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'requestDisplayModes',
      display,
    );
    return queued ?? false;
  }

  /// Asks the host to change the streamed output's mode for this session. The
  /// host puts the original mode back when the session ends. False means the
  /// request was not queued (no live session, view-only access, or a mode
  /// outside the announced list); the host's answer then arrives in
  /// [ViewerStatus.displayModeChanges] / [ViewerStatus.displayModeError], and
  /// an accepted change is followed by a StreamInfo with the new source size.
  static Future<bool> setDisplayMode({
    required int display,
    required int width,
    required int height,
    required int refresh,
  }) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'setDisplayMode',
      <String, Object>{
        'display': display,
        'width': width,
        'height': height,
        'refresh': refresh,
      },
    );
    return queued ?? false;
  }

  /// Turns automatic display following on or off for this session. The host
  /// moves its own capture to the display holding [cursor] (the host pointer)
  /// or [window] (the foreground window), reusing the normal switch path.
  /// False means the request was not queued (no live session or a view-only
  /// session); the host's answer then arrives in
  /// [ViewerStatus.displayFollowFlags] / [ViewerStatus.displayFollowError].
  static Future<bool> setDisplayFollow({
    required bool cursor,
    required bool window,
  }) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'setDisplayFollow',
      <String, Object>{'cursor': cursor, 'window': window},
    );
    return queued ?? false;
  }

  /// Asks the host to change the stream's encoder, frame-rate cap and quality
  /// preset. [codec] is a [ViewerStreamCodec] value, [maxFps] 0 follows the
  /// host's configured rate, and [quality] is a [ViewerStreamQuality] value.
  /// No input permission is needed. False means the request was not queued
  /// (no session); the host's answer arrives in [ViewerStatus.streamOptionCode]
  /// and reports the values it actually runs in [ViewerStatus.codec],
  /// [ViewerStatus.framerate] and [ViewerStatus.streamQuality].
  static Future<bool> setStreamOptions({
    required int codec,
    required int maxFps,
    required int quality,
    int chroma = 0,
  }) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'setStreamOptions',
      <String, Object>{'codec': codec, 'maxFps': maxFps, 'quality': quality, 'chroma': chroma},
    );
    return queued ?? false;
  }

  /// Asks the host to inject Ctrl+Alt+Del. This is not a key chord: Windows
  /// only accepts the Secure Attention Sequence through `SendSAS`, which the
  /// installed service can call. False means the request was not queued (no
  /// live session or view-only access); the host's answer then arrives in
  /// [ViewerStatus.sasCode] / [ViewerStatus.sasError], counted by
  /// [ViewerStatus.sasUpdates].
  static Future<bool> sendSas() async {
    final bool? queued = await _channel.invokeMethod<bool>('sendSas');
    return queued ?? false;
  }

  /// Queues one host-control action. False means it was not queued (no live
  /// session or the matching permission is missing). The host's answer
  /// arrives in [ViewerStatus.hostActionCode] / [ViewerStatus.hostActionError],
  /// counted by [ViewerStatus.hostActionUpdates]; [ViewerStatus.blockInputActive]
  /// follows the block state the host reports.
  static Future<bool> requestHostAction(
    ViewerHostAction action, {
    bool blockInput = false,
  }) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'requestHostAction',
      <String, Object>{'action': action.code, 'blockInput': blockInput},
    );
    return queued ?? false;
  }

  /// Queues one OS password for the host, which types it at its logon or lock
  /// screen. False means it was not queued: no live session, view-only access,
  /// or the host did not grant both input and OS login. The host re-checks its
  /// state right before typing, so a password sent to an unlocked or UAC
  /// desktop is refused rather than typed. The answer arrives in
  /// [ViewerStatus.osLoginCode] / [ViewerStatus.osLoginError], counted by
  /// [ViewerStatus.osLoginUpdates]. The password is never logged or echoed.
  static Future<bool> requestOsLogin(String password) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'requestOsLogin',
      password,
    );
    return queued ?? false;
  }

  /// Asks the host for its active Windows sessions. False means it was not
  /// queued: no live session, or the host did not grant
  /// [ViewerPermission.sessionSelect] (only its service backend does). The
  /// answer arrives in [ViewerStatus.sessions] /
  /// [ViewerStatus.sessionListCode], counted by
  /// [ViewerStatus.sessionListUpdates].
  static Future<bool> requestElevation() async =>
      await _channel.invokeMethod<bool>('requestElevation') ?? false;

  static Future<bool> requestSessionList() async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'requestSessionList',
    );
    return queued ?? false;
  }

  /// Asks the host to move the stream to Windows session [id]. False means it
  /// was not queued: no live session, view-only access, or the host did not
  /// grant [ViewerPermission.sessionSelect]. The answer arrives in
  /// [ViewerStatus.sessionSelectCode] / [ViewerStatus.sessionSelectError],
  /// counted by [ViewerStatus.sessionSelectUpdates]. An accepted switch ends
  /// this connection, so the caller reconnects and lands in the target.
  static Future<bool> requestSessionSelect(int id) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'requestSessionSelect',
      <String, Object>{'id': id},
    );
    return queued ?? false;
  }

  /// Reads or changes the host's exposed policy switches. [keys] selects the
  /// switches ([ViewerRemoteConfigSwitch] bits) and [values] their new state;
  /// zero [keys] asks for the current snapshot. False means it was not queued:
  /// no live session, the host did not grant
  /// [ViewerPermission.remoteConfig], or a bit is unknown. The host's answer,
  /// including the flags now in effect, arrives in
  /// [ViewerStatus.remoteConfigFlags] / [ViewerStatus.remoteConfigCode],
  /// counted by [ViewerStatus.remoteConfigUpdates].
  static Future<bool> requestRemoteConfig(int keys, int values) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'requestRemoteConfig',
      <String, Object>{'keys': keys, 'values': values},
    );
    return queued ?? false;
  }

  /// Queues one UTF-8 chat line for the host. False means it was not queued:
  /// no streaming session, empty or oversized text (over 4096 bytes), or a
  /// full outgoing queue. The host's lines arrive through [takeChat].
  static Future<bool> sendChat(String text) async {
    final bool? queued = await _channel.invokeMethod<bool>('sendChat', text);
    return queued ?? false;
  }

  /// Queues one block of composed text for the host to type as Unicode, so
  /// the host keyboard layout does not matter. False means it was not queued:
  /// no streaming session, the host did not grant input control, empty or
  /// oversized text (over 4096 bytes), or a full outgoing queue.
  static Future<bool> sendText(String text) async {
    final bool? queued = await _channel.invokeMethod<bool>('sendText', text);
    return queued ?? false;
  }

  /// Pops the oldest line the host sent, or null when none is waiting. The
  /// page drains this on its poll timer, so a burst arrives in order.
  static Future<String?> takeChat() {
    return _channel.invokeMethod<String>('takeChat');
  }

  /// Opens remote console slot [terminalId] (1..[kViewerMaxTerminals]) at
  /// [columns] x [rows] characters (1..500 by 1..200). A non-zero [serviceId]
  /// reattaches to a console the host kept after an earlier disconnect, which
  /// requires the host to have announced terminal persistence. False means it
  /// was not queued: no streaming session, the host did not grant
  /// [ViewerPermission.terminal], that slot already has a console open, or the
  /// previous one's output has not been drained yet. The host's answer arrives
  /// through [ViewerStatus.terminal].
  static Future<bool> openTerminal({
    required int columns,
    required int rows,
    int terminalId = 1,
    int serviceId = 0,
  }) async {
    final bool? opened = await _channel
        .invokeMethod<bool>('openTerminal', <String, Object>{
          'columns': columns,
          'rows': rows,
          'terminalId': terminalId,
          'serviceId': serviceId,
        });
    return opened ?? false;
  }

  /// Queues raw console input for slot [terminalId]. Printable text is UTF-8;
  /// control keys travel as single bytes (for example `\x03` for Ctrl+C).
  /// False means that console is not open or its 256 KiB input queue is full.
  static Future<bool> sendTerminalInput(
    String input, {
    int terminalId = 1,
  }) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'sendTerminalInput',
      <String, Object>{'text': input, 'terminalId': terminalId},
    );
    return queued ?? false;
  }

  /// Asks the host to resize console slot [terminalId]. False means no console
  /// is open or the size is out of range. The host applies it best effort; the
  /// acknowledged size arrives in the slot's columns and rows.
  static Future<bool> resizeTerminal({
    required int columns,
    required int rows,
    int terminalId = 1,
  }) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'resizeTerminal',
      <String, Object>{
        'columns': columns,
        'rows': rows,
        'terminalId': terminalId,
      },
    );
    return queued ?? false;
  }

  /// Ends console slot [terminalId]. The host closes the shell; output already
  /// in flight is still returned by [takeTerminalOutput] until it reports
  /// `ended`.
  static Future<void> closeTerminal({int terminalId = 1}) {
    return _channel.invokeMethod<void>('closeTerminal', <String, Object>{
      'terminalId': terminalId,
    });
  }

  /// Pops the oldest output of console slot [terminalId]. Returns null when
  /// nothing is waiting and the console has not ended; otherwise the bytes and
  /// whether that was the last piece. A UTF-8 sequence may be split across
  /// pieces, so the panel decodes only after it has reassembled them.
  static Future<ViewerTerminalOutput?> takeTerminalOutput({
    int terminalId = 1,
  }) async {
    final Map<Object?, Object?>? reply = await _channel
        .invokeMethod<Map<Object?, Object?>>(
          'takeTerminalOutput',
          <String, Object>{'terminalId': terminalId},
        );
    if (reply == null) {
      return null;
    }
    final Object? data = reply['data'];
    final Uint8List bytes = data is Uint8List ? data : Uint8List(0);
    final bool ended = reply['ended'] == true;
    if (bytes.isEmpty && !ended) {
      return null;
    }
    return ViewerTerminalOutput(data: bytes, ended: ended);
  }

  /// Points the camera decoder at a Flutter texture output, the same opaque
  /// pointer [FlutterGpuTextureRenderer.output] returns for the texture the
  /// camera panel registered. 0 detaches, so decoded pictures are dropped
  /// until a new output is attached.
  static Future<bool> setCameraOutput(int output) async {
    final bool? attached = await _channel.invokeMethod<bool>(
      'setCameraOutput',
      output,
    );
    return attached ?? false;
  }

  /// Asks the host to enumerate its cameras. False means the request was not
  /// queued: no streaming session, the host did not grant
  /// [ViewerPermission.camera], or a camera is starting or running. The answer
  /// arrives through [ViewerStatus.cameraPhase], [ViewerStatus.cameraCode],
  /// [ViewerStatus.cameraReason] and [ViewerStatus.cameras].
  static Future<bool> requestCameraList() async {
    final bool? queued = await _channel.invokeMethod<bool>('requestCameraList');
    return queued ?? false;
  }

  /// Asks the host to open [cameraId] from the newest [ViewerStatus.cameras].
  /// False means the request was not queued: no streaming session, no camera
  /// permission, the id is not in the list, or another camera is still
  /// starting or running. Pictures arrive on the texture registered through
  /// [setCameraOutput].
  static Future<bool> startCamera(int cameraId) async {
    final bool? started = await _channel.invokeMethod<bool>(
      'startCamera',
      <String, Object>{'cameraId': cameraId},
    );
    return started ?? false;
  }

  /// Stops the running camera, or cancels one that is still starting. Safe
  /// when nothing is running.
  static Future<void> stopCamera() {
    return _channel.invokeMethod<void>('stopCamera');
  }

  /// Binds a viewer-side port-forward listener on 127.0.0.1:[localPort]
  /// (0 picks a free port) and asks the host to relay it to
  /// [remoteHost]:[remotePort]. Returns the rule id that names it in
  /// [ViewerStatus.portForwardRules], or 0 when the request was not queued:
  /// no streaming session, or the host did not grant
  /// [ViewerPermission.portForward]. Rules run in parallel, up to the host's
  /// eight-target budget.
  static Future<int> startPortForward({
    required int localPort,
    required String remoteHost,
    required int remotePort,
  }) async {
    final int? ruleId = await _channel.invokeMethod<int>(
      'startPortForward',
      <String, Object>{
        'localPort': localPort,
        'remoteHost': remoteHost,
        'remotePort': remotePort,
      },
    );
    return ruleId ?? 0;
  }

  /// Stops one rule's listener and every relayed connection, and tells the
  /// host to drop the target. Safe when the id is unknown.
  static Future<void> stopPortForward({required int ruleId}) {
    return _channel.invokeMethod<void>('stopPortForward', <String, Object>{
      'ruleId': ruleId,
    });
  }

  /// Enables the native physical keyboard hook and detaches the local IME.
  /// Returns whether the hook owns input; false keeps Flutter's fallback.
  /// Turning this off restores the IME for local text fields.
  static Future<bool> setKeyCapture(bool enable) async {
    final bool? active = await _channel.invokeMethod<bool>(
      'setKeyCapture',
      enable,
    );
    return active ?? false;
  }

  static Future<void> setKeyboardMode(ViewerKeyboardMode mode) =>
      _channel.invokeMethod<void>('setKeyboardMode', mode.name);

  static Future<int> keyboardState({int fallback = 0}) async {
    try {
      return await _channel.invokeMethod<int>('keyboardState') ?? fallback;
    } on PlatformException {
      return fallback;
    } on MissingPluginException {
      return fallback;
    }
  }

  /// Pins the local pointer to the viewer window and turns motion into
  /// relative moves for the host, so a drag never hits a screen edge. [scale]
  /// is physical window pixels per captured pixel. False means native capture
  /// stayed off (no view window); the page then keeps sending absolute moves.
  static Future<bool> setRelativeMouse({
    required bool enabled,
    required double scale,
  }) async {
    final int scaleMilli = (scale * 1000).round().clamp(1, 100000);
    final bool? active = await _channel.invokeMethod<bool>(
      'setRelativeMouse',
      <String, Object>{'enabled': enabled, 'scaleMilli': scaleMilli},
    );
    return active ?? false;
  }

  /// Pauses or resumes the native rich-clipboard bridge. While paused neither
  /// direction touches the local clipboard. The host's own clipboard gate is
  /// applied on top of this switch.
  static Future<void> setClipboardSync(bool enabled) {
    return _channel.invokeMethod<void>('setClipboardSync', enabled);
  }

  /// Starts or stops a local recording of the received stream. Nothing is sent
  /// to the host and nothing is uploaded: the engine muxes the host's
  /// already-encoded stream straight to an MP4. [directory] is used when the
  /// recording starts (empty means the engine's default, the user's Videos
  /// folder). The engine waits for the next key frame and rolls into a new
  /// file when the picture size changes. Progress and failures arrive through
  /// [ViewerStatus.recording], [ViewerStatus.recordPath] and
  /// [ViewerStatus.recordError].
  static Future<void> setRecording(bool enabled, {String directory = ''}) {
    return _channel.invokeMethod<void>('setRecording', <String, Object?>{
      'enabled': enabled,
      'directory': directory,
    });
  }

  /// Queues a screenshot of the current remote picture. The engine reads the
  /// newest decoded frame back from the GPU and writes a PNG on this machine;
  /// nothing is sent to the host. [directory] is the target folder (empty
  /// means the engine's default, `%USERPROFILE%\\Pictures\\chast`). Throws a
  /// [PlatformException] when no picture stream is running. The outcome
  /// arrives through [ViewerStatus.screenshotUpdates],
  /// [ViewerStatus.screenshotPath] and [ViewerStatus.screenshotError].
  static Future<void> takeScreenshot({String directory = ''}) {
    return _channel.invokeMethod<void>('takeScreenshot', <String, Object?>{
      'directory': directory,
    });
  }

  /// Opens the system file picker and queues the chosen files for upload to
  /// the host. Returns how many roots were queued, or null when the dialog was
  /// cancelled. Throws a [PlatformException] when no session is streaming or
  /// the host does not allow file transfer. Progress arrives through
  /// [ViewerStatus.filePhase]; the page polls it with the status timer.
  static Future<int?> sendFile() {
    return _channel.invokeMethod<int>('sendFile');
  }

  /// Queues [paths] for upload the same way [sendFile] does, but without a
  /// picker. Files and folders are both accepted; folders are expanded into
  /// their files on the native side. Returns how many roots were queued.
  static Future<int?> sendFileAt(List<String> paths) {
    return _channel.invokeMethod<int>('sendFileAt', paths);
  }

  /// Opens the system file picker and queues the chosen files for upload into
  /// the shared [directory] being browsed ('' is the shared root). Returns how
  /// many roots were queued, or null when the dialog was cancelled.
  static Future<int?> sendFileTo(String directory) {
    return _channel.invokeMethod<int>('sendFileTo', <String, Object?>{
      'directory': directory,
    });
  }

  /// Queues already-known [paths] for upload into the shared [directory] being
  /// browsed ('' is the shared root). Dropped files use this so they land
  /// beside the picker's uploads instead of always in the receive root.
  static Future<int?> sendFilesTo(List<String> paths, String directory) {
    return _channel.invokeMethod<int>('sendFileAt', <String, Object?>{
      'paths': paths,
      'directory': directory,
    });
  }

  /// Asks the host to list one directory below its shared receive root.
  /// [directory] is '/'-separated and relative ('' is the root). False means
  /// the request was not queued: no streaming session, the host did not grant
  /// [ViewerPermission.fileTransfer], or too many requests are in flight. The
  /// answer arrives through [ViewerStatus.fileListAnswers] and
  /// [takeFileList].
  static Future<bool> requestFileList(
    String directory, [
    String cursor = '',
  ]) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'requestFileList',
      <String, Object>{'directory': directory, 'cursor': cursor},
    );
    return queued ?? false;
  }

  /// Asks the host to send one regular file from its shared root. [path] is
  /// '/'-separated and relative. False means the request was not queued (see
  /// [requestFileList]). On acceptance the host answers through
  /// [ViewerStatus.fileDownloadCode] and then sends the file like any other
  /// download: [ViewerStatus.downloadPhase] and [ViewerStatus.downloadName]
  /// follow it into the viewer's Downloads\chast folder.
  static Future<bool> requestFileDownload(String path) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'requestFileDownload',
      path,
    );
    return queued ?? false;
  }

  /// Asks the host to create a folder, rename one entry or remove one below
  /// its shared root. [path] is '/'-separated and relative; [newName] is one
  /// component and is only used by [ViewerFileOperation.rename]. False means
  /// the request was not queued (see [requestFileList]). The answer arrives
  /// through [ViewerStatus.fileOpAnswers] and [ViewerStatus.fileOpCode];
  /// reload the listing afterwards.
  static Future<bool> fileOperation(
    ViewerFileOperation operation,
    String path, {
    String newName = '',
  }) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'fileOperation',
      <String, Object?>{
        'operation': operation.wireValue,
        'path': path,
        'newName': newName,
      },
    );
    return queued ?? false;
  }

  /// Asks the viewer to download [path] and everything below it into the local
  /// receive directory, mirroring the remote tree under the folder's name.
  /// [path] is '/'-separated, relative and non-empty. False means the walk was
  /// not started (see [requestFileList], or one is already running). Progress
  /// and the outcome arrive through the `folderDownload*` status fields.
  static Future<bool> downloadFolder(String path) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'downloadFolder',
      path,
    );
    return queued ?? false;
  }

  /// Asks the viewer to download several selected entries in one walk: every
  /// folder mirrors below its own name and every file lands at the top of the
  /// receive directory. [entries] must be non-empty; each [ViewerFileSelection]
  /// path is relative and '/'-separated. False means the walk was not started
  /// (see [requestFileList], or one is already running). Progress and the
  /// outcome arrive through the `folderDownload*` status fields, with
  /// [ViewerStatus.folderDownloadRoots] naming the selection size.
  static Future<bool> downloadEntries(List<ViewerFileSelection> entries) async {
    if (entries.isEmpty) {
      return false;
    }
    final bool? queued = await _channel.invokeMethod<bool>(
      'downloadEntries',
      entries
          .map(
            (ViewerFileSelection entry) => <String, Object?>{
              'path': entry.path,
              'directory': entry.directory,
            },
          )
          .toList(),
    );
    return queued ?? false;
  }

  /// Stops the running folder download; the file in flight is aborted and the
  /// outcome is published as cancelled.
  static Future<void> cancelFolderDownload() async {
    await _channel.invokeMethod<void>('cancelFolderDownload');
  }

  /// Stops the single-file download in progress and removes its partial file.
  /// Folder-walk and clipboard transfers keep running; a cancel while nothing
  /// is downloading is a no-op.
  static Future<void> cancelDownload() async {
    await _channel.invokeMethod<void>('cancelDownload');
  }

  /// Drops every ordinary upload still queued or in flight and asks the host
  /// to abort the active transfer. Clipboard uploads keep running; a cancel
  /// while nothing is uploading is a no-op.
  static Future<void> cancelUploads() async {
    await _channel.invokeMethod<void>('cancelUploads');
  }

  /// Drains the finished transfers the native session queued for the audit.
  /// The queue survives status polls, so a file that finished between two
  /// polls is not lost; call this after every poll and once more after the
  /// session stops. Returns an empty list when nothing finished.
  static Future<List<ViewerCompletedFile>> takeCompletedFiles() async {
    final Object? result = await _channel.invokeMethod<Object?>(
      'takeCompletedFiles',
    );
    if (result is! List) {
      return const <ViewerCompletedFile>[];
    }
    final List<ViewerCompletedFile> files = <ViewerCompletedFile>[];
    for (final Object? entry in result) {
      if (entry is Map) {
        final ViewerCompletedFile? file = ViewerCompletedFile.fromMap(
          Map<Object?, Object?>.from(entry),
        );
        if (file != null) {
          files.add(file);
        }
      }
    }
    return files;
  }

  /// Pops the newest directory listing the host sent. Returns null when none
  /// is waiting; a listing for an older request is superseded by a newer one.
  static Future<ViewerFileListing?> takeFileList() async {
    final Map<Object?, Object?>? map = await _channel
        .invokeMethod<Map<Object?, Object?>>('takeFileList');
    if (map == null) {
      return null;
    }
    final List<ViewerFileEntry> entries = <ViewerFileEntry>[];
    final Object? raw = map['entries'];
    if (raw is List) {
      for (final Object? item in raw) {
        if (item is Map) {
          final ViewerFileEntry? entry = ViewerFileEntry.fromMap(
            Map<Object?, Object?>.from(item),
          );
          if (entry != null) {
            entries.add(entry);
          }
        }
      }
    }
    return ViewerFileListing(
      directory: map['directory']?.toString() ?? '',
      code: (map['code'] as num?)?.toInt() ?? 0,
      reason: map['reason']?.toString() ?? '',
      truncated: map['truncated'] == true,
      cursor: map['cursor']?.toString() ?? '',
      nextCursor: map['nextCursor']?.toString() ?? '',
      entries: entries,
    );
  }

  /// Installs the handler for files dropped on the viewer window. Only one
  /// page listens at a time; passing null detaches it.
  static void setDropHandler(void Function(List<String> paths)? handler) {
    _dropHandler = handler;
    _installMethodHandler();
  }

  static void setKeyboardEscapeHandler(void Function()? handler) {
    _keyboardEscapeHandler = handler;
    _installMethodHandler();
  }

  static void setKeyboardTextRejectedHandler(void Function()? handler) {
    _keyboardTextRejectedHandler = handler;
    _installMethodHandler();
  }

  /// Routes the native window's close button through [handler] so a session
  /// window can shut its session down before the window disappears. Passing
  /// null restores the default close behavior.
  static Future<void> setWindowCloseHandler(
    Future<void> Function()? handler,
  ) async {
    _windowCloseHandler = handler;
    _installMethodHandler();
    try {
      await _channel.invokeMethod<void>(
        'setWindowCloseHandler',
        handler != null,
      );
    } on MissingPluginException {
      // Widget tests and non-Windows embedders do not have native windows.
    }
  }

  static void _installMethodHandler() {
    if (_dropHandler == null &&
        _windowCloseHandler == null &&
        _keyboardTextRejectedHandler == null &&
        _keyboardEscapeHandler == null) {
      _channel.setMethodCallHandler(null);
      return;
    }
    _channel.setMethodCallHandler((MethodCall call) async {
      if (call.method == 'keyboardTextRejected') {
        _keyboardTextRejectedHandler?.call();
        return;
      }
      if (call.method == 'keyboardEscape') {
        _keyboardEscapeHandler?.call();
        return;
      }
      if (call.method == 'windowCloseRequested') {
        await _windowCloseHandler?.call();
        return;
      }
      if (call.method == 'droppedFiles' && call.arguments is List) {
        final List<String> paths = (call.arguments as List)
            .whereType<String>()
            .where((String path) => path.isNotEmpty)
            .toList(growable: false);
        if (paths.isNotEmpty) {
          _dropHandler?.call(paths);
        }
      }
    });
  }

  static void Function(List<String> paths)? _dropHandler;
  static Future<void> Function()? _windowCloseHandler;
  static void Function()? _keyboardEscapeHandler;
  static void Function()? _keyboardTextRejectedHandler;

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
    final bool? muted = await _channel.invokeMethod<bool>(
      'setAudioMuted',
      mute,
    );
    return muted ?? false;
  }

  /// Switches the microphone uplink on or off. The native side opens the
  /// capture device and the CMIC stream; switching off releases them.
  /// Returns false when the host did not grant the microphone, so the caller
  /// can explain the tap.
  static Future<bool> setMicrophone(bool enabled) async {
    final bool? applied = await _channel.invokeMethod<bool>(
      'setMicrophone',
      enabled,
    );
    return applied ?? false;
  }

  /// Starts or ends a voice call that plays the host's microphone. The
  /// answer, packets and a host-initiated close arrive through
  /// [ViewerStatus.voiceEnabled] / [ViewerStatus.voiceError]. Returns false
  /// when the host did not grant the microphone.
  static Future<bool> setVoiceCall(bool enabled) async {
    final bool? applied = await _channel.invokeMethod<bool>(
      'setVoiceCall',
      enabled,
    );
    return applied ?? false;
  }

  /// Asks the host to mask its own screen while the stream continues. The
  /// answer arrives through [ViewerStatus.privacyEnabled]; returns false when
  /// the host did not grant the privacy bit.
  static Future<bool> setPrivacyMode(bool enabled) async {
    final bool? applied = await _channel.invokeMethod<bool>(
      'setPrivacyMode',
      enabled,
    );
    return applied ?? false;
  }

  /// Sends one display-management request (protocol v47). [action] is a
  /// [ViewerDisplayControlAction] value, [value] the count, toggle state or
  /// privacy implementation, and [target] the host-issued output id for a
  /// toggle. The answer arrives in [ViewerStatus.displayControl].
  static Future<bool> controlDisplays(
    int action,
    int value, {
    int target = 0,
  }) async {
    final bool? queued = await _channel.invokeMethod<bool>(
      'controlDisplays',
      <String, int>{
        'action': action,
        'value': value,
        if (target != 0) 'target': target,
      },
    );
    return queued ?? false;
  }

  /// Pins the capture endpoint; an empty id follows the default
  /// communications device. An open uplink is reopened on the new device.
  static Future<bool> selectMicrophone(String deviceId) async {
    final bool? applied = await _channel.invokeMethod<bool>(
      'selectMicrophone',
      deviceId,
    );
    return applied ?? false;
  }

  /// Lists the machine's active capture endpoints, default first.
  static Future<List<ViewerMicrophoneDevice>> listMicrophones() async {
    final Object? result = await _channel.invokeMethod<Object?>(
      'listMicrophones',
    );
    if (result is! List) {
      return const <ViewerMicrophoneDevice>[];
    }
    final List<ViewerMicrophoneDevice> devices = <ViewerMicrophoneDevice>[];
    for (final Object? entry in result) {
      if (entry is Map) {
        final ViewerMicrophoneDevice? device = ViewerMicrophoneDevice.fromMap(
          entry,
        );
        if (device != null) {
          devices.add(device);
        }
      }
    }
    return List<ViewerMicrophoneDevice>.unmodifiable(devices);
  }

  /// Pins the playback endpoint; an empty id follows the Windows default
  /// output. A live player is rebuilt with the same decoder and clock offset.
  static Future<bool> selectAudioOutput(String deviceId) async {
    final bool? applied = await _channel.invokeMethod<bool>(
      'selectAudioOutput',
      deviceId,
    );
    return applied ?? false;
  }

  /// Switches microphone echo cancellation and noise suppression. The native
  /// side reopens the capture on its next poll; the choices are kept.
  static Future<bool> setAudioProcessing({
    required bool aec,
    required bool noiseSuppression,
  }) async {
    final bool? applied = await _channel.invokeMethod<bool>(
      'setAudioProcessing',
      <String, bool>{'aec': aec, 'noiseSuppression': noiseSuppression},
    );
    return applied ?? false;
  }

  /// Lists the machine's active playback endpoints, default first.
  static Future<List<ViewerAudioOutput>> listAudioOutputs() async {
    final Object? result = await _channel.invokeMethod<Object?>(
      'listAudioOutputs',
    );
    if (result is! List) {
      return const <ViewerAudioOutput>[];
    }
    final List<ViewerAudioOutput> devices = <ViewerAudioOutput>[];
    for (final Object? entry in result) {
      if (entry is Map) {
        final ViewerAudioOutput? device = ViewerAudioOutput.fromMap(entry);
        if (device != null) {
          devices.add(device);
        }
      }
    }
    return List<ViewerAudioOutput>.unmodifiable(devices);
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

/// Touch and pen transitions share one action space in the protocol.
abstract final class ViewerTouchAction {
  static const int down = 0;
  static const int move = 1;
  static const int up = 2;
  static const int cancel = 3;
}

/// Pen button/eraser flags, matching Windows' PEN_FLAG_* values.
abstract final class ViewerPenFlag {
  static const int barrel = 1;
  static const int inverted = 2;
  static const int eraser = 4;
}

/// Simultaneous contacts both the protocol and the host injector accept
/// (chast::kMaxTouchContacts).
const int kViewerMaxTouchContacts = 10;

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
/// Right Alt. The native hook removes the synthetic Ctrl using its scan code;
/// this Flutter fallback preserves the keys it actually receives.
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

/// Hello display index meaning "merge every display into one picture"
/// (chast::kDisplayAll in chast/protocol.h).
const int kViewerDisplayAll = 0xFFFE;

/// Bits in [ViewerStatus.displayFollowFlags] (chast::kDisplayFollow* in
/// chast/protocol.h): which automatic display switches the host performs.
abstract final class ViewerDisplayFollow {
  static const int cursor = 1;
  static const int window = 2;
}

/// The permission mask a host announces (chast::Permission in
/// chast/protocol.h). Input covers pointer and keyboard injection; clipboard
/// covers both directions; audio is granted only while an audio stream is open.
abstract final class ViewerPermission {
  static const int input = 0x0001;
  static const int clipboard = 0x0002;
  static const int audio = 0x0004;
  static const int fileTransfer = 0x0008;
  static const int microphone = 0x0010;
  static const int privacyMode = 0x0020;
  static const int portForward = 0x0080;
  static const int terminal = 0x0100;
  // The host's consent to a local recording of the received stream; advisory,
  // because the host cannot see or stop the recording itself.
  static const int record = 0x0200;
  static const int restart = 0x0800;
  static const int blockInput = 0x1000;
  // The host lets the viewer type its OS password at a logon or lock screen.
  // The host also requires [input] and re-checks its desktop per request.
  static const int osLogin = 0x2000;
  // The host can list and switch the Windows session it streams from. Only
  // the service backend grants the bit; a tray host never sends it.
  static const int sessionSelect = 0x4000;
  // The host lets this viewer read and change the policy switches the host
  // owner exposed. The host re-checks its own switches on every request.
  static const int remoteConfig = 0x8000;
}

/// Bits of the host-settings snapshot exchanged with a remote-config request
/// (chast::kRemoteConfig* in chast/protocol.h).
abstract final class ViewerRemoteConfigSwitch {
  static const int lockAfterSessionEnd = 1 << 0;
  static const int clipboard = 1 << 1;
  static const int fileTransfer = 1 << 2;
  static const int audio = 1 << 3;
  static const int microphone = 1 << 4;
  static const int privacyMode = 1 << 5;
  static const int restart = 1 << 6;
  static const int blockInput = 1 << 7;
  static const int osLogin = 1 << 8;
  static const int sessionSelect = 1 << 9;
  static const int terminal = 1 << 10;
  static const int portForward = 1 << 11;
  static const int camera = 1 << 12;
  static const int record = 1 << 13;
  static const int all = 0x3FFF;
}

/// Why a remote-configuration request was refused (chast::RemoteConfigCode).
enum ViewerRemoteConfigCode {
  ok(0),
  denied(1),
  unsupported(2),
  busy(3),
  failed(4);

  const ViewerRemoteConfigCode(this.code);
  final int code;
}

/// Why a session list was not returned (chast::SessionListCode).
enum ViewerSessionListCode {
  ok(0),
  denied(1),
  unsupported(2),
  failed(3);

  const ViewerSessionListCode(this.code);
  final int code;
}

/// Why a session switch was not performed (chast::SessionSelectCode).
enum ViewerSessionSelectCode {
  ok(0),
  notFound(1),
  denied(2),
  unsupported(3),
  busy(4),
  failed(5);

  const ViewerSessionSelectCode(this.code);
  final int code;
}

/// Why Ctrl+Alt+Del did not reach the host's secure desktop
/// (chast::SasCode in chast/protocol.h).
enum ViewerSasCode {
  ok(0),
  denied(1),
  unsupported(2),
  failed(3);

  const ViewerSasCode(this.code);
  final int code;
}

/// One host-control action (chast::HostAction in chast/protocol.h).
enum ViewerHostAction {
  restart(1),
  lockScreen(2),
  blockInput(3);

  const ViewerHostAction(this.code);
  final int code;
}

/// Why a host-control action was not performed (chast::HostActionCode).
enum ViewerHostActionCode {
  ok(0),
  denied(1),
  unsupported(2),
  busy(3),
  failed(4);

  const ViewerHostActionCode(this.code);
  final int code;
}

/// Why a display-management request was not applied
/// (chast::DisplayControlCode in chast/protocol.h).
enum ViewerDisplayControlCode {
  ok(0),
  denied(1),
  unsupported(2),
  busy(3),
  failed(4);

  const ViewerDisplayControlCode(this.code);
  final int code;
}

/// One display-management request kind (chast::DisplayControlAction).
enum ViewerDisplayControlAction {
  query(0),
  setCount(1),
  restore(2),
  selectPrivacy(3),
  toggleOutput(4);

  const ViewerDisplayControlAction(this.code);
  final int code;
}

/// Encoder preference for a session (chast::StreamCodecPreference in
/// chast/protocol.h). Auto keeps the host's tier ladder, which prefers HEVC
/// when the GPU supports it.
enum ViewerStreamCodec {
  auto(0),
  h264(1),
  hevc(2),
  av1(3),
  vp9(4);

  const ViewerStreamCodec(this.code);
  final int code;

  String get label {
    switch (this) {
      case ViewerStreamCodec.auto:
        return tr('自动');
      case ViewerStreamCodec.h264:
        return 'H.264';
      case ViewerStreamCodec.hevc:
        return 'HEVC';
      case ViewerStreamCodec.av1:
        return 'AV1';
      case ViewerStreamCodec.vp9:
        return 'VP9';
    }
  }
}

/// Quality preset (chast::StreamQuality). It scales the host's video bitrate
/// budget; adaptive bitrate can still go below the preset when the link
/// degrades.
enum ViewerStreamQuality {
  high(0),
  balanced(1),
  smooth(2);

  const ViewerStreamQuality(this.code);
  final int code;

  String get label {
    switch (this) {
      case ViewerStreamQuality.high:
        return tr('高清');
      case ViewerStreamQuality.balanced:
        return tr('均衡');
      case ViewerStreamQuality.smooth:
        return tr('流畅');
    }
  }
}

/// Why the host refused new stream preferences (chast::StreamOptionsCode).
enum ViewerStreamOptionsCode {
  ok(0),
  unsupported(1),
  failed(2),
  busy(3);

  const ViewerStreamOptionsCode(this.code);
  final int code;
}

/// Which desktop the host is capturing (chast::DesktopKind in
/// chast/protocol.h). A secure desktop or an unreachable one pauses the
/// picture until the user desktop comes back.
enum ViewerDesktopKind {
  defaultDesktop(0),
  secure(1),
  unavailable(2);

  const ViewerDesktopKind(this.code);

  final int code;

  static ViewerDesktopKind fromCode(int code) {
    for (final ViewerDesktopKind kind in values) {
      if (kind.code == code) {
        return kind;
      }
    }
    return defaultDesktop;
  }
}

/// The viewer's upload state (chast_viewer::FilePhase in
/// app/windows/viewer/viewer_engine.h).
abstract final class ViewerFilePhase {
  static const int idle = 0;
  static const int hashing = 1;
  static const int offered = 2;
  static const int sending = 3;
  static const int done = 4;
  static const int failed = 5;
}

/// One bit per clipboard format in [ViewerStatus.clipboardSentFormats] and
/// [ViewerStatus.clipboardReceivedFormats] (chast::ClipboardFormat).
abstract final class ViewerClipboardFormat {
  static const int text = 1 << 0;
  static const int html = 1 << 1;
  static const int rtf = 1 << 2;
  static const int dib = 1 << 3;

  /// "文本 + 图片" style summary; empty when [mask] is 0.
  static String describe(int mask) {
    final List<String> names = <String>[
      if ((mask & text) != 0) tr('文本'),
      if ((mask & html) != 0) 'HTML',
      if ((mask & rtf) != 0) 'RTF',
      if ((mask & dib) != 0) tr('图片'),
    ];
    return names.join(' + ');
  }
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
      return tr('访问令牌不正确。请向主机确认当前令牌后重试。');
    case ViewerDenial.tokenUsed:
      return tr('该一次性令牌已被使用，请让主机重新生成一个。');
    case ViewerDenial.declined:
      return tr('主机拒绝了本次连接请求，请让主机确认后重试。');
    case ViewerDenial.wrongPassword:
      return tr('密码不正确，请重试。');
    case ViewerDenial.passwordRequired:
      return tr('主机要求输入连接密码。');
    case ViewerDenial.twoFactorRequired:
      return tr('主机要求两步验证码。');
    case ViewerDenial.wrongTwoFactor:
      return tr('两步验证码不正确或已过期，请重新输入。');
    case ViewerDenial.lockedOut:
      return tr('尝试次数过多，主机已暂时拒绝此地址。请稍后再试。');
    case ViewerDenial.notWhitelisted:
      return tr('本机地址不在主机的允许列表中。');
    case ViewerDenial.authTimeout:
      return tr('主机未在限定时间内完成验证，请重试。');
    default:
      return '';
  }
}

/// Flutter's Windows embedder turns one wheel notch (WHEEL_DELTA) into this
/// many logical pixels of PointerScrollEvent.scrollDelta. The viewer converts
/// back with this constant so a notch on the client is a notch on the host.
const double kViewerScrollPixelsPerNotch = 53.0;
