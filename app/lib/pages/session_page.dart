import 'dart:async';
import 'dart:collection';
import 'dart:io';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_gpu_texture_renderer/flutter_gpu_texture_renderer.dart';

import '../account_store.dart';
import '../display_stream_bridge.dart';
import '../display_stream_controller.dart';
import '../server_api.dart';
import '../server_identity.dart';
import '../server_settings.dart';
import '../session_window.dart';
import '../stream_prefs.dart';
import '../target.dart';
import '../theme.dart';
import '../receiver_status_events.dart';
import '../reconnect_policy.dart';
import '../rdp.dart';
import '../recording_retention.dart';
import '../remote_key_sequence.dart';
import '../remote_pointer.dart';
import '../trust_store.dart';
import '../tunnel_agent.dart';
import '../view_prefs.dart';
import '../viewer_bridge.dart';
import '../widgets/camera_panel.dart';
import '../widgets/display_stream_view.dart';
import '../widgets/chat_panel.dart';
import '../widgets/file_browser_panel.dart';
import '../widgets/remote_toolbar.dart';
import '../widgets/send_text_dialog.dart';
import '../widgets/remote_view.dart';
import '../widgets/stats_overlay.dart';
import '../widgets/terminal_tabs.dart';
import 'package:chast_viewer/l10n.dart';

enum _DisplayStreamLayout { single, grid, desktop }

class SessionPage extends StatefulWidget {
  const SessionPage({
    super.key,
    required this.target,
    this.account,
    this.settings = const ServerSettings(),
    this.identityStore,
    this.trustStore,
    this.fileTransferOnly = false,
    this.sessionMode = 'desktop',
    this.rdpPort = 0,
    this.windowSession = false,
    this.initialDisplay = 65535,
  });

  final ConnectTarget target;

  /// Ask the host for a session without a screen stream (protocol v32): the
  /// page shows only the shared-folder browser. The host refuses the Hello
  /// when it does not allow file transfer, and the page reports that refusal
  /// like any other.
  final bool fileTransferOnly;

  /// The connection type requested from the host (protocol v41): 'desktop',
  /// 'file_transfer', 'terminal', 'camera' or 'port_forward'. The standalone
  /// modes carry no screen stream and show their own panel instead of the
  /// remote-screen stack.
  final String sessionMode;

  /// Positive when this page is the RDP shortcut: it runs a port-forward
  /// session and, once the viewer listener is up, forwards the host's
  /// 127.0.0.1:[rdpPort] and opens the system RDP client against it.
  final int rdpPort;

  /// True when this page owns its native window (started through
  /// [SessionWindowRequest.open]). Leaving then closes the window instead of
  /// popping a route, and the window's close button asks the page to shut the
  /// session down first.
  final bool windowSession;

  /// The output this window should show from the start; [kViewerDisplayAuto]
  /// follows the host's choice.
  final int initialDisplay;

  /// The signed-in account, if any. It is used to report a finished remote
  /// session and, when enabled, to upload finished recordings.
  final AccountSession? account;

  /// Connection-service settings captured when the page opened. Only the
  /// recording upload switch is read here; the server URL comes from
  /// [account] so a settings edit elsewhere cannot split the two.
  final ServerSettings settings;

  /// Server identity pins; tests inject a file-backed store.
  final ServerIdentityStore? identityStore;

  /// Host certificate pins; tests inject an isolated file-backed store.
  final TrustStore? trustStore;

  @override
  State<SessionPage> createState() => _SessionPageState();
}

class _SessionPageState extends State<SessionPage> with WidgetsBindingObserver {
  static const Duration _pollInterval = Duration(milliseconds: 200);
  static const Duration _hideDelay = Duration(seconds: 5);

  final FocusNode _focus = FocusNode(debugLabel: 'session');
  final FlutterGpuTextureRenderer _gpuTexture = FlutterGpuTextureRenderer();
  late final TrustStore _trustStore = widget.trustStore ?? TrustStore();
  late final ServerIdentityStore _identityStore =
      widget.identityStore ?? ServerIdentityStore();

  Timer? _pollTimer;
  int _pollEpoch = 0;
  Timer? _hideTimer;
  // Managed-host access tickets (v62). The signed ticket is fetched from the
  // server before Start and refreshed every 20 seconds, because the host
  // enforces the signed 60-second deadline on its own clock.
  static const Duration _ticketRenewInterval = Duration(seconds: 20);
  Timer? _ticketTimer;
  bool _ticketRenewBusy = false;
  String _ticketPublicKey = '';
  // The account this page sees. A token refreshed here is written back to the
  // shared store so ticket renewal and audit reports survive the 30-minute
  // access-token lifetime.
  AccountSession? _account;
  Future<AccountSession?>? _accountRefresh;
  ViewerStatus _status = const ViewerStatus();
  // Local clipboard switch, independent of the host's gate: turning it off
  // stops both directions without asking the host to change permissions. The
  // native side owns the actual read/write; this only pauses or resumes it.
  bool _clipboardSync = true;
  // One picker at a time; the native side also refuses an upload while the
  // previous one is active, this just keeps the dialog from stacking.
  bool _filePickBusy = false;
  // Recording upload. The engine names a new file when recording starts and
  // rolls to another one when the frame size changes, so a path is finished
  // either when it stops being the newest path or when recording stops. Paths
  // are only tracked while the engine reports recording, which keeps a stale
  // path from a previous session out of the queue.
  final Set<String> _recordedPaths = <String>{};
  final Map<String, DateTime> _recordStartedAt = <String, DateTime>{};
  final List<_FinishedRecording> _recordingUploads = <_FinishedRecording>[];
  bool _uploadingRecordings = false;
  // An upload holds its file open, so retention must not delete it.
  String _activeUploadPath = '';
  // Recording policy. Auto-record starts once per live session when the host
  // grants recording; a manual stop suppresses it until the next session, and
  // a refused start is not retried in the same one. Retention runs after a
  // recording completes; 0 keeps everything.
  bool _autoRecord = false;
  int _recordKeep = 0;
  bool _autoRecordAttempted = false;
  bool _autoRecordSuppressed = false;
  bool _wasLive = false;
  String _recordErrorSeen = '';
  // Session chat. The page owns the transcript and the native transport; the
  // panel is a modeless overlay. While its input holds focus, physical keys
  // belong to the text field, so remote forwarding and the native key-capture
  // switch pause until focus leaves.
  final List<ChatEntry> _chatEntries = <ChatEntry>[];
  final FocusNode _chatInputFocus = FocusNode(debugLabel: 'chat-input');
  bool _chatOpen = false;
  bool _chatTyping = false;
  bool _chatDraining = false;
  // Remote console. Like chat it is a modeless overlay; while its focus node
  // owns the keyboard every key goes to the host's console instead of the
  // remote desktop. The panel itself owns the screen model and the output
  // poll; the page only decides when it is on screen.
  final FocusNode _terminalFocus = FocusNode(debugLabel: 'remote-terminal');
  bool _terminalOpen = false;
  bool _terminalTyping = false;
  // Protocol v42: the host keeps a console across a disconnect. Service ids
  // are remembered per slot here (the engine's status restarts at 0 with the
  // new session) and handed back to a rebuilt tab so it attaches instead of
  // starting a fresh shell. Only a persistent console has one.
  final List<int> _terminalServiceIds = List<int>.filled(
    kViewerMaxTerminals,
    0,
  );
  // Console slots the open panel shows. The tabs widget owns the choices; the
  // page keeps the list so a reconnect can restore exactly those consoles
  // that outlived the session and close-all can stop every one of them.
  List<int> _terminalTabIds = <int>[1];
  // Host camera preview. The native side decodes into a second plugin
  // texture, so the panel only owns that texture's life; opening registers
  // it and attaches the output, closing detaches and unregisters. The panel
  // never takes the keyboard, so remote input keeps flowing underneath.
  bool _cameraOpen = false;
  bool _cameraOpening = false;
  int _cameraTextureId = -1;
  // A standalone camera session opens the panel and starts a camera on its
  // own: there is no remote picture to click through first.
  bool _cameraAutoStarted = false;
  // The standalone port-forward session raises its setup dialog once when the
  // session turns live; closing it leaves the status card behind.
  bool _portForwardPrompted = false;
  // RDP shortcut: the forward starts once, then the client opens as soon as
  // the engine publishes the listener's bound port.
  int _rdpRuleId = 0;
  bool _rdpStarting = false;
  bool _rdpLaunched = false;
  // Host shared-folder browser. Modeless like chat; it never takes the
  // keyboard, so remote input keeps flowing while it is open.
  bool _fileBrowserOpen = false;
  // Same-name prompt. The native side holds the offer until the dialog
  // answers; the answered key (direction and transfer id) is kept briefly so
  // a poll still carrying the old prompt cannot reopen it.
  bool _conflictDialogOpen = false;
  String _answeredConflictKey = '';
  DateTime? _answeredConflictAt;
  // The directory the open browser last listed; dropped files follow it so
  // they land where the user is looking instead of the receive root.
  String _fileBrowserDirectory = '';
  // The ordering the open browser was last left in, remembered for the next
  // open during this session.
  FileBrowserSort _fileBrowserSort = FileBrowserSort.name;
  bool _fileBrowserSortAscending = true;
  // Whether the browser shows entries the host marked hidden; remembered per
  // device like the sort, matching RustDesk's remote "show hidden" option.
  bool _fileBrowserShowHidden = false;
  int _terminalFontSize = 13;
  // The last port-forward failure already shown, so a stable error text does
  // not pop a snack bar on every poll. Cleared when the engine clears it.
  String _portForwardErrorSeen = '';
  // Mirrors the engine's rule list so the open port-forward dialog updates
  // while it is up; the page itself rebuilds from _status.
  final ValueNotifier<List<ViewerPortForwardRule>> _portForwardRules =
      ValueNotifier<List<ViewerPortForwardRule>>(
        const <ViewerPortForwardRule>[],
      );
  String? _startError;
  int _textureId = -1;
  DisplayStreamController? _displayStreams;
  bool _openingDisplayWindow = false;
  List<int>? _displaySelection;
  _DisplayStreamLayout? _displayStreamLayout;
  bool get _independentDisplays => _displayStreams?.state.active == true;
  Future<void>? _teardown;
  Future<void>? _starting;
  Future<void>? _shutdownFuture;
  bool _polling = false;
  bool _sawFrame = false;
  bool _over = false;
  bool _collapsed = false;
  bool _pinned = false;
  bool _statsVisible = false;
  bool _fullscreen = false;
  bool _reconnecting = false;
  bool _closing = false;
  bool _controlEnabled = false;
  bool _nativeKeyCapture = false;
  bool _localKeyboardUnavailable = false;
  ViewerKeyboardMode _keyboardMode = ViewerKeyboardMode.physical;
  bool _keyboardChanging = false;
  int _keyCaptureGeneration = 0;
  // Relative mode pins the local pointer to the window and sends host motion
  // itself; it is session-only and starts off.
  bool _relativeMouse = false;
  bool _swallowEscapeChordUp = false;
  // An accepted Windows session switch ends this connection on purpose; the
  // page reconnects through the backoff ladder instead of showing a drop.
  bool _sessionSwitchPending = false;
  bool _elevationSubmitting = false;
  bool _elevationReconnectPending = false;
  double _wheelY = 0;
  double _wheelX = 0;
  int _heldButtons = 0;
  // Physical button -> host button at press time. Release must lift the same
  // host button even if "swap left and right" is toggled while it is held.
  final Map<int, int> _remoteButtons = <int, int>{};
  MouseMapping _mouse = const MouseMapping();
  final LinkedHashSet<int> _heldKeys = LinkedHashSet<int>();
  // Touch contacts on the host, keyed by the pointer that owns each slot.
  final RemoteContactTracker _contacts = RemoteContactTracker();
  // The pointer currently drawing with the pen, or null when the pen is up.
  int? _penPointer;
  ViewFit _fit = ViewFit.adaptive;
  // What the next connection asks the server for. Kept across reconnects so
  // "重新连接" returns to the same output; kViewerDisplayAuto leaves the
  // choice to the server. An accepted in-session switch updates it too.
  int _display = kViewerDisplayAuto;
  // The output an in-session switch is waiting on, or null when none is. The
  // toolbar marks it while the host works; a refusal drops it and leaves
  // [_display] pointing at the output that is still streaming.
  int? _pendingDisplay;
  // The output chosen by the host when this connection started in automatic
  // mode. Keep it stable while local multi-display layouts change focus.
  int? _serverDefaultDisplay;
  // Resolution menu. The mode list is fetched once the stream is live for the
  // displayed output and again after an accepted change, because the host's
  // current-mode marker moves with it.
  int? _modesRequestedFor;
  int _modesRequestedLists = 0;
  DateTime _modesRequestedAt = DateTime.fromMillisecondsSinceEpoch(0);
  bool _modesRefreshPending = false;
  // Picture preferences, remembered across launches like RustDesk's image
  // quality. A fresh host session starts with its own defaults, so the page
  // seeds these from ui.json and re-sends them after every start. A change is
  // optimistic: a refusal restores the previous selection and explains itself
  // in a snack bar.
  ViewerStreamCodec _streamCodec = ViewerStreamCodec.auto;
  bool _streamFullColor = false;
  ViewerStreamQuality _streamQuality = ViewerStreamQuality.high;
  // RustDesk's custom image quality in percent. Kept while a preset is
  // selected so the custom entry reopens with the last number.
  int _streamQualityPercent = 50;
  int _streamMaxFps = 0;
  // Answers already consumed and requests still in flight, so a stale answer
  // cannot overwrite a newer menu choice.
  int _streamOptionSeen = 0;
  int _streamPending = 0;
  _StreamOptionSnapshot? _streamRollback;
  // Logical pixels per captured pixel, as the frame is currently drawn. The
  // host's pointer is scaled by this times the window's device pixel ratio.
  double _viewScale = 1.0;
  // Remote-cursor state as last handed to the native side. The sprite never
  // comes back here; only "is one installed" does, and that is what decides
  // whether the local pointer is hidden.
  bool _cursorSynced = false;
  bool _cursorWanted = false;
  bool _cursorInstalled = false;
  int _cursorSeen = -1;
  double _cursorScale = 0;
  bool _cursorBusy = false;
  bool _cursorDirty = false;
  // View-only pointer: the host's sprite drawn over the picture because no
  // local cursor is installed to stand in for it. The image is decoded once
  // per shape; positions arrive at RustDesk's 33 ms pointer cadence.
  Timer? _remoteCursorTimer;
  bool _remoteCursorBusy = false;
  final ValueNotifier<RemoteCursorView?> _remoteCursor =
      ValueNotifier<RemoteCursorView?>(null);
  ui.Image? _remoteCursorImage;
  int? _remoteCursorImageShape;
  // The identity this page pins for the current connection. Empty only until
  // the first contact, whose certificate is then confirmed and remembered.
  String _pinnedFingerprint = '';
  // First-use confirmation runs once per connection attempt that has no
  // stored pin. `_fingerprintUnpersisted` stays set when the user accepted a
  // fingerprint but the trust store could not save it, so the UI can say the
  // connection is not actually pinned.
  bool _fingerprintPrompted = false;
  bool _fingerprintUnpersisted = false;
  // Interactive authentication. The password is loaded from this account's
  // DPAPI store once, then kept for reconnects; the code is cleared whenever
  // the password changes because the host checks them in that order.
  bool _credentialsLoaded = false;
  bool _rememberPassword = false;
  bool _trustDevice = false;
  String _password = '';
  String _totp = '';
  bool _authPromptOpen = false;
  int _challengeDenial = 0;
  // One tunnel per connection attempt. Reconnects must look up the host again:
  // a running helper can still hold a dead NAT mapping or an expired token.
  TunnelAgent? _tunnel;
  Future<TunnelAgent>? _tunnelStarting;
  // Explicit management override for a restricted device. The administrator
  // token is collected on demand and kept only in memory; the tunnel agent
  // re-signs the override on transport refresh, so this mode never does an
  // ordinary viewer lookup and every reconnect is a fresh audited request.
  String _emergencyAdmin = '';
  String _emergencyReason = '';
  StreamSubscription<TunnelAccessTicket>? _emergencyTickets;

  bool get _emergencyActive => _emergencyAdmin.isNotEmpty;
  // Automatic reconnect after an unexpected drop. The policy holds the
  // attempt count and backoff; [_autoTimer] is the pending delay. The series
  // starts only after a session has shown frames, so a refused password or a
  // changed identity never spins here; it survives attempts that fail before
  // their first frame.
  final AutoReconnectPolicy _autoPolicy = AutoReconnectPolicy();
  Timer? _autoTimer;
  // The first frame marks a real remote session; a terminal state reports it
  // once to the signed-in account's server. Handshake failures stay out of
  // this trail because no session existed yet.
  DateTime? _auditStartedAt;
  // One file-audit drain at a time; a poll that arrives during a slow post
  // leaves the native queue untouched for the next tick.
  bool _fileReportBusy = false;

  bool get _inputActive =>
      !_keyboardChanging &&
      _controlEnabled &&
      _status.acceptsInput &&
      _startError == null;

  // "Show my cursor": the viewer pointer drawn on the host desktop. It is a
  // draw-only hint, so it keeps working while remote control is off.
  bool _showMyCursor = false;
  bool _cursorHintOnHost = false;
  int _cursorHintSentMs = 0;
  // The hint is throttled rather than queued: only the latest position can
  // still be where the pointer is. ~30 updates per second is smooth enough
  // and keeps a fast mouse from flooding the control stream.
  static const int _cursorHintIntervalMs = 33;

  String get _prefsPeerId => widget.target.preferencesKey;

  /// Physical cursor pixels per captured pixel. The native side needs this to
  /// size the host's sprite the way the frame is being drawn, so a stream
  /// shown at half size gets a half-size pointer.
  double get _cursorTargetScale =>
      _viewScale * (MediaQuery.maybeDevicePixelRatioOf(context) ?? 1.0);

  @override
  void initState() {
    super.initState();
    _account = widget.account;
    _display = widget.initialDisplay;
    WidgetsBinding.instance.addObserver(this);
    _chatInputFocus.addListener(_onChatFocusChanged);
    _terminalFocus.addListener(_onTerminalFocusChanged);
    _focus.addListener(_onKeyboardFocusChanged);
    ViewerBridge.setKeyboardEscapeHandler(() {
      if (mounted && _relativeMouse) setState(_disableRelativeMouse);
    });
    ViewerBridge.setDropHandler(_sendDroppedFiles);
    if (widget.windowSession) {
      unawaited(ViewerBridge.setWindowCloseHandler(_closeNativeWindow));
    }
    final StreamPreferences prefs = StreamPreferences.load(
      peerId: _prefsPeerId,
    );
    _streamCodec = _streamCodecFor(prefs.codec);
    _streamFullColor = prefs.fullColor;
    _streamQuality = _streamQualityFor(prefs.quality);
    _streamQualityPercent = prefs.qualityPercent;
    _streamMaxFps = prefs.maxFps;
    ViewerBridge.setKeyboardTextRejectedHandler(() {
      if (mounted && !_closing) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text(tr('文字未发送，请检查控制权限或连接状态'))));
      }
    });
    final ViewPreferences viewPrefs = ViewPreferences.load(
      peerId: _prefsPeerId,
    );
    _keyboardMode = viewerKeyboardModeFromName(viewPrefs.keyboardMode);
    _autoRecord = viewPrefs.autoRecord;
    _recordKeep = viewPrefs.recordKeep;
    _fit = viewPrefs.fit == 1 ? ViewFit.original : ViewFit.adaptive;
    _statsVisible = viewPrefs.stats;
    _fileBrowserSort = _fileSortFor(viewPrefs.fileSort);
    _showMyCursor = viewPrefs.showMyCursor;
    _fileBrowserSortAscending = viewPrefs.fileSortAscending;
    _fileBrowserDirectory = viewPrefs.browserDirectory;
    _fileBrowserShowHidden = viewPrefs.browserShowHidden;
    _terminalFontSize = viewPrefs.terminalFontSize;
    _mouse = MouseMapping(
      reverseWheel: viewPrefs.reverseWheel,
      swapButtons: viewPrefs.swapButtons,
    );
    _starting = _start();
    unawaited(_starting);
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _closing = true;
    ViewerBridge.setDropHandler(null);
    ViewerBridge.setKeyboardEscapeHandler(null);
    ViewerBridge.setKeyboardTextRejectedHandler(null);
    if (widget.windowSession) {
      unawaited(ViewerBridge.setWindowCloseHandler(null));
    }
    _stopPolling();
    _ticketTimer?.cancel();
    _ticketTimer = null;
    _hideTimer?.cancel();
    _autoTimer?.cancel();
    _chatInputFocus.removeListener(_onChatFocusChanged);
    _chatInputFocus.dispose();
    _terminalFocus.removeListener(_onTerminalFocusChanged);
    _terminalFocus.dispose();
    _focus.removeListener(_onKeyboardFocusChanged);
    _setKeyCapture(false);
    _focus.dispose();
    _portForwardRules.dispose();
    _clearCursor();
    _remoteCursorTimer?.cancel();
    _remoteCursorTimer = null;
    _remoteCursorImage?.dispose();
    _remoteCursorImage = null;
    _remoteCursor.dispose();
    final DateTime? auditStartedAt = _auditStartedAt;
    _auditStartedAt = null;
    if (auditStartedAt != null) {
      unawaited(_reportFinishedSession(auditStartedAt, 'closed'));
    }
    unawaited(_shutdown());
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      _onKeyboardFocusChanged();
      return;
    }
    if (!_controlEnabled) {
      return;
    }
    // Switching away means the key-ups for anything still held will never
    // reach the page; lift them here and tell the host to drop whatever it
    // still holds. Control stays on for when the window comes back.
    _releaseRemoteInput();
    _setKeyCapture(false);
  }

  Future<void> _reportFinishedSession(DateTime startedAt, String detail) async {
    final AccountSession? account = await _activeAccount();
    if (account == null || !account.isActiveAt(DateTime.now())) {
      return;
    }
    final String peer = widget.target.hostId.isNotEmpty
        ? widget.target.hostId
        : widget.target.label;
    try {
      await ServerApi.reportConnection(
        ServerSettings(url: account.server),
        account.token,
        peerId: peer,
        startedAt: startedAt,
        endedAt: DateTime.now(),
        detail: detail,
        auditRef: _status.auditRef,
        route: _connectionRoute,
      );
    } catch (_) {
      // Audit delivery must not disturb the session or the returned page.
    }
  }

  /// Posts every file the native side finished since the last drain. The
  /// engine queues one row per completed transfer, so the audit can be
  /// checked file by file instead of inferred from session counters.
  /// Delivery is best effort: a failed batch is dropped, exactly like the
  /// session report, and the next poll drains whatever is still queued.
  Future<void> _reportCompletedFiles() async {
    if (_fileReportBusy) {
      return;
    }
    _fileReportBusy = true;
    try {
      final List<ViewerCompletedFile> files =
          await ViewerBridge.takeCompletedFiles();
      if (files.isEmpty) {
        return;
      }
      final AccountSession? account = await _activeAccount();
      if (account == null || !account.isActiveAt(DateTime.now())) {
        return;
      }
      final String peer = widget.target.hostId.isNotEmpty
          ? widget.target.hostId
          : widget.target.label;
      final String auditRef = _status.auditRef;
      final String route = _connectionRoute;
      final ServerSettings settings = ServerSettings(url: account.server);
      // The server accepts at most 200 rows per call.
      for (int start = 0; start < files.length; start += 200) {
        final int end = start + 200 < files.length ? start + 200 : files.length;
        final List<FileTransferReport> items = <FileTransferReport>[
          for (final ViewerCompletedFile file in files.sublist(start, end))
            FileTransferReport(
              direction: file.upload ? 'upload' : 'download',
              name: file.name,
              size: file.size,
              success: file.success,
              detail: file.detail,
              occurredAt: file.occurredAtMs > 0
                  ? DateTime.fromMillisecondsSinceEpoch(
                      file.occurredAtMs,
                      isUtc: true,
                    )
                  : null,
            ),
        ];
        try {
          await ServerApi.reportFileTransfersBatch(
            settings,
            account.token,
            peerId: peer,
            items: items,
            auditRef: auditRef,
            route: route,
          );
        } catch (_) {
          // Keep draining: one lost batch must not hold back the rest.
        }
      }
    } catch (_) {
      // The channel may be gone during teardown; auditing never disturbs it.
    } finally {
      _fileReportBusy = false;
    }
  }

  /// Watches the engine's recording path. A path leaves the tracked set when
  /// recording stops or when the engine rolls to a new segment, and only then
  /// is it a complete MP4.
  void _trackRecording(ViewerStatus status) {
    final String current = status.recordPath.trim();
    if (status.recording && current.isNotEmpty) {
      _recordedPaths.add(current);
      _recordStartedAt.putIfAbsent(current, () => DateTime.now());
    }
    final List<String> finished = <String>[
      for (final String path in _recordedPaths)
        if (!status.recording || path != current) path,
    ];
    for (final String path in finished) {
      _recordedPaths.remove(path);
      _queueRecordingUpload(path, _recordStartedAt.remove(path));
    }
    if (finished.isNotEmpty && !status.recording && _recordKeep > 0) {
      unawaited(_pruneRecordings(finished.last));
    }
  }

  /// Applies the opt-in retention policy after a recording ends. Files still
  /// open or waiting to upload are protected, and only viewer-named recordings
  /// are considered.
  Future<void> _pruneRecordings(String completedPath) async {
    if (_recordKeep < 1) {
      return;
    }
    final String normalized = completedPath.replaceAll('/', '\\');
    final int slash = normalized.lastIndexOf('\\');
    if (slash <= 0) {
      return;
    }
    final String directory = normalized.substring(0, slash);
    final Set<String> protect = <String>{
      ..._recordedPaths,
      for (final _FinishedRecording job in _recordingUploads) job.path,
      if (_activeUploadPath.isNotEmpty) _activeUploadPath,
    };
    try {
      final int removed = await pruneRecordings(
        directory,
        _recordKeep,
        protect: protect,
      );
      if (removed > 0) {
        debugPrint('viewer: pruned $removed old recording file(s)');
      }
    } catch (_) {
      // Retention is best effort; a locked file is retried next time.
    }
  }

  /// Auto-record and the recording failure prompt. The engine reports the
  /// newest failure until a recording starts successfully, so only a new text
  /// is shown. Auto-record fires once per live span, after the first picture
  /// and only with the host's recording consent.
  void _watchRecording(ViewerStatus status) {
    final bool live = status.isLive;
    if (live && !_wasLive) {
      _autoRecordAttempted = false;
      _autoRecordSuppressed = false;
    }
    _wasLive = live;
    final String error = status.recordError.trim();
    if (error.isNotEmpty && error != _recordErrorSeen) {
      _recordErrorSeen = error;
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              tr('录制失败：{error}', <String, Object?>{'error': error}),
            ),
            duration: const Duration(seconds: 5),
          ),
        );
      }
    } else if (error.isEmpty) {
      _recordErrorSeen = '';
    }
    if (_autoRecord &&
        live &&
        !_reconnecting &&
        !status.recording &&
        !_autoRecordAttempted &&
        !_autoRecordSuppressed &&
        status.hasFrame &&
        status.acceptsRecording) {
      _autoRecordAttempted = true;
      unawaited(_applyRecording(true));
    }
  }

  /// Same-name prompts for file transfers (protocol v69). The engine holds
  /// the offer until an answer; a remembered policy answers later conflicts
  /// itself, so only ask while none is set. Responses are remembered briefly
  /// by direction and transfer id because a poll can still carry the prompt
  /// that was just answered.
  void _watchFileConflict(ViewerStatus status) {
    if (!status.fileConflictPending || _conflictDialogOpen) {
      return;
    }
    final String key =
        '${status.fileConflictDownload ? 1 : 0}:${status.fileConflictTransferId}';
    final DateTime? answeredAt = _answeredConflictAt;
    if (key == _answeredConflictKey &&
        answeredAt != null &&
        DateTime.now().difference(answeredAt) < const Duration(seconds: 2)) {
      return;
    }
    _conflictDialogOpen = true;
    unawaited(_showFileConflictDialog(status, key));
  }

  Future<void> _showFileConflictDialog(ViewerStatus status, String key) async {
    final _FileConflictAnswer? answer = await showDialog<_FileConflictAnswer>(
      context: context,
      barrierDismissible: false,
      builder: (BuildContext context) => _FileConflictDialog(status: status),
    );
    _conflictDialogOpen = false;
    if (answer == null) {
      return;
    }
    _answeredConflictKey = key;
    _answeredConflictAt = DateTime.now();
    try {
      await ViewerBridge.answerFileConflict(
        answer.decision,
        remember: answer.remember,
      );
    } on MissingPluginException {
      // The engine went away with the session; nothing is left to answer.
    } on PlatformException {
      // Treated as stale; the next poll asks again if the prompt remains.
    }
  }

  void _setAutoRecord(bool enabled) {
    setState(() => _autoRecord = enabled);
    ViewPreferences.update(autoRecord: enabled);
    if (!enabled) {
      return;
    }
    _autoRecordSuppressed = false;
    if (_status.acceptsRecording && !_status.recording && _status.hasFrame) {
      _autoRecordAttempted = true;
      unawaited(_applyRecording(true));
    }
  }

  void _setRecordKeep(int keep) {
    setState(() => _recordKeep = keep);
    ViewPreferences.update(recordKeep: keep);
  }

  void _queueRecordingUpload(String path, DateTime? startedAt) {
    final AccountSession? account = _account;
    if (account == null ||
        !account.isActiveAt(DateTime.now()) ||
        !widget.settings.uploadRecordings) {
      return;
    }
    _recordingUploads.add(_FinishedRecording(path: path, startedAt: startedAt));
    unawaited(_processRecordingUploads());
  }

  Future<void> _processRecordingUploads() async {
    if (_uploadingRecordings) {
      return;
    }
    _uploadingRecordings = true;
    try {
      while (_recordingUploads.isNotEmpty) {
        final _FinishedRecording job = _recordingUploads.removeAt(0);
        final AccountSession? account = await _activeAccount();
        if (account == null ||
            !account.isActiveAt(DateTime.now()) ||
            !widget.settings.uploadRecordings) {
          continue;
        }
        final File file = File(job.path);
        try {
          if (!await file.exists() || await file.length() <= 0) {
            continue;
          }
          final String name = _recordingFileName(job.path);
          if (mounted) {
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(
                content: Text(
                  tr('正在上传录制：{name}', <String, Object?>{'name': name}),
                ),
                duration: const Duration(seconds: 2),
              ),
            );
          }
          final DateTime endedAt = DateTime.now();
          final DateTime startedAt = job.startedAt ?? endedAt;
          _activeUploadPath = job.path;
          await ServerApi.uploadRecording(
            ServerSettings(url: account.server),
            account.token,
            file: file,
            name: name,
            peerId: widget.target.hostId.isNotEmpty
                ? widget.target.hostId
                : widget.target.label,
            startedAtMs: startedAt.toUtc().millisecondsSinceEpoch,
            durationMs: endedAt.difference(startedAt).inMilliseconds,
          );
          if (mounted) {
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(
                content: Text(
                  tr('录制已上传：{name}', <String, Object?>{'name': name}),
                ),
                duration: const Duration(seconds: 3),
              ),
            );
          }
        } on ServerApiException catch (error) {
          if (mounted) {
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(
                content: Text(
                  tr('录制上传失败：{error}', <String, Object?>{
                    'error': error.message,
                  }),
                ),
                duration: const Duration(seconds: 4),
              ),
            );
          }
        } catch (error) {
          if (mounted) {
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(
                content: Text(
                  tr('录制上传失败：{error}', <String, Object?>{'error': error}),
                ),
                duration: const Duration(seconds: 4),
              ),
            );
          }
        } finally {
          _activeUploadPath = '';
        }
      }
    } finally {
      _uploadingRecordings = false;
    }
  }

  static String _recordingFileName(String path) {
    final String normalized = path.replaceAll('\\', '/');
    final int slash = normalized.lastIndexOf('/');
    final String name = slash < 0
        ? normalized
        : normalized.substring(slash + 1);
    return name.trim().isEmpty ? 'recording.mp4' : name.trim();
  }

  Future<void> _start() async {
    for (int attempt = 0; ; ++attempt) {
      try {
        await _startOnce();
        return;
      } on MissingPluginException {
        if (attempt > 0 || !mounted) {
          _cancelAutoReconnect();
          if (mounted) {
            setState(() => _startError = tr('原生查看器尚未加载'));
          }
          return;
        }
        // The method channels can appear a beat after the first frame.
        await Future<void>.delayed(const Duration(milliseconds: 300));
        if (!mounted) {
          return;
        }
      } on PlatformException catch (error) {
        _cancelAutoReconnect();
        if (mounted) {
          setState(() => _startError = error.message ?? error.code);
        }
        return;
      } on ServerApiException catch (error) {
        _cancelAutoReconnect();
        if (mounted) {
          setState(
            () => _startError = tr('无法获取访问票据：{p1}', <String, Object?>{
              'p1': error.message,
            }),
          );
        }
        return;
      } on TunnelException catch (error) {
        if (mounted) {
          // A refused override ends this attempt; forget the token so the
          // overlay offers a fresh administrator login instead of retrying
          // credentials the server already rejected.
          final bool dropEmergency = _emergencyActive && !error.retryable;
          setState(() {
            _startError = tr('无法通过连接服务打开设备：{p1}', <String, Object?>{
              'p1': error.message,
            });
            if (dropEmergency) {
              _emergencyAdmin = '';
              _emergencyReason = '';
            }
          });
          final Duration? delay = _autoPolicy.afterStartupFailure(
            retryable: error.retryable && !_closing && !_identityChanged,
          );
          _scheduleAutoReconnect(delay);
        }
        return;
      }
    }
  }

  /// The signed-in account token is handed to the tunnel only when it belongs
  /// to the target deployment. A restricted managed device admits no shared
  /// link key, so this is what makes lookup succeed; a token from another
  /// server would authenticate against the wrong account namespace.
  Future<String> _tunnelAccountToken(ServerSettings settings) async {
    final AccountSession? account = await _activeAccount();
    if (account == null || !account.isActiveAt(DateTime.now().toUtc())) {
      return '';
    }
    final String accountServer = normalizeServerUrl(account.server);
    if (accountServer.isEmpty ||
        accountServer != normalizeServerUrl(settings.url)) {
      return '';
    }
    return account.token;
  }

  /// Reuses the agent within an attempt. _reconnect retires it before starting
  /// the next attempt so addresses, tokens and path selection are refreshed.
  Future<TunnelAgent?> _ensureTunnel() async {
    if (!widget.target.viaServer) {
      return null;
    }
    final TunnelAgent? existing = _tunnel;
    if (existing != null) {
      if (existing.running) {
        return existing;
      }
      // The agent died while the session was up; a reconnect needs a fresh
      // process and a fresh loopback port.
      _tunnel = null;
      _tunnelStarting = null;
      unawaited(existing.stop());
    }
    final ServerSettings settings = ServerSettings(
      url: widget.target.server,
      mode: widget.target.mode,
      key: widget.target.serverKey,
      linkKey: widget.target.linkKey,
      proxy: widget.settings.proxy,
    );
    // Server identity, audit reports and recording uploads use the same
    // outbound path as the tunnel agent; an invalid value fails those calls
    // instead of letting them dial directly.
    ServerApi.proxy = widget.settings.proxy;
    _tunnelStarting ??= () async {
      final String serverKey = await _confirmedServerKey(settings);
      final String accountToken = await _tunnelAccountToken(settings);
      EmergencyAccessRequest? emergency;
      if (_emergencyActive) {
        // The ticket is bound to the application client identity, while the
        // tunnel contributes its own ephemeral transport key. Both must come
        // from this process, so the request is assembled here rather than
        // carried in from the dialog.
        final String clientKey = await ViewerBridge.ticketIdentity();
        if (clientKey.isEmpty) {
          throw TunnelException(tr('无法读取查看器票据身份'));
        }
        emergency = EmergencyAccessRequest(
          adminToken: _emergencyAdmin,
          reason: _emergencyReason,
          clientPublicKey: clientKey,
        );
      }
      return TunnelAgent.start(
        server: settings,
        hostId: widget.target.hostId,
        accountToken: accountToken,
        serverPublicKey: serverKey,
        emergency: emergency,
      );
    }();
    try {
      final TunnelAgent agent = await _tunnelStarting!;
      _tunnel = agent;
      _tunnelStarting = null;
      _watchEmergencyTickets(agent);
      return agent;
    } catch (_) {
      _tunnelStarting = null;
      rethrow;
    }
  }

  /// Follows the tickets the agent re-signs while an override is active. The
  /// ordinary 20-second lookup renewal does not apply here: only the agent
  /// holds the transport key the override was minted against, so it is the
  /// only party that can request the next ticket.
  void _watchEmergencyTickets(TunnelAgent agent) {
    unawaited(_emergencyTickets?.cancel());
    _emergencyTickets = null;
    if (!_emergencyActive) {
      return;
    }
    _emergencyTickets = agent.accessTickets.listen((TunnelAccessTicket ticket) {
      if (!mounted || _closing || _reconnecting || _tunnel != agent) {
        return;
      }
      unawaited(_renewEmergencyTicket(ticket.ticket));
    });
  }

  Future<void> _renewEmergencyTicket(String ticket) async {
    if (_ticketRenewBusy) {
      return;
    }
    _ticketRenewBusy = true;
    try {
      await ViewerBridge.renewAccessTicket(ticket);
    } on MissingPluginException {
      // Reported by the start path.
    } on PlatformException {
      // A rejected renewal leaves the previous lease in force; the host
      // closes the session at its deadline rather than widening access.
    } finally {
      _ticketRenewBusy = false;
    }
  }

  /// Confirms the server key before the agent is allowed to look up the host
  /// or ask for a punch. The first contact shows the fingerprint and pins it;
  /// a later key change is refused until the user explicitly accepts it.
  /// The returned key is also handed to `chast-tunnel viewer --server-pubkey`,
  /// so a substituted lookup cannot be used even outside this page.
  Future<String> _confirmedServerKey(ServerSettings settings) async {
    final ServerIdentity live;
    try {
      live = await ServerApi.serverIdentity(settings);
    } on ServerApiException catch (error) {
      throw TunnelException(
        tr('无法读取服务器身份：{p2}', <String, Object?>{'p2': error.message}),
        retryable: error.retryable,
      );
    }
    final ServerIdentityPin? pinned = await _identityStore.pinFor(settings.url);
    if (pinned != null && pinned.identity.publicKey == live.publicKey) {
      return live.publicKey;
    }
    if (!mounted) {
      throw TunnelException(tr('服务器身份未确认'));
    }
    final bool? accepted = await showDialog<bool>(
      context: context,
      barrierDismissible: false,
      builder: (BuildContext context) {
        return _ServerIdentityDialog(
          server: settings.url,
          identity: live,
          previous: pinned?.identity,
        );
      },
    );
    if (accepted != true) {
      throw TunnelException(tr('服务器身份未确认，已取消连接'));
    }
    await _identityStore.pin(settings.url, live, previous: pinned);
    return live.publicKey;
  }

  Future<void> _startOnce() async {
    final TunnelAgent? tunnel = await _ensureTunnel();
    if (!mounted) {
      await _stopSession();
      return;
    }
    // A files-only session has no picture, so it neither registers a texture
    // nor needs the GPU sink; the native side skips the decoder entirely.
    int id = -1;
    int output = 0;
    if (!widget.fileTransferOnly) {
      id = await _gpuTexture.registerTexture() ?? -1;
      if (id >= 0) {
        try {
          output = await _gpuTexture.output(id) ?? 0;
        } catch (_) {
          await _gpuTexture.unregisterTexture(id);
          rethrow;
        }
      }
      if (id < 0 || output == 0) {
        if (id >= 0) {
          await _gpuTexture.unregisterTexture(id);
        }
        if (mounted) {
          setState(() => _startError = tr('无法创建视频纹理'));
        }
        return;
      }
    }
    _textureId = id;
    final String storedPin = await _trustStore.fingerprintFor(
      widget.target.trustKey,
    );
    if (!mounted) {
      await _stopSession();
      return;
    }
    // An accepted fingerprint that could not be persisted still pins this
    // process's later reconnects, so it is not silently downgraded to an
    // unverified handshake.
    final String pin = storedPin.isNotEmpty
        ? storedPin
        : (_fingerprintUnpersisted ? _pinnedFingerprint : '');
    _pinnedFingerprint = pin;
    _fingerprintPrompted = pin.isNotEmpty;
    if (!_credentialsLoaded) {
      _credentialsLoaded = true;
      try {
        final String? saved = await ViewerBridge.loadPassword(
          widget.target.trustKey,
        );
        if (saved != null && saved.isNotEmpty) {
          _password = saved;
          _rememberPassword = true;
        }
      } on PlatformException {
        // A store that cannot be read just means the host asks for a password.
      } on MissingPluginException {
        // The native side is not up yet; the first start reports it.
      }
      if (!mounted) {
        await _stopSession();
        return;
      }
    }
    try {
      // A managed host refuses a Hello without a ticket, so the lookup is
      // part of starting the session rather than an optional add-on. An
      // unmanaged host answers without one and keeps local authentication.
      final String accessTicket = await _fetchAccessTicket();
      // Direct connections fall back to the host's adjacent TCP port; a
      // tunnelled session prefers the server's shared carrier, which
      // multiplexes every host on one port and needs this host's token.
      final String carrierHost;
      final int carrierPort;
      final String carrierHostId;
      final String carrierToken;
      final bool proxied;
      try {
        proxied = effectiveProxyUrl(widget.settings.proxy).isNotEmpty;
      } on FormatException catch (error) {
        throw TunnelException(error.message);
      }
      if (tunnel != null && proxied) {
        // The agent already carries every datagram through the proxy. A
        // direct carrier dial from the engine would bypass it, so leave the
        // external carrier empty; the engine then only retries loopback.
        carrierHost = '';
        carrierPort = 0;
        carrierHostId = '';
        carrierToken = '';
      } else if (tunnel != null && tunnel.carrierPort != 0) {
        carrierHost = tunnel.carrierHost;
        carrierPort = tunnel.carrierPort;
        carrierHostId = widget.target.hostId;
        carrierToken = tunnel.carrierToken;
      } else if (tunnel != null) {
        carrierHost = tunnel.relayHost;
        carrierPort = tunnel.relayPort;
        carrierHostId = '';
        carrierToken = '';
      } else {
        carrierHost = widget.target.host;
        carrierPort = widget.target.port < 65535 ? widget.target.port + 1 : 0;
        carrierHostId = '';
        carrierToken = '';
      }
      await ViewerBridge.start(
        host: tunnel?.host ?? widget.target.host,
        port: tunnel?.port ?? widget.target.port,
        gpuOutput: output,
        display: widget.fileTransferOnly || widget.sessionMode != 'desktop'
            ? kViewerDisplayAuto
            : _display,
        fingerprint: pin,
        allowUnverified: pin.isEmpty,
        accessToken: widget.target.token,
        password: _password,
        totp: _totp,
        trustDevice: _trustDevice,
        // With an outbound proxy the agent already forces the carrier, and the
        // engine's only carrier endpoint is the agent's loopback, so the
        // WebSocket preference cannot apply there.
        transport: widget.settings.useWebSocket && !proxied ? 'ws' : 'auto',
        carrierHost: carrierHost,
        carrierPort: carrierPort,
        carrierHostId: carrierHostId,
        carrierToken: carrierToken,
        fileTransferOnly: widget.fileTransferOnly,
        sessionMode: widget.sessionMode,
        accessTicket: accessTicket,
      );
      _startTicketRenewal(accessTicket);
      // The native session starts with clipboard sync on; reapply this page's
      // switch before the first poll can show it as enabled.
      try {
        await ViewerBridge.setClipboardSync(_clipboardSync);
      } on PlatformException {
        // The session is already gone; start would have thrown otherwise.
      } on MissingPluginException {
        throw StateError('the viewer is not available');
      }
      // Same for the picture preferences: a new host session starts with its
      // defaults. Queue the page's choice before the first poll so the menus
      // do not briefly show the host's defaults.
      if (_streamCodec != ViewerStreamCodec.auto ||
          _streamQuality != ViewerStreamQuality.high ||
          _streamMaxFps != 0 ||
          _streamFullColor) {
        ++_streamPending;
        try {
          if (!await ViewerBridge.setStreamOptions(
            codec: _streamCodec.code,
            maxFps: _streamMaxFps,
            quality: _streamQuality.code,
            qualityPercent: _streamQuality == ViewerStreamQuality.custom
                ? _streamQualityPercent
                : 0,
            chroma: _streamFullColor ? 1 : 0,
          )) {
            --_streamPending;
          }
        } on PlatformException {
          --_streamPending;
          // The session is already gone; start would have thrown otherwise.
        } on MissingPluginException {
          --_streamPending;
          throw StateError('the viewer is not available');
        }
      }
    } catch (_) {
      await _stopSession();
      _teardown = null;
      rethrow;
    }
    if (!mounted || _closing) {
      await _stopSession();
      return;
    }
    setState(() => _startError = null);
    _startPolling();
  }

  /// Stops the native session, then releases the plugin texture. The native
  /// side clears its output pointer during stop, so unregistering afterwards
  /// cannot leave it with a dangling D3D11Output.
  Future<void> _stopSession() {
    return _teardown ??= _teardownSession();
  }

  Future<void> _teardownSession() async {
    _stopPolling();
    _ticketTimer?.cancel();
    _ticketTimer = null;
    final displays = _displayStreams;
    _displayStreams = null;
    displays?.removeListener(_displayStreamsChanged);
    // Stop registration before stopping native media; retry cleanup after
    // Stop has detached every output if the channel failed during detach.
    try {
      await displays?.close();
    } catch (_) {
      /* Retained until Stop. */
    }
    final int id = _textureId;
    final int cameraId = _cameraTextureId;
    _textureId = -1;
    _cameraTextureId = -1;
    _cameraOpen = false;
    try {
      await ViewerBridge.stop();
      await displays?.close(sessionStopped: true);
    } on MissingPluginException {
      // Nothing to stop.
    } on PlatformException {
      // A stale session is better than a stuck one.
    }
    if (cameraId >= 0) {
      await _gpuTexture.unregisterTexture(cameraId);
    }
    if (id >= 0) {
      await _gpuTexture.unregisterTexture(id);
    }
    displays?.dispose();
  }

  void _stopPolling() {
    _pollTimer?.cancel();
    _pollTimer = null;
    ++_pollEpoch;
  }

  void _startPolling() {
    _stopPolling();
    _pollTimer = Timer.periodic(_pollInterval, (_) => unawaited(_poll()));
  }

  /// The signed-in account with a token that still works. The page opens with
  /// a snapshot, so once that token nears expiry it first reuses what another
  /// window persisted (its proactive refresh rotates the token) and only then
  /// renews with the refresh token itself, writing the result back.
  Future<AccountSession?> _activeAccount() async {
    final AccountSession? current = _account;
    if (current == null) {
      return null;
    }
    if (current.isActiveAt(
      DateTime.now().toUtc().add(const Duration(minutes: 2)),
    )) {
      return current;
    }
    return _accountRefresh ??= _refreshAccount(current).whenComplete(() {
      _accountRefresh = null;
    });
  }

  Future<AccountSession?> _refreshAccount(AccountSession current) async {
    final DateTime now = DateTime.now().toUtc();
    final AccountSessionStore store = AccountSessionStore(
      protectSecret: ViewerBridge.protectSecret,
      unprotectSecret: ViewerBridge.unprotectSecret,
    );
    try {
      final AccountSession? stored = await store.load();
      if (stored != null &&
          stored.server == current.server &&
          stored.username == current.username &&
          stored.isActiveAt(DateTime.now().toUtc())) {
        _account = stored;
        return stored;
      }
    } catch (_) {
      // Fall through to renewing this page's copy.
    }
    if (!current.canRefreshAt(now)) {
      return current.isActiveAt(DateTime.now().toUtc()) ? current : null;
    }
    try {
      final AccountLoginResult renewed = await ServerApi.refresh(
        ServerSettings(url: current.server),
        current.refreshToken,
      );
      final DateTime issued = DateTime.now().toUtc();
      final AccountSession session = current.copyWith(
        token: renewed.token,
        expiresAt: issued.add(Duration(milliseconds: renewed.expiresInMs)),
        refreshToken: renewed.refreshToken.isEmpty
            ? null
            : renewed.refreshToken,
        refreshExpiresAt: renewed.refreshToken.isEmpty
            ? null
            : issued.add(Duration(milliseconds: renewed.refreshExpiresInMs)),
      );
      _account = session;
      unawaited(store.save(session));
      return session;
    } catch (_) {
      // A transient failure leaves the current token in force until it expires.
      return current.isActiveAt(DateTime.now().toUtc()) ? current : null;
    }
  }

  /// Fetches the ticket a managed host requires. Empty when this session has
  /// no account, no server-side host id, or the host is not managed; a
  /// refusal from the server is surfaced instead of silently downgrading to
  /// local authentication, which a managed host would reject anyway.
  Future<String> _fetchAccessTicket() async {
    if (_emergencyActive) {
      // The agent obtained the override immediately before the tunnel became
      // ready, so its first ticket is the one this session starts with.
      final String ticket = _tunnel?.accessTicket ?? '';
      if (ticket.isEmpty) {
        throw TunnelException(tr('隧道代理没有返回应急访问票据'));
      }
      return ticket;
    }
    final AccountSession? account = await _activeAccount();
    if (account == null ||
        !account.isActiveAt(DateTime.now()) ||
        !widget.target.viaServer ||
        widget.target.hostId.isEmpty) {
      return '';
    }
    final String publicKey = await ViewerBridge.ticketIdentity();
    if (publicKey.isEmpty) {
      return '';
    }
    _ticketPublicKey = publicKey;
    final HostInfo info = await ServerApi.lookup(
      ServerSettings(url: account.server),
      widget.target.hostId,
      accountToken: account.token,
      clientPublicKey: publicKey,
    );
    final HostAccessTicket? ticket = info.accessTicket;
    if (ticket == null) {
      return '';
    }
    final int now = DateTime.now().millisecondsSinceEpoch;
    if (ticket.expiresAtMs > 0 && ticket.expiresAtMs <= now) {
      throw ServerApiException(tr('访问票据已过期'));
    }
    return ticket.ticket;
  }

  void _startTicketRenewal(String ticket) {
    _ticketTimer?.cancel();
    _ticketTimer = null;
    // In emergency mode the tunnel agent re-signs the override itself and
    // pushes each replacement; a lookup with the viewer account would be
    // refused by the restricted device.
    if (_emergencyActive || ticket.isEmpty) {
      return;
    }
    _ticketTimer = Timer.periodic(
      _ticketRenewInterval,
      (_) => unawaited(_renewAccessTicket()),
    );
  }

  /// Refreshes the signed ticket before the host's lease ends. An explicit
  /// refusal means the account or device authorization changed, so the
  /// session stops at once rather than waiting for the deadline; a transient
  /// failure leaves the existing deadline in force.
  Future<void> _renewAccessTicket() async {
    if (_emergencyActive || _ticketRenewBusy || !mounted || _closing) {
      return;
    }
    final AccountSession? account = await _activeAccount();
    if (account == null ||
        !account.isActiveAt(DateTime.now()) ||
        _ticketPublicKey.isEmpty) {
      return;
    }
    _ticketRenewBusy = true;
    try {
      final HostInfo info = await ServerApi.lookup(
        ServerSettings(url: account.server),
        widget.target.hostId,
        accountToken: account.token,
        clientPublicKey: _ticketPublicKey,
      );
      final HostAccessTicket? ticket = info.accessTicket;
      if (ticket == null) {
        return;
      }
      await ViewerBridge.renewAccessTicket(ticket.ticket);
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.unauthorized ||
          error.statusCode == HttpStatus.forbidden) {
        _ticketTimer?.cancel();
        _ticketTimer = null;
        try {
          await ViewerBridge.stop();
        } catch (_) {
          // The transport may already be gone; the poll reports the end.
        }
      }
    } on MissingPluginException {
      _ticketTimer?.cancel();
      _ticketTimer = null;
    } on PlatformException {
      // The bridge rejected the ticket shape; the host closes at the lease.
    } finally {
      _ticketRenewBusy = false;
    }
  }

  Future<void> _poll() async {
    if (_polling || !mounted || _closing || _reconnecting) {
      return;
    }
    final int epoch = _pollEpoch;
    _polling = true;
    try {
      final ViewerStatus status = await ViewerBridge.status();
      if (!mounted || _closing || _reconnecting || epoch != _pollEpoch) {
        return;
      }
      _applyStatus(status);
      if (_displayStreams == null &&
          _displaySelection != null &&
          status.isLive) {
        _createDisplayStreams();
        await _displayStreams!.select(_displaySelection!);
      }
      await _displayStreams?.refresh();
      // Drain after the status update: the audit reference and route of the
      // connection are only current once _applyStatus has read this poll.
      unawaited(_reportCompletedFiles());
    } on MissingPluginException {
      // Reported by _start; keep waiting for the channel to come up.
    } on PlatformException {
      // A single failed poll is transient; the next tick retries.
    } finally {
      _polling = false;
    }
  }

  void _applyStatus(ViewerStatus status) {
    // Captured before the setState below replaces _status: the answer checks
    // further down need the snapshot this poll is being compared against.
    final ViewerStatus previous = _status;
    if (_serverDefaultDisplay == null &&
        _display == kViewerDisplayAuto &&
        status.isLive &&
        status.streamedDisplayIndex != null) {
      _serverDefaultDisplay = status.streamedDisplayIndex;
    }
    // First contact: the native side connected without a pin so it could
    // report the certificate. Ask before remembering it; every later
    // connection is verified against the accepted value.
    if (_startError == null &&
        _pinnedFingerprint.isEmpty &&
        status.serverFingerprint.isNotEmpty &&
        !_fingerprintPrompted) {
      _fingerprintPrompted = true;
      _pinnedFingerprint = status.serverFingerprint;
      unawaited(_confirmFirstFingerprint(status.serverFingerprint));
    }
    _trackRecording(status);
    _watchRecording(status);
    _watchFileConflict(status);
    _syncPortForwardRules(status.portForwardRules);
    // Track each persistent console's service id while it is alive. An ending
    // or refusal leaves nothing to attach to, and a poll from a fresh engine
    // (id still 0) must not erase what the page remembers for a reconnect.
    for (int slot = 0; slot < kViewerMaxTerminals; slot++) {
      final ViewerTerminalStatus terminal = status.terminal(slot + 1);
      if (terminal.phase == ViewerTerminalPhase.ended ||
          terminal.phase == ViewerTerminalPhase.failed) {
        _terminalServiceIds[slot] = 0;
      } else if (terminal.serviceId != 0) {
        _terminalServiceIds[slot] = terminal.serviceId;
      }
    }
    final int previousPermissions = _status.permissions;
    final int previousPermissionUpdates = _status.permissionUpdates;
    // A new native session restarts these counters at zero; that first poll is
    // not an answer to anything sent before the restart.
    if (status.streamOptionResults < _streamOptionSeen) {
      _streamOptionSeen = status.streamOptionResults;
      _streamPending = 0;
      _streamRollback = null;
    }
    final bool streamAnswered = status.streamOptionResults > _streamOptionSeen;
    _StreamOptionSnapshot? streamRollback;
    bool streamRefused = false;
    if (streamAnswered) {
      _streamOptionSeen = status.streamOptionResults;
      if (_streamPending > 0) {
        --_streamPending;
      }
      streamRefused = status.streamOptionCode != 0;
      if (_streamPending == 0) {
        if (streamRefused) {
          streamRollback = _streamRollback;
        }
        _streamRollback = null;
      }
    }
    final bool streamSettled = streamAnswered && _streamPending == 0;
    // A refused password or one-time code is a step, not an end: the session
    // is dead, but the card stays away while the dialog collects fresh
    // credentials and reconnects.
    final bool challenge =
        _startError == null &&
        status.isOver &&
        !_over &&
        _isCredentialChallenge(status.denial);
    final bool nowOver = status.isOver && !challenge;
    final bool justOver = nowOver && !_over;
    if (status.hasFrame && _auditStartedAt == null) {
      _auditStartedAt = DateTime.now();
    }
    if (status.hasFrame && !_sawFrame) {
      _sawFrame = true;
      _collapsed = false;
      // The series did its job: the next drop starts a fresh one.
      _autoTimer?.cancel();
      _autoTimer = null;
      _autoPolicy.reset();
      _scheduleHide();
    }
    if (nowOver && !_over) {
      _over = true;
      _hideTimer?.cancel();
      _pollTimer?.cancel();
      _collapsed = false;
    }
    final bool droppedControl =
        (nowOver || !status.inputEnabled) && _controlEnabled;
    if (droppedControl) {
      _controlEnabled = false;
      _disableRelativeMouse();
    }
    // The host answers a switch with one result. Compare the counters with
    // the previous poll rather than the error text: two identical refusals
    // must still read as two answers, and a late answer for a request the
    // picker has already replaced must not win.
    final int? pendingDisplay = _pendingDisplay;
    final bool switchRefused =
        pendingDisplay != null &&
        status.displaySwitchFailures > _status.displaySwitchFailures;
    final bool switchAccepted =
        pendingDisplay != null &&
        !switchRefused &&
        status.displaySwitches > _status.displaySwitches;
    // A mode change is answered the same way: the counters say whether this
    // poll carries a new answer, so an old reason cannot be read twice.
    final bool modeChangeAccepted =
        status.displayModeChanges > _status.displayModeChanges;
    final bool modeChangeRefused =
        status.displayModeFailures > _status.displayModeFailures;
    // The host answers every follow request (and an input revocation) with a
    // state message; only a refusal deserves a toast.
    final bool followRefused =
        status.displayFollowResults > _status.displayFollowResults &&
        status.displayFollowCode != 0;
    // One upload reaches an end this poll. The counters survive the phase
    // reset, so a success and a failure can never both arrive; the name and
    // the reason stay in the status until the next upload starts.
    final bool fileDone = status.filesSent > _status.filesSent;
    final bool fileFailed = status.fileFailures > _status.fileFailures;
    final bool fileCancelled = status.fileCancels > _status.fileCancels;
    final bool downloadDone = status.filesReceived > _status.filesReceived;
    final bool downloadFailed =
        status.fileReceiveFailures > _status.fileReceiveFailures;
    final bool chatArrived = status.chatReceived > _status.chatReceived;
    final bool chatFailed = status.chatFailures > _status.chatFailures;
    if (modeChangeAccepted) {
      _modesRefreshPending = true;
      _modesRequestedFor = null;
    }
    setState(() {
      if (switchAccepted) {
        _display = pendingDisplay;
      }
      if (switchAccepted || switchRefused || nowOver) {
        _pendingDisplay = null;
      }
      if (streamSettled) {
        if (streamRefused) {
          if (streamRollback != null) {
            _streamCodec = streamRollback.codec;
            _streamQuality = streamRollback.quality;
            _streamQualityPercent = streamRollback.qualityPercent;
            _streamMaxFps = streamRollback.maxFps;
            _streamFullColor = streamRollback.fullColor;
          } else {
            // A preference re-applied after a reconnect was refused; show the
            // values the host actually runs.
            _streamFullColor = status.chroma == 1;
            _streamCodec = status.codec == 'vp9'
                ? ViewerStreamCodec.vp9
                : status.codec == 'hevc'
                ? ViewerStreamCodec.hevc
                : (status.codec == 'av1'
                      ? ViewerStreamCodec.av1
                      : ViewerStreamCodec.h264);
            _streamQuality = _streamQualityFor(status.streamQuality);
            if (status.streamQualityPercent != 0) {
              _streamQualityPercent = status.streamQualityPercent;
            }
          }
        } else {
          _streamQuality = _streamQualityFor(status.streamQuality);
          if (status.streamQualityPercent != 0) {
            _streamQualityPercent = status.streamQualityPercent;
          }
        }
      }
      _status = status;
      _fullscreen = status.fullscreen;
    });
    _maybeOpenControlPanel(status);
    if (streamSettled) {
      _saveStreamPreferences();
    }
    if (streamSettled && streamRefused) {
      final String reason = status.streamOptionError;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            reason.isEmpty
                ? tr('画面设置未生效')
                : tr('画面设置未生效：{reason}', <String, Object?>{'reason': reason}),
          ),
          duration: const Duration(seconds: 4),
        ),
      );
    }
    // A session switch answer. Acceptance ends this connection on purpose, so
    // the page reconnects through the ladder instead of showing a drop; the
    // answer may arrive on the same poll that reports the end.
    if (status.sessionSelectUpdates > previous.sessionSelectUpdates) {
      final String reason = status.sessionSelectError;
      if (status.sessionSelectCode == ViewerSessionSelectCode.ok.code) {
        _sessionSwitchPending = true;
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              tr('正在切换到会话 {id}…', <String, Object?>{
                'id': status.sessionSelectId,
              }),
            ),
            duration: const Duration(seconds: 4),
          ),
        );
      } else {
        final String detail = reason.isEmpty
            ? _sessionSelectCodeText(status.sessionSelectCode)
            : reason;
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              tr('切换会话失败：{detail}', <String, Object?>{'detail': detail}),
            ),
            duration: const Duration(seconds: 5),
          ),
        );
      }
    }
    if (status.elevationUpdates > previous.elevationUpdates) {
      if (status.elevationCode == 4) _sessionSwitchPending = true;
      if (status.elevationRequestId != 0 ||
          (_elevationReconnectPending && status.elevationCode == 2)) {
        String? message;
        switch (status.elevationCode) {
          case 2:
            message = tr('主机已取得管理员权限');
            _elevationReconnectPending = false;
          case 3:
            message = tr('等待主机本地确认 UAC…');
          case 4:
            _elevationReconnectPending = true;
            message = tr('主机正在取得管理员权限，即将重新连接…');
          case 5:
            message = tr('主机本地取消了提权请求');
          case 6:
            message = tr('提权需要远程键鼠控制权限');
          case 7:
            message = tr('主机已有提权请求，请稍后重试');
          case 8:
            message = tr('提权失败：{reason}', <String, Object?>{
              'reason': status.elevationReason,
            });
        }
        if (message != null) {
          ScaffoldMessenger.of(
            context,
          ).showSnackBar(SnackBar(content: Text(message)));
        }
      } else if (_elevationReconnectPending && status.elevationCode == 1) {
        _elevationReconnectPending = false;
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text(tr('提权未完成，已重新连接到原主机'))));
      }
    }
    if (justOver) {
      if (_cameraOpen) {
        unawaited(_closeCameraPanel());
      }
      if (_fileBrowserOpen) {
        _closeFileBrowser();
      }
      final DateTime? auditStartedAt = _auditStartedAt;
      _auditStartedAt = null;
      if (auditStartedAt != null) {
        unawaited(_reportFinishedSession(auditStartedAt, status.error));
      }
      if (_sessionSwitchPending) {
        _sessionSwitchPending = false;
        _scheduleSessionSwitchReconnect();
      } else {
        _maybeStartAutoReconnect(status);
      }
    }
    if (switchRefused) {
      final String reason = status.displaySwitchError;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            reason.isEmpty
                ? tr('切换显示失败，已回到当前输出')
                : tr('切换显示失败：{reason}', <String, Object?>{'reason': reason}),
          ),
          duration: const Duration(seconds: 4),
        ),
      );
    }
    if (modeChangeRefused) {
      final String reason = status.displayModeError;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            reason.isEmpty
                ? tr('切换分辨率失败')
                : tr('切换分辨率失败：{reason}', <String, Object?>{'reason': reason}),
          ),
          duration: const Duration(seconds: 4),
        ),
      );
    }
    if (followRefused) {
      final String reason = status.displayFollowError;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            reason.isEmpty
                ? tr('无法跟随远程显示')
                : tr('无法跟随远程显示：{reason}', <String, Object?>{'reason': reason}),
          ),
          duration: const Duration(seconds: 4),
        ),
      );
    }
    if (fileDone) {
      final String name = status.lastFileName;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            name.isEmpty
                ? tr('文件已发送')
                : tr('文件已发送：{name}', <String, Object?>{'name': name}),
          ),
          duration: const Duration(seconds: 3),
        ),
      );
    } else if (fileFailed) {
      final String reason = status.lastFileError;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            reason.isEmpty
                ? tr('文件发送失败')
                : tr('文件发送失败：{reason}', <String, Object?>{'reason': reason}),
          ),
          duration: const Duration(seconds: 5),
        ),
      );
    } else if (fileCancelled) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(tr('已取消上传')),
          duration: const Duration(seconds: 3),
        ),
      );
    }
    if (downloadDone) {
      final String name = status.downloadName;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            name.isEmpty
                ? tr('文件已接收')
                : tr('文件已接收：{name}', <String, Object?>{'name': name}),
          ),
          duration: const Duration(seconds: 3),
        ),
      );
    } else if (downloadFailed) {
      final String reason = status.downloadError;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            reason.isEmpty
                ? tr('文件接收失败')
                : tr('文件接收失败：{reason}', <String, Object?>{'reason': reason}),
          ),
          duration: const Duration(seconds: 5),
        ),
      );
    }
    // A microphone failure stops only the uplink; video and system sound keep
    // running. The counter tells a new failure from the stale reason text.
    if (!nowOver && status.microphoneErrors > previous.microphoneErrors) {
      final String reason = status.microphoneError;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            reason.isEmpty
                ? tr('麦克风已停止')
                : tr('麦克风已停止：{reason}', <String, Object?>{'reason': reason}),
          ),
          duration: const Duration(seconds: 5),
        ),
      );
    }
    // System audio failures happen asynchronously after video is already up.
    // Surface them once per new counter/reason/state change instead of making
    // the operator open the statistics overlay to discover silence.
    if (!nowOver && viewerAudioFailureRaised(previous, status)) {
      final String reason = status.audioError;
      final String prefix = status.audioAnnounced && !status.audioEnabled
          ? tr('系统音频已停止')
          : tr('系统音频出现问题');
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            reason.isEmpty
                ? prefix
                : tr('{prefix}：{reason}', <String, Object?>{
                    'prefix': prefix,
                    'reason': reason,
                  }),
          ),
          duration: const Duration(seconds: 5),
        ),
      );
    }
    // Voice call setup is asynchronous. A host refusal, decoder/player setup
    // failure and a host-initiated close all arrive through voiceError after
    // the toggle call has already returned successfully.
    if (!nowOver && viewerVoiceFailureRaised(previous, status)) {
      final String reason = status.voiceError;
      final String prefix = previous.voiceEnabled && !status.voiceEnabled
          ? tr('语音通话已结束')
          : status.voiceEnabled
          ? tr('语音通话出现问题')
          : tr('语音通话未能开启');
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            reason.isEmpty
                ? prefix
                : tr('{prefix}：{reason}', <String, Object?>{
                    'prefix': prefix,
                    'reason': reason,
                  }),
          ),
          duration: const Duration(seconds: 5),
        ),
      );
    }
    // Every privacy answer is one event. A refusal or a host-initiated
    // disable explains itself; a success just confirms the new state.
    if (!nowOver && status.privacyUpdates > previous.privacyUpdates) {
      final String reason = status.privacyError;
      final String text;
      if (status.privacyCode != 0) {
        text = reason.isEmpty
            ? _privacyCodeText(status.privacyCode)
            : '${_privacyCodeText(status.privacyCode)}：$reason';
      } else {
        text = status.privacyEnabled
            ? tr('隐私模式已开启：主机本地画面已被遮挡')
            : tr('隐私模式已关闭：主机本地画面已恢复');
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(text), duration: const Duration(seconds: 4)),
      );
    }
    // Display-management answers only interrupt the operator when something
    // was refused; a success is visible in the menu itself.
    if (!nowOver &&
        status.displayControlUpdates > previous.displayControlUpdates &&
        status.displayControlCode != ViewerDisplayControlCode.ok.code) {
      final String reason = status.displayControlError;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            reason.isEmpty
                ? tr('显示设置未应用')
                : tr('显示设置未应用：{reason}', <String, Object?>{'reason': reason}),
          ),
          duration: const Duration(seconds: 4),
        ),
      );
    }
    // Every Ctrl+Alt+Del request gets one answer. A refusal or unsupported
    // host explains itself; success only means SendSAS was invoked, because
    // Windows never reports whether the secure desktop actually appeared.
    if (!nowOver && status.sasUpdates > previous.sasUpdates) {
      final String reason = status.sasError;
      final String text;
      if (status.sasCode == ViewerSasCode.ok.code) {
        text = tr('已请求主机发送 Ctrl+Alt+Del（Windows 不回报是否成功）');
      } else {
        final String detail = reason.isEmpty
            ? _sasCodeText(status.sasCode)
            : reason;
        text = tr('Ctrl+Alt+Del 未发送：{detail}', <String, Object?>{
          'detail': detail,
        });
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(text), duration: const Duration(seconds: 4)),
      );
    }
    // Every host-control action gets one answer. A refusal explains itself;
    // success confirms the new state, and a host-initiated block release
    // arrives as a denial with the block off.
    if (!nowOver && status.hostActionUpdates > previous.hostActionUpdates) {
      final String reason = status.hostActionError;
      final String action = _hostActionText(status.hostActionAction);
      final String text;
      if (status.hostActionCode != ViewerHostActionCode.ok.code) {
        final String detail = reason.isEmpty
            ? _hostActionCodeText(status.hostActionCode)
            : reason;
        text = tr('{action}未执行：{detail}', <String, Object?>{
          'action': action,
          'detail': detail,
        });
      } else if (status.hostActionAction == ViewerHostAction.blockInput.code) {
        text = status.blockInputActive ? tr('主机本地输入已阻止') : tr('主机本地输入已恢复');
      } else if (status.hostActionAction == ViewerHostAction.lockScreen.code) {
        text = tr('已请求主机锁屏');
      } else {
        text = tr('已请求主机重启');
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(text), duration: const Duration(seconds: 4)),
      );
    }
    // Every OS-login request gets one answer. The password itself never comes
    // back; success only means the host typed it at its logon or lock screen.
    if (!nowOver && status.osLoginUpdates > previous.osLoginUpdates) {
      final String reason = status.osLoginError;
      final String text;
      if (status.osLoginCode == ViewerHostActionCode.ok.code) {
        text = tr('已在主机登录/锁屏界面输入密码');
      } else {
        final String detail = reason.isEmpty
            ? _osLoginCodeText(status.osLoginCode)
            : reason;
        text = tr('系统登录失败：{detail}', <String, Object?>{'detail': detail});
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(text), duration: const Duration(seconds: 5)),
      );
    }
    // Screenshots finish on the session thread; the counter pairs the newest
    // path or failure with this poll.
    if (!nowOver && status.screenshotUpdates > previous.screenshotUpdates) {
      final String reason = status.screenshotError;
      final String path = status.screenshotPath;
      final String text;
      if (reason.isNotEmpty) {
        text = tr('截图失败：{reason}', <String, Object?>{'reason': reason});
      } else if (path.isEmpty) {
        text = tr('截图已保存');
      } else {
        text = tr('截图已保存：{path}', <String, Object?>{'path': path});
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(text), duration: const Duration(seconds: 4)),
      );
    }
    if (chatArrived) {
      unawaited(_drainChat());
    }
    if (!nowOver && chatFailed) {
      final String reason = status.chatError;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            reason.isEmpty
                ? tr('聊天消息发送失败')
                : tr('聊天消息发送失败：{reason}', <String, Object?>{'reason': reason}),
          ),
          duration: const Duration(seconds: 5),
        ),
      );
    }
    if (!nowOver) {
      final String forwardError = status.portForwardRules
          .map((ViewerPortForwardRule rule) => rule.error)
          .firstWhere((String text) => text.isNotEmpty, orElse: () => '');
      if (forwardError.isEmpty) {
        _portForwardErrorSeen = '';
      } else if (forwardError != _portForwardErrorSeen) {
        _portForwardErrorSeen = forwardError;
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              tr('端口转发：{p4}', <String, Object?>{'p4': forwardError}),
            ),
            duration: const Duration(seconds: 5),
          ),
        );
      }
    }
    if (droppedControl) {
      _setKeyCapture(false);
    }
    if (!nowOver &&
        status.state == ViewerState.streaming &&
        previousPermissionUpdates > 0 &&
        status.permissionUpdates > previousPermissionUpdates) {
      _notifyPermissionChange(previousPermissions, status);
    }
    if (challenge && !_authPromptOpen && _challengeDenial != status.denial) {
      _challengeDenial = status.denial;
      unawaited(_handleCredentialChallenge(status.denial));
    }
    _maybeRequestModes(status, nowOver);
    if (nowOver) {
      _clearCursor();
      return;
    }
    _syncCursor();
  }

  /// Hands the newest host pointer state to the native side while control is
  /// on. Cheap to call every poll: nothing happens unless the host's counter,
  /// the control gate or the drawn scale changed since the last call.
  void _syncCursor() {
    final bool enabled = _inputActive && !_independentDisplays;
    // While the page cannot replace the local cursor, the host's pointer is
    // drawn over the picture instead, so a view-only session still shows it.
    _setRemoteCursorPolling(
      !enabled &&
          !_independentDisplays &&
          _status.state == ViewerState.streaming &&
          _status.hasFrame,
    );
    final double scale = _cursorTargetScale;
    if (_cursorSynced &&
        enabled == _cursorWanted &&
        _status.cursorUpdates == _cursorSeen &&
        (_cursorScale - scale).abs() < 0.001) {
      return;
    }
    _cursorSynced = true;
    _cursorWanted = enabled;
    _cursorSeen = _status.cursorUpdates;
    _cursorScale = scale;
    unawaited(_refreshCursor());
  }

  Future<void> _refreshCursor() async {
    if (_cursorBusy) {
      // A call is in flight; the last state it saw may already be stale, so
      // run once more when it lands rather than dropping the change.
      _cursorDirty = true;
      return;
    }
    _cursorBusy = true;
    bool installed = _cursorInstalled;
    try {
      installed = await ViewerBridge.refreshRemoteCursor(
        enabled: _cursorWanted,
        scale: _cursorScale,
      );
    } on PlatformException {
      installed = _cursorInstalled;
    } on MissingPluginException {
      installed = _cursorInstalled;
    } finally {
      _cursorBusy = false;
    }
    if (!mounted) {
      return;
    }
    if (installed != _cursorInstalled) {
      setState(() => _cursorInstalled = installed);
    }
    if (_cursorDirty) {
      _cursorDirty = false;
      await _refreshCursor();
    }
  }

  void _clearCursor() {
    _setRemoteCursorPolling(false);
    final bool touched = _cursorSynced || _cursorInstalled;
    _cursorSynced = false;
    _cursorWanted = false;
    _cursorSeen = -1;
    _cursorScale = 0;
    if (_cursorInstalled) {
      _cursorInstalled = false;
      if (mounted) {
        setState(() {});
      }
    }
    // Also clear when only a refresh was ever sent: the host may have
    // installed a cursor without this page having seen the reply yet.
    if (touched) {
      unawaited(_guardCursorClear());
    }
  }

  Future<void> _guardCursorClear() async {
    try {
      await ViewerBridge.clearRemoteCursor();
    } on PlatformException {
      // The session is already going away.
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  /// Starts or stops the view-only pointer poll. RustDesk sends pointer state
  /// on a 33 ms service, so this asks at the same cadence.
  void _setRemoteCursorPolling(bool on) {
    if (on) {
      _remoteCursorTimer ??= Timer.periodic(
        const Duration(milliseconds: 33),
        (_) => unawaited(_pollRemoteCursor()),
      );
      return;
    }
    _remoteCursorTimer?.cancel();
    _remoteCursorTimer = null;
    _remoteCursorBusy = false;
    _remoteCursor.value = null;
    _dropRemoteCursorImage();
  }

  void _dropRemoteCursorImage() {
    final ui.Image? old = _remoteCursorImage;
    _remoteCursorImage = null;
    _remoteCursorImageShape = null;
    if (old != null) {
      // The overlay built this frame may still paint the image.
      WidgetsBinding.instance.addPostFrameCallback((_) => old.dispose());
    }
  }

  Future<void> _pollRemoteCursor() async {
    if (_remoteCursorBusy || !mounted) {
      return;
    }
    _remoteCursorBusy = true;
    try {
      final RemoteCursorView? cursor = await ViewerBridge.takeRemoteCursor();
      if (!mounted) {
        return;
      }
      if (cursor == null) {
        _remoteCursor.value = null;
        return;
      }
      final Uint8List? bgra = cursor.bgra;
      if (bgra != null &&
          bgra.length == cursor.width * cursor.height * 4 &&
          cursor.width > 0 &&
          cursor.height > 0 &&
          _remoteCursorImageShape != cursor.shapeId) {
        final Completer<ui.Image> decoded = Completer<ui.Image>();
        ui.decodeImageFromPixels(
          bgra,
          cursor.width,
          cursor.height,
          ui.PixelFormat.bgra8888,
          decoded.complete,
        );
        final ui.Image image = await decoded.future;
        if (!mounted) {
          image.dispose();
          return;
        }
        _dropRemoteCursorImage();
        _remoteCursorImage = image;
        _remoteCursorImageShape = cursor.shapeId;
      }
      if (!cursor.visible ||
          cursor.shapeId == 0 ||
          _remoteCursorImageShape != cursor.shapeId) {
        _remoteCursor.value = null;
        return;
      }
      _remoteCursor.value = cursor;
    } on PlatformException {
      // The session is already going away.
    } on MissingPluginException {
      // The native viewer is going away.
    } finally {
      _remoteCursorBusy = false;
    }
  }

  Widget _buildRemoteCursorOverlay(Size drawn) {
    return ValueListenableBuilder<RemoteCursorView?>(
      valueListenable: _remoteCursor,
      builder: (BuildContext context, RemoteCursorView? cursor, Widget? child) {
        final ui.Image? image = _remoteCursorImage;
        if (cursor == null || !cursor.visible || image == null) {
          return const SizedBox.shrink();
        }
        final int width = _status.sourceWidth > 0
            ? _status.sourceWidth
            : _status.width;
        final int height = _status.sourceHeight > 0
            ? _status.sourceHeight
            : _status.height;
        if (width <= 0 ||
            height <= 0 ||
            drawn.width <= 0 ||
            drawn.height <= 0) {
          return const SizedBox.shrink();
        }
        final double sx = drawn.width / width;
        final double sy = drawn.height / height;
        // x/y is the hotspot, so the sprite's top-left sits before it.
        return Stack(
          children: <Widget>[
            Positioned(
              left: (cursor.x - cursor.hotspotX) * sx,
              top: (cursor.y - cursor.hotspotY) * sy,
              width: cursor.width * sx,
              height: cursor.height * sy,
              child: RawImage(
                image: image,
                fit: BoxFit.fill,
                filterQuality: FilterQuality.low,
              ),
            ),
          ],
        );
      },
    );
  }

  void _onViewScale(double scale) {
    if ((_viewScale - scale).abs() < 0.0005) {
      return;
    }
    _viewScale = scale;
    _syncCursor();
    if (_relativeMouse) {
      // The native path maps window pixels to captured pixels, so a resize
      // must update the factor it divides by.
      _applyRelativeMouse(true);
    }
  }

  void _toggleClipboardSync() {
    final bool enabled = !_clipboardSync;
    setState(() => _clipboardSync = enabled);
    unawaited(_applyClipboardSync(enabled));
  }

  Future<void> _applyClipboardSync(bool enabled) async {
    try {
      await ViewerBridge.setClipboardSync(enabled);
    } on PlatformException {
      // The session is going away; the next start reapplies the switch.
    } on MissingPluginException {
      // Same.
    }
  }

  void _toggleRecording() {
    if (_status.recording) {
      // Auto-record must not undo a deliberate stop in the same session.
      _autoRecordSuppressed = true;
    } else {
      // A retry deserves a fresh prompt if it fails for the same reason.
      _recordErrorSeen = '';
    }
    unawaited(_applyRecording(!_status.recording));
  }

  Future<void> _applyRecording(bool enabled) async {
    try {
      await ViewerBridge.setRecording(enabled);
    } on PlatformException {
      // The session is going away; the next start reapplies the switch.
    } on MissingPluginException {
      // Same.
    }
  }

  void _takeScreenshot() {
    unawaited(_saveScreenshot());
  }

  Future<void> _saveScreenshot() async {
    try {
      await ViewerBridge.takeScreenshot();
    } on PlatformException catch (error) {
      if (mounted) {
        final String reason = error.message ?? '';
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              reason.isEmpty
                  ? tr('截图失败')
                  : tr('截图失败：{reason}', <String, Object?>{'reason': reason}),
            ),
            duration: const Duration(seconds: 4),
          ),
        );
      }
    } on MissingPluginException {
      // The viewer is going away.
    }
  }

  Future<void> _sendFile() => _startUpload(ViewerBridge.sendFile);

  Future<void> _sendDroppedFiles(List<String> paths) => _startUpload(
    () => ViewerBridge.sendFilesTo(
      paths,
      _fileBrowserOpen ? _fileBrowserDirectory : '',
    ),
  );

  Future<void> _startUpload(Future<int?> Function() start) async {
    if (_filePickBusy) {
      return;
    }
    _filePickBusy = true;
    try {
      final int? queued = await start();
      if (queued != null && mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              tr('已加入发送队列（{queued} 项），进度显示在画面下方', <String, Object?>{
                'queued': queued,
              }),
            ),
            duration: const Duration(seconds: 2),
          ),
        );
      }
    } on PlatformException catch (error) {
      if (mounted) {
        final String reason = error.message ?? '';
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              reason.isEmpty
                  ? tr('无法发送文件')
                  : tr('无法发送文件：{reason}', <String, Object?>{'reason': reason}),
            ),
            duration: const Duration(seconds: 4),
          ),
        );
      }
    } on MissingPluginException {
      // The viewer is going away.
    } finally {
      _filePickBusy = false;
    }
  }

  void _scheduleHide() {
    _hideTimer?.cancel();
    if (_pinned || _over || !_sawFrame) {
      return;
    }
    _hideTimer = Timer(_hideDelay, () {
      if (!mounted || _over) {
        return;
      }
      setState(() => _collapsed = true);
    });
  }

  void _onHover(PointerHoverEvent event) {
    if (!_sawFrame || _over || _pinned) {
      return;
    }
    final double dy = event.localPosition.dy;
    if (_collapsed) {
      if (dy <= 48) {
        setState(() => _collapsed = false);
        _scheduleHide();
      }
      return;
    }
    if (dy <= 64) {
      _scheduleHide();
    }
  }

  void _togglePin() {
    final bool pinned = !_pinned;
    setState(() {
      _pinned = pinned;
      if (pinned) {
        _collapsed = false;
      }
    });
    if (pinned) {
      _hideTimer?.cancel();
    } else {
      _scheduleHide();
    }
  }

  void _collapse() {
    _hideTimer?.cancel();
    setState(() => _collapsed = true);
  }

  void _expand() {
    setState(() => _collapsed = false);
    _scheduleHide();
  }

  void _setFit(ViewFit fit) {
    setState(() => _fit = fit);
    ViewPreferences.update(
      peerId: _prefsPeerId,
      fit: fit == ViewFit.original ? 1 : 0,
    );
  }

  void _toggleReverseWheel() {
    setState(
      () => _mouse = MouseMapping(
        reverseWheel: !_mouse.reverseWheel,
        swapButtons: _mouse.swapButtons,
      ),
    );
    ViewPreferences.update(
      peerId: _prefsPeerId,
      reverseWheel: _mouse.reverseWheel,
    );
  }

  void _toggleSwapButtons() {
    setState(
      () => _mouse = MouseMapping(
        reverseWheel: _mouse.reverseWheel,
        swapButtons: !_mouse.swapButtons,
      ),
    );
    ViewPreferences.update(
      peerId: _prefsPeerId,
      swapButtons: _mouse.swapButtons,
    );
  }

  void _toggleStats() {
    setState(() => _statsVisible = !_statsVisible);
    ViewPreferences.update(peerId: _prefsPeerId, stats: _statsVisible);
  }

  void _toggleShowMyCursor() {
    final bool enabled = !_showMyCursor;
    setState(() => _showMyCursor = enabled);
    ViewPreferences.update(peerId: _prefsPeerId, showMyCursor: enabled);
    if (!enabled) {
      _hideCursorHint();
    }
  }

  void _onCursorHint(Offset frame, {DisplayVideoStream? display}) {
    if (!_showMyCursor) {
      return;
    }
    final (int x, int y) = _desktopPoint(frame, display);
    final int now = DateTime.now().millisecondsSinceEpoch;
    if (_cursorHintOnHost && now - _cursorHintSentMs < _cursorHintIntervalMs) {
      return;
    }
    _cursorHintSentMs = now;
    _cursorHintOnHost = true;
    unawaited(ViewerBridge.showCursor(x: x, y: y, visible: true));
  }

  /// The press edge skips the move throttle: a click that lands between two
  /// samples still ripples on the host at once, and the following moves
  /// throttle from this send instead of stacking another hint.
  void _onCursorHintPress(Offset frame, {DisplayVideoStream? display}) {
    if (!_showMyCursor) {
      return;
    }
    final (int x, int y) = _desktopPoint(frame, display);
    _cursorHintSentMs = DateTime.now().millisecondsSinceEpoch;
    _cursorHintOnHost = true;
    unawaited(
      ViewerBridge.showCursor(x: x, y: y, visible: true, pressed: true),
    );
  }

  void _hideCursorHint() {
    if (!_cursorHintOnHost) {
      return;
    }
    _cursorHintOnHost = false;
    unawaited(ViewerBridge.showCursor(x: 0, y: 0, visible: false));
  }

  void _rememberFileSort(FileBrowserSort sort, bool ascending) {
    _fileBrowserSort = sort;
    _fileBrowserSortAscending = ascending;
    ViewPreferences.update(
      peerId: _prefsPeerId,
      fileSort: sort.index,
      fileSortAscending: ascending,
    );
  }

  void _rememberFileBrowserDirectory(String directory) {
    _fileBrowserDirectory = directory;
    ViewPreferences.update(peerId: _prefsPeerId, browserDirectory: directory);
  }

  void _rememberFileShowHidden(bool showHidden) {
    _fileBrowserShowHidden = showHidden;
    ViewPreferences.update(peerId: _prefsPeerId, browserShowHidden: showHidden);
  }

  void _rememberTerminalFontSize(int size) {
    _terminalFontSize = size;
    ViewPreferences.update(peerId: _prefsPeerId, terminalFontSize: size);
  }

  Future<void> _setFullscreen(bool enable) async {
    try {
      final bool actual = await ViewerBridge.setFullscreen(enable);
      if (!mounted) {
        return;
      }
      setState(() => _fullscreen = actual);
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(error.message ?? tr('无法切换全屏'))));
    } on MissingPluginException {
      // No native window to resize.
    }
  }

  Future<void> _toggleFullscreen() => _setFullscreen(!_fullscreen);

  Future<void> _toggleAudioMute() async {
    if (!_status.acceptsAudio) {
      return;
    }
    try {
      await ViewerBridge.setAudioMuted(!_status.audioMuted);
    } on PlatformException {
      // The session is closing; the next poll reports the final state.
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  Future<void> _toggleMicrophone() async {
    if (!_status.acceptsMicrophone) {
      return;
    }
    final bool turnOn = !_status.microphoneEnabled;
    try {
      final bool applied = await ViewerBridge.setMicrophone(turnOn);
      if (!mounted || applied) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(tr('主机未授权麦克风，无法开启')),
          duration: Duration(seconds: 4),
        ),
      );
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(error.message ?? tr('无法切换麦克风'))));
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  Future<void> _toggleVoiceCall() async {
    if (!_status.acceptsVoiceCall) {
      return;
    }
    final bool turnOn = !_status.voiceEnabled;
    try {
      final bool applied = await ViewerBridge.setVoiceCall(turnOn);
      if (!mounted || applied) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(tr('主机未授权麦克风，无法开启')),
          duration: Duration(seconds: 4),
        ),
      );
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(error.message ?? tr('无法切换语音通话'))));
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  Future<void> _togglePrivacy() async {
    if (!_status.acceptsPrivacy) {
      return;
    }
    final bool turnOn = !_status.privacyEnabled;
    try {
      final bool applied = await ViewerBridge.setPrivacyMode(turnOn);
      if (!mounted || applied) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(tr('主机未授权隐私模式，无法切换')),
          duration: Duration(seconds: 4),
        ),
      );
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(error.message ?? tr('无法切换隐私模式'))));
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  Future<void> _controlDisplays(int action, int value, {int target = 0}) async {
    try {
      final bool queued = await ViewerBridge.controlDisplays(
        action,
        value,
        target: target,
      );
      if (mounted && !queued) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text(tr('显示设置未应用'))));
      }
    } on PlatformException catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text(error.message ?? tr('显示设置未应用'))));
      }
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  /// Runs one entry of the toolbar's host-control menu. Restart interrupts
  /// this session, so it asks first; the host's answer arrives later through
  /// [ViewerStatus.hostActionUpdates].
  Future<void> _requestElevation() async {
    if (_elevationSubmitting || !_status.elevationAvailable) return;
    setState(() => _elevationSubmitting = true);
    try {
      final bool? confirmed = await showDialog<bool>(
        context: context,
        builder: (BuildContext context) => AlertDialog(
          title: Text(tr('请求管理员权限')),
          content: Text(tr('主机本地需要确认 Windows UAC。确认后连接会短暂中断并自动重连，其他查看者也会断开。')),
          actions: <Widget>[
            TextButton(
              onPressed: () => Navigator.pop(context, false),
              child: Text(tr('取消')),
            ),
            FilledButton(
              onPressed: () => Navigator.pop(context, true),
              child: Text(tr('请求管理员权限')),
            ),
          ],
        ),
      );
      if (confirmed != true || !mounted || !_status.elevationAvailable) return;
      final bool queued = await ViewerBridge.requestElevation();
      if (!queued && mounted) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text(tr('提权请求未发送，请确认连接和控制权限'))));
      }
    } on PlatformException catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text(error.message ?? error.code)));
      }
    } finally {
      if (mounted) setState(() => _elevationSubmitting = false);
    }
  }

  Future<void> _onHostAction(ViewerHostAction action) async {
    switch (action) {
      case ViewerHostAction.restart:
        if (!_status.acceptsRestart) {
          return;
        }
        final bool? confirmed = await showDialog<bool>(
          context: context,
          builder: (BuildContext dialogContext) => AlertDialog(
            title: Text(tr('重启主机？')),
            content: Text(tr('主机会立即重启，本次会话将中断。')),
            actions: <Widget>[
              TextButton(
                onPressed: () => Navigator.of(dialogContext).pop(false),
                child: Text(tr('取消')),
              ),
              FilledButton(
                onPressed: () => Navigator.of(dialogContext).pop(true),
                child: Text(tr('重启')),
              ),
            ],
          ),
        );
        if (confirmed != true || !mounted) {
          return;
        }
        await _requestHostAction(ViewerHostAction.restart);
        return;
      case ViewerHostAction.lockScreen:
        if (!_status.acceptsLockScreen) {
          return;
        }
        await _requestHostAction(ViewerHostAction.lockScreen);
        return;
      case ViewerHostAction.blockInput:
        if (!_status.acceptsBlockInput) {
          return;
        }
        await _requestHostAction(
          ViewerHostAction.blockInput,
          blockInput: !_status.blockInputActive,
        );
        return;
    }
  }

  Future<void> _requestHostAction(
    ViewerHostAction action, {
    bool blockInput = false,
  }) async {
    try {
      final bool queued = await ViewerBridge.requestHostAction(
        action,
        blockInput: blockInput,
      );
      if (!mounted || queued) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(tr('主机操作未能排队：请确认主机权限与会话状态')),
          duration: const Duration(seconds: 4),
        ),
      );
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(error.message ?? tr('无法执行主机操作'))));
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  /// Asks for the host's OS password and queues it for typing at the host's
  /// logon or lock screen. The host re-checks its desktop right before typing,
  /// so a password offered to an unlocked or UAC desktop is refused instead of
  /// typed. The password is never stored; only the answer comes back through
  /// [ViewerStatus.osLoginUpdates].
  Future<void> _openOsLoginDialog() async {
    if (!_status.acceptsOsLogin) {
      return;
    }
    final TextEditingController passwordController = TextEditingController();
    String? error;
    final String? password = await showDialog<String>(
      context: context,
      builder: (BuildContext dialogContext) {
        return StatefulBuilder(
          builder: (BuildContext context, StateSetter setDialogState) {
            void submit() {
              final String text = passwordController.text;
              if (text.isEmpty) {
                setDialogState(() => error = tr('请输入主机的登录密码'));
                return;
              }
              Navigator.of(dialogContext).pop(text);
            }

            return AlertDialog(
              title: Text(tr('输入主机登录密码')),
              content: SizedBox(
                width: 340,
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: <Widget>[
                    Text(
                      tr('密码会由主机在登录或锁屏界面键入；其他画面下主机会拒绝。密码不会保存或回传。'),
                      style: const TextStyle(
                        fontSize: 12,
                        color: Colors.white54,
                      ),
                    ),
                    const SizedBox(height: 12),
                    TextField(
                      controller: passwordController,
                      autofocus: true,
                      obscureText: true,
                      decoration: InputDecoration(
                        labelText: tr('密码'),
                        errorText: error,
                      ),
                      onSubmitted: (_) => submit(),
                    ),
                  ],
                ),
              ),
              actions: <Widget>[
                TextButton(
                  onPressed: () => Navigator.of(dialogContext).pop(),
                  child: Text(tr('取消')),
                ),
                FilledButton(onPressed: submit, child: Text(tr('输入密码'))),
              ],
            );
          },
        );
      },
    );
    passwordController.dispose();
    if (password == null || password.isEmpty || !mounted) {
      return;
    }
    try {
      final bool queued = await ViewerBridge.requestOsLogin(password);
      if (!mounted || queued) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(tr('系统登录未能排队：请确认主机权限与会话状态')),
          duration: const Duration(seconds: 4),
        ),
      );
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(error.message ?? tr('无法发送登录密码'))));
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  /// Asks the host for its active Windows sessions and lets the operator pick
  /// one. An accepted switch ends this connection on purpose; the page then
  /// reconnects through the ladder and lands in the target session.
  Future<void> _openSessionDialog() async {
    if (!_status.acceptsSessionSelect) {
      return;
    }
    final int baseline = _status.sessionListUpdates;
    try {
      final bool queued = await ViewerBridge.requestSessionList();
      if (!mounted) {
        return;
      }
      if (!queued) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(tr('会话列表未能排队：请确认主机权限与会话状态')),
            duration: const Duration(seconds: 4),
          ),
        );
        return;
      }
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(error.message ?? tr('无法读取主机会话'))));
      return;
    } on MissingPluginException {
      // The native viewer is going away.
      return;
    }
    if (!mounted) {
      return;
    }
    await showDialog<void>(
      context: context,
      builder: (BuildContext dialogContext) {
        return _SessionPickerDialog(initial: _status, baseline: baseline);
      },
    );
  }

  /// Reads the host's exposed policy switches and lets the operator change
  /// them. The dialog keeps polling while it is open: the save is confirmed
  /// only when the host answers with an Ok RemoteConfigState.
  Future<void> _openRemoteConfigDialog() async {
    if (!_status.remoteConfigSupported) {
      return;
    }
    try {
      final bool queued = await ViewerBridge.requestRemoteConfig(0, 0);
      if (!mounted) {
        return;
      }
      if (!queued) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(tr('主机设置未能读取：请确认主机权限与会话状态')),
            duration: const Duration(seconds: 4),
          ),
        );
        return;
      }
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(error.message ?? tr('无法读取主机设置'))));
      return;
    } on MissingPluginException {
      // The native viewer is going away.
      return;
    }
    if (!mounted) {
      return;
    }
    final bool? saved = await showDialog<bool>(
      context: context,
      builder: (BuildContext dialogContext) {
        return _RemoteConfigDialog(initial: _status);
      },
    );
    if (saved == true && mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(tr('主机设置已更新')),
          duration: const Duration(seconds: 3),
        ),
      );
    }
  }

  /// Starts the reconnect ladder after an accepted session switch. The host
  /// ends the old connection itself, so this is not a drop; the service may
  /// still be stopping the old worker, and the ladder covers that window.
  void _scheduleSessionSwitchReconnect() {
    if (_closing || _autoTimer != null) {
      return;
    }
    // Elevation can need time to roll back a child that did not start. Keep
    // the retry count bounded while covering its startup/cleanup deadline.
    _autoPolicy.reset(hostRestart: _elevationReconnectPending);
    // Reserve the first attempt; a failed reconnect continues the ladder at
    // the next backoff instead of starting over at one second.
    _autoPolicy.next();
    setState(() {});
    _autoTimer = Timer(const Duration(milliseconds: 800), () {
      _autoTimer = null;
      if (!mounted || _closing) {
        return;
      }
      unawaited(_reconnect(automatic: true));
    });
  }

  String _hostActionText(int action) {
    if (action == ViewerHostAction.restart.code) {
      return tr('重启主机');
    }
    if (action == ViewerHostAction.lockScreen.code) {
      return tr('锁定主机屏幕');
    }
    if (action == ViewerHostAction.blockInput.code) {
      return tr('阻止本地输入');
    }
    return tr('主机操作');
  }

  String _osLoginCodeText(int code) {
    if (code == ViewerHostActionCode.denied.code) {
      return tr('主机未授权系统登录');
    }
    if (code == ViewerHostActionCode.unsupported.code) {
      return tr('主机当前不在登录或锁屏界面');
    }
    if (code == ViewerHostActionCode.busy.code) {
      return tr('另一查看者正在控制主机');
    }
    if (code == ViewerHostActionCode.failed.code) {
      return tr('主机无法输入密码');
    }
    return tr('主机拒绝了系统登录');
  }

  String _hostActionCodeText(int code) {
    if (code == ViewerHostActionCode.denied.code) {
      return tr('主机未授权该操作');
    }
    if (code == ViewerHostActionCode.unsupported.code) {
      return tr('主机不支持该操作');
    }
    if (code == ViewerHostActionCode.busy.code) {
      return tr('另一查看者正在控制主机');
    }
    if (code == ViewerHostActionCode.failed.code) {
      return tr('主机执行失败');
    }
    return tr('主机拒绝了该操作');
  }

  /// Keeps the open dialog's rule list in step with the engine. The value is
  /// replaced only when something actually changed, so polls do not rebuild
  /// the dialog on every tick.
  void _syncPortForwardRules(List<ViewerPortForwardRule> rules) {
    final List<ViewerPortForwardRule> previous = _portForwardRules.value;
    bool changed = previous.length != rules.length;
    if (!changed) {
      for (int i = 0; i < rules.length; ++i) {
        final ViewerPortForwardRule a = previous[i];
        final ViewerPortForwardRule b = rules[i];
        if (a.ruleId != b.ruleId ||
            a.active != b.active ||
            a.localPort != b.localPort ||
            a.remoteHost != b.remoteHost ||
            a.remotePort != b.remotePort ||
            a.connections != b.connections ||
            a.failures != b.failures ||
            a.bytesUp != b.bytesUp ||
            a.bytesDown != b.bytesDown ||
            a.error != b.error) {
          changed = true;
          break;
        }
      }
    }
    if (changed) {
      _portForwardRules.value = rules;
    }
  }

  /// Opens the port-forward sheet. Each rule owns a 127.0.0.1 listener on the
  /// viewer and one host target, so rules run side by side; the sheet lists
  /// the running rules with their counters and appends a new one.
  Future<void> _openPortForwardDialog() async {
    if (!_status.acceptsPortForward) {
      return;
    }
    final TextEditingController localController = TextEditingController(
      text: '0',
    );
    final TextEditingController hostController = TextEditingController(
      text: '127.0.0.1',
    );
    final TextEditingController remoteController = TextEditingController();
    String? error;
    bool adding = false;
    await showDialog<void>(
      context: context,
      builder: (BuildContext dialogContext) {
        return StatefulBuilder(
          builder: (BuildContext context, StateSetter setDialogState) {
            Future<void> add() async {
              final int? local = int.tryParse(localController.text.trim());
              final int? remote = int.tryParse(remoteController.text.trim());
              final String host = hostController.text.trim();
              if (local == null || local < 0 || local > 65535) {
                setDialogState(() => error = tr('本地端口须为 0–65535（0 表示自动分配）'));
                return;
              }
              if (host.isEmpty ||
                  remote == null ||
                  remote < 1 ||
                  remote > 65535) {
                setDialogState(() => error = tr('请填写目标主机和 1–65535 的目标端口'));
                return;
              }
              setDialogState(() {
                error = null;
                adding = true;
              });
              try {
                final int ruleId = await ViewerBridge.startPortForward(
                  localPort: local,
                  remoteHost: host,
                  remotePort: remote,
                );
                if (!dialogContext.mounted) {
                  return;
                }
                if (ruleId == 0) {
                  setDialogState(() {
                    error = tr('无法开始端口转发：主机未开放该权限');
                    adding = false;
                  });
                  return;
                }
                remoteController.clear();
                setDialogState(() => adding = false);
              } on PlatformException catch (failure) {
                if (!dialogContext.mounted) {
                  return;
                }
                setDialogState(() {
                  error = failure.message ?? tr('端口转发操作失败');
                  adding = false;
                });
              } on MissingPluginException {
                if (dialogContext.mounted) {
                  setDialogState(() => adding = false);
                }
              }
            }

            return AlertDialog(
              title: Text(tr('端口转发')),
              content: SizedBox(
                width: 420,
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    Text(
                      tr(
                        '本机在 127.0.0.1 上监听，主机把每个连入的 TCP 连接转发到目标地址。本地端口填 0 由系统分配。',
                      ),
                      style: const TextStyle(fontSize: 12, height: 1.4),
                    ),
                    const SizedBox(height: 12),
                    ValueListenableBuilder<List<ViewerPortForwardRule>>(
                      valueListenable: _portForwardRules,
                      builder:
                          (
                            BuildContext context,
                            List<ViewerPortForwardRule> rules,
                            Widget? child,
                          ) {
                            if (rules.isEmpty) {
                              return Text(
                                tr('尚无规则，填写下面的目标后开始转发。'),
                                style: const TextStyle(
                                  fontSize: 11.5,
                                  color: Colors.white54,
                                ),
                              );
                            }
                            return ConstrainedBox(
                              constraints: const BoxConstraints(maxHeight: 220),
                              child: SingleChildScrollView(
                                child: Column(
                                  mainAxisSize: MainAxisSize.min,
                                  children: <Widget>[
                                    for (final ViewerPortForwardRule rule
                                        in rules)
                                      _buildPortForwardRuleRow(rule),
                                  ],
                                ),
                              ),
                            );
                          },
                    ),
                    const SizedBox(height: 14),
                    Text(
                      tr('添加规则'),
                      style: const TextStyle(
                        fontSize: 12.5,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                    const SizedBox(height: 8),
                    TextField(
                      controller: localController,
                      keyboardType: TextInputType.number,
                      decoration: InputDecoration(
                        labelText: tr('本地端口（127.0.0.1）'),
                        isDense: true,
                      ),
                    ),
                    const SizedBox(height: 10),
                    TextField(
                      controller: hostController,
                      decoration: InputDecoration(
                        labelText: tr('目标主机'),
                        isDense: true,
                      ),
                    ),
                    const SizedBox(height: 10),
                    TextField(
                      controller: remoteController,
                      keyboardType: TextInputType.number,
                      decoration: InputDecoration(
                        labelText: tr('目标端口'),
                        isDense: true,
                      ),
                    ),
                    const SizedBox(height: 8),
                    Text(
                      tr('最多同时运行 8 条规则。'),
                      style: const TextStyle(
                        fontSize: 11,
                        color: Colors.white38,
                      ),
                    ),
                    if (error != null) ...<Widget>[
                      const SizedBox(height: 10),
                      Text(
                        error!,
                        style: TextStyle(
                          fontSize: 12,
                          color: Theme.of(context).colorScheme.error,
                        ),
                      ),
                    ],
                  ],
                ),
              ),
              actions: <Widget>[
                TextButton(
                  onPressed: () => Navigator.of(dialogContext).pop(),
                  child: Text(tr('取消')),
                ),
                FilledButton(
                  onPressed: adding ? null : () => unawaited(add()),
                  child: Text(tr('添加规则')),
                ),
              ],
            );
          },
        );
      },
    );
    localController.dispose();
    hostController.dispose();
    remoteController.dispose();
  }

  Future<void> _stopPortForwardRule(int ruleId) async {
    try {
      await ViewerBridge.stopPortForward(ruleId: ruleId);
    } on PlatformException catch (failure) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(failure.message ?? tr('端口转发操作失败'))),
      );
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  /// One rule row: the listener, the target, its state and counters, and the
  /// button that stops just this rule.
  Widget _buildPortForwardRuleRow(ViewerPortForwardRule rule) {
    final String state = rule.active
        ? tr('转发中')
        : (rule.error.isNotEmpty ? rule.error : tr('等待主机确认'));
    return Container(
      margin: const EdgeInsets.only(bottom: 8),
      padding: const EdgeInsets.fromLTRB(12, 8, 6, 8),
      decoration: BoxDecoration(
        color: Colors.white10,
        borderRadius: BorderRadius.circular(8),
      ),
      child: Row(
        children: <Widget>[
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(
                  tr('127.0.0.1:{p7} → {p8}:{p9}', <String, Object?>{
                    'p7': rule.localPort,
                    'p8': rule.remoteHost,
                    'p9': rule.remotePort,
                  }),
                  style: const TextStyle(color: Colors.white, fontSize: 12.5),
                ),
                const SizedBox(height: 2),
                Text(
                  state,
                  style: TextStyle(
                    fontSize: 11,
                    color: rule.error.isNotEmpty
                        ? Theme.of(context).colorScheme.error
                        : Colors.white54,
                  ),
                ),
                if (rule.active) ...<Widget>[
                  const SizedBox(height: 2),
                  Text(
                    tr(
                      '连接 {p10} · 失败 {p11} · 上行 {p12} · 下行 {p13}',
                      <String, Object?>{
                        'p10': rule.connections,
                        'p11': rule.failures,
                        'p12': _formatBytes(rule.bytesUp),
                        'p13': _formatBytes(rule.bytesDown),
                      },
                    ),
                    style: const TextStyle(
                      fontSize: 10.5,
                      color: Colors.white38,
                    ),
                  ),
                ],
              ],
            ),
          ),
          IconButton(
            icon: const Icon(Icons.link_off_rounded, size: 18),
            tooltip: tr('停止规则'),
            onPressed: () => unawaited(_stopPortForwardRule(rule.ruleId)),
          ),
        ],
      ),
    );
  }

  /// The host's PrivacyModeCode (chast::PrivacyModeCode) in the page's words.
  String _privacyCodeText(int code) {
    switch (code) {
      case 1:
        return tr('主机不允许隐私模式');
      case 2:
        return tr('主机系统不支持隐私模式');
      case 3:
        return tr('主机无法开启隐私模式');
      default:
        return tr('隐私模式未能切换');
    }
  }

  /// Reads the live endpoint list for the toolbar's long-press menu, so a
  /// device plugged in mid-session is offered too.
  Future<List<ViewerMicrophoneDevice>> _loadMicrophones() async {
    try {
      final List<ViewerMicrophoneDevice> devices =
          await ViewerBridge.listMicrophones();
      if (devices.isEmpty && mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(tr('没有可用的麦克风设备')),
            duration: Duration(seconds: 4),
          ),
        );
      }
      return devices;
    } on PlatformException catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(error.message ?? tr('无法列出麦克风设备'))),
        );
      }
      return const <ViewerMicrophoneDevice>[];
    } on MissingPluginException {
      return const <ViewerMicrophoneDevice>[];
    }
  }

  Future<void> _selectMicrophone(String deviceId) async {
    try {
      await ViewerBridge.selectMicrophone(deviceId);
    } on PlatformException {
      // The session is closing; the next poll reports the final state.
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  /// Reads the live playback endpoints for the volume button's long-press
  /// menu, so a device plugged in mid-session is offered too.
  Future<List<ViewerAudioOutput>> _loadAudioOutputs() async {
    try {
      final List<ViewerAudioOutput> devices =
          await ViewerBridge.listAudioOutputs();
      if (devices.isEmpty && mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(tr('没有可用的音频输出设备')),
            duration: Duration(seconds: 4),
          ),
        );
      }
      return devices;
    } on PlatformException catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(error.message ?? tr('无法列出音频输出设备'))),
        );
      }
      return const <ViewerAudioOutput>[];
    } on MissingPluginException {
      return const <ViewerAudioOutput>[];
    }
  }

  Future<void> _selectAudioOutput(String deviceId) async {
    try {
      final bool applied = await ViewerBridge.selectAudioOutput(deviceId);
      if (!mounted || applied) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(tr('当前会话无法切换输出设备')),
          duration: Duration(seconds: 4),
        ),
      );
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(error.message ?? tr('无法切换音频输出'))));
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  Future<void> _setAudioProcessing(bool aec, bool noiseSuppression) async {
    try {
      final bool applied = await ViewerBridge.setAudioProcessing(
        aec: aec,
        noiseSuppression: noiseSuppression,
      );
      if (!mounted || applied) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(tr('当前会话无法修改麦克风处理')),
          duration: Duration(seconds: 4),
        ),
      );
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(error.message ?? tr('无法修改麦克风处理'))));
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  /// The output the toolbar should mark. A switch in flight is shown as
  /// selected while the host works; a merged stream has no single streamed
  /// entry, so the merge item stays highlighted; otherwise an explicit
  /// request is authoritative, and after "server chooses" the list is
  /// matched against the stream's origin and size, which is how HelloAck
  /// identifies the captured output.
  int get _activeDisplay {
    if (_independentDisplays) {
      if (_displayStreamLayout == _DisplayStreamLayout.desktop) {
        return kViewerDisplayAll;
      }
      if (_displayStreamLayout == _DisplayStreamLayout.grid) {
        return -3;
      }
      final streams = _displayStreams!.state.streams;
      return streams.length == 1
          ? streams.single.displayIndex
          : kViewerDisplayAuto;
    }
    if (_pendingDisplay != null) {
      return _pendingDisplay!;
    }
    if (_status.mergedLayout) {
      return kViewerDisplayAll;
    }
    if (_display != kViewerDisplayAuto) {
      return _display;
    }
    return _status.streamedDisplayIndex ?? kViewerDisplayAuto;
  }

  bool get _followCursor =>
      _status.displayFollowFlags & ViewerDisplayFollow.cursor != 0;

  bool get _followWindow =>
      _status.displayFollowFlags & ViewerDisplayFollow.window != 0;

  /// Hands one or both follow modes to the host. With follow on, the output
  /// choice belongs to the host, so the local picker returns to "server
  /// chooses" and the active marker tracks whatever output starts streaming.
  Future<bool> _setFollow({required bool cursor, required bool window}) async {
    bool queued = false;
    try {
      queued = await ViewerBridge.setDisplayFollow(
        cursor: cursor,
        window: window,
      );
    } on PlatformException {
      queued = false;
    } on MissingPluginException {
      queued = false;
    }
    if (!mounted) {
      return false;
    }
    if (queued && (cursor || window)) {
      setState(() {
        _display = kViewerDisplayAuto;
        _pendingDisplay = null;
      });
    }
    if (!queued) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(tr('无法设置跟随：会话未在串流，或主机未开放输入')),
          duration: const Duration(seconds: 3),
        ),
      );
    }
    return queued;
  }

  void _toggleFollowCursor() {
    unawaited(_setFollow(cursor: !_followCursor, window: _followWindow));
  }

  void _toggleFollowWindow() {
    unawaited(_setFollow(cursor: _followCursor, window: !_followWindow));
  }

  Future<void> _switchDisplay(int display) async {
    if (_reconnecting) {
      return;
    }
    if (display == kViewerDisplayAll) {
      await _selectDisplayStreams(
        _status.displays.map((d) => d.index).toList(),
        layout: _DisplayStreamLayout.desktop,
        reconnectDisplay: kViewerDisplayAuto,
      );
      return;
    }
    if (_displayStreams != null) {
      final int selected = display == kViewerDisplayAuto
          ? _defaultDisplayIndex()
          : display;
      if (selected >= 0) {
        await _selectDisplayStreams(
          [selected],
          layout: _DisplayStreamLayout.single,
          reconnectDisplay: display,
        );
      }
      return;
    }
    // A manual choice takes the host's automatic following out of the loop;
    // otherwise the host would switch back on its next poll.
    if (_followCursor || _followWindow) {
      await _setFollow(cursor: false, window: false);
      if (!mounted) {
        return;
      }
    }
    // Tapping the output that is already shown is not a request.
    if (display == _activeDisplay) {
      return;
    }
    // Selecting this connection's display also works for view-only sessions.
    if (_status.isLive && !_over) {
      setState(() => _pendingDisplay = display);
      bool queued = false;
      try {
        queued = await ViewerBridge.switchDisplay(display);
      } on PlatformException {
        queued = false;
      } on MissingPluginException {
        queued = false;
      }
      if (!mounted) {
        return;
      }
      if (queued) {
        return;
      }
      setState(() => _pendingDisplay = null);
    }
    if (display == _display) {
      return;
    }
    _display = display;
    await _reconnect();
  }

  void _createDisplayStreams() {
    _displayStreams = DisplayStreamController()
      ..addListener(_displayStreamsChanged);
  }

  void _displayStreamsChanged() {
    if (!mounted || _closing || _reconnecting) return;
    final controller = _displayStreams;
    if (controller == null) return;
    if (controller.state.active && !controller.busy) {
      _displaySelection = controller.state.streams
          .map((s) => s.displayIndex)
          .toList();
      _displayStreamLayout ??= controller.state.streams.length == 1
          ? _DisplayStreamLayout.single
          : _DisplayStreamLayout.grid;
    }
    setState(() {});
    _syncCursor();
  }

  int _defaultDisplayIndex() {
    final remembered = _serverDefaultDisplay;
    if (remembered != null &&
        _status.displays.any((display) => display.index == remembered)) {
      return remembered;
    }
    for (final display in _status.displays) {
      if (display.primary) return display.index;
    }
    return _status.displays.isEmpty ? -1 : _status.displays.first.index;
  }

  Future<void> _selectDisplayStreams(
    List<int> displays, {
    required _DisplayStreamLayout layout,
    int? reconnectDisplay,
  }) async {
    if (!_status.isLive || _reconnecting || _closing) return;
    if (_followCursor || _followWindow) {
      if (!await _setFollow(cursor: false, window: false)) return;
    }
    if (!mounted || _closing || _reconnecting) return;
    if (_relativeMouse) _toggleRelativeMouse();
    if (_displayStreams == null) _createDisplayStreams();
    final controller = _displayStreams!;
    final available = _status.displays.map((display) => display.index).toSet();
    final normalized = displays.where(available.contains).toSet().toList()
      ..sort();
    final current = controller.state.streams
        .map((stream) => stream.displayIndex)
        .toSet();
    if (controller.state.active &&
        current.length == normalized.length &&
        current.containsAll(normalized)) {
      setState(() {
        _displayStreamLayout = layout;
        _displaySelection = normalized;
        if (reconnectDisplay != null) _display = reconnectDisplay;
      });
      return;
    }
    final accepted = await controller.select(normalized);
    if (accepted && mounted && controller == _displayStreams) {
      setState(() {
        _displayStreamLayout = layout;
        _displaySelection = normalized;
        if (reconnectDisplay != null) _display = reconnectDisplay;
      });
    }
    await controller.refresh();
    if (!accepted &&
        mounted &&
        !_closing &&
        controller == _displayStreams &&
        controller.error.isNotEmpty &&
        !_independentDisplays) {
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(controller.error)));
    }
  }

  void _focusDisplay(int id) {
    _focus.requestFocus();
    unawaited(_displayStreams?.focus(id));
  }

  Widget _buildIndependentDisplays() {
    final controller = _displayStreams!;
    final textures = controller.textures;
    return Padding(
      padding: const EdgeInsets.only(top: 64, bottom: 12, left: 12, right: 12),
      child: Column(
        children: [
          if (controller.error.isNotEmpty)
            Text(
              controller.error,
              style: const TextStyle(color: Colors.orangeAccent),
            ),
          Expanded(
            child: textures.isEmpty
                ? Center(
                    child: Text(
                      tr('未选择显示器，请从显示器菜单添加'),
                      style: const TextStyle(color: Colors.white70),
                    ),
                  )
                : LayoutBuilder(
                    builder: (context, constraints) {
                      final columns =
                          textures.length > 1 && constraints.maxWidth >= 600
                          ? 2
                          : 1;
                      final rows = (textures.length / columns).ceil();
                      return GridView.count(
                        crossAxisCount: columns,
                        mainAxisSpacing: 8,
                        crossAxisSpacing: 8,
                        childAspectRatio:
                            ((constraints.maxWidth - (columns - 1) * 8) /
                                columns) /
                            ((constraints.maxHeight / rows).clamp(
                              180.0,
                              double.infinity,
                            )),
                        children: [
                          for (final texture in textures)
                            _buildIndependentDisplay(texture),
                        ],
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }

  Widget _buildIndependentDisplay(DisplayTexture texture) {
    final stream = texture.stream;
    final controller = _displayStreams!;
    return DisplayStreamView(
      key: ValueKey(stream.streamId),
      texture: texture,
      focused: controller.focused == stream.streamId,
      onFocus: () => _focusDisplay(stream.streamId),
      onClose: controller.busy
          ? null
          : () => unawaited(
              _selectDisplayStreams([
                for (final other in controller.state.streams)
                  if (other.streamId != stream.streamId) other.displayIndex,
              ], layout: _DisplayStreamLayout.grid),
            ),
      view: _buildDisplayRemoteView(texture),
    );
  }

  Widget _buildDisplayRemoteView(DisplayTexture texture, {ViewFit? fit}) {
    final stream = texture.stream;
    return ListenableBuilder(
      listenable: texture,
      builder: (context, _) => RemoteView(
        key: ValueKey('display-texture-${stream.streamId}'),
        textureId: texture.textureId,
        videoWidth: stream.width,
        videoHeight: stream.height,
        sourceWidth: stream.sourceWidth,
        sourceHeight: stream.sourceHeight,
        fit: fit ?? _fit,
        captureInput: _inputActive,
        hideCursor:
            texture.pointer?.visible == true &&
            texture.cursor != null &&
            texture.cursorShape == texture.pointer?.shapeId,
        overlayBuilder: (size) => displayCursorOverlay(texture, size),
        onFocusRequested: () => _focusDisplay(stream.streamId),
        onHover: (p, b) => _onPointerHover(p, b, display: stream),
        onMove: (p, b) => _onPointerMove(p, b, display: stream),
        onButton: (p, b, down) => _onPointerButton(p, b, down, display: stream),
        onScroll: (p, d) => _onPointerScroll(p, d, display: stream),
        onGesture: (p, zoom, x, y) =>
            _onGesture(p, zoom, x, y, display: stream),
        onDevicePointer: (e, p) => _onDevicePointer(e, p, display: stream),
        onDeviceCancel: (e, p) => _onDeviceCancel(e, p, display: stream),
        onCursorHint: (p) => _onCursorHint(p, display: stream),
        onCursorHintPress: (p) => _onCursorHintPress(p, display: stream),
        onCursorHintExit: _hideCursorHint,
      ),
    );
  }

  Widget _buildSingleDisplayStream() {
    final controller = _displayStreams!;
    final texture =
        controller.selected ??
        (controller.textures.isEmpty ? null : controller.textures.first);
    if (texture == null) {
      return Center(
        child: Text(
          tr('未选择显示器，请从显示器菜单添加'),
          style: const TextStyle(color: Colors.white70),
        ),
      );
    }
    return _buildDisplayRemoteView(texture);
  }

  Widget _buildDesktopDisplays() {
    final controller = _displayStreams!;
    final textures = controller.textures;
    if (textures.isEmpty) {
      return Center(
        child: Text(
          tr('未选择显示器，请从显示器菜单添加'),
          style: const TextStyle(color: Colors.white70),
        ),
      );
    }
    int left = textures.first.stream.originX;
    int top = textures.first.stream.originY;
    int right = left + _streamSourceWidth(textures.first.stream);
    int bottom = top + _streamSourceHeight(textures.first.stream);
    for (final texture in textures.skip(1)) {
      final stream = texture.stream;
      left = math.min(left, stream.originX);
      top = math.min(top, stream.originY);
      right = math.max(right, stream.originX + _streamSourceWidth(stream));
      bottom = math.max(bottom, stream.originY + _streamSourceHeight(stream));
    }
    final double desktopWidth = math.max(1, right - left).toDouble();
    final double desktopHeight = math.max(1, bottom - top).toDouble();
    return Padding(
      padding: const EdgeInsets.only(top: 64),
      child: LayoutBuilder(
        builder: (context, constraints) {
          final available = constraints.biggest;
          final double scale = _fit == ViewFit.adaptive
              ? math.min(
                  available.width / desktopWidth,
                  available.height / desktopHeight,
                )
              : 1.0;
          final size = Size(desktopWidth * scale, desktopHeight * scale);
          final canvas = SizedBox.fromSize(
            size: size,
            child: Stack(
              clipBehavior: Clip.hardEdge,
              children: [
                for (final texture in textures)
                  Positioned(
                    left: (texture.stream.originX - left) * scale,
                    top: (texture.stream.originY - top) * scale,
                    width: _streamSourceWidth(texture.stream) * scale,
                    height: _streamSourceHeight(texture.stream) * scale,
                    child: DecoratedBox(
                      decoration: BoxDecoration(
                        border: Border.all(
                          color: controller.focused == texture.stream.streamId
                              ? Colors.lightBlueAccent
                              : Colors.black54,
                        ),
                      ),
                      child: texture.stream.ready
                          ? _buildDisplayRemoteView(
                              texture,
                              fit: ViewFit.adaptive,
                            )
                          : const ColoredBox(color: Color(0xff18191d)),
                    ),
                  ),
              ],
            ),
          );
          if (_fit == ViewFit.adaptive) return Center(child: canvas);
          if (_inputActive) {
            return ClipRect(
              child: OverflowBox(
                alignment: Alignment.topLeft,
                minWidth: size.width,
                maxWidth: size.width,
                minHeight: size.height,
                maxHeight: size.height,
                child: canvas,
              ),
            );
          }
          return SingleChildScrollView(
            child: SingleChildScrollView(
              scrollDirection: Axis.horizontal,
              child: canvas,
            ),
          );
        },
      ),
    );
  }

  int _streamSourceWidth(DisplayVideoStream stream) =>
      stream.sourceWidth > 0 ? stream.sourceWidth : stream.width;

  int _streamSourceHeight(DisplayVideoStream stream) =>
      stream.sourceHeight > 0 ? stream.sourceHeight : stream.height;

  /// Adds the display to this connection and opens a view of its existing
  /// decoder. The runner shares the native session without connection secrets.
  Future<void> _openDisplayWindow(int display) async {
    if (_openingDisplayWindow || !_status.isLive || _closing || _reconnecting) {
      return;
    }
    _openingDisplayWindow = true;
    try {
      final selected =
          _displayStreams?.state.streams.map((s) => s.displayIndex).toSet() ??
          <int>{};
      if (selected.isEmpty) {
        final primary = _status.streamedDisplayIndex ?? _defaultDisplayIndex();
        if (primary >= 0) selected.add(primary);
      }
      selected.add(display);
      await _selectDisplayStreams(
        selected.toList(),
        layout: _displayStreamLayout ?? _DisplayStreamLayout.single,
      );
      final controller = _displayStreams;
      if (controller == null) return;
      final deadline = DateTime.now().add(const Duration(seconds: 10));
      while (mounted &&
          !_closing &&
          !_reconnecting &&
          controller == _displayStreams) {
        await controller.refresh();
        if (!mounted ||
            _closing ||
            _reconnecting ||
            controller != _displayStreams) {
          return;
        }
        final stream = controller.state.streams
            .where((s) => s.displayIndex == display)
            .firstOrNull;
        if (stream != null &&
            await DisplayStreamBridge.openWindow(
              stream.streamId,
              peerId: _prefsPeerId,
            )) {
          return;
        }
        if (!controller.busy || DateTime.now().isAfter(deadline)) break;
        await Future<void>.delayed(const Duration(milliseconds: 100));
      }
    } on PlatformException {
      // A stopped session or exhausted window limit is reported below.
    } on MissingPluginException {
      // Older runners do not support shared display windows.
    } finally {
      _openingDisplayWindow = false;
    }
    if (!mounted || _closing || _reconnecting) return;
    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text(tr('无法打开会话窗口'))));
  }

  /// Fetches the mode list for the streamed output once the session is live.
  /// One query is in flight at a time; a list already covering the displayed
  /// output is reused until an accepted change makes its current marker stale.
  void _maybeRequestModes(ViewerStatus status, bool nowOver) {
    if (nowOver || _over || !status.acceptsInput) {
      return;
    }
    final int? streamed = status.streamedDisplayIndex;
    if (streamed == null) {
      return;
    }
    if (_modesRefreshPending &&
        status.displayModeLists > _modesRequestedLists) {
      _modesRefreshPending = false;
    }
    // The display fields start at display 0 with an empty list, which would
    // otherwise look like a loaded answer for the common single-display case.
    if (status.displayModesDisplay == streamed &&
        status.displayModeLists > 0 &&
        !_modesRefreshPending) {
      return;
    }
    final DateTime now = DateTime.now();
    if (_modesRequestedFor == streamed &&
        now.difference(_modesRequestedAt) < const Duration(seconds: 2)) {
      return;
    }
    if (now.difference(_modesRequestedAt) < const Duration(seconds: 1)) {
      return;
    }
    _modesRequestedFor = streamed;
    _modesRequestedLists = status.displayModeLists;
    _modesRequestedAt = now;
    unawaited(_requestModes(streamed));
  }

  Future<void> _requestModes(int display) async {
    try {
      await ViewerBridge.requestDisplayModes(display);
    } on PlatformException {
      // The next poll retries while the stream stays live.
    } on MissingPluginException {
      // Nothing to talk to.
    }
  }

  Future<void> _changeDisplayMode(ViewerDisplayMode mode) async {
    final int? display = _status.streamedDisplayIndex;
    if (display == null || !_status.acceptsInput || _over) {
      return;
    }
    bool queued = false;
    try {
      queued = await ViewerBridge.setDisplayMode(
        display: display,
        width: mode.width,
        height: mode.height,
        refresh: mode.refresh,
      );
    } on PlatformException {
      queued = false;
    } on MissingPluginException {
      queued = false;
    }
    if (!mounted) {
      return;
    }
    if (!queued) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(tr('无法切换分辨率：会话未在串流，或主机未开放输入')),
          duration: Duration(seconds: 3),
        ),
      );
    }
  }

  /// Applies one menu choice. The selection moves immediately; the host's
  /// answer either confirms it or restores the previous one. No input
  /// permission is needed, so this works in a view-only session.
  Future<void> _changeStreamOptions({
    ViewerStreamCodec? codec,
    ViewerStreamQuality? quality,
    int? qualityPercent,
    int? maxFps,
    bool? fullColor,
  }) async {
    final bool nextFullColor =
        fullColor ??
        (codec != null && codec != ViewerStreamCodec.vp9
            ? false
            : _streamFullColor);
    final ViewerStreamCodec nextCodec = fullColor == true
        ? ViewerStreamCodec.vp9
        : codec ?? _streamCodec;
    final ViewerStreamQuality nextQuality = quality ?? _streamQuality;
    final int nextQualityPercent = qualityPercent ?? _streamQualityPercent;
    final int nextFps = maxFps ?? _streamMaxFps;
    final bool customPercentChanged =
        nextQuality == ViewerStreamQuality.custom &&
        nextQualityPercent != _streamQualityPercent;
    if (nextCodec == _streamCodec &&
        nextQuality == _streamQuality &&
        nextFps == _streamMaxFps &&
        nextFullColor == _streamFullColor &&
        !customPercentChanged) {
      return;
    }
    final _StreamOptionSnapshot previous = _StreamOptionSnapshot(
      _streamCodec,
      _streamQuality,
      _streamQualityPercent,
      _streamMaxFps,
      _streamFullColor,
    );
    setState(() {
      _streamCodec = nextCodec;
      _streamQuality = nextQuality;
      _streamQualityPercent = nextQualityPercent;
      _streamMaxFps = nextFps;
      _streamFullColor = nextFullColor;
      _streamRollback = previous;
    });
    // Count the request before awaiting the platform reply: the host's answer
    // can be seen by a poll before this continuation resumes.
    ++_streamPending;
    bool queued = false;
    try {
      queued = await ViewerBridge.setStreamOptions(
        codec: nextCodec.code,
        maxFps: nextFps,
        quality: nextQuality.code,
        qualityPercent: nextQuality == ViewerStreamQuality.custom
            ? nextQualityPercent
            : 0,
        chroma: nextFullColor ? 1 : 0,
      );
    } on PlatformException {
      queued = false;
    } on MissingPluginException {
      queued = false;
    }
    if (!mounted) {
      return;
    }
    if (queued) {
      return;
    }
    // Nothing was sent, so no answer will arrive; put the menu back now.
    --_streamPending;
    setState(() {
      _streamCodec = previous.codec;
      _streamQuality = previous.quality;
      _streamQualityPercent = previous.qualityPercent;
      _streamMaxFps = previous.maxFps;
      _streamFullColor = previous.fullColor;
      _streamRollback = null;
    });
    _saveStreamPreferences();
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(tr('当前无法修改画面设置')), duration: Duration(seconds: 3)),
    );
  }

  /// Only the settled values are written: a choice the host refused must not
  /// reappear as the remembered one on the next launch.
  void _saveStreamPreferences() {
    StreamPreferences.save(
      StreamPreferences(
        codec: _streamCodec.code,
        quality: _streamQuality.code,
        qualityPercent: _streamQualityPercent,
        fullColor: _streamFullColor,
        maxFps: _streamMaxFps,
      ),
      peerId: _prefsPeerId,
    );
  }

  /// True when the server proved a different identity than the one stored for
  /// this endpoint. The native side refuses that connection; the card then
  /// offers to replace the pin explicitly.
  bool get _identityChanged =>
      _pinnedFingerprint.isNotEmpty &&
      _status.serverFingerprint.isNotEmpty &&
      _status.serverFingerprint != _pinnedFingerprint;

  /// TOFU for the host certificate. The native side has already completed the
  /// unpinned handshake, so the user sees the fingerprint before it is
  /// remembered; rejecting ends the session instead of leaving it unverified.
  Future<void> _confirmFirstFingerprint(String fingerprint) async {
    final bool accepted =
        await showDialog<bool>(
          context: context,
          barrierDismissible: false,
          builder: (BuildContext context) => _HostFingerprintDialog(
            host: widget.target.label,
            fingerprint: fingerprint,
          ),
        ) ??
        false;
    if (!mounted) {
      return;
    }
    if (!accepted) {
      await _stopSession();
      if (!mounted) {
        return;
      }
      setState(() {
        _pinnedFingerprint = '';
        _fingerprintPrompted = false;
        _fingerprintUnpersisted = false;
        _startError = tr('未确认主机证书指纹，已终止连接');
      });
      return;
    }
    final String saved = await _trustStore.trust(
      widget.target.trustKey,
      fingerprint,
    );
    if (!mounted) {
      return;
    }
    setState(() {
      _pinnedFingerprint = fingerprint;
      _fingerprintUnpersisted = saved.isEmpty;
    });
    if (saved.isEmpty) {
      _showFingerprintWarning();
    }
  }

  void _showFingerprintWarning() {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(tr('无法保存主机指纹，本次连接不会固定证书；下次连接需要重新确认。')),
        duration: const Duration(seconds: 6),
      ),
    );
  }

  Future<void> _trustNewIdentity() async {
    final String fresh = _status.serverFingerprint;
    if (fresh.isEmpty || _reconnecting) {
      return;
    }
    final String saved = await _trustStore.trust(widget.target.trustKey, fresh);
    if (!mounted) {
      return;
    }
    setState(() {
      _pinnedFingerprint = fresh;
      _fingerprintUnpersisted = saved.isEmpty;
    });
    if (saved.isEmpty) {
      _showFingerprintWarning();
    }
    await _reconnect();
  }

  /// Explains a live grant change. Input loss has already dropped control and
  /// the host lifts the keys it still holds, so this only tells the user why.
  void _notifyPermissionChange(int before, ViewerStatus status) {
    final int lost = before & ~status.permissions;
    final int gained = status.permissions & ~before;
    String names(int mask) => viewerPermissionNames(
      mask,
      translate: (String source) => tr(source),
    ).join(AppLanguage.isEnglish ? ', ' : '、');

    final String lostNames = names(lost);
    final String gainedNames = names(gained);
    final String text;
    if (lostNames.isNotEmpty && gainedNames.isNotEmpty) {
      text = tr('主机已收回 {lostNames}，并授予 {gainedNames}。', <String, Object?>{
        'lostNames': lostNames,
        'gainedNames': gainedNames,
      });
    } else if (lostNames.isNotEmpty) {
      text = tr('主机已收回 {lostNames}。', <String, Object?>{
        'lostNames': lostNames,
      });
    } else if (gainedNames.isNotEmpty) {
      text = tr('主机已授予 {gainedNames}。', <String, Object?>{
        'gainedNames': gainedNames,
      });
    } else {
      return;
    }
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(text), duration: const Duration(seconds: 4)),
    );
  }

  /// Collects the credential the host asked for and reconnects with it. A
  /// password challenge also offers to keep the password in this Windows
  /// account's DPAPI store; a two-factor challenge offers to trust the device.
  Future<void> _handleCredentialChallenge(int denial) async {
    if (_authPromptOpen || !mounted) {
      return;
    }
    _authPromptOpen = true;
    final bool passwordStep =
        denial == ViewerDenial.passwordRequired ||
        denial == ViewerDenial.wrongPassword;
    final _CredentialResult? entered = await showDialog<_CredentialResult>(
      context: context,
      barrierDismissible: false,
      builder: (BuildContext context) => _CredentialDialog(
        passwordStep: passwordStep,
        initialPassword: _password,
        initialRemember: passwordStep ? _rememberPassword : _trustDevice,
        detail: accessDenialText(denial),
      ),
    );
    _authPromptOpen = false;
    if (!mounted) {
      return;
    }
    if (entered == null) {
      // Backing out is an ordinary end: the card explains the refusal and
      // offers a retry, which asks again.
      final String detail = accessDenialText(denial);
      setState(() {
        _over = true;
        _startError = detail.isEmpty ? tr('身份验证未完成') : detail;
      });
      _pollTimer?.cancel();
      _clearCursor();
      return;
    }
    if (passwordStep) {
      _password = entered.text;
      // The host checks the password before the code; a new password needs a
      // new code.
      _totp = '';
      _rememberPassword = entered.remember;
      try {
        await ViewerBridge.savePassword(
          widget.target.trustKey,
          entered.remember ? entered.text : '',
        );
      } on PlatformException {
        // The connection still works with the password held in memory.
      } on MissingPluginException {
        // Same.
      }
    } else {
      _totp = entered.text;
      _trustDevice = entered.remember;
    }
    if (!mounted) {
      return;
    }
    await _reconnect();
  }

  /// Ends an automatic-reconnect series. A manual reconnect or a display
  /// switch also resets the attempt count, so the next drop starts its own
  /// 1/2/4/8/15 s ladder.
  void _cancelAutoReconnect() {
    _autoTimer?.cancel();
    _autoTimer = null;
    _autoPolicy.reset();
  }

  /// Arms the next automatic attempt after an unexpected failure. The first
  /// attempt needs a frame from this connection; later attempts keep the
  /// series alive even if the host stays down and no frame arrives.
  void _maybeStartAutoReconnect(ViewerStatus status) {
    if (_closing || _autoTimer != null) {
      return;
    }
    final bool eligible =
        _startError == null &&
        status.state == ViewerState.failed &&
        status.denial == 0 &&
        !_identityChanged;
    if (!eligible) {
      _cancelAutoReconnect();
      return;
    }
    final bool seriesActive = _autoPolicy.waiting && _autoPolicy.attempt > 0;
    if (!seriesActive && !_sawFrame) {
      return;
    }
    _scheduleAutoReconnect(_autoPolicy.next());
  }

  void _scheduleAutoReconnect(Duration? delay) {
    if (!mounted || _closing || _autoTimer != null) {
      return;
    }
    if (delay == null) {
      // The ladder is exhausted: fall back to the normal card, whose manual
      // reconnect starts a fresh series.
      setState(() {});
      return;
    }
    setState(() {});
    _autoTimer = Timer(delay, () {
      _autoTimer = null;
      if (!mounted || _closing) {
        return;
      }
      unawaited(_reconnect(automatic: true));
    });
  }

  Future<void> _reconnect({bool automatic = false}) async {
    if (_reconnecting || _closing) {
      return;
    }
    if (!automatic) {
      _cancelAutoReconnect();
    }
    // A persistent console outlives the widgets below; keep its tab open so it
    // attaches again once the new session is streaming. A session console dies
    // with the old session, so only slots with a service id come back.
    final List<int> resumeTabs = <int>[
      for (int slot = 0; slot < kViewerMaxTerminals; slot++)
        if (_terminalServiceIds[slot] != 0) slot + 1,
    ];
    final bool resumeTerminal = _terminalOpen && resumeTabs.isNotEmpty;
    setState(() {
      _reconnecting = true;
      _startError = null;
      _challengeDenial = 0;
    });
    _stopPolling();
    _hideTimer?.cancel();
    if (_controlEnabled) {
      // The session is about to be torn down; lift what this page still holds
      // so the short window before Stop() does not leave a key down.
      _releaseRemoteInput();
    }
    await _starting;
    _teardown = null;
    // A manual reconnect ends a live session; report it before the teardown.
    final DateTime? auditStartedAt = _auditStartedAt;
    _auditStartedAt = null;
    if (auditStartedAt != null) {
      unawaited(_reportFinishedSession(auditStartedAt, tr('重新连接')));
    }
    await _stopSession();
    // Teardown fails queued uploads and an unfinished download; drain those
    // rows while the old audit reference and route are still current.
    await _reportCompletedFiles();
    await _stopTunnel();
    _clearCursor();
    if (!mounted || _closing) {
      return;
    }
    setState(() {
      _status = const ViewerStatus();
      _sawFrame = false;
      _over = false;
      _collapsed = false;
      _reconnecting = false;
      _controlEnabled = false;
      _pendingDisplay = null;
      _serverDefaultDisplay = null;
      _wheelX = 0;
      _wheelY = 0;
      _heldButtons = 0;
      _heldKeys.clear();
      _contacts.clear();
      _penPointer = null;
      _chatEntries.clear();
      _chatOpen = false;
      _chatTyping = false;
      _terminalOpen = resumeTerminal;
      _terminalTyping = false;
      _terminalTabIds = resumeTerminal ? resumeTabs : <int>[1];
      if (!resumeTerminal) {
        _terminalServiceIds.fillRange(0, kViewerMaxTerminals, 0);
      }
      _cameraOpen = false;
      _fileBrowserOpen = false;
    });
    if (_chatInputFocus.hasFocus) {
      _chatInputFocus.unfocus();
    }
    if (_terminalFocus.hasFocus) {
      _terminalFocus.unfocus();
    }
    _setKeyCapture(false);
    _starting = _start();
    await _starting;
  }

  /// Collects an administrator login and a stated reason, then retries the
  /// connection through an explicit override. The server keeps the permission
  /// ceiling and host local authentication, so a refusal (non-admin account,
  /// unmanaged device, ...) comes back through the tunnel and is shown as the
  /// normal start error instead of silently downgrading.
  Future<void> _promptEmergencyAccess() async {
    if (_reconnecting || _closing) {
      return;
    }
    final _EmergencyAccessResult? result =
        await showDialog<_EmergencyAccessResult>(
          context: context,
          barrierDismissible: false,
          builder: (BuildContext context) => _EmergencyAccessDialog(
            server: widget.target.server,
            proxy: widget.settings.proxy,
          ),
        );
    if (result == null || !mounted || _closing) {
      return;
    }
    setState(() {
      _emergencyAdmin = result.token;
      _emergencyReason = result.reason;
      _startError = null;
    });
    await _reconnect();
  }

  Future<void> _shutdown() {
    return _shutdownFuture ??= _shutdownOnce();
  }

  Future<void> _shutdownOnce() async {
    _closing = true;
    await _starting;
    _teardown = null;
    await _stopSession();
    await _reportCompletedFiles();
    await _stopTunnel();
  }

  Future<void> _stopTunnel() async {
    final TunnelAgent? previous = _tunnel;
    final StreamSubscription<TunnelAccessTicket>? tickets = _emergencyTickets;
    _tunnel = null;
    _tunnelStarting = null;
    _emergencyTickets = null;
    await tickets?.cancel();
    await previous?.stop();
  }

  /// The native window's close button. The session has to wind down before the
  /// window (and with it the engine) disappears; once that is done the native
  /// side is asked to close the window for real.
  Future<void> _closeNativeWindow() async {
    _autoTimer?.cancel();
    _pollTimer?.cancel();
    _hideTimer?.cancel();
    _setKeyCapture(false);
    _clearCursor();
    try {
      await _shutdown();
    } finally {
      await SessionWindowRequest.close();
    }
  }

  Future<void> _leave() async {
    if (_closing) {
      return;
    }
    _closing = true;
    _autoTimer?.cancel();
    _autoTimer = null;
    _pollTimer?.cancel();
    _hideTimer?.cancel();
    if (_fullscreen) {
      unawaited(ViewerBridge.setFullscreen(false));
    }
    _setKeyCapture(false);
    _clearCursor();
    await _shutdown();
    if (!mounted) {
      return;
    }
    if (widget.windowSession) {
      await SessionWindowRequest.close();
    } else {
      Navigator.of(context).pop();
    }
  }

  void _cleanupAfterPop() {
    _autoTimer?.cancel();
    _autoTimer = null;
    _pollTimer?.cancel();
    _hideTimer?.cancel();
    if (_fullscreen) {
      unawaited(ViewerBridge.setFullscreen(false));
    }
    _setKeyCapture(false);
    _clearCursor();
    unawaited(_shutdown());
  }

  KeyEventResult _onKey(FocusNode node, KeyEvent event) {
    if (_chatTyping || _terminalTyping || !_focus.hasPrimaryFocus) {
      // Local controls own their characters, shortcuts and IME composition.
      return KeyEventResult.ignored;
    }
    if (_inputActive) {
      if (_nativeKeyCapture || _keyboardMode == ViewerKeyboardMode.local) {
        // Native capture already enqueues keys. Caps/Num are deliberately
        // left local so Windows maintains the lock state used by the hook.
        return KeyEventResult.handled;
      }
      if (_swallowEscapeChordUp &&
          event.logicalKey == LogicalKeyboardKey.keyR) {
        // Keep repeats and the matching release local after capture exits.
        if (event is KeyUpEvent) _swallowEscapeChordUp = false;
        return KeyEventResult.handled;
      }
      if (event is KeyDownEvent &&
          _relativeMouse &&
          event.logicalKey == LogicalKeyboardKey.keyR &&
          HardwareKeyboard.instance.isControlPressed &&
          HardwareKeyboard.instance.isAltPressed &&
          HardwareKeyboard.instance.isShiftPressed) {
        setState(_disableRelativeMouse);
        _swallowEscapeChordUp = true;
        return KeyEventResult.handled;
      }
      if (event is KeyDownEvent ||
          event is KeyRepeatEvent ||
          event is KeyUpEvent) {
        // Flutter's usbHidUsage carries the usage page in the upper half
        // (0x00070004 for A); the protocol wants the page-0x07 id alone.
        final int raw = event.physicalKey.usbHidUsage;
        final int usage = raw & 0xFFFF;
        if ((raw >> 16) == 0x07 && usage > 0) {
          if (event is! KeyUpEvent) {
            _heldKeys.add(usage);
          } else {
            _heldKeys.remove(usage);
          }
          unawaited(_sendKey(usage, event is! KeyUpEvent));
          return KeyEventResult.handled;
        }
      }
      return KeyEventResult.ignored;
    }
    if (event is! KeyDownEvent) {
      return KeyEventResult.ignored;
    }
    if (_chatOpen && event.logicalKey == LogicalKeyboardKey.escape) {
      _closeChat();
      return KeyEventResult.handled;
    }
    if (_terminalOpen && event.logicalKey == LogicalKeyboardKey.escape) {
      _closeTerminalPanel();
      return KeyEventResult.handled;
    }
    if (_cameraOpen && event.logicalKey == LogicalKeyboardKey.escape) {
      unawaited(_closeCameraPanel());
      return KeyEventResult.handled;
    }
    if (_fileBrowserOpen && event.logicalKey == LogicalKeyboardKey.escape) {
      _closeFileBrowser();
      return KeyEventResult.handled;
    }
    if (event.logicalKey == LogicalKeyboardKey.escape) {
      if (_fullscreen) {
        unawaited(_setFullscreen(false));
      } else {
        _leave();
      }
      return KeyEventResult.handled;
    }
    if (event.logicalKey == LogicalKeyboardKey.f11) {
      unawaited(_toggleFullscreen());
      return KeyEventResult.handled;
    }
    return KeyEventResult.ignored;
  }

  /// Remote-control shortcuts: while control is on every key goes to the
  /// host, including Esc and F11. Local exit and fullscreen stay on the
  /// toolbar, which the pointer reveals at the top edge.
  void _toggleControl() {
    if (!_status.acceptsInput) {
      return;
    }
    if (_controlEnabled) {
      // The host's injector only releases on teardown, so lifting a chord
      // before dropping the gate keeps a toggle-off from leaving keys or a
      // button down on the far side.
      _releaseRemoteInput();
      _disableRelativeMouse();
    }
    final bool terminalHadKeyboard = _terminalTyping;
    setState(() {
      _controlEnabled = !_controlEnabled;
      _wheelX = 0;
      _wheelY = 0;
    });
    if (_controlEnabled && _chatTyping) {
      // Turning control back on while the chat input owns the keyboard would
      // leave every keystroke local; hand the keyboard back to the session.
      _chatInputFocus.unfocus();
    }
    _setKeyCapture(_inputActive && !_chatTyping && !_terminalTyping);
    _syncCursor();
    if (_controlEnabled && !_chatTyping) {
      // The toolbar button may claim focus while it handles the tap; take it
      // back once the frame settles so keys go to the host immediately.
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted || !_inputActive) {
          return;
        }
        if (terminalHadKeyboard && _terminalOpen) {
          _terminalFocus.requestFocus();
        } else {
          _focus.requestFocus();
        }
      });
    }
  }

  void _releaseRemoteInput() {
    _swallowEscapeChordUp = false;
    for (final int usage in _heldKeys.toList().reversed) {
      unawaited(_sendKey(usage, false));
    }
    _heldKeys.clear();
    for (final int bit in <int>[
      ViewerMouse.left,
      ViewerMouse.right,
      ViewerMouse.middle,
      ViewerMouse.back,
      ViewerMouse.forward,
    ]) {
      if (_heldButtons & bit != 0) {
        _sendMouse(
          ViewerMouse.buttonUp,
          buttons: _remoteButtons[bit] ?? _mouse.buttons(bit),
        );
      }
    }
    _heldButtons = 0;
    _remoteButtons.clear();
    _cancelRemotePointers();
    // Even if an Up above was lost, or the host saw a press this page never
    // recorded, this lifts whatever it still holds.
    unawaited(_guardSend(ViewerBridge.releaseInput()));
  }

  void _onChatFocusChanged() {
    final bool focused = _chatInputFocus.hasFocus;
    if (_chatTyping == focused) {
      return;
    }
    if (focused) {
      // Keys already down on the host must not stay down while the text field
      // takes the keyboard.
      _releaseRemoteInput();
    }
    setState(() => _chatTyping = focused);
    _setKeyCapture(_inputActive && !focused && !_terminalTyping);
  }

  /// Mirrors [_onChatFocusChanged] for the console. The panel handles its own
  /// key events; this only arbitrates the native key-capture switch and the
  /// release of keys the remote desktop may still hold.
  void _onTerminalFocusChanged() {
    final bool focused = _terminalFocus.hasFocus;
    if (_terminalTyping == focused) {
      return;
    }
    if (focused) {
      _releaseRemoteInput();
    }
    setState(() => _terminalTyping = focused);
    _setKeyCapture(_inputActive && !focused && !_chatTyping);
  }

  /// A standalone session has no remote screen to click through, so the page
  /// opens the mode's panel as soon as the session turns live. Runs after
  /// every status poll; each action fires at most once.
  void _maybeOpenControlPanel(ViewerStatus status) {
    final String mode = widget.sessionMode != 'desktop'
        ? widget.sessionMode
        : status.sessionMode;
    if (!status.isLive || status.isOver) {
      return;
    }
    if (mode == 'camera') {
      if (!_cameraOpen && !_cameraOpening && status.acceptsCamera) {
        unawaited(_openCameraPanel());
      }
      if (_cameraOpen &&
          !_cameraAutoStarted &&
          status.cameras.isNotEmpty &&
          (status.cameraPhase == ViewerCameraPhase.idle ||
              status.cameraPhase == ViewerCameraPhase.listed)) {
        _cameraAutoStarted = true;
        ViewerCamera camera = status.cameras.first;
        for (final ViewerCamera candidate in status.cameras) {
          if (candidate.isDefault) {
            camera = candidate;
            break;
          }
        }
        unawaited(_startCamera(camera.id));
      }
    } else if (mode == 'port_forward' &&
        !_portForwardPrompted &&
        status.acceptsPortForward) {
      _portForwardPrompted = true;
      if (widget.rdpPort > 0) {
        unawaited(_startRdpSession());
      } else {
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (mounted && !_status.isOver) {
            unawaited(_openPortForwardDialog());
          }
        });
      }
    }
  }

  /// Starts the RDP shortcut's single forward rule, then waits for the engine
  /// to publish the bound listener port before opening the RDP client.
  Future<void> _startRdpSession() async {
    if (_rdpStarting || _rdpRuleId != 0 || _rdpLaunched) {
      return;
    }
    _rdpStarting = true;
    try {
      final int ruleId = await ViewerBridge.startPortForward(
        localPort: 0,
        remoteHost: '127.0.0.1',
        remotePort: widget.rdpPort,
      );
      if (!mounted) {
        return;
      }
      if (ruleId == 0) {
        _showRdpMessage(tr('无法开始 RDP 转发：主机未开放端口转发'));
        return;
      }
      _rdpRuleId = ruleId;
      for (int attempt = 0; attempt < 50 && mounted; ++attempt) {
        await Future<void>.delayed(const Duration(milliseconds: 100));
        if (!mounted) {
          return;
        }
        for (final ViewerPortForwardRule rule in _portForwardRules.value) {
          if (rule.ruleId == ruleId && rule.active && rule.localPort != 0) {
            _rdpLaunched = true;
            await _launchRdpClient(rule.localPort);
            return;
          }
        }
        if (_status.isOver) {
          break;
        }
      }
      if (mounted) {
        _showRdpMessage(tr('RDP 转发未就绪：请确认主机允许端口转发'));
      }
    } on PlatformException catch (failure) {
      if (mounted) {
        _showRdpMessage(failure.message ?? tr('RDP 转发未就绪：请确认主机允许端口转发'));
      }
    } on MissingPluginException {
      // The native viewer is going away; _start reports the real error.
    } finally {
      _rdpStarting = false;
    }
  }

  Future<void> _launchRdpClient(int localPort) async {
    try {
      await launchRdpClient(localPort);
      if (!mounted) {
        return;
      }
      _showRdpMessage(
        tr('已启动远程桌面：127.0.0.1:{port}', <String, Object?>{'port': localPort}),
      );
    } on ProcessException catch (failure) {
      if (mounted) {
        _showRdpMessage(
          tr('无法启动远程桌面：{error}', <String, Object?>{'error': failure.message}),
        );
      }
    }
  }

  void _showRdpMessage(String text) {
    if (!mounted) {
      return;
    }
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(text), duration: const Duration(seconds: 4)),
    );
  }

  void _toggleTerminal() {
    if (_terminalOpen) {
      _closeTerminalPanel();
    } else {
      _openTerminalPanel();
    }
  }

  void _openTerminalPanel() {
    if (_terminalOpen || !_status.acceptsTerminal) {
      return;
    }
    // Chat and the console both want the keyboard; keep one overlay at a
    // time so Escape and typing have a single owner.
    _closeChat();
    setState(() => _terminalOpen = true);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted && _terminalOpen) {
        _terminalFocus.requestFocus();
      }
    });
  }

  void _closeTerminalPanel() {
    if (!_terminalOpen) {
      return;
    }
    if (_terminalFocus.hasFocus) {
      _terminalFocus.unfocus();
    }
    // Closing the overlay ends every console it shows. The panel no longer
    // closes its console on dispose (a reconnect disposes the widgets but
    // keeps the service), so this is the one place a user-initiated close
    // sends the stop, and a persistent service dies with it.
    for (final int id in _terminalTabIds) {
      unawaited(ViewerBridge.closeTerminal(terminalId: id));
    }
    _terminalTabIds = <int>[1];
    _terminalServiceIds.fillRange(0, kViewerMaxTerminals, 0);
    setState(() => _terminalOpen = false);
    _setKeyCapture(_inputActive && !_chatTyping);
    if (_inputActive) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted && _inputActive && !_terminalTyping) {
          _focus.requestFocus();
        }
      });
    }
  }

  void _onTerminalTabsChanged(List<int> ids) {
    _terminalTabIds = List<int>.of(ids);
    // A tab that is no longer open must not be resumed: its console was
    // stopped when the tab closed, even if the poll has not caught up yet.
    for (int slot = 0; slot < kViewerMaxTerminals; slot++) {
      if (!_terminalTabIds.contains(slot + 1)) {
        _terminalServiceIds[slot] = 0;
      }
    }
  }

  void _toggleCamera() {
    if (_cameraOpen) {
      unawaited(_closeCameraPanel());
    } else {
      unawaited(_openCameraPanel());
    }
  }

  void _toggleFileBrowser() {
    if (_fileBrowserOpen) {
      _closeFileBrowser();
      return;
    }
    if (!_status.acceptsFiles) {
      return;
    }
    setState(() => _fileBrowserOpen = true);
  }

  void _closeFileBrowser() {
    if (!_fileBrowserOpen) {
      return;
    }
    setState(() => _fileBrowserOpen = false);
  }

  /// Registers the camera's own plugin texture and attaches it to the native
  /// camera sink before asking the host for its camera list. The desktop
  /// stream keeps using [_textureId]; the two surfaces are independent.
  Future<void> _openCameraPanel() async {
    if (_cameraOpen || _cameraOpening || !_status.acceptsCamera) {
      return;
    }
    _cameraOpening = true;
    try {
      await _openCameraPanelOnce();
    } finally {
      _cameraOpening = false;
    }
  }

  Future<void> _openCameraPanelOnce() async {
    final int id = await _gpuTexture.registerTexture() ?? -1;
    int output = 0;
    if (id >= 0) {
      try {
        output = await _gpuTexture.output(id) ?? 0;
      } catch (_) {
        await _gpuTexture.unregisterTexture(id);
        rethrow;
      }
    }
    if (id < 0 || output == 0) {
      if (id >= 0) {
        await _gpuTexture.unregisterTexture(id);
      }
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(tr('无法创建摄像头纹理')),
            duration: Duration(seconds: 4),
          ),
        );
      }
      return;
    }
    if (!mounted || !_status.acceptsCamera) {
      await _gpuTexture.unregisterTexture(id);
      return;
    }
    // The console and the chat take the keyboard; the camera preview does
    // not, but keeping one overlay at a time keeps Escape unambiguous.
    _closeChat();
    _closeTerminalPanel();
    _cameraTextureId = id;
    setState(() => _cameraOpen = true);
    if (_inputActive) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted && _inputActive) {
          _focus.requestFocus();
        }
      });
    }
    try {
      await ViewerBridge.setCameraOutput(output);
      await ViewerBridge.requestCameraList();
    } on MissingPluginException {
      await _closeCameraPanel();
    } on PlatformException {
      await _closeCameraPanel();
    }
  }

  Future<void> _closeCameraPanel() async {
    if (!_cameraOpen && _cameraTextureId < 0) {
      return;
    }
    final int id = _cameraTextureId;
    _cameraTextureId = -1;
    if (mounted) {
      setState(() => _cameraOpen = false);
    } else {
      _cameraOpen = false;
    }
    try {
      // Detach first: the engine must stop writing into the output before the
      // plugin texture behind it goes away.
      await ViewerBridge.stopCamera();
      await ViewerBridge.setCameraOutput(0);
    } on MissingPluginException {
      // The session is gone; its camera output went with it.
    } on PlatformException {
      // Same; the stopped session tears the sink down regardless.
    }
    if (id >= 0) {
      await _gpuTexture.unregisterTexture(id);
    }
    if (_inputActive) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted && _inputActive && !_chatTyping && !_terminalTyping) {
          _focus.requestFocus();
        }
      });
    }
  }

  /// The host keeps one camera slot open, so a new choice always stops the
  /// previous stream before asking for the new one. The engine processes the
  /// two queued commands in order.
  Future<void> _startCamera(int cameraId) async {
    if (!_cameraOpen) {
      return;
    }
    try {
      await ViewerBridge.stopCamera();
      final bool started = await ViewerBridge.startCamera(cameraId);
      if (!started && mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(tr('摄像头请求未发送，请稍后重试')),
            duration: Duration(seconds: 4),
          ),
        );
      }
    } on MissingPluginException {
      // The session is gone; the status already says so.
    } on PlatformException {
      // A refused start is reported through the camera phase.
    }
  }

  Future<void> _stopCamera() async {
    try {
      await ViewerBridge.stopCamera();
    } on MissingPluginException {
      // The session is gone.
    } on PlatformException {
      // The engine cleans up when the stream ends anyway.
    }
  }

  Future<void> _openSendTextDialog() async {
    await showDialog<bool>(
      context: context,
      builder: (BuildContext context) =>
          SendTextDialog(onSend: (String text) => ViewerBridge.sendText(text)),
    );
  }

  void _toggleChat() {
    if (_chatOpen) {
      _closeChat();
    } else {
      _openChat();
    }
  }

  void _openChat() {
    if (_terminalOpen) {
      _closeTerminalPanel();
    }
    if (!_chatOpen) {
      setState(() => _chatOpen = true);
    }
    // The toolbar tap leaves focus on its button; hand it to the input once
    // the panel is laid out so typing works immediately.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted && _chatOpen) {
        _chatInputFocus.requestFocus();
      }
    });
  }

  void _closeChat() {
    if (_chatInputFocus.hasFocus) {
      _chatInputFocus.unfocus();
    }
    setState(() => _chatOpen = false);
    _setKeyCapture(_inputActive && !_terminalTyping);
    if (_inputActive) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted && _inputActive && !_chatTyping) {
          _focus.requestFocus();
        }
      });
    }
  }

  void _appendChat(ChatEntry entry) {
    _chatEntries.add(entry);
    if (_chatEntries.length > 200) {
      _chatEntries.removeRange(0, _chatEntries.length - 200);
    }
  }

  /// Moves host lines out of the native queue. The status counter is only a
  /// wake-up: the queue itself decides how many lines are waiting, so a burst
  /// arriving between polls is never half-read.
  Future<void> _drainChat() async {
    if (_chatDraining) {
      return;
    }
    _chatDraining = true;
    try {
      while (mounted) {
        final String? text = await ViewerBridge.takeChat();
        if (text == null || text.isEmpty) {
          break;
        }
        if (!mounted) {
          return;
        }
        setState(() {
          _appendChat(ChatEntry(text: text, mine: false, at: DateTime.now()));
          // A message is worth surfacing even while the panel is closed; the
          // input is not focused, so remote keyboard forwarding keeps working.
          _chatOpen = true;
        });
      }
    } on PlatformException {
      // A closing session rejects calls; the status poll reports why.
    } on MissingPluginException {
      // The native viewer is going away.
    } finally {
      _chatDraining = false;
    }
  }

  Future<bool> _sendChat(String text) async {
    try {
      final bool queued = await ViewerBridge.sendChat(text);
      if (!queued) {
        return false;
      }
      if (!mounted) {
        return true;
      }
      setState(() {
        _appendChat(ChatEntry(text: text, mine: true, at: DateTime.now()));
        _chatOpen = true;
      });
      return true;
    } on PlatformException {
      return false;
    } on MissingPluginException {
      return false;
    }
  }

  Future<void> _changeKeyboardMode(
    ViewerKeyboardMode mode, {
    bool persist = true,
  }) async {
    if (_keyboardChanging || mode == _keyboardMode) return;
    _keyboardChanging = true;
    _releaseRemoteInput();
    _setKeyCapture(false);
    try {
      await ViewerBridge.setKeyboardMode(mode);
      if (!mounted || _closing) return;
      setState(() => _keyboardMode = mode);
      if (persist) {
        ViewPreferences.update(peerId: _prefsPeerId, keyboardMode: mode.name);
      }
    } on PlatformException catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(error.message ?? tr('无法切换键盘模式'))),
        );
      }
    } finally {
      _keyboardChanging = false;
      if (mounted && !_closing) {
        _focus.requestFocus();
        _setKeyCapture(_inputActive);
      }
    }
  }

  void _setKeyCapture(bool enable) {
    final AppLifecycleState? lifecycle = WidgetsBinding.instance.lifecycleState;
    final bool wanted =
        enable &&
        _focus.hasPrimaryFocus &&
        !_chatTyping &&
        !_terminalTyping &&
        (lifecycle == null || lifecycle == AppLifecycleState.resumed);
    if (!wanted) {
      _disableRelativeMouse();
    }
    final int generation = ++_keyCaptureGeneration;
    if (!wanted) _nativeKeyCapture = false;
    unawaited(_updateKeyCapture(wanted, generation));
  }

  Future<void> _updateKeyCapture(bool enable, int generation) async {
    bool captured = false;
    try {
      if (enable) await ViewerBridge.setKeyboardMode(_keyboardMode);
      if (!mounted || generation != _keyCaptureGeneration) return;
      captured = await ViewerBridge.setKeyCapture(enable);
    } on PlatformException {
      // Keep the Flutter path when native capture is unavailable.
    } on MissingPluginException {
      // Widget tests and other embedders use the Flutter path.
    }
    if (mounted && generation == _keyCaptureGeneration) {
      _nativeKeyCapture = enable && captured;
      if (enable && _keyboardMode == ViewerKeyboardMode.local && !captured) {
        if (!_localKeyboardUnavailable) {
          _localKeyboardUnavailable = true;
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(content: Text(tr('本地输入法未启用，请重新聚焦画面或切换物理按键'))),
          );
        }
      } else if (captured || _keyboardMode != ViewerKeyboardMode.local) {
        _localKeyboardUnavailable = false;
      }
    }
  }

  void _onKeyboardFocusChanged() {
    if (_focus.hasPrimaryFocus && mounted && !_closing) {
      final prefs = ViewPreferences.load(peerId: _prefsPeerId);
      setState(() {
        _fit = prefs.fit == 1 ? ViewFit.original : ViewFit.adaptive;
        _statsVisible = prefs.stats;
        _mouse = MouseMapping(
          reverseWheel: prefs.reverseWheel,
          swapButtons: prefs.swapButtons,
        );
      });
      final mode = viewerKeyboardModeFromName(prefs.keyboardMode);
      if (mode != _keyboardMode) {
        unawaited(_changeKeyboardMode(mode, persist: false));
      }
    }
    if (!_focus.hasPrimaryFocus && _inputActive) _releaseRemoteInput();
    _setKeyCapture(_inputActive && _focus.hasPrimaryFocus);
  }

  void _toggleRelativeMouse() {
    if (!_inputActive) {
      return;
    }
    final bool enable = !_relativeMouse;
    setState(() => _relativeMouse = enable);
    _applyRelativeMouse(enable);
    if (enable) {
      // The toolbar button owns focus while its tap is handled; keys, and the
      // exit chord, must keep reaching this page.
      _focus.requestFocus();
    }
  }

  void _disableRelativeMouse() {
    if (!_relativeMouse) {
      return;
    }
    _relativeMouse = false;
    _swallowEscapeChordUp = false;
    _applyRelativeMouse(false);
  }

  void _applyRelativeMouse(bool enable) {
    unawaited(_requestRelativeMouse(enable));
  }

  Future<bool> _requestRelativeMouse(bool enable) async {
    bool active = false;
    try {
      active = await ViewerBridge.setRelativeMouse(
        enabled: enable,
        scale: _cursorTargetScale,
      );
    } on PlatformException {
      active = false;
    } on MissingPluginException {
      active = false;
    }
    if (enable && !active) {
      // The native view refused capture; fall back to absolute moves instead
      // of leaving the page in a mode that never sends pointer motion.
      if (mounted && _relativeMouse) {
        setState(() => _relativeMouse = false);
      }
      _applyRelativeMouse(false);
    }
    return active;
  }

  Future<void> _guardSend(Future<bool> send) async {
    try {
      await send;
    } on PlatformException {
      // A closing session rejects events; the status poll reports why.
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  /// Modifier keys held and locks on right now, in the bit layout
  /// [ViewerModifier] and [ViewerLock] share with the host.
  int _inputState() {
    final HardwareKeyboard keyboard = HardwareKeyboard.instance;
    return modifierBitsFor(keyboard.physicalKeysPressed) |
        lockBitsFor(keyboard.lockModesEnabled);
  }

  void _sendMouse(int action, {int buttons = 0, int x = 0, int y = 0}) {
    unawaited(
      _guardSend(
        ViewerBridge.sendMouse(
          action: action,
          buttons: buttons,
          x: x,
          y: y,
          modifiers: _inputState(),
        ),
      ),
    );
  }

  void _sendTouch(
    int action,
    int contact, {
    int x = 0,
    int y = 0,
    int pressure = 0,
  }) {
    unawaited(
      _guardSend(
        ViewerBridge.sendTouch(
          action: action,
          contact: contact,
          x: x,
          y: y,
          pressure: pressure,
          modifiers: _inputState(),
        ),
      ),
    );
  }

  void _sendPen(
    int action, {
    int x = 0,
    int y = 0,
    int pressure = 0,
    int flags = 0,
    int tiltX = 0,
    int tiltY = 0,
  }) {
    unawaited(
      _guardSend(
        ViewerBridge.sendPen(
          action: action,
          x: x,
          y: y,
          pressure: pressure,
          flags: flags,
          tiltX: tiltX,
          tiltY: tiltY,
          modifiers: _inputState(),
        ),
      ),
    );
  }

  /// Lifts every touch contact and the pen the host may still hold. The
  /// subsequent [ViewerBridge.releaseInput] repeats this host-side, but the
  /// tracker must be emptied here so a later gesture starts with clean slots.
  void _cancelRemotePointers() {
    if (_contacts.contacts.isEmpty && _penPointer == null) {
      return;
    }
    final int modifiers = _inputState();
    for (final int contact in _contacts.contacts.toList()) {
      unawaited(
        _guardSend(
          ViewerBridge.sendTouch(
            action: ViewerTouchAction.cancel,
            contact: contact,
            x: 0,
            y: 0,
            modifiers: modifiers,
          ),
        ),
      );
    }
    _contacts.clear();
    if (_penPointer != null) {
      _penPointer = null;
      unawaited(
        _guardSend(
          ViewerBridge.sendPen(
            action: ViewerTouchAction.cancel,
            x: 0,
            y: 0,
            modifiers: modifiers,
          ),
        ),
      );
    }
  }

  Future<void> _sendKey(int usage, bool down) {
    return _guardSend(
      ViewerBridge.sendKey(
        usage: usage,
        down: down,
        modifiers: _inputState(),
        physical: true,
      ),
    );
  }

  /// Like [_sendKey], but with an explicit bitmap so a synthetic chord can
  /// describe the modifiers it is holding instead of what this machine's
  /// keyboard happens to have down.
  Future<void> _sendKeyWithBits(int usage, bool down, int modifiers) {
    return _guardSend(
      ViewerBridge.sendKey(usage: usage, down: down, modifiers: modifiers),
    );
  }

  /// Sends one toolbar shortcut as a press chord: modifiers down, the key
  /// down and up, then the modifiers up in reverse order. Every event carries
  /// the bitmap at that instant, matching a physical chord on the host.
  Future<void> _sendChord(List<int> modifierUsages, int keyUsage) async {
    final int modifiers = await ViewerBridge.keyboardState(
      fallback: _inputState(),
    );
    for (final RemoteKeyEvent event in buildRemoteChord(
      modifierUsages,
      keyUsage,
      modifiers,
    )) {
      await _sendKeyWithBits(event.usage, event.down, event.modifiers);
    }
  }

  /// Win is itself a modifier, so a tap clears its bit before the key-up event
  /// rather than sending a second, duplicate down.
  Future<void> _sendWinTap() async {
    final int modifiers = await ViewerBridge.keyboardState(
      fallback: _inputState(),
    );
    for (final RemoteKeyEvent event in buildRemoteWinTap(
      modifiers,
      ViewerModifier.leftGui,
    )) {
      await _sendKeyWithBits(event.usage, event.down, event.modifiers);
    }
  }

  Future<void> _sendSpecialAction(RemoteSpecialAction action) async {
    if (!_inputActive) {
      return;
    }
    // The menu may have taken primary focus. Return key ownership so real
    // modifier releases following this shortcut still reach the host.
    _focus.requestFocus();
    if (action == RemoteSpecialAction.sas) {
      bool queued = false;
      try {
        queued = await ViewerBridge.sendSas();
      } on PlatformException {
        queued = false;
      } on MissingPluginException {
        queued = false;
      }
      if (!queued && mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(tr('Ctrl+Alt+Del 未能排队：远程键鼠尚未就绪')),
            duration: Duration(seconds: 3),
          ),
        );
      }
      return;
    }
    switch (action) {
      case RemoteSpecialAction.win:
        await _sendWinTap();
        return;
      case RemoteSpecialAction.altTab:
        await _sendChord(<int>[0xE2], 0x2B);
        return;
      case RemoteSpecialAction.altF4:
        await _sendChord(<int>[0xE2], 0x3D);
        return;
      case RemoteSpecialAction.taskManager:
        await _sendChord(<int>[0xE0, 0xE1], 0x29);
        return;
      case RemoteSpecialAction.sas:
        return;
    }
  }

  String _sasCodeText(int code) {
    if (code == ViewerSasCode.denied.code) {
      return tr('主机未授予输入权限');
    }
    if (code == ViewerSasCode.unsupported.code) {
      return tr('主机不支持发送 Ctrl+Alt+Del（托盘主机需改用服务运行）');
    }
    if (code == ViewerSasCode.failed.code) {
      return tr('主机调用 SendSAS 失败');
    }
    return tr('主机未发送 Ctrl+Alt+Del');
  }

  // Input travels in captured-desktop pixels; the encoded picture may be
  // smaller, so the clamp follows the source size.
  int get _sourceWidth =>
      _status.sourceWidth > 0 ? _status.sourceWidth : _status.width;

  int get _sourceHeight =>
      _status.sourceHeight > 0 ? _status.sourceHeight : _status.height;

  (int, int) _desktopPoint(Offset frame, [DisplayVideoStream? display]) =>
      frameToDesktop(
        frame,
        originX: display?.originX ?? _status.originX,
        originY: display?.originY ?? _status.originY,
        width: display == null
            ? _sourceWidth
            : (display.sourceWidth > 0 ? display.sourceWidth : display.width),
        height: display == null
            ? _sourceHeight
            : (display.sourceHeight > 0
                  ? display.sourceHeight
                  : display.height),
      );

  void _onPointerHover(
    Offset frame,
    int buttons, {
    DisplayVideoStream? display,
  }) {
    if (_relativeMouse) {
      // Native capture already reported the motion; an absolute move would
      // drag the host pointer back to the pinned local position.
      return;
    }
    final (int x, int y) = _desktopPoint(frame, display);
    _sendMouse(ViewerMouse.move, buttons: _mouse.buttons(buttons), x: x, y: y);
  }

  void _onPointerMove(
    Offset frame,
    int buttons, {
    DisplayVideoStream? display,
  }) {
    if (_relativeMouse) {
      return;
    }
    final (int x, int y) = _desktopPoint(frame, display);
    _sendMouse(ViewerMouse.move, buttons: _mouse.buttons(buttons), x: x, y: y);
  }

  void _onPointerButton(
    Offset frame,
    int button,
    bool down, {
    DisplayVideoStream? display,
  }) {
    if (display != null) _onPointerMove(frame, _heldButtons, display: display);
    if (down) {
      _heldButtons |= button;
      final int remote = _mouse.buttons(button);
      _remoteButtons[button] = remote;
      _sendMouse(ViewerMouse.buttonDown, buttons: remote);
    } else {
      _heldButtons &= ~button;
      final int remote =
          _remoteButtons.remove(button) ?? _mouse.buttons(button);
      _sendMouse(ViewerMouse.buttonUp, buttons: remote);
    }
  }

  void _onPointerScroll(
    Offset frame,
    Offset delta, {
    DisplayVideoStream? display,
  }) {
    if (display != null) _onPointerMove(frame, _heldButtons, display: display);
    _wheelY += delta.dy;
    _wheelX += delta.dx;
    final int stepsY = (_wheelY / kViewerScrollPixelsPerNotch)
        .truncate()
        .clamp(-10, 10)
        .toInt();
    if (stepsY != 0) {
      _wheelY -= stepsY * kViewerScrollPixelsPerNotch;
      // Flutter's positive dy scrolls down; Windows WHEEL_DELTA is positive
      // when the wheel turns up (away from the user).
      _sendMouse(
        ViewerMouse.wheel,
        y: _mouse.wheelY(-stepsY * kViewerWheelDelta),
      );
    }
    final int stepsX = (_wheelX / kViewerScrollPixelsPerNotch)
        .truncate()
        .clamp(-10, 10)
        .toInt();
    if (stepsX != 0) {
      _wheelX -= stepsX * kViewerScrollPixelsPerNotch;
      _sendMouse(
        ViewerMouse.wheelHorizontal,
        x: _mouse.wheelX(stepsX * kViewerWheelDelta),
      );
    }
  }

  void _onGesture(
    Offset frame,
    bool zoom,
    int deltaX,
    int deltaY, {
    DisplayVideoStream? display,
  }) {
    if (display != null) _onPointerMove(frame, _heldButtons, display: display);
    unawaited(
      _guardSend(
        ViewerBridge.sendGesture(
          zoom: zoom,
          x: deltaX,
          y: deltaY,
          modifiers: _inputState(),
        ),
      ),
    );
  }

  void _onDevicePointer(
    PointerEvent event,
    Offset frame, {
    DisplayVideoStream? display,
  }) {
    final (int x, int y) = _desktopPoint(frame, display);
    if (event.kind == PointerDeviceKind.touch) {
      _handleTouchPointer(event, x, y);
    } else {
      _handlePenPointer(event, x, y);
    }
  }

  void _onDeviceCancel(
    PointerEvent event,
    Offset frame, {
    DisplayVideoStream? display,
  }) {
    final (int x, int y) = _desktopPoint(frame, display);
    if (event.kind == PointerDeviceKind.touch) {
      _handleTouchPointer(event, x, y);
    } else {
      _handlePenPointer(event, x, y);
    }
  }

  void _handleTouchPointer(PointerEvent event, int x, int y) {
    final int pressure = windowsPointerPressure(
      event.pressure,
      event.pressureMin,
      event.pressureMax,
    );
    if (event is PointerDownEvent) {
      final int? contact = _contacts.acquire(event.pointer);
      if (contact != null) {
        _sendTouch(
          ViewerTouchAction.down,
          contact,
          x: x,
          y: y,
          pressure: pressure,
        );
      }
      return;
    }
    if (event is PointerMoveEvent) {
      final int? contact = _contacts.slotFor(event.pointer);
      if (contact != null) {
        _sendTouch(
          ViewerTouchAction.move,
          contact,
          x: x,
          y: y,
          pressure: pressure,
        );
      }
      return;
    }
    final int? contact = _contacts.release(event.pointer);
    if (contact == null) {
      return;
    }
    _sendTouch(
      event is PointerCancelEvent
          ? ViewerTouchAction.cancel
          : ViewerTouchAction.up,
      contact,
      x: x,
      y: y,
      pressure: pressure,
    );
  }

  void _handlePenPointer(PointerEvent event, int x, int y) {
    final int pressure = windowsPointerPressure(
      event.pressure,
      event.pressureMin,
      event.pressureMax,
    );
    final int flags = windowsPenFlags(event.kind, event.buttons);
    final (int tiltX, int tiltY) = windowsPenTilt(
      event.tilt,
      event.orientation,
    );
    void send(int action) => _sendPen(
      action,
      x: x,
      y: y,
      pressure: pressure,
      flags: flags,
      tiltX: tiltX,
      tiltY: tiltY,
    );
    if (event is PointerDownEvent) {
      _penPointer = event.pointer;
      send(ViewerTouchAction.down);
      return;
    }
    if (_penPointer != event.pointer) {
      return;
    }
    if (event is PointerMoveEvent) {
      send(ViewerTouchAction.move);
      return;
    }
    _penPointer = null;
    send(
      event is PointerCancelEvent
          ? ViewerTouchAction.cancel
          : ViewerTouchAction.up,
    );
  }

  /// Path selected by the embedded agent, empty for a direct connection that
  /// does not use the connection service.
  String get _tunnelPath {
    if (!widget.target.viaServer) {
      return '';
    }
    return _tunnel?.pathLabel ?? tr('连接中');
  }

  /// Audit route of the current connection. A direct connection that never
  /// touched the connection service is `direct`; the agent's `direct` label
  /// means the server introduced the peers, which the audit calls `punch`.
  /// Empty while the path is still being chosen, so no route is guessed.
  String get _connectionRoute {
    if (!widget.target.viaServer) {
      return 'direct';
    }
    switch (_tunnel?.path) {
      case 'direct':
        return 'punch';
      case 'relay':
        return 'relay';
      case 'carrier':
        return 'carrier';
      default:
        return '';
    }
  }

  String get _stateText {
    switch (_status.state) {
      case ViewerState.idle:
      case ViewerState.connecting:
        return tr('正在建立连接…');
      case ViewerState.handshaking:
        return tr('正在协商视频流…');
      case ViewerState.streaming:
        return tr('等待远端画面…');
      case ViewerState.ended:
        return tr('会话已结束');
      case ViewerState.failed:
        return tr('连接失败');
    }
  }

  /// True when the connection was refused by the host's access control. The
  /// token lives on the home page, so the card sends the user back to edit it
  /// instead of offering another attempt with the same one.
  bool get _denied => _startError == null && _status.denial != 0;

  String get _overTitle {
    if (_startError != null) {
      return tr('无法发起连接');
    }
    if (_denied) {
      return tr('服务端拒绝了连接');
    }
    if (_status.state == ViewerState.failed) {
      return tr('连接中断');
    }
    return tr('会话已结束');
  }

  String get _overDetail {
    if (_startError != null) {
      return _startError!;
    }
    final String denial = accessDenialText(_status.denial);
    if (denial.isNotEmpty) {
      // The server's own reason follows the viewer's wording; it is what a
      // host operator sees in their log, so it helps to compare.
      const String prefix = 'handshake: permission denied: ';
      final String reason = _status.error.startsWith(prefix)
          ? _status.error.substring(prefix.length)
          : '';
      return reason.isEmpty
          ? denial
          : tr('{denial}\n\n服务端说明：{reason}', <String, Object?>{
              'denial': denial,
              'reason': reason,
            });
    }
    if (_status.error.isNotEmpty) {
      return _status.error;
    }
    return tr('远端已结束视频流。');
  }

  @override
  Widget build(BuildContext context) {
    // A file-transfer-only session has no picture: the page is the shared
    // folder browser with a small header, and the remote-screen stack below
    // never applies.
    if (widget.fileTransferOnly || _status.filesOnly) {
      return _buildFilesOnly();
    }
    // Terminal, camera and port-forward sessions carry no screen stream: the
    // page is the mode's panel with a small header. The widget field drives
    // the layout before the HelloAck; the status echo confirms it afterwards.
    final String controlMode = widget.sessionMode != 'desktop'
        ? widget.sessionMode
        : _status.sessionMode;
    if (_status.controlOnly || controlMode != 'desktop') {
      return _buildControlOnly(controlMode);
    }
    final bool hasFrame = _status.hasFrame || _independentDisplays;
    final bool showOverlay = _startError != null || _status.isOver;

    return PopScope(
      canPop: true,
      onPopInvokedWithResult: (bool didPop, Object? result) {
        if (didPop) {
          _cleanupAfterPop();
        }
      },
      child: Scaffold(
        backgroundColor: Colors.black,
        body: Focus(
          focusNode: _focus,
          autofocus: true,
          onKeyEvent: _onKey,
          child: Listener(
            behavior: HitTestBehavior.translucent,
            onPointerHover: _onHover,
            child: Stack(
              fit: StackFit.expand,
              children: <Widget>[
                if (_independentDisplays)
                  switch (_displayStreamLayout) {
                    _DisplayStreamLayout.desktop => _buildDesktopDisplays(),
                    _DisplayStreamLayout.single => _buildSingleDisplayStream(),
                    _ => _buildIndependentDisplays(),
                  }
                else if (hasFrame && _textureId >= 0)
                  GestureDetector(
                    behavior: HitTestBehavior.opaque,
                    onDoubleTap: _inputActive ? null : _toggleFullscreen,
                    child: RemoteView(
                      textureId: _textureId,
                      videoWidth: _status.width,
                      videoHeight: _status.height,
                      sourceWidth: _status.sourceWidth,
                      sourceHeight: _status.sourceHeight,
                      fit: _fit,
                      captureInput: _inputActive,
                      hideCursor:
                          _inputActive && (_cursorInstalled || _relativeMouse),
                      onScaleChanged: _onViewScale,
                      onHover: _inputActive ? _onPointerHover : null,
                      onMove: _inputActive ? _onPointerMove : null,
                      onButton: _inputActive ? _onPointerButton : null,
                      onScroll: _inputActive ? _onPointerScroll : null,
                      onGesture: _inputActive ? _onGesture : null,
                      onDevicePointer: _inputActive ? _onDevicePointer : null,
                      onDeviceCancel: _inputActive ? _onDeviceCancel : null,
                      onCursorHint: _onCursorHint,
                      onCursorHintPress: _onCursorHintPress,
                      onCursorHintExit: _hideCursorHint,
                      onFocusRequested: _focus.requestFocus,
                      overlayBuilder: _inputActive
                          ? null
                          : _buildRemoteCursorOverlay,
                    ),
                  )
                else
                  const ColoredBox(color: Colors.black),
                if (_autoPolicy.waiting && showOverlay)
                  _buildAutoReconnect(hasFrame)
                else if (showOverlay)
                  _buildDisconnected(hasFrame)
                else if (!hasFrame)
                  _buildWaiting(),
                if (hasFrame && !showOverlay)
                  Positioned(
                    top: 12,
                    left: 0,
                    right: 0,
                    child: Center(
                      child: RemoteToolbar(
                        collapsed: _collapsed,
                        pinned: _pinned,
                        fullscreen: _fullscreen,
                        statsVisible: _statsVisible,
                        fit: _fit,
                        controlEnabled: _controlEnabled,
                        inputAvailable: _status.acceptsInput,
                        showMyCursor: _showMyCursor,
                        onToggleShowCursor: _toggleShowMyCursor,
                        clipboardEnabled: _clipboardSync,
                        clipboardAvailable: _status.acceptsClipboard,
                        clipboardDirection: _status.clipboardDirection,
                        fileAvailable: _status.acceptsFiles,
                        fileActive: _status.fileActive,
                        audioAvailable: _status.acceptsAudio,
                        audioMuted: _status.audioMuted,
                        audioDevice: _status.audioDevice,
                        onLoadAudioOutputs: _loadAudioOutputs,
                        onSelectAudioOutput: _selectAudioOutput,
                        microphoneAvailable: _status.acceptsMicrophone,
                        microphoneEnabled: _status.microphoneEnabled,
                        microphoneDevice: _status.microphoneDevice,
                        audioAec: _status.audioAec,
                        audioNoiseSuppression: _status.audioNoiseSuppression,
                        onLoadMicrophones: _loadMicrophones,
                        onSelectMicrophone: _selectMicrophone,
                        onSetAudioProcessing: _setAudioProcessing,
                        voiceAvailable: _status.acceptsVoiceCall,
                        voiceEnabled: _status.voiceEnabled,
                        voicePending: _status.voicePending,
                        onToggleVoiceCall: () => unawaited(_toggleVoiceCall()),
                        privacyAvailable: _status.acceptsPrivacy,
                        privacyEnabled: _status.privacyEnabled,
                        osLoginAvailable: _status.acceptsOsLogin,
                        onOsLogin: () => unawaited(_openOsLoginDialog()),
                        sessionSelectAvailable: _status.acceptsSessionSelect,
                        elevationAvailable:
                            _status.elevationAvailable && !_elevationSubmitting,
                        elevationPending:
                            _status.elevationPending || _elevationSubmitting,
                        elevated: _status.elevationCode == 2,
                        onElevation: () => unawaited(_requestElevation()),
                        onSessionSelect: () => unawaited(_openSessionDialog()),
                        remoteConfigAvailable: _status.remoteConfigSupported,
                        onRemoteConfig: () =>
                            unawaited(_openRemoteConfigDialog()),
                        restartAvailable: _status.acceptsRestart,
                        lockAvailable: _status.acceptsLockScreen,
                        blockInputAvailable: _status.acceptsBlockInput,
                        blockInputActive: _status.blockInputActive,
                        onHostAction: (ViewerHostAction action) =>
                            unawaited(_onHostAction(action)),
                        portForwardAvailable: _status.acceptsPortForward,
                        portForwardActive: _status.portForwardRules.any(
                          (ViewerPortForwardRule rule) => rule.active,
                        ),
                        onOpenPortForward: () =>
                            unawaited(_openPortForwardDialog()),
                        displays: _status.displays,
                        activeDisplay: _activeDisplay,
                        independentDisplays: _independentDisplays,
                        onIndependentDisplays: _status.isLive
                            ? () => unawaited(
                                _selectDisplayStreams(
                                  _status.displays.map((d) => d.index).toList(),
                                  layout: _DisplayStreamLayout.grid,
                                  reconnectDisplay: kViewerDisplayAuto,
                                ),
                              )
                            : null,
                        followCursor: _followCursor,
                        followWindow: _followWindow,
                        onToggleFollowCursor: _toggleFollowCursor,
                        onToggleFollowWindow: _toggleFollowWindow,
                        onSelectDisplay: (int index) =>
                            unawaited(_switchDisplay(index)),
                        onOpenDisplayWindow: _status.isLive
                            ? (int index) =>
                                  unawaited(_openDisplayWindow(index))
                            : null,
                        modes:
                            _status.displayModesDisplay ==
                                    _status.streamedDisplayIndex &&
                                _status.displayModesCode == 0
                            ? _status.displayModes
                            : const <ViewerDisplayMode>[],
                        modeChangeAvailable:
                            _status.acceptsInput &&
                            !_status.mergedLayout &&
                            !_independentDisplays,
                        onSelectMode: (ViewerDisplayMode mode) =>
                            unawaited(_changeDisplayMode(mode)),
                        streamOptionsAvailable: _status.isLive,
                        streamCodec: _streamCodec,
                        streamFullColor: _streamFullColor,
                        streamActualFormat:
                            "${_status.codec.toUpperCase()} · ${_status.chroma == 1 ? '4:4:4' : '4:2:0'}",
                        onSelectStreamFullColor: (value) =>
                            unawaited(_changeStreamOptions(fullColor: value)),
                        streamQuality: _streamQuality,
                        streamQualityPercent: _streamQualityPercent,
                        streamMaxFps: _streamMaxFps,
                        onSelectStreamCodec: (ViewerStreamCodec codec) =>
                            unawaited(_changeStreamOptions(codec: codec)),
                        onSelectStreamQuality: (ViewerStreamQuality quality) =>
                            unawaited(_changeStreamOptions(quality: quality)),
                        onSelectStreamQualityPercent: (int percent) =>
                            unawaited(
                              _changeStreamOptions(
                                quality: ViewerStreamQuality.custom,
                                qualityPercent: percent,
                              ),
                            ),
                        onSelectStreamFps: (int fps) =>
                            unawaited(_changeStreamOptions(maxFps: fps)),
                        onToggleControl: _toggleControl,
                        keyboardMode: _keyboardMode,
                        onKeyboardMode: (mode) =>
                            unawaited(_changeKeyboardMode(mode)),
                        reverseWheel: _mouse.reverseWheel,
                        swapButtons: _mouse.swapButtons,
                        onToggleReverseWheel: _toggleReverseWheel,
                        onToggleSwapButtons: _toggleSwapButtons,
                        relativeMouse: _relativeMouse,
                        onToggleRelativeMouse: _independentDisplays
                            ? null
                            : _toggleRelativeMouse,
                        specialKeysAvailable: _inputActive,
                        onSpecialAction: (RemoteSpecialAction action) =>
                            unawaited(_sendSpecialAction(action)),
                        onSendText: () => unawaited(_openSendTextDialog()),
                        screenshotAvailable: _independentDisplays
                            ? _displayStreams?.selected?.stream.ready == true
                            : _status.hasFrame,
                        onScreenshot: _takeScreenshot,
                        onToggleClipboard: _toggleClipboardSync,
                        onSendFile: () => unawaited(_sendFile()),
                        fileBrowseOpen: _fileBrowserOpen,
                        onToggleFileBrowse: _toggleFileBrowser,
                        chatOpen: _chatOpen,
                        onToggleChat: _toggleChat,
                        terminalAvailable: _status.acceptsTerminal,
                        terminalOpen: _terminalOpen,
                        onToggleTerminal: _toggleTerminal,
                        cameraAvailable: _status.acceptsCamera,
                        cameraOpen: _cameraOpen,
                        onToggleCamera: _toggleCamera,
                        onToggleAudio: () => unawaited(_toggleAudioMute()),
                        onToggleMicrophone: () =>
                            unawaited(_toggleMicrophone()),
                        displayControlAvailable:
                            _status.isLive && !_status.filesOnly,
                        displayControlSupported:
                            _status.displayControlSupported,
                        displayControlManaged: _status.displayControlManaged,
                        displayControlBusy: _status.displayControlBusy,
                        displayControlCount: _status.displayControlCount,
                        displayControlPrivacy: _status.displayControlPrivacy,
                        onControlDisplays: _controlDisplays,
                        displayControlOutputs: _status.displayControlOutputs,
                        onToggleVirtualDisplay: (int target, bool enabled) =>
                            unawaited(
                              _controlDisplays(
                                4,
                                enabled ? 1 : 0,
                                target: target,
                              ),
                            ),
                        onTogglePrivacy: () => unawaited(_togglePrivacy()),
                        recordingAvailable:
                            (_status.acceptsRecording &&
                                (!_independentDisplays ||
                                    _displayStreams?.selected?.stream.ready ==
                                        true)) ||
                            _status.recording,
                        recordingDenied:
                            _status.isLive && !_status.acceptsRecording,
                        recording: _status.recording,
                        onToggleRecording: _toggleRecording,
                        autoRecord: _autoRecord,
                        recordKeep: _recordKeep,
                        onAutoRecordChanged: _setAutoRecord,
                        onRecordKeepChanged: _setRecordKeep,
                        onTogglePin: _togglePin,
                        onToggleStats: _toggleStats,
                        onToggleFullscreen: _toggleFullscreen,
                        onCollapse: _collapse,
                        onExpand: _expand,
                        onClose: _leave,
                        onFitChanged: _setFit,
                      ),
                    ),
                  ),
                if (hasFrame && !showOverlay && _status.secureDesktop)
                  Positioned(
                    top: 54,
                    left: 0,
                    right: 0,
                    child: Center(
                      child: IgnorePointer(
                        child: Container(
                          padding: const EdgeInsets.symmetric(
                            horizontal: 12,
                            vertical: 7,
                          ),
                          decoration: BoxDecoration(
                            color: Colors.black.withValues(alpha: 0.68),
                            borderRadius: BorderRadius.circular(6),
                            border: Border.all(
                              color: Colors.amber.withValues(alpha: 0.45),
                            ),
                          ),
                          child: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: <Widget>[
                              const Icon(
                                Icons.lock_rounded,
                                size: 14,
                                color: Colors.amberAccent,
                              ),
                              const SizedBox(width: 7),
                              Text(
                                _status.desktop == ViewerDesktopKind.secure
                                    ? tr('主机已进入安全桌面（锁屏或 UAC），画面暂停')
                                    : tr('主机暂时无法采集桌面（锁屏或 UAC），恢复后自动继续'),
                                style: const TextStyle(
                                  color: Colors.white70,
                                  fontSize: 11,
                                ),
                              ),
                            ],
                          ),
                        ),
                      ),
                    ),
                  ),
                if (hasFrame && !showOverlay && _fingerprintUnpersisted)
                  Positioned(
                    top: 96,
                    left: 0,
                    right: 0,
                    child: Center(
                      child: IgnorePointer(
                        child: Container(
                          padding: const EdgeInsets.symmetric(
                            horizontal: 12,
                            vertical: 7,
                          ),
                          decoration: BoxDecoration(
                            color: Colors.black.withValues(alpha: 0.68),
                            borderRadius: BorderRadius.circular(6),
                            border: Border.all(
                              color: Colors.orangeAccent.withValues(
                                alpha: 0.45,
                              ),
                            ),
                          ),
                          child: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: <Widget>[
                              const Icon(
                                Icons.shield_outlined,
                                size: 14,
                                color: Colors.orangeAccent,
                              ),
                              const SizedBox(width: 7),
                              Text(
                                tr('主机指纹未能保存，本次连接未固定证书'),
                                style: const TextStyle(
                                  color: Colors.white70,
                                  fontSize: 11,
                                ),
                              ),
                            ],
                          ),
                        ),
                      ),
                    ),
                  ),
                if (hasFrame && !showOverlay && _statsVisible)
                  Positioned(
                    top: 10,
                    right: 10,
                    child: StatsOverlay(
                      status: _status,
                      target: widget.target,
                      path: _tunnelPath,
                    ),
                  ),
                if (hasFrame && !showOverlay && _inputActive && !_cameraOpen)
                  Positioned(
                    left: 10,
                    bottom: 10,
                    child: IgnorePointer(
                      child: Container(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 9,
                          vertical: 5,
                        ),
                        decoration: BoxDecoration(
                          color: Colors.black.withValues(alpha: 0.55),
                          borderRadius: BorderRadius.circular(6),
                          border: Border.all(color: Colors.white12),
                        ),
                        child: Text(
                          tr('远程键鼠已开启 · Esc 与 F11 已转发，工具栏可退出'),
                          style: TextStyle(
                            color: Colors.white54,
                            fontSize: 10.5,
                          ),
                        ),
                      ),
                    ),
                  ),
                if (hasFrame &&
                    !showOverlay &&
                    (_status.fileActive || _status.downloadActive))
                  Positioned(
                    left: 0,
                    right: 0,
                    bottom: 46,
                    child: Center(
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        children: <Widget>[
                          if (_status.downloadActive) _buildDownloadProgress(),
                          if (_status.downloadActive && _status.fileActive)
                            const SizedBox(height: 6),
                          if (_status.fileActive) _buildFileProgress(),
                        ],
                      ),
                    ),
                  ),
                if (hasFrame &&
                    !showOverlay &&
                    !_chatOpen &&
                    !_terminalOpen &&
                    (_status.privacyEnabled ||
                        _status.blockInputActive ||
                        (_status.acceptsAudio && _status.audioMuted)))
                  Positioned(
                    right: 10,
                    bottom: 10,
                    child: IgnorePointer(
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        crossAxisAlignment: CrossAxisAlignment.end,
                        children: <Widget>[
                          if (_status.privacyEnabled)
                            _statusBadge(Icons.shield_rounded, tr('隐私模式')),
                          if (_status.privacyEnabled &&
                              _status.blockInputActive)
                            const SizedBox(height: 6),
                          if (_status.blockInputActive)
                            _statusBadge(
                              Icons.pause_circle_rounded,
                              tr('主机输入已阻止'),
                            ),
                          if ((_status.privacyEnabled ||
                                  _status.blockInputActive) &&
                              _status.acceptsAudio &&
                              _status.audioMuted)
                            const SizedBox(height: 6),
                          if (_status.acceptsAudio && _status.audioMuted)
                            _statusBadge(Icons.volume_off_rounded, tr('已静音')),
                        ],
                      ),
                    ),
                  ),
                if (hasFrame && !showOverlay && _chatOpen)
                  Positioned(
                    right: 12,
                    bottom: 12,
                    child: ChatPanel(
                      entries: _chatEntries,
                      sent: _status.chatSent,
                      received: _status.chatReceived,
                      onSend: _sendChat,
                      onClose: _closeChat,
                      inputFocusNode: _chatInputFocus,
                    ),
                  ),
                if (hasFrame && !showOverlay && _cameraOpen)
                  Positioned(
                    left: 12,
                    bottom: 12,
                    child: SizedBox(
                      width: 440,
                      child: CameraPanel(
                        status: _status,
                        textureId: _cameraTextureId,
                        onRefresh: () => unawaited(
                          _guardSend(ViewerBridge.requestCameraList()),
                        ),
                        onStart: (int cameraId) =>
                            unawaited(_startCamera(cameraId)),
                        onStop: () => unawaited(_stopCamera()),
                        onDismiss: () => unawaited(_closeCameraPanel()),
                      ),
                    ),
                  ),
                if (hasFrame && !showOverlay && _terminalOpen)
                  Positioned.fill(
                    child: Padding(
                      padding: const EdgeInsets.fromLTRB(16, 56, 16, 56),
                      child: Align(
                        alignment: Alignment.bottomRight,
                        child: FractionallySizedBox(
                          widthFactor: 0.7,
                          heightFactor: 0.7,
                          child: TerminalTabs(
                            status: _status,
                            focusNode: _terminalFocus,
                            initialTabs: _terminalTabIds,
                            resumeServiceIds: _terminalServiceIds,
                            onTabsChanged: _onTerminalTabsChanged,
                            onDismiss: _closeTerminalPanel,
                            initialFontSize: _terminalFontSize,
                            onFontSizeChanged: _rememberTerminalFontSize,
                          ),
                        ),
                      ),
                    ),
                  ),
                if (hasFrame && !showOverlay && _fileBrowserOpen)
                  Positioned(
                    right: 12,
                    top: 64,
                    child: FileBrowserPanel(
                      status: _status,
                      onRequestList: ViewerBridge.requestFileList,
                      onRequestDownload: ViewerBridge.requestFileDownload,
                      onTakeList: ViewerBridge.takeFileList,
                      onUpload: ViewerBridge.sendFileTo,
                      onFileOperation:
                          (
                            ViewerFileOperation op,
                            String path,
                            String newName,
                          ) => ViewerBridge.fileOperation(
                            op,
                            path,
                            newName: newName,
                          ),
                      onDownloadFolder: ViewerBridge.downloadFolder,
                      onDownloadEntries: ViewerBridge.downloadEntries,
                      onCancelFolderDownload: ViewerBridge.cancelFolderDownload,
                      onCancelUpload: ViewerBridge.cancelUploads,
                      onCancelDownload: ViewerBridge.cancelDownload,
                      initialDirectory: _fileBrowserDirectory,
                      onDirectoryChanged: _rememberFileBrowserDirectory,
                      initialSort: _fileBrowserSort,
                      initialSortAscending: _fileBrowserSortAscending,
                      initialShowHidden: _fileBrowserShowHidden,
                      onSortChanged: _rememberFileSort,
                      onShowHiddenChanged: _rememberFileShowHidden,
                      onClose: _closeFileBrowser,
                    ),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  /// The whole page for a session without a screen stream: a small header
  /// with the target and transport, and the shared-folder browser taking the
  /// rest. The browser's close button leaves the session because there is
  /// nothing else to return to.
  Widget _buildFilesOnly() {
    final bool showOverlay = _startError != null || _status.isOver;
    final bool live = _status.isLive;
    return PopScope(
      canPop: true,
      onPopInvokedWithResult: (bool didPop, Object? result) {
        if (didPop) {
          _cleanupAfterPop();
        }
      },
      child: Scaffold(
        backgroundColor: Colors.black,
        body: Column(
          children: <Widget>[
            Container(
              height: 44,
              padding: const EdgeInsets.symmetric(horizontal: 12),
              color: const Color(0xFF1A1B20),
              child: Row(
                children: <Widget>[
                  const Icon(
                    Icons.folder_shared_rounded,
                    size: 16,
                    color: kAccentBright,
                  ),
                  const SizedBox(width: 8),
                  Text(
                    tr('文件传输'),
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 12.5,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: Text(
                      widget.target.label,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        color: Colors.white54,
                        fontSize: 11.5,
                      ),
                    ),
                  ),
                  if (live)
                    Text(
                      _status.transportLabel,
                      style: const TextStyle(
                        color: Colors.white38,
                        fontSize: 11,
                      ),
                    ),
                  const SizedBox(width: 8),
                  SizedBox(
                    width: 30,
                    height: 30,
                    child: IconButton(
                      tooltip: tr('关闭'),
                      padding: EdgeInsets.zero,
                      icon: const Icon(
                        Icons.close_rounded,
                        size: 17,
                        color: Colors.white70,
                      ),
                      onPressed: _leave,
                    ),
                  ),
                ],
              ),
            ),
            const Divider(height: 1, color: Colors.white12),
            Expanded(
              child: Container(
                color: Colors.black,
                child: Center(
                  child: _autoPolicy.waiting && showOverlay
                      ? _buildAutoReconnect(false)
                      : showOverlay
                      ? _buildDisconnected(false)
                      : !live
                      ? _buildWaiting()
                      : LayoutBuilder(
                          builder:
                              (
                                BuildContext context,
                                BoxConstraints constraints,
                              ) {
                                final double panelWidth =
                                    (constraints.maxWidth - 48).clamp(
                                      300.0,
                                      860.0,
                                    );
                                final double panelHeight =
                                    (constraints.maxHeight - 32).clamp(
                                      280.0,
                                      720.0,
                                    );
                                return FileBrowserPanel(
                                  width: panelWidth,
                                  height: panelHeight,
                                  status: _status,
                                  onRequestList: ViewerBridge.requestFileList,
                                  onRequestDownload:
                                      ViewerBridge.requestFileDownload,
                                  onTakeList: ViewerBridge.takeFileList,
                                  onUpload: ViewerBridge.sendFileTo,
                                  onFileOperation:
                                      (
                                        ViewerFileOperation op,
                                        String path,
                                        String newName,
                                      ) => ViewerBridge.fileOperation(
                                        op,
                                        path,
                                        newName: newName,
                                      ),
                                  onDownloadFolder: ViewerBridge.downloadFolder,
                                  onDownloadEntries:
                                      ViewerBridge.downloadEntries,
                                  onCancelFolderDownload:
                                      ViewerBridge.cancelFolderDownload,
                                  onCancelUpload: ViewerBridge.cancelUploads,
                                  onCancelDownload: ViewerBridge.cancelDownload,
                                  initialDirectory: _fileBrowserDirectory,
                                  onDirectoryChanged:
                                      _rememberFileBrowserDirectory,
                                  initialSort: _fileBrowserSort,
                                  initialSortAscending:
                                      _fileBrowserSortAscending,
                                  initialShowHidden: _fileBrowserShowHidden,
                                  onSortChanged: _rememberFileSort,
                                  onShowHiddenChanged: _rememberFileShowHidden,
                                  onClose: _leave,
                                );
                              },
                        ),
                ),
              ),
            ),
            if (live && (_status.fileActive || _status.downloadActive))
              Padding(
                padding: const EdgeInsets.fromLTRB(12, 0, 12, 10),
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: _buildFileProgress(),
                ),
              ),
          ],
        ),
      ),
    );
  }

  /// The whole page for a standalone session (protocol v41): a small header
  /// naming the connection type and the mode's own panel in the body. The
  /// page never shows the remote-screen stack; the close button leaves the
  /// session.
  Widget _buildControlOnly(String mode) {
    final bool showOverlay = _startError != null || _status.isOver;
    final bool live = _status.isLive;
    final (IconData icon, String title) = switch (mode) {
      'terminal' => (Icons.terminal_rounded, tr('远程终端')),
      'camera' => (Icons.videocam_rounded, tr('主机摄像头')),
      'port_forward' => (Icons.alt_route_rounded, tr('端口转发')),
      _ => (Icons.cable_rounded, tr('远程会话')),
    };
    return PopScope(
      canPop: true,
      onPopInvokedWithResult: (bool didPop, Object? result) {
        if (didPop) {
          _cleanupAfterPop();
        }
      },
      child: Scaffold(
        backgroundColor: Colors.black,
        body: Column(
          children: <Widget>[
            Container(
              height: 44,
              padding: const EdgeInsets.symmetric(horizontal: 12),
              color: const Color(0xFF1A1B20),
              child: Row(
                children: <Widget>[
                  Icon(icon, size: 16, color: kAccentBright),
                  const SizedBox(width: 8),
                  Text(
                    title,
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 12.5,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: Text(
                      widget.target.label,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        color: Colors.white54,
                        fontSize: 11.5,
                      ),
                    ),
                  ),
                  if (live)
                    Text(
                      _status.transportLabel,
                      style: const TextStyle(
                        color: Colors.white38,
                        fontSize: 11,
                      ),
                    ),
                  const SizedBox(width: 8),
                  SizedBox(
                    width: 30,
                    height: 30,
                    child: IconButton(
                      tooltip: tr('关闭'),
                      padding: EdgeInsets.zero,
                      icon: const Icon(
                        Icons.close_rounded,
                        size: 17,
                        color: Colors.white70,
                      ),
                      onPressed: _leave,
                    ),
                  ),
                ],
              ),
            ),
            const Divider(height: 1, color: Colors.white12),
            Expanded(
              child: Container(
                color: Colors.black,
                child: _autoPolicy.waiting && showOverlay
                    ? _buildAutoReconnect(false)
                    : showOverlay
                    ? _buildDisconnected(false)
                    : !live
                    ? _buildWaiting()
                    : _buildControlBody(mode),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildControlBody(String mode) {
    switch (mode) {
      case 'terminal':
        return Padding(
          padding: const EdgeInsets.all(12),
          child: TerminalTabs(
            status: _status,
            focusNode: _terminalFocus,
            initialTabs: _terminalTabIds,
            resumeServiceIds: _terminalServiceIds,
            onTabsChanged: _onTerminalTabsChanged,
            onDismiss: () {
              // The X ends every console before the standalone session goes;
              // otherwise a persistent service would outlive the panel the
              // operator just dismissed.
              for (final int id in _terminalTabIds) {
                unawaited(ViewerBridge.closeTerminal(terminalId: id));
              }
              _terminalTabIds = <int>[1];
              _terminalServiceIds.fillRange(0, kViewerMaxTerminals, 0);
              unawaited(_leave());
            },
            initialFontSize: _terminalFontSize,
            onFontSizeChanged: _rememberTerminalFontSize,
          ),
        );
      case 'camera':
        if (!_cameraOpen) {
          return _cameraOpening
              ? _buildControlNote(
                  icon: Icons.videocam_rounded,
                  title: tr('正在打开摄像头…'),
                  detail: tr('面板准备好后会自动列出主机上的摄像头。'),
                )
              : _buildControlNote(
                  icon: Icons.videocam_rounded,
                  title: tr('主机摄像头'),
                  detail: tr('连接已建立，可以打开主机摄像头。'),
                  actionLabel: tr('打开摄像头'),
                  onAction: () => unawaited(_openCameraPanel()),
                );
        }
        return Center(
          child: Padding(
            padding: const EdgeInsets.all(12),
            child: SizedBox(
              width: 560,
              child: CameraPanel(
                status: _status,
                textureId: _cameraTextureId,
                onRefresh: () =>
                    unawaited(_guardSend(ViewerBridge.requestCameraList())),
                onStart: (int cameraId) => unawaited(_startCamera(cameraId)),
                onStop: () => unawaited(_stopCamera()),
                onDismiss: _leave,
              ),
            ),
          ),
        );
      case 'port_forward':
        return Center(child: _buildPortForwardCard());
      default:
        return _buildWaiting();
    }
  }

  Widget _buildPortForwardCard() {
    final ViewerStatus status = _status;
    final List<ViewerPortForwardRule> rules = status.portForwardRules;
    if (rules.isEmpty) {
      return _buildControlNote(
        icon: Icons.alt_route_rounded,
        title: tr('端口转发'),
        detail: tr('本机在 127.0.0.1 上监听，主机把每个连入的 TCP 连接转发到目标地址。本地端口填 0 由系统分配。'),
        actionLabel: tr('配置端口转发'),
        onAction: () => unawaited(_openPortForwardDialog()),
      );
    }
    return Container(
      width: 420,
      padding: const EdgeInsets.fromLTRB(26, 24, 26, 20),
      decoration: BoxDecoration(
        color: const Color(0xFF1C1D22),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: Colors.white12),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(
            children: <Widget>[
              const Icon(
                Icons.alt_route_rounded,
                size: 18,
                color: kAccentBright,
              ),
              const SizedBox(width: 8),
              Text(
                tr('端口转发'),
                style: const TextStyle(
                  color: Colors.white,
                  fontSize: 13.5,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          ConstrainedBox(
            constraints: const BoxConstraints(maxHeight: 260),
            child: SingleChildScrollView(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: <Widget>[
                  for (final ViewerPortForwardRule rule in rules)
                    _buildPortForwardRuleRow(rule),
                ],
              ),
            ),
          ),
          const SizedBox(height: 12),
          SizedBox(
            width: double.infinity,
            child: FilledButton.icon(
              onPressed: () => unawaited(_openPortForwardDialog()),
              icon: const Icon(Icons.add_link_rounded, size: 18),
              label: Text(tr('添加规则')),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildControlNote({
    required IconData icon,
    required String title,
    required String detail,
    String? actionLabel,
    VoidCallback? onAction,
  }) {
    return Center(
      child: Container(
        width: 380,
        padding: const EdgeInsets.fromLTRB(26, 26, 26, 22),
        decoration: BoxDecoration(
          color: const Color(0xFF1C1D22),
          borderRadius: BorderRadius.circular(14),
          border: Border.all(color: Colors.white12),
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Icon(icon, size: 30, color: Colors.white54),
            const SizedBox(height: 12),
            Text(
              title,
              style: const TextStyle(
                color: Colors.white,
                fontSize: 14,
                fontWeight: FontWeight.w600,
              ),
            ),
            const SizedBox(height: 8),
            Text(
              detail,
              textAlign: TextAlign.center,
              style: const TextStyle(
                color: Colors.white54,
                fontSize: 12,
                height: 1.45,
              ),
            ),
            if (actionLabel != null && onAction != null) ...<Widget>[
              const SizedBox(height: 16),
              FilledButton(onPressed: onAction, child: Text(actionLabel)),
            ],
          ],
        ),
      ),
    );
  }

  /// A small bottom-right note for a state that does not stop the stream.
  Widget _statusBadge(IconData icon, String text) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.55),
        borderRadius: BorderRadius.circular(6),
        border: Border.all(color: Colors.white12),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          Icon(icon, size: 13, color: Colors.white54),
          const SizedBox(width: 5),
          Text(
            text,
            style: const TextStyle(color: Colors.white54, fontSize: 10.5),
          ),
        ],
      ),
    );
  }

  /// The upload sits above the bottom edge so it does not cover the keyboard
  /// reminder or the mute badge, and it never takes pointer input.
  Widget _buildFileProgress() {
    final ViewerStatus status = _status;
    final bool sending = status.filePhase == ViewerFilePhase.sending;
    final String phase = switch (status.filePhase) {
      ViewerFilePhase.hashing => tr('正在校验'),
      ViewerFilePhase.offered => tr('等待主机接收'),
      ViewerFilePhase.sending => tr('正在发送'),
      _ => tr('准备发送'),
    };
    final String detail = status.fileSize > 0
        ? '${_formatBytes(status.fileSent)} / ${_formatBytes(status.fileSize)}'
        : '';
    final String path = status.fileRelativePath.isEmpty
        ? status.fileName
        : '${status.fileRelativePath}/${status.fileName}';
    // fileQueue counts the entry on screen too; the note is for the rest.
    final String waiting = status.fileQueue > 1
        ? tr('队列剩余 {p18} 个', <String, Object?>{'p18': status.fileQueue - 1})
        : '';
    return IgnorePointer(
      child: Container(
        width: 340,
        padding: const EdgeInsets.fromLTRB(10, 8, 10, 9),
        decoration: BoxDecoration(
          color: Colors.black.withValues(alpha: 0.62),
          borderRadius: BorderRadius.circular(6),
          border: Border.all(color: Colors.white12),
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Row(
              children: <Widget>[
                const Icon(
                  Icons.upload_file_rounded,
                  size: 13,
                  color: Colors.white70,
                ),
                const SizedBox(width: 6),
                Expanded(
                  child: Text(
                    path,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(
                      color: Colors.white70,
                      fontSize: 10.5,
                    ),
                  ),
                ),
                const SizedBox(width: 8),
                Text(
                  phase,
                  style: const TextStyle(color: Colors.white38, fontSize: 9.5),
                ),
              ],
            ),
            const SizedBox(height: 6),
            ClipRRect(
              borderRadius: BorderRadius.circular(2),
              child: LinearProgressIndicator(
                value: sending ? status.fileProgress : null,
                minHeight: 3,
                backgroundColor: Colors.white12,
                valueColor: const AlwaysStoppedAnimation<Color>(kAccentBright),
              ),
            ),
            if (detail.isNotEmpty || waiting.isNotEmpty) ...<Widget>[
              const SizedBox(height: 4),
              Row(
                children: <Widget>[
                  if (detail.isNotEmpty)
                    Text(
                      detail,
                      style: const TextStyle(
                        color: Colors.white38,
                        fontSize: 9.5,
                      ),
                    ),
                  if (waiting.isNotEmpty) ...<Widget>[
                    const Spacer(),
                    Text(
                      waiting,
                      style: const TextStyle(
                        color: Colors.white38,
                        fontSize: 9.5,
                      ),
                    ),
                  ],
                ],
              ),
            ],
          ],
        ),
      ),
    );
  }

  /// The host's download, drawn below the upload when both run at once. The
  /// bytes are already in the receive directory, so this only reports them.
  Widget _buildDownloadProgress() {
    final ViewerStatus status = _status;
    final String detail = status.downloadSize > 0
        ? '${_formatBytes(status.downloadReceived)} / '
              '${_formatBytes(status.downloadSize)}'
        : '';
    return IgnorePointer(
      child: Container(
        width: 340,
        padding: const EdgeInsets.fromLTRB(10, 8, 10, 9),
        decoration: BoxDecoration(
          color: Colors.black.withValues(alpha: 0.62),
          borderRadius: BorderRadius.circular(6),
          border: Border.all(color: Colors.white12),
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Row(
              children: <Widget>[
                const Icon(
                  Icons.download_rounded,
                  size: 13,
                  color: Colors.white70,
                ),
                const SizedBox(width: 6),
                Expanded(
                  child: Text(
                    status.downloadName,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(
                      color: Colors.white70,
                      fontSize: 10.5,
                    ),
                  ),
                ),
                const SizedBox(width: 8),
                Text(
                  tr('正在接收'),
                  style: TextStyle(color: Colors.white38, fontSize: 9.5),
                ),
              ],
            ),
            const SizedBox(height: 6),
            ClipRRect(
              borderRadius: BorderRadius.circular(2),
              child: LinearProgressIndicator(
                value: status.downloadProgress,
                minHeight: 3,
                backgroundColor: Colors.white12,
                valueColor: const AlwaysStoppedAnimation<Color>(kAccentBright),
              ),
            ),
            if (detail.isNotEmpty) ...<Widget>[
              const SizedBox(height: 4),
              Text(
                detail,
                style: const TextStyle(color: Colors.white38, fontSize: 9.5),
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _buildWaiting() {
    final bool streaming = _status.state == ViewerState.streaming;
    return Container(
      color: Colors.black,
      child: Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            const SizedBox(
              width: 30,
              height: 30,
              child: CircularProgressIndicator(
                strokeWidth: 2.4,
                color: kAccentBright,
                backgroundColor: Colors.white12,
              ),
            ),
            const SizedBox(height: 18),
            Text(
              _stateText,
              style: const TextStyle(color: Colors.white70, fontSize: 14),
            ),
            const SizedBox(height: 6),
            Text(
              widget.target.label,
              style: const TextStyle(
                color: Colors.white38,
                fontSize: 12,
                letterSpacing: 0.3,
              ),
            ),
            if (_tunnelPath.isNotEmpty) ...<Widget>[
              const SizedBox(height: 4),
              Text(
                tr('隧道 · {_tunnelPath}', <String, Object?>{
                  '_tunnelPath': _tunnelPath,
                }),
                style: const TextStyle(
                  color: Colors.white30,
                  fontSize: 11,
                  letterSpacing: 0.3,
                ),
              ),
            ],
            if (streaming && _status.width > 0) ...<Widget>[
              const SizedBox(height: 10),
              Text(
                '${_status.codecLabel} · ${_status.width}×${_status.height}',
                style: const TextStyle(color: Colors.white30, fontSize: 11),
              ),
            ],
            const SizedBox(height: 20),
            TextButton.icon(
              onPressed: _leave,
              style: TextButton.styleFrom(
                foregroundColor: Colors.white54,
                textStyle: const TextStyle(fontSize: 12),
              ),
              icon: const Icon(Icons.close_rounded, size: 15),
              label: Text(tr('取消连接')),
            ),
          ],
        ),
      ),
    );
  }

  /// Shown while an unexpected drop is waiting out its backoff. The native
  /// session is already gone; the page only holds the texture and the tunnel
  /// until the next attempt, so leaving remains safe and immediate.
  Widget _buildAutoReconnect(bool hasFrame) {
    final String detail = _status.error;
    return Container(
      color: hasFrame ? Colors.black.withValues(alpha: 0.55) : Colors.black,
      child: Center(
        child: Container(
          width: 384,
          padding: const EdgeInsets.fromLTRB(26, 26, 26, 22),
          decoration: BoxDecoration(
            color: const Color(0xFF1C1D22),
            borderRadius: BorderRadius.circular(14),
            border: Border.all(color: Colors.white12),
            boxShadow: const <BoxShadow>[
              BoxShadow(
                color: Color(0x80000000),
                blurRadius: 30,
                offset: Offset(0, 12),
              ),
            ],
          ),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              const SizedBox(
                width: 30,
                height: 30,
                child: CircularProgressIndicator(
                  strokeWidth: 2.4,
                  color: kAccentBright,
                  backgroundColor: Colors.white12,
                ),
              ),
              const SizedBox(height: 16),
              Text(
                tr('连接中断'),
                style: TextStyle(
                  color: Colors.white,
                  fontSize: 17,
                  fontWeight: FontWeight.w600,
                ),
              ),
              const SizedBox(height: 8),
              Text(
                tr('正在自动重连（第 {p24}/{p25} 次）', <String, Object?>{
                  'p24': _autoPolicy.attempt,
                  'p25': _autoPolicy.attemptLimit,
                }),
                style: const TextStyle(color: Colors.white54, fontSize: 12.5),
              ),
              if (detail.isNotEmpty) ...<Widget>[
                const SizedBox(height: 8),
                Text(
                  detail,
                  textAlign: TextAlign.center,
                  style: const TextStyle(
                    color: Colors.white38,
                    fontSize: 11.5,
                    height: 1.4,
                  ),
                ),
              ],
              const SizedBox(height: 18),
              TextButton.icon(
                onPressed: _leave,
                style: TextButton.styleFrom(
                  foregroundColor: Colors.white54,
                  textStyle: const TextStyle(fontSize: 12),
                ),
                icon: const Icon(Icons.close_rounded, size: 15),
                label: Text(tr('取消连接')),
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildDisconnected(bool hasFrame) {
    final int anomalies =
        _status.crcErrors +
        _status.decodeErrors +
        _status.protocolErrors +
        _status.gaps;
    final bool failed =
        _startError != null || _status.state == ViewerState.failed;
    return Container(
      color: hasFrame ? Colors.black.withValues(alpha: 0.55) : Colors.black,
      child: Center(
        child: Container(
          width: 384,
          padding: const EdgeInsets.fromLTRB(26, 26, 26, 22),
          decoration: BoxDecoration(
            color: const Color(0xFF1C1D22),
            borderRadius: BorderRadius.circular(14),
            border: Border.all(color: Colors.white12),
            boxShadow: const <BoxShadow>[
              BoxShadow(
                color: Color(0x80000000),
                blurRadius: 30,
                offset: Offset(0, 12),
              ),
            ],
          ),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              Container(
                width: 46,
                height: 46,
                decoration: BoxDecoration(
                  color: (failed ? Colors.redAccent : kAccent).withValues(
                    alpha: 0.14,
                  ),
                  shape: BoxShape.circle,
                ),
                child: Icon(
                  _denied
                      ? Icons.lock_outline_rounded
                      : (failed
                            ? Icons.link_off_rounded
                            : Icons.videocam_off_rounded),
                  size: 23,
                  color: failed ? Colors.redAccent : kAccentBright,
                ),
              ),
              const SizedBox(height: 16),
              Text(
                _overTitle,
                style: const TextStyle(
                  color: Colors.white,
                  fontSize: 17,
                  fontWeight: FontWeight.w600,
                ),
              ),
              const SizedBox(height: 8),
              Text(
                _overDetail,
                textAlign: TextAlign.center,
                style: const TextStyle(
                  color: Colors.white54,
                  fontSize: 12.5,
                  height: 1.5,
                ),
              ),
              if (_identityChanged) ...<Widget>[
                const SizedBox(height: 14),
                Container(
                  padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
                  decoration: BoxDecoration(
                    color: const Color(0xFFFFB74D).withValues(alpha: 0.08),
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(
                      color: const Color(0xFFFFB74D).withValues(alpha: 0.32),
                    ),
                  ),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Row(
                        children: <Widget>[
                          Icon(
                            Icons.shield_outlined,
                            size: 14,
                            color: Color(0xFFFFB74D),
                          ),
                          SizedBox(width: 6),
                          Text(
                            tr('服务器证书指纹已变化'),
                            style: TextStyle(
                              color: Color(0xFFFFCC80),
                              fontSize: 12,
                              fontWeight: FontWeight.w600,
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: 6),
                      Text(
                        tr('已固定 {p26} · 当前 {p27}', <String, Object?>{
                          'p26': shortenFingerprint(_pinnedFingerprint),
                          'p27': shortenFingerprint(_status.serverFingerprint),
                        }),
                        style: const TextStyle(
                          color: Colors.white54,
                          fontSize: 11,
                          height: 1.4,
                        ),
                      ),
                      const SizedBox(height: 4),
                      Text(
                        tr('对端可能重装或换了证书。确认这是同一台机器后再信任；否则请不要继续，避免连到冒充的主机。'),
                        style: TextStyle(
                          color: Colors.white38,
                          fontSize: 11,
                          height: 1.4,
                        ),
                      ),
                    ],
                  ),
                ),
              ],
              if (_fingerprintUnpersisted) ...<Widget>[
                const SizedBox(height: 14),
                Container(
                  padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
                  decoration: BoxDecoration(
                    color: const Color(0xFFFFB74D).withValues(alpha: 0.08),
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(
                      color: const Color(0xFFFFB74D).withValues(alpha: 0.32),
                    ),
                  ),
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      const Icon(
                        Icons.shield_outlined,
                        size: 14,
                        color: Color(0xFFFFB74D),
                      ),
                      const SizedBox(width: 6),
                      Expanded(
                        child: Text(
                          tr('这次连接没有固定主机证书：指纹无法保存到本地，下次连接会重新确认。'),
                          style: const TextStyle(
                            color: Colors.white54,
                            fontSize: 11,
                            height: 1.4,
                          ),
                        ),
                      ),
                    ],
                  ),
                ),
              ],
              if (_status.received > 0) ...<Widget>[
                const SizedBox(height: 12),
                Container(height: 1, color: Colors.white12),
                const SizedBox(height: 10),
                Text(
                  tr('接收 {p28} 帧 · 解码 {p29} 帧{p30}', <String, Object?>{
                    'p28': _status.received,
                    'p29': _status.decoded,
                    'p30': anomalies > 0
                        ? tr(' · 异常 {anomalies}', <String, Object?>{
                            'anomalies': anomalies,
                          })
                        : '',
                  }),
                  style: const TextStyle(color: Colors.white38, fontSize: 11),
                ),
              ],
              const SizedBox(height: 20),
              if (_denied)
                SizedBox(
                  width: double.infinity,
                  height: 38,
                  child: FilledButton(
                    onPressed: _leave,
                    style: FilledButton.styleFrom(
                      backgroundColor: kAccent,
                      foregroundColor: Colors.white,
                      elevation: 0,
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(8),
                      ),
                      textStyle: const TextStyle(
                        fontSize: 13,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                    child: Text(tr('返回并修改令牌')),
                  ),
                )
              else
                Row(
                  children: <Widget>[
                    Expanded(
                      child: SizedBox(
                        height: 38,
                        child: OutlinedButton(
                          onPressed: _leave,
                          style: OutlinedButton.styleFrom(
                            foregroundColor: Colors.white70,
                            side: const BorderSide(color: Colors.white24),
                            shape: RoundedRectangleBorder(
                              borderRadius: BorderRadius.circular(8),
                            ),
                            textStyle: const TextStyle(fontSize: 13),
                          ),
                          child: Text(tr('返回')),
                        ),
                      ),
                    ),
                    const SizedBox(width: 10),
                    Expanded(
                      child: SizedBox(
                        height: 38,
                        child: FilledButton(
                          onPressed: _reconnecting
                              ? null
                              : (_identityChanged
                                    ? () => unawaited(_trustNewIdentity())
                                    : _reconnect),
                          style: FilledButton.styleFrom(
                            backgroundColor: kAccent,
                            foregroundColor: Colors.white,
                            disabledBackgroundColor: kAccent.withValues(
                              alpha: 0.5,
                            ),
                            elevation: 0,
                            shape: RoundedRectangleBorder(
                              borderRadius: BorderRadius.circular(8),
                            ),
                            textStyle: const TextStyle(
                              fontSize: 13,
                              fontWeight: FontWeight.w600,
                            ),
                          ),
                          child: _reconnecting
                              ? const SizedBox(
                                  width: 15,
                                  height: 15,
                                  child: CircularProgressIndicator(
                                    strokeWidth: 2,
                                    valueColor: AlwaysStoppedAnimation<Color>(
                                      Colors.white,
                                    ),
                                  ),
                                )
                              : Text(
                                  _identityChanged ? tr('信任并重新连接') : tr('重新连接'),
                                ),
                        ),
                      ),
                    ),
                  ],
                ),
              if (failed && !_denied && !_identityChanged) ...<Widget>[
                if (_emergencyActive)
                  Padding(
                    padding: const EdgeInsets.only(top: 8),
                    child: Text(
                      tr('本次会话由管理员应急授权，操作已记录审计'),
                      textAlign: TextAlign.center,
                      style: const TextStyle(
                        color: Color(0xFFFFCC80),
                        fontSize: 11.5,
                      ),
                    ),
                  )
                else if (widget.target.viaServer &&
                    widget.target.hostId.isNotEmpty)
                  Padding(
                    padding: const EdgeInsets.only(top: 2),
                    child: TextButton.icon(
                      onPressed: _reconnecting
                          ? null
                          : () => unawaited(_promptEmergencyAccess()),
                      style: TextButton.styleFrom(
                        foregroundColor: const Color(0xFFFFCC80),
                        textStyle: const TextStyle(fontSize: 12),
                      ),
                      icon: const Icon(
                        Icons.admin_panel_settings_outlined,
                        size: 15,
                      ),
                      label: Text(tr('管理员应急访问')),
                    ),
                  ),
              ],
            ],
          ),
        ),
      ),
    );
  }
}

/// True when the host refused the Hello only because it wants credentials,
/// i.e. the viewer can solve it without the operator changing the token.
bool _isCredentialChallenge(int denial) {
  return denial == ViewerDenial.passwordRequired ||
      denial == ViewerDenial.wrongPassword ||
      denial == ViewerDenial.twoFactorRequired ||
      denial == ViewerDenial.wrongTwoFactor;
}

class _CredentialResult {
  const _CredentialResult(this.text, this.remember);

  final String text;
  final bool remember;
}

class _EmergencyAccessResult {
  const _EmergencyAccessResult({required this.token, required this.reason});

  final String token;
  final String reason;
}

/// Collects the administrator login and the mandatory reason for an explicit
/// override. The login happens here so a wrong password or two-step code is
/// reported inline instead of surfacing as a tunnel start failure. The token
/// is returned to the page and never stored.
class _EmergencyAccessDialog extends StatefulWidget {
  const _EmergencyAccessDialog({required this.server, required this.proxy});

  final String server;
  final String proxy;

  @override
  State<_EmergencyAccessDialog> createState() => _EmergencyAccessDialogState();
}

class _EmergencyAccessDialogState extends State<_EmergencyAccessDialog> {
  final TextEditingController _username = TextEditingController();
  final TextEditingController _password = TextEditingController();
  final TextEditingController _totp = TextEditingController();
  final TextEditingController _reason = TextEditingController();
  bool _busy = false;
  String? _error;

  @override
  void dispose() {
    _username.dispose();
    _password.dispose();
    _totp.dispose();
    _reason.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    if (_busy) {
      return;
    }
    final String reason = _reason.text.trim().replaceAll(RegExp(r'\s+'), ' ');
    if (reason.isEmpty) {
      setState(() => _error = tr('请填写应急访问原因'));
      return;
    }
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      ServerApi.proxy = widget.proxy;
      final AccountLoginResult login = await ServerApi.login(
        ServerSettings(url: widget.server),
        username: _username.text,
        password: _password.text,
        totpCode: _totp.text,
        deviceName: Platform.localHostname,
      );
      if (!mounted) {
        return;
      }
      Navigator.of(
        context,
      ).pop(_EmergencyAccessResult(token: login.token, reason: reason));
    } on ServerApiException catch (error) {
      if (!mounted) {
        return;
      }
      setState(() {
        _busy = false;
        _error = error.message;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: Text(tr('管理员应急访问')),
      content: SizedBox(
        width: 360,
        child: SingleChildScrollView(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Text(
                tr(
                  '受限设备拒绝普通账号时，管理员可为本次连接签发 60 秒应急票据。操作会记录管理员身份和填写的原因，且仍受设备权限上限与主机本地认证约束。',
                ),
                style: const TextStyle(
                  color: Colors.white54,
                  fontSize: 12,
                  height: 1.4,
                ),
              ),
              const SizedBox(height: 14),
              TextField(
                controller: _username,
                autofocus: true,
                enabled: !_busy,
                textInputAction: TextInputAction.next,
                decoration: InputDecoration(labelText: tr('管理员用户名')),
              ),
              const SizedBox(height: 10),
              TextField(
                controller: _password,
                enabled: !_busy,
                obscureText: true,
                textInputAction: TextInputAction.next,
                decoration: InputDecoration(labelText: tr('管理员密码')),
              ),
              const SizedBox(height: 10),
              TextField(
                controller: _totp,
                enabled: !_busy,
                keyboardType: TextInputType.number,
                maxLength: 16,
                decoration: InputDecoration(
                  labelText: tr('两步验证码（可选）'),
                  counterText: '',
                ),
              ),
              const SizedBox(height: 10),
              TextField(
                controller: _reason,
                enabled: !_busy,
                maxLength: 160,
                onSubmitted: (_) => _submit(),
                decoration: InputDecoration(
                  labelText: tr('应急原因'),
                  hintText: tr('例如：工单 4711，远程桌面无法登录'),
                  counterText: '',
                  errorText: _error,
                ),
              ),
            ],
          ),
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: _busy ? null : () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
        FilledButton(
          onPressed: _busy ? null : _submit,
          child: _busy
              ? const SizedBox(
                  width: 16,
                  height: 16,
                  child: CircularProgressIndicator(strokeWidth: 2),
                )
              : Text(tr('登录并连接')),
        ),
      ],
    );
  }
}

class _CredentialDialog extends StatefulWidget {
  const _CredentialDialog({
    required this.passwordStep,
    required this.initialPassword,
    required this.initialRemember,
    required this.detail,
  });

  final bool passwordStep;
  final String initialPassword;
  final bool initialRemember;
  final String detail;

  @override
  State<_CredentialDialog> createState() => _CredentialDialogState();
}

class _CredentialDialogState extends State<_CredentialDialog> {
  late final TextEditingController _text = TextEditingController(
    text: widget.passwordStep ? widget.initialPassword : '',
  );
  late bool _remember = widget.initialRemember;
  String? _error;

  @override
  void dispose() {
    _text.dispose();
    super.dispose();
  }

  void _submit() {
    final String value = _text.text.trim();
    if (value.isEmpty) {
      setState(
        () => _error = widget.passwordStep ? tr('请输入密码') : tr('请输入两步验证码'),
      );
      return;
    }
    Navigator.of(context).pop(_CredentialResult(value, _remember));
  }

  @override
  Widget build(BuildContext context) {
    final bool passwordStep = widget.passwordStep;
    return AlertDialog(
      title: Text(passwordStep ? tr('输入连接密码') : tr('输入两步验证码')),
      content: SizedBox(
        width: 320,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            if (widget.detail.isNotEmpty) ...<Widget>[
              Text(
                widget.detail,
                style: const TextStyle(
                  color: Colors.white54,
                  fontSize: 12,
                  height: 1.4,
                ),
              ),
              const SizedBox(height: 14),
            ],
            TextField(
              controller: _text,
              autofocus: true,
              obscureText: passwordStep,
              keyboardType: passwordStep
                  ? TextInputType.visiblePassword
                  : TextInputType.number,
              maxLength: passwordStep ? 128 : 16,
              onSubmitted: (_) => _submit(),
              decoration: InputDecoration(
                labelText: passwordStep ? tr('密码') : tr('6 位验证码'),
                errorText: _error,
                counterText: '',
              ),
            ),
            const SizedBox(height: 8),
            CheckboxListTile(
              value: _remember,
              onChanged: (bool? value) =>
                  setState(() => _remember = value ?? false),
              contentPadding: EdgeInsets.zero,
              controlAffinity: ListTileControlAffinity.leading,
              dense: true,
              title: Text(
                passwordStep ? tr('在本机加密保存密码') : tr('信任此设备，下次跳过两步验证'),
                style: const TextStyle(fontSize: 12.5),
              ),
            ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
        FilledButton(onPressed: _submit, child: Text(tr('连接'))),
      ],
    );
  }
}

/// What the operator chose for a same-name conflict. The decision numbers
/// match the protocol: 0 skip, 1 overwrite, 2 cancel.
class _FileConflictAnswer {
  const _FileConflictAnswer(this.decision, this.remember);

  final int decision;
  final bool remember;
}

/// RustDesk-style prompt for a file that already exists on the other side.
/// Skip keeps the existing file, Overwrite replaces it and Cancel ends the
/// transfer (for an upload queue or folder download, the rest of it too).
class _FileConflictDialog extends StatefulWidget {
  const _FileConflictDialog({required this.status});

  final ViewerStatus status;

  @override
  State<_FileConflictDialog> createState() => _FileConflictDialogState();
}

class _FileConflictDialogState extends State<_FileConflictDialog> {
  bool _remember = false;

  @override
  Widget build(BuildContext context) {
    final ViewerStatus status = widget.status;
    final String existing = <String>[
      _formatBytes(status.fileConflictExistingSize),
      _formatConflictTime(status.fileConflictExistingTime),
    ].where((String part) => part.isNotEmpty).join(' · ');
    return AlertDialog(
      title: Text(tr('文件已存在')),
      content: SizedBox(
        width: 400,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text(
              status.fileConflictDownload
                  ? tr('主机发送的文件与本地已有文件同名：')
                  : tr('要上传的文件与主机上的文件同名：'),
              style: const TextStyle(fontSize: 12.5),
            ),
            const SizedBox(height: 8),
            SelectableText(
              status.fileConflictName,
              style: const TextStyle(fontWeight: FontWeight.w600),
            ),
            const SizedBox(height: 8),
            Text(
              tr('已有文件：{detail}', <String, Object?>{'detail': existing}),
              style: const TextStyle(fontSize: 12.5),
            ),
            Text(
              tr('新文件：{size}', <String, Object?>{
                'size': _formatBytes(status.fileConflictSize),
              }),
              style: const TextStyle(fontSize: 12.5),
            ),
            if (status.fileConflictIdentical) ...<Widget>[
              const SizedBox(height: 8),
              Text(
                tr('两个文件的大小和修改时间相同，内容可能一致。'),
                style: const TextStyle(
                  fontSize: 12,
                  color: Colors.orangeAccent,
                ),
              ),
            ],
            CheckboxListTile(
              value: _remember,
              onChanged: (bool? value) =>
                  setState(() => _remember = value ?? false),
              contentPadding: EdgeInsets.zero,
              controlAffinity: ListTileControlAffinity.leading,
              dense: true,
              title: Text(
                tr('对队列中的其余文件使用相同操作'),
                style: const TextStyle(fontSize: 12.5),
              ),
            ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () =>
              Navigator.of(context).pop(const _FileConflictAnswer(2, false)),
          child: Text(tr('取消传输')),
        ),
        TextButton(
          onPressed: () =>
              Navigator.of(context).pop(_FileConflictAnswer(0, _remember)),
          child: Text(tr('跳过')),
        ),
        FilledButton(
          onPressed: () =>
              Navigator.of(context).pop(_FileConflictAnswer(1, _remember)),
          child: Text(tr('覆盖')),
        ),
      ],
    );
  }
}

String _formatConflictTime(int seconds) {
  if (seconds <= 0) {
    return '';
  }
  final DateTime at = DateTime.fromMillisecondsSinceEpoch(seconds * 1000);
  String two(int value) => value.toString().padLeft(2, '0');
  return '${at.year}-${two(at.month)}-${two(at.day)} '
      '${two(at.hour)}:${two(at.minute)}';
}

String _formatBytes(int bytes) {
  if (bytes < 1024) {
    return '$bytes B';
  }
  const List<String> units = <String>['KiB', 'MiB', 'GiB', 'TiB'];
  double value = bytes / 1024;
  int unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    ++unit;
  }
  return '${value.toStringAsFixed(value >= 100 ? 0 : 1)} ${units[unit]}';
}

/// What the picture menu showed before an optimistic change, so a refused
/// request can put it back.
class _StreamOptionSnapshot {
  const _StreamOptionSnapshot(
    this.codec,
    this.quality,
    this.qualityPercent,
    this.maxFps,
    this.fullColor,
  );

  final ViewerStreamCodec codec;
  final ViewerStreamQuality quality;
  final int qualityPercent;
  final int maxFps;
  final bool fullColor;
}

ViewerStreamQuality _streamQualityFor(int code) {
  for (final ViewerStreamQuality quality in ViewerStreamQuality.values) {
    if (quality.code == code) {
      return quality;
    }
  }
  return ViewerStreamQuality.high;
}

ViewerStreamCodec _streamCodecFor(int code) {
  for (final ViewerStreamCodec codec in ViewerStreamCodec.values) {
    if (codec.code == code) {
      return codec;
    }
  }
  return ViewerStreamCodec.auto;
}

FileBrowserSort _fileSortFor(int index) {
  for (final FileBrowserSort sort in FileBrowserSort.values) {
    if (sort.index == index) {
      return sort;
    }
  }
  return FileBrowserSort.name;
}

/// First-use and rotation confirmation for the rendezvous server key. The
/// fingerprint is the only value a user can compare out of band, so both the
/// old and the new one are shown in full when it changes.
class _ServerIdentityDialog extends StatelessWidget {
  const _ServerIdentityDialog({
    required this.server,
    required this.identity,
    this.previous,
  });

  final String server;
  final ServerIdentity identity;
  final ServerIdentity? previous;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final bool changed = previous != null;
    const TextStyle mono = TextStyle(
      fontFamily: 'Consolas',
      fontSize: 13,
      letterSpacing: 0.8,
      height: 1.5,
    );
    return AlertDialog(
      title: Text(changed ? tr('服务器身份已变化') : tr('确认服务器身份')),
      content: SingleChildScrollView(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text(server, style: theme.textTheme.bodySmall),
            const SizedBox(height: 12),
            if (changed) ...<Widget>[
              Text(tr('此前固定的指纹'), style: theme.textTheme.labelMedium),
              const SizedBox(height: 4),
              SelectableText(
                formatServerFingerprint(previous!.fingerprint),
                style: mono,
              ),
              const SizedBox(height: 12),
              Text(tr('本次连接提供的指纹'), style: theme.textTheme.labelMedium),
            ] else
              Text(
                tr('首次连接将固定这台服务器的身份。请与管理员公布的值核对：'),
                style: theme.textTheme.bodyMedium,
              ),
            const SizedBox(height: 4),
            SelectableText(
              formatServerFingerprint(identity.fingerprint),
              style: mono,
            ),
            const SizedBox(height: 12),
            Text(
              changed
                  ? tr('如果服务器没有更换密钥，这可能是中间人攻击。确认后旧指纹会被替换，并写入本地审计。')
                  : tr('以后连接若遇到不同身份会被拒绝，并在确认后更换。'),
              style: theme.textTheme.bodySmall,
            ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(false),
          child: Text(tr('取消')),
        ),
        FilledButton(
          key: const Key('server-identity-accept'),
          onPressed: () => Navigator.of(context).pop(true),
          child: Text(changed ? tr('信任新身份') : tr('信任并连接')),
        ),
      ],
    );
  }
}

/// First-use confirmation for the host's QUIC certificate. The fingerprint is
/// the only value a user can compare with the host operator, so it is shown in
/// full; rejecting stops the session instead of continuing unverified.
class _HostFingerprintDialog extends StatelessWidget {
  const _HostFingerprintDialog({required this.host, required this.fingerprint});

  final String host;
  final String fingerprint;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    const TextStyle mono = TextStyle(
      fontFamily: 'Consolas',
      fontSize: 13,
      letterSpacing: 0.8,
      height: 1.5,
    );
    return AlertDialog(
      title: Text(tr('确认主机证书')),
      content: SingleChildScrollView(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text(
              tr('首次连接到 {host}。请与主机管理员核对证书指纹：', <String, Object?>{
                'host': host,
              }),
              style: theme.textTheme.bodyMedium,
            ),
            const SizedBox(height: 8),
            SelectableText(formatServerFingerprint(fingerprint), style: mono),
            const SizedBox(height: 12),
            Text(
              tr('确认后会记住该指纹；以后证书变化会被拒绝，直到你再次确认。'),
              style: theme.textTheme.bodySmall,
            ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(false),
          child: Text(tr('取消连接')),
        ),
        FilledButton(
          key: const Key('host-fingerprint-accept'),
          onPressed: () => Navigator.of(context).pop(true),
          child: Text(tr('信任并连接')),
        ),
      ],
    );
  }
}

class _FinishedRecording {
  const _FinishedRecording({required this.path, this.startedAt});

  final String path;
  final DateTime? startedAt;
}

String _sessionListCodeText(int code) {
  if (code == ViewerSessionListCode.denied.code) {
    return tr('主机未授权读取会话列表');
  }
  if (code == ViewerSessionListCode.unsupported.code) {
    return tr('主机不支持读取 Windows 会话');
  }
  if (code == ViewerSessionListCode.failed.code) {
    return tr('主机无法读取会话列表');
  }
  return tr('主机拒绝了会话列表');
}

String _sessionSelectCodeText(int code) {
  if (code == ViewerSessionSelectCode.notFound.code) {
    return tr('目标会话已不活跃');
  }
  if (code == ViewerSessionSelectCode.denied.code) {
    return tr('主机未授权会话切换');
  }
  if (code == ViewerSessionSelectCode.unsupported.code) {
    return tr('主机不支持切换 Windows 会话');
  }
  if (code == ViewerSessionSelectCode.busy.code) {
    return tr('另一查看者正在控制主机');
  }
  if (code == ViewerSessionSelectCode.failed.code) {
    return tr('主机无法切换会话');
  }
  return tr('主机拒绝了会话切换');
}

/// One editable host policy switch.
class _RemoteConfigToggle {
  const _RemoteConfigToggle(this.bit, this.label);

  final int bit;
  final String label;
}

const List<_RemoteConfigToggle> _remoteConfigToggles = <_RemoteConfigToggle>[
  _RemoteConfigToggle(
    ViewerRemoteConfigSwitch.lockAfterSessionEnd,
    '会话结束后锁定本机',
  ),
  _RemoteConfigToggle(ViewerRemoteConfigSwitch.clipboard, '剪贴板同步'),
  _RemoteConfigToggle(ViewerRemoteConfigSwitch.fileTransfer, '文件传输'),
  _RemoteConfigToggle(ViewerRemoteConfigSwitch.audio, '声音播放'),
  _RemoteConfigToggle(ViewerRemoteConfigSwitch.microphone, '麦克风'),
  _RemoteConfigToggle(ViewerRemoteConfigSwitch.privacyMode, '隐私模式'),
  _RemoteConfigToggle(ViewerRemoteConfigSwitch.restart, '远程重启'),
  _RemoteConfigToggle(ViewerRemoteConfigSwitch.blockInput, '阻止本地输入'),
  _RemoteConfigToggle(ViewerRemoteConfigSwitch.osLogin, '操作系统登录'),
  _RemoteConfigToggle(ViewerRemoteConfigSwitch.sessionSelect, 'Windows 会话切换'),
  _RemoteConfigToggle(ViewerRemoteConfigSwitch.terminal, '远程终端'),
  _RemoteConfigToggle(ViewerRemoteConfigSwitch.portForward, '端口转发'),
  _RemoteConfigToggle(ViewerRemoteConfigSwitch.camera, '摄像头'),
  _RemoteConfigToggle(ViewerRemoteConfigSwitch.record, '录制授权'),
];

String _remoteConfigCodeText(int code) {
  switch (code) {
    case 1:
      return tr('主机拒绝了这次修改');
    case 2:
      return tr('主机不支持修改该设置');
    case 3:
      return tr('另一个查看者正在控制主机');
    case 4:
      return tr('主机无法保存这次修改');
  }
  return tr('主机拒绝了这次修改');
}

/// Edits the host's exposed policy switches. Only the switches the operator
/// actually changed are sent; the host's answer (the stored snapshot) is what
/// closes the dialog, so a refusal keeps the edits on screen.
class _RemoteConfigDialog extends StatefulWidget {
  const _RemoteConfigDialog({required this.initial});

  final ViewerStatus initial;

  @override
  State<_RemoteConfigDialog> createState() => _RemoteConfigDialogState();
}

class _RemoteConfigDialogState extends State<_RemoteConfigDialog> {
  late ViewerStatus _status = widget.initial;
  // The snapshot the checkboxes were seeded from; a save sends the difference.
  late int _baseFlags = widget.initial.remoteConfigFlags;
  late int _flags = widget.initial.remoteConfigFlags;
  int? _savingSince;
  bool _dirty = false;
  bool _saving = false;
  String? _error;
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    _timer = Timer.periodic(
      const Duration(milliseconds: 250),
      (_) => unawaited(_poll()),
    );
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  Future<void> _poll() async {
    try {
      final ViewerStatus next = await ViewerBridge.status();
      if (!mounted) {
        return;
      }
      bool applied = false;
      String? refusal;
      final int? savingSince = _savingSince;
      if (_saving &&
          savingSince != null &&
          next.remoteConfigUpdates > savingSince) {
        if (next.remoteConfigCode == ViewerRemoteConfigCode.ok.code) {
          applied = true;
        } else {
          refusal = next.remoteConfigError.isNotEmpty
              ? next.remoteConfigError
              : _remoteConfigCodeText(next.remoteConfigCode);
        }
      }
      setState(() {
        // A fresh answer from the read request replaces the seed while the
        // operator has not touched anything; unsaved edits always win.
        if (!_dirty &&
            next.remoteConfigUpdates != _status.remoteConfigUpdates) {
          _baseFlags = next.remoteConfigFlags;
          _flags = next.remoteConfigFlags;
        }
        _status = next;
        if (applied) {
          _saving = false;
        }
        if (refusal != null) {
          _saving = false;
          _savingSince = null;
          _error = refusal;
        }
      });
      if (applied && mounted) {
        Navigator.of(context).pop(true);
      }
    } on MissingPluginException {
      // The native viewer is going away.
    } on PlatformException {
      // A single failed poll is transient; the next tick retries.
    }
  }

  Future<void> _save() async {
    if (_saving) {
      return;
    }
    int keys = 0;
    int values = 0;
    for (final _RemoteConfigToggle toggle in _remoteConfigToggles) {
      final bool now = (_flags & toggle.bit) != 0;
      final bool was = (_baseFlags & toggle.bit) != 0;
      if (now != was) {
        keys |= toggle.bit;
        if (now) {
          values |= toggle.bit;
        }
      }
    }
    if (keys == 0) {
      Navigator.of(context).pop(false);
      return;
    }
    setState(() {
      _saving = true;
      _savingSince = _status.remoteConfigUpdates;
      _error = null;
    });
    try {
      final bool queued = await ViewerBridge.requestRemoteConfig(keys, values);
      if (!mounted) {
        return;
      }
      if (!queued) {
        setState(() {
          _saving = false;
          _savingSince = null;
          _error = tr('请求未能排队：请确认主机权限与会话状态');
        });
      }
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      setState(() {
        _saving = false;
        _savingSince = null;
        _error = error.message ?? tr('无法修改主机设置');
      });
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: Text(tr('主机设置')),
      content: SizedBox(
        width: 360,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Text(
              tr('这里只列出主机开放给查看者的开关；改动会在主机保存后生效。'),
              style: const TextStyle(fontSize: 12, color: Colors.white54),
            ),
            const SizedBox(height: 8),
            Flexible(
              child: SingleChildScrollView(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: <Widget>[
                    for (final _RemoteConfigToggle toggle
                        in _remoteConfigToggles)
                      CheckboxListTile(
                        dense: true,
                        visualDensity: VisualDensity.compact,
                        contentPadding: EdgeInsets.zero,
                        controlAffinity: ListTileControlAffinity.leading,
                        title: Text(
                          tr(toggle.label),
                          style: const TextStyle(fontSize: 13),
                        ),
                        value: (_flags & toggle.bit) != 0,
                        onChanged: _saving
                            ? null
                            : (bool? value) {
                                setState(() {
                                  _flags = value == true
                                      ? _flags | toggle.bit
                                      : _flags & ~toggle.bit;
                                  _dirty = true;
                                  _error = null;
                                });
                              },
                      ),
                  ],
                ),
              ),
            ),
            if (_saving)
              Padding(
                padding: const EdgeInsets.only(top: 8),
                child: Row(
                  children: <Widget>[
                    const SizedBox(
                      width: 16,
                      height: 16,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    ),
                    const SizedBox(width: 10),
                    Expanded(child: Text(tr('正在等待主机确认…'))),
                  ],
                ),
              ),
            if (_error != null)
              Padding(
                padding: const EdgeInsets.only(top: 8),
                child: Text(
                  tr('修改未生效：{reason}', <String, Object?>{'reason': _error!}),
                  style: const TextStyle(fontSize: 12, color: Colors.redAccent),
                ),
              ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: _saving ? null : () => Navigator.of(context).pop(false),
          child: Text(tr('取消')),
        ),
        FilledButton(
          onPressed: _saving ? null : () => unawaited(_save()),
          child: Text(tr('保存')),
        ),
      ],
    );
  }
}

/// Picks one of the host's active Windows sessions. The dialog polls the
/// viewer status only while it is open, so the page's own poll can keep
/// updating the session underneath. The newest list answer is the one whose
/// counter differs from [baseline].
class _SessionPickerDialog extends StatefulWidget {
  const _SessionPickerDialog({required this.initial, required this.baseline});

  final ViewerStatus initial;
  final int baseline;

  @override
  State<_SessionPickerDialog> createState() => _SessionPickerDialogState();
}

class _SessionPickerDialogState extends State<_SessionPickerDialog> {
  late ViewerStatus _status = widget.initial;
  late int _pendingSince = widget.baseline;
  Timer? _timer;
  bool _selecting = false;
  bool _refreshing = false;

  @override
  void initState() {
    super.initState();
    _timer = Timer.periodic(
      const Duration(milliseconds: 250),
      (_) => unawaited(_poll()),
    );
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  Future<void> _poll() async {
    try {
      final ViewerStatus next = await ViewerBridge.status();
      if (!mounted) {
        return;
      }
      setState(() => _status = next);
    } on MissingPluginException {
      // The native viewer is going away.
    } on PlatformException {
      // A single failed poll is transient; the next tick retries.
    }
  }

  Future<void> _refresh() async {
    if (_refreshing) {
      return;
    }
    setState(() {
      _refreshing = true;
      _pendingSince = _status.sessionListUpdates;
    });
    try {
      await ViewerBridge.requestSessionList();
    } on PlatformException {
      // The counter does not move, so the spinner stays until a real answer.
    } on MissingPluginException {
      // The native viewer is going away.
    } finally {
      if (mounted) {
        setState(() => _refreshing = false);
      }
    }
  }

  Future<void> _select(ViewerRemoteSession session) async {
    if (_selecting) {
      return;
    }
    setState(() => _selecting = true);
    try {
      final bool queued = await ViewerBridge.requestSessionSelect(session.id);
      if (!mounted) {
        return;
      }
      if (queued) {
        Navigator.of(context).pop();
        return;
      }
      setState(() => _selecting = false);
    } on PlatformException {
      if (mounted) {
        setState(() => _selecting = false);
      }
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  @override
  Widget build(BuildContext context) {
    final bool waiting = _status.sessionListUpdates == _pendingSince;
    final bool answered = !waiting;
    final bool ok =
        answered && _status.sessionListCode == ViewerSessionListCode.ok.code;
    final List<ViewerRemoteSession> sessions = _status.sessions;
    Widget body;
    if (waiting) {
      body = Row(
        children: <Widget>[
          const SizedBox(
            width: 18,
            height: 18,
            child: CircularProgressIndicator(strokeWidth: 2),
          ),
          const SizedBox(width: 12),
          Expanded(child: Text(tr('正在读取主机会话…'))),
        ],
      );
    } else if (!ok) {
      final String detail = _status.sessionListError.isNotEmpty
          ? _status.sessionListError
          : _sessionListCodeText(_status.sessionListCode);
      body = Text(tr('读取会话列表失败：{detail}', <String, Object?>{'detail': detail}));
    } else if (sessions.isEmpty) {
      body = Text(tr('主机上没有可切换的已登录会话。'));
    } else {
      body = Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          Text(
            tr('切换到该会话会结束当前连接，并在目标会话继续。'),
            style: const TextStyle(fontSize: 12, color: Colors.white54),
          ),
          const SizedBox(height: 8),
          Flexible(
            child: ListView(
              shrinkWrap: true,
              children: <Widget>[
                for (final ViewerRemoteSession session in sessions)
                  ListTile(
                    dense: true,
                    leading: const Icon(Icons.desktop_windows_outlined),
                    title: Text(session.label),
                    subtitle: session.current ? Text(tr('当前会话')) : null,
                    enabled: !_selecting && session.active && !session.current,
                    onTap: () => unawaited(_select(session)),
                  ),
              ],
            ),
          ),
        ],
      );
    }
    return AlertDialog(
      title: Text(tr('切换主机的 Windows 会话')),
      content: SizedBox(width: 380, child: body),
      actions: <Widget>[
        TextButton(
          onPressed: _refreshing ? null : () => unawaited(_refresh()),
          child: Text(tr('刷新')),
        ),
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
      ],
    );
  }
}
