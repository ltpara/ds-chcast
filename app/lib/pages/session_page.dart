import 'dart:async';
import 'dart:collection';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_gpu_texture_renderer/flutter_gpu_texture_renderer.dart';

import '../server_settings.dart';
import '../target.dart';
import '../theme.dart';
import '../reconnect_policy.dart';
import '../remote_key_sequence.dart';
import '../trust_store.dart';
import '../tunnel_agent.dart';
import '../viewer_bridge.dart';
import '../widgets/remote_toolbar.dart';
import '../widgets/remote_view.dart';
import '../widgets/stats_overlay.dart';

class SessionPage extends StatefulWidget {
  const SessionPage({super.key, required this.target});

  final ConnectTarget target;

  @override
  State<SessionPage> createState() => _SessionPageState();
}

class _SessionPageState extends State<SessionPage> with WidgetsBindingObserver {
  static const Duration _pollInterval = Duration(milliseconds: 200);
  static const Duration _hideDelay = Duration(seconds: 5);

  final FocusNode _focus = FocusNode(debugLabel: 'session');
  final FlutterGpuTextureRenderer _gpuTexture = FlutterGpuTextureRenderer();
  final TrustStore _trustStore = TrustStore();

  Timer? _pollTimer;
  Timer? _hideTimer;
  ViewerStatus _status = const ViewerStatus();
  // Local clipboard switch, independent of the host's gate: turning it off
  // stops both directions without asking the host to change permissions. The
  // native side owns the actual read/write; this only pauses or resumes it.
  bool _clipboardSync = true;
  // One picker at a time; the native side also refuses an upload while the
  // previous one is active, this just keeps the dialog from stacking.
  bool _filePickBusy = false;
  String? _startError;
  int _textureId = -1;
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
  double _wheelY = 0;
  double _wheelX = 0;
  int _heldButtons = 0;
  final LinkedHashSet<int> _heldKeys = LinkedHashSet<int>();
  ViewFit _fit = ViewFit.adaptive;
  // What the next connection asks the server for. Kept across reconnects so
  // "重新连接" returns to the same output; kViewerDisplayAuto leaves the
  // choice to the server. An accepted in-session switch updates it too.
  int _display = kViewerDisplayAuto;
  // The output an in-session switch is waiting on, or null when none is. The
  // toolbar marks it while the host works; a refusal drops it and leaves
  // [_display] pointing at the output that is still streaming.
  int? _pendingDisplay;
  // Resolution menu. The mode list is fetched once the stream is live for the
  // displayed output and again after an accepted change, because the host's
  // current-mode marker moves with it.
  int? _modesRequestedFor;
  int _modesRequestedLists = 0;
  DateTime _modesRequestedAt = DateTime.fromMillisecondsSinceEpoch(0);
  bool _modesRefreshPending = false;
  // Picture preferences for this session. A fresh host session starts with its
  // own defaults, so the page keeps the choices and re-sends them after every
  // start. A change is optimistic: a refusal restores the previous selection
  // and explains itself in a snack bar.
  ViewerStreamCodec _streamCodec = ViewerStreamCodec.auto;
  ViewerStreamQuality _streamQuality = ViewerStreamQuality.high;
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
  // The identity this page pins for the current connection. Empty only until
  // the first contact, whose certificate is then remembered by the store.
  String _pinnedFingerprint = '';
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
  // One viewer tunnel for the lifetime of this page; reconnects reuse it so
  // the host sees a stable NAT mapping and the selected path survives.
  TunnelAgent? _tunnel;
  Future<TunnelAgent>? _tunnelStarting;
  // Automatic reconnect after an unexpected drop. The policy holds the
  // attempt count and backoff; [_autoTimer] is the pending delay. The series
  // starts only after a session has shown frames, so a refused password or a
  // changed identity never spins here; it survives attempts that fail before
  // their first frame.
  final AutoReconnectPolicy _autoPolicy = AutoReconnectPolicy();
  Timer? _autoTimer;

  bool get _inputActive => _controlEnabled && _status.acceptsInput && _startError == null;

  /// Physical cursor pixels per captured pixel. The native side needs this to
  /// size the host's sprite the way the frame is being drawn, so a stream
  /// shown at half size gets a half-size pointer.
  double get _cursorTargetScale =>
      _viewScale * (MediaQuery.maybeDevicePixelRatioOf(context) ?? 1.0);

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    ViewerBridge.setDropHandler(_sendDroppedFiles);
    _startPolling();
    _starting = _start();
    unawaited(_starting);
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _closing = true;
    ViewerBridge.setDropHandler(null);
    _pollTimer?.cancel();
    _hideTimer?.cancel();
    _autoTimer?.cancel();
    _focus.dispose();
    _setKeyCapture(false);
    _clearCursor();
    unawaited(_shutdown());
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed || !_controlEnabled) {
      return;
    }
    // Switching away means the key-ups for anything still held will never
    // reach the page; lift them here and tell the host to drop whatever it
    // still holds. Control stays on for when the window comes back.
    _releaseRemoteInput();
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
            setState(() => _startError = '原生查看器尚未加载');
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
      } on TunnelException catch (error) {
        // A connection service that cannot start is not a transient drop;
        // retrying it in a loop would just hide the failure.
        _cancelAutoReconnect();
        if (mounted) {
          setState(() => _startError = '无法通过连接服务打开设备：${error.message}');
        }
        return;
      }
    }
  }

  /// Starts the viewer agent on first use. A reconnect keeps the same agent,
  /// so its direct/relay decision and the host's NAT mapping do not reset.
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
    _tunnelStarting ??= TunnelAgent.start(
      server: ServerSettings(
        url: widget.target.server,
        mode: widget.target.mode,
      ),
      hostId: widget.target.hostId,
    );
    try {
      final TunnelAgent agent = await _tunnelStarting!;
      _tunnel = agent;
      _tunnelStarting = null;
      return agent;
    } catch (_) {
      _tunnelStarting = null;
      rethrow;
    }
  }

  Future<void> _startOnce() async {
    final TunnelAgent? tunnel = await _ensureTunnel();
    if (!mounted) {
      await _stopSession();
      return;
    }
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
        setState(() => _startError = '无法创建视频纹理');
      }
      return;
    }
    _textureId = id;
    final String pin = await _trustStore.fingerprintFor(widget.target.trustKey);
    if (!mounted) {
      await _stopSession();
      return;
    }
    _pinnedFingerprint = pin;
    if (!_credentialsLoaded) {
      _credentialsLoaded = true;
      try {
        final String? saved = await ViewerBridge.loadPassword(widget.target.trustKey);
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
      await ViewerBridge.start(
        host: tunnel?.host ?? widget.target.host,
        port: tunnel?.port ?? widget.target.port,
        gpuOutput: output,
        display: _display,
        fingerprint: pin,
        allowUnverified: pin.isEmpty,
        accessToken: widget.target.token,
        password: _password,
        totp: _totp,
        trustDevice: _trustDevice,
      );
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
          _streamMaxFps != 0) {
        ++_streamPending;
        try {
          if (!await ViewerBridge.setStreamOptions(
            codec: _streamCodec.code,
            maxFps: _streamMaxFps,
            quality: _streamQuality.code,
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
    if (!mounted) {
      await _stopSession();
      return;
    }
    setState(() => _startError = null);
  }

  /// Stops the native session, then releases the plugin texture. The native
  /// side clears its output pointer during stop, so unregistering afterwards
  /// cannot leave it with a dangling D3D11Output.
  Future<void> _stopSession() {
    return _teardown ??= _teardownSession();
  }

  Future<void> _teardownSession() async {
    final int id = _textureId;
    _textureId = -1;
    try {
      await ViewerBridge.stop();
    } on MissingPluginException {
      // Nothing to stop.
    } on PlatformException {
      // A stale session is better than a stuck one.
    }
    if (id >= 0) {
      await _gpuTexture.unregisterTexture(id);
    }
  }

  void _startPolling() {
    _pollTimer?.cancel();
    _pollTimer = Timer.periodic(_pollInterval, (_) => unawaited(_poll()));
  }

  Future<void> _poll() async {
    if (_polling || !mounted) {
      return;
    }
    _polling = true;
    try {
      final ViewerStatus status = await ViewerBridge.status();
      if (!mounted) {
        return;
      }
      _applyStatus(status);
    } on MissingPluginException {
      // Reported by _start; keep waiting for the channel to come up.
    } on PlatformException {
      // A single failed poll is transient; the next tick retries.
    } finally {
      _polling = false;
    }
  }

  void _applyStatus(ViewerStatus status) {
    // First contact: remember the identity the server just proved it holds,
    // so every later connection to this endpoint is pinned to it.
    if (_pinnedFingerprint.isEmpty && status.serverFingerprint.isNotEmpty) {
      _pinnedFingerprint = status.serverFingerprint;
      unawaited(_trustStore.trust(widget.target.trustKey, status.serverFingerprint));
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
    final bool challenge = _startError == null &&
        status.isOver &&
        !_over &&
        _isCredentialChallenge(status.denial);
    final bool nowOver = status.isOver && !challenge;
    final bool justOver = nowOver && !_over;
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
    final bool droppedControl = (nowOver || !status.inputEnabled) && _controlEnabled;
    if (droppedControl) {
      _controlEnabled = false;
    }
    // The host answers a switch with one result. Compare the counters with
    // the previous poll rather than the error text: two identical refusals
    // must still read as two answers, and a late answer for a request the
    // picker has already replaced must not win.
    final int? pendingDisplay = _pendingDisplay;
    final bool switchRefused = pendingDisplay != null &&
        status.displaySwitchFailures > _status.displaySwitchFailures;
    final bool switchAccepted = pendingDisplay != null &&
        !switchRefused &&
        status.displaySwitches > _status.displaySwitches;
    // A mode change is answered the same way: the counters say whether this
    // poll carries a new answer, so an old reason cannot be read twice.
    final bool modeChangeAccepted =
        status.displayModeChanges > _status.displayModeChanges;
    final bool modeChangeRefused =
        status.displayModeFailures > _status.displayModeFailures;
    // One upload reaches an end this poll. The counters survive the phase
    // reset, so a success and a failure can never both arrive; the name and
    // the reason stay in the status until the next upload starts.
    final bool fileDone = status.filesSent > _status.filesSent;
    final bool fileFailed = status.fileFailures > _status.fileFailures;
    final bool downloadDone = status.filesReceived > _status.filesReceived;
    final bool downloadFailed =
        status.fileReceiveFailures > _status.fileReceiveFailures;
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
            _streamMaxFps = streamRollback.maxFps;
          } else {
            // A preference re-applied after a reconnect was refused; show the
            // values the host actually runs.
            _streamCodec = status.codec == 'hevc'
                ? ViewerStreamCodec.hevc
                : ViewerStreamCodec.h264;
            _streamQuality = _streamQualityFor(status.streamQuality);
          }
        } else {
          _streamQuality = _streamQualityFor(status.streamQuality);
        }
      }
      _status = status;
      _fullscreen = status.fullscreen;
    });
    if (streamSettled && streamRefused) {
      final String reason = status.streamOptionError;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text(reason.isEmpty ? '画面设置未生效' : '画面设置未生效：$reason'),
        duration: const Duration(seconds: 4),
      ));
    }
    if (justOver) {
      _maybeStartAutoReconnect(status);
    }
    if (switchRefused) {
      final String reason = status.displaySwitchError;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text(reason.isEmpty ? '切换显示失败，已回到当前输出' : '切换显示失败：$reason'),
        duration: const Duration(seconds: 4),
      ));
    }
    if (modeChangeRefused) {
      final String reason = status.displayModeError;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text(reason.isEmpty ? '切换分辨率失败' : '切换分辨率失败：$reason'),
        duration: const Duration(seconds: 4),
      ));
    }
    if (fileDone) {
      final String name = status.lastFileName;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text(name.isEmpty ? '文件已发送' : '文件已发送：$name'),
        duration: const Duration(seconds: 3),
      ));
    } else if (fileFailed) {
      final String reason = status.lastFileError;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text(reason.isEmpty ? '文件发送失败' : '文件发送失败：$reason'),
        duration: const Duration(seconds: 5),
      ));
    }
    if (downloadDone) {
      final String name = status.downloadName;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text(name.isEmpty ? '文件已接收' : '文件已接收：$name'),
        duration: const Duration(seconds: 3),
      ));
    } else if (downloadFailed) {
      final String reason = status.downloadError;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text(reason.isEmpty ? '文件接收失败' : '文件接收失败：$reason'),
        duration: const Duration(seconds: 5),
      ));
    }
    // A microphone failure stops only the uplink; video and system sound keep
    // running. The counter tells a new failure from the stale reason text.
    if (!nowOver && status.microphoneErrors > _status.microphoneErrors) {
      final String reason = status.microphoneError;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text(reason.isEmpty ? '麦克风已停止' : '麦克风已停止：$reason'),
        duration: const Duration(seconds: 5),
      ));
    }
    // Every privacy answer is one event. A refusal or a host-initiated
    // disable explains itself; a success just confirms the new state.
    if (!nowOver && status.privacyUpdates > _status.privacyUpdates) {
      final String reason = status.privacyError;
      final String text;
      if (status.privacyCode != 0) {
        text = reason.isEmpty
            ? _privacyCodeText(status.privacyCode)
            : '${_privacyCodeText(status.privacyCode)}：$reason';
      } else {
        text = status.privacyEnabled ? '隐私模式已开启：主机本地画面已被遮挡' : '隐私模式已关闭：主机本地画面已恢复';
      }
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text(text),
        duration: const Duration(seconds: 4),
      ));
    }
    // Every Ctrl+Alt+Del request gets one answer. A refusal or unsupported
    // host explains itself; success confirms the sequence was injected.
    if (!nowOver && status.sasUpdates > _status.sasUpdates) {
      final String reason = status.sasError;
      final String text;
      if (status.sasCode == ViewerSasCode.ok.code) {
        text = 'Ctrl+Alt+Del 已发送到主机';
      } else {
        final String detail =
            reason.isEmpty ? _sasCodeText(status.sasCode) : reason;
        text = 'Ctrl+Alt+Del 未发送：$detail';
      }
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text(text),
        duration: const Duration(seconds: 4),
      ));
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
    final bool enabled = _inputActive;
    final double scale = _cursorTargetScale;
    if (_cursorSynced && enabled == _cursorWanted && _status.cursorUpdates == _cursorSeen &&
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

  void _onViewScale(double scale) {
    if ((_viewScale - scale).abs() < 0.0005) {
      return;
    }
    _viewScale = scale;
    _syncCursor();
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

  Future<void> _sendFile() => _startUpload(ViewerBridge.sendFile);

  Future<void> _sendDroppedFiles(List<String> paths) =>
      _startUpload(() => ViewerBridge.sendFileAt(paths));

  Future<void> _startUpload(Future<int?> Function() start) async {
    if (_filePickBusy) {
      return;
    }
    _filePickBusy = true;
    try {
      final int? queued = await start();
      if (queued != null && mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(
          content: Text('已加入发送队列（$queued 项），进度显示在画面下方'),
          duration: const Duration(seconds: 2),
        ));
      }
    } on PlatformException catch (error) {
      if (mounted) {
        final String reason = error.message ?? '';
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(
          content: Text(reason.isEmpty ? '无法发送文件' : '无法发送文件：$reason'),
          duration: const Duration(seconds: 4),
        ));
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

  void _setFit(ViewFit fit) => setState(() => _fit = fit);

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
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(error.message ?? '无法切换全屏')),
      );
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
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
        content: Text('主机未授权麦克风，无法开启'),
        duration: Duration(seconds: 4),
      ));
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(error.message ?? '无法切换麦克风')),
      );
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
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
        content: Text('主机未授权隐私模式，无法切换'),
        duration: Duration(seconds: 4),
      ));
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(error.message ?? '无法切换隐私模式')),
      );
    } on MissingPluginException {
      // The native viewer is going away.
    }
  }

  /// The host's PrivacyModeCode (chast::PrivacyModeCode) in the page's words.
  String _privacyCodeText(int code) {
    switch (code) {
      case 1:
        return '主机不允许隐私模式';
      case 2:
        return '主机系统不支持隐私模式';
      case 3:
        return '主机无法开启隐私模式';
      default:
        return '隐私模式未能切换';
    }
  }

  /// Reads the live endpoint list for the toolbar's long-press menu, so a
  /// device plugged in mid-session is offered too.
  Future<List<ViewerMicrophoneDevice>> _loadMicrophones() async {
    try {
      final List<ViewerMicrophoneDevice> devices = await ViewerBridge.listMicrophones();
      if (devices.isEmpty && mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
          content: Text('没有可用的麦克风设备'),
          duration: Duration(seconds: 4),
        ));
      }
      return devices;
    } on PlatformException catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(error.message ?? '无法列出麦克风设备')),
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
      final List<ViewerAudioOutput> devices = await ViewerBridge.listAudioOutputs();
      if (devices.isEmpty && mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
          content: Text('没有可用的音频输出设备'),
          duration: Duration(seconds: 4),
        ));
      }
      return devices;
    } on PlatformException catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(error.message ?? '无法列出音频输出设备')),
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
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
        content: Text('当前会话无法切换输出设备'),
        duration: Duration(seconds: 4),
      ));
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(error.message ?? '无法切换音频输出')),
      );
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
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
        content: Text('当前会话无法修改麦克风处理'),
        duration: Duration(seconds: 4),
      ));
    } on PlatformException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(error.message ?? '无法修改麦克风处理')),
      );
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

  Future<void> _switchDisplay(int display) async {
    if (_reconnecting) {
      return;
    }
    // Tapping the output that is already shown is not a request.
    if (display == _activeDisplay) {
      return;
    }
    // A live session moves its capture without a reconnect, but only while
    // the host grants input; otherwise the Hello-time selection is the only
    // way to change outputs.
    if (_status.isLive && _status.acceptsInput && !_over) {
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
    if (_modesRefreshPending && status.displayModeLists > _modesRequestedLists) {
      _modesRefreshPending = false;
    }
    if (status.displayModesDisplay == streamed && !_modesRefreshPending) {
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
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
        content: Text('无法切换分辨率：会话未在串流，或主机未开放输入'),
        duration: Duration(seconds: 3),
      ));
    }
  }

  /// Applies one menu choice. The selection moves immediately; the host's
  /// answer either confirms it or restores the previous one. No input
  /// permission is needed, so this works in a view-only session.
  Future<void> _changeStreamOptions({
    ViewerStreamCodec? codec,
    ViewerStreamQuality? quality,
    int? maxFps,
  }) async {
    final ViewerStreamCodec nextCodec = codec ?? _streamCodec;
    final ViewerStreamQuality nextQuality = quality ?? _streamQuality;
    final int nextFps = maxFps ?? _streamMaxFps;
    if (nextCodec == _streamCodec &&
        nextQuality == _streamQuality &&
        nextFps == _streamMaxFps) {
      return;
    }
    final _StreamOptionSnapshot previous =
        _StreamOptionSnapshot(_streamCodec, _streamQuality, _streamMaxFps);
    setState(() {
      _streamCodec = nextCodec;
      _streamQuality = nextQuality;
      _streamMaxFps = nextFps;
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
      _streamMaxFps = previous.maxFps;
      _streamRollback = null;
    });
    ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
      content: Text('当前无法修改画面设置'),
      duration: Duration(seconds: 3),
    ));
  }

  /// True when the server proved a different identity than the one stored for
  /// this endpoint. The native side refuses that connection; the card then
  /// offers to replace the pin explicitly.
  bool get _identityChanged =>
      _pinnedFingerprint.isNotEmpty &&
      _status.serverFingerprint.isNotEmpty &&
      _status.serverFingerprint != _pinnedFingerprint;

  Future<void> _trustNewIdentity() async {
    final String fresh = _status.serverFingerprint;
    if (fresh.isEmpty || _reconnecting) {
      return;
    }
    await _trustStore.trust(widget.target.trustKey, fresh);
    if (!mounted) {
      return;
    }
    _pinnedFingerprint = fresh;
    await _reconnect();
  }

  /// Explains a live grant change. Input loss has already dropped control and
  /// the host lifts the keys it still holds, so this only tells the user why.
  void _notifyPermissionChange(int before, ViewerStatus status) {
    final int lost = before & ~status.permissions;
    final int gained = status.permissions & ~before;
    String names(int mask) {
      final List<String> parts = <String>[];
      if ((mask & ViewerPermission.input) != 0) {
        parts.add('输入控制');
      }
      if ((mask & ViewerPermission.clipboard) != 0) {
        parts.add('剪贴板');
      }
      if ((mask & ViewerPermission.audio) != 0) {
        parts.add('音频');
      }
      if ((mask & ViewerPermission.microphone) != 0) {
        parts.add('麦克风');
      }
      if ((mask & ViewerPermission.privacyMode) != 0) {
        parts.add('隐私模式');
      }
      return parts.join('、');
    }

    final String lostNames = names(lost);
    final String gainedNames = names(gained);
    final String text;
    if (lostNames.isNotEmpty && gainedNames.isNotEmpty) {
      text = '主机已收回 $lostNames，并授予 $gainedNames。';
    } else if (lostNames.isNotEmpty) {
      text = '主机已收回 $lostNames。';
    } else if (gainedNames.isNotEmpty) {
      text = '主机已授予 $gainedNames。';
    } else {
      return;
    }
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      content: Text(text),
      duration: const Duration(seconds: 4),
    ));
  }

  /// Collects the credential the host asked for and reconnects with it. A
  /// password challenge also offers to keep the password in this Windows
  /// account's DPAPI store; a two-factor challenge offers to trust the device.
  Future<void> _handleCredentialChallenge(int denial) async {
    if (_authPromptOpen || !mounted) {
      return;
    }
    _authPromptOpen = true;
    final bool passwordStep = denial == ViewerDenial.passwordRequired ||
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
        _startError = detail.isEmpty ? '身份验证未完成' : detail;
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
    final bool eligible = _startError == null &&
        status.state == ViewerState.failed &&
        status.denial == 0 &&
        !_identityChanged;
    if (!eligible) {
      _cancelAutoReconnect();
      return;
    }
    final bool seriesActive =
        _autoPolicy.waiting && _autoPolicy.attempt > 0;
    if (!seriesActive && !_sawFrame) {
      return;
    }
    final Duration? delay = _autoPolicy.next();
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
    if (_reconnecting) {
      return;
    }
    if (!automatic) {
      _cancelAutoReconnect();
    }
    setState(() {
      _reconnecting = true;
      _startError = null;
      _challengeDenial = 0;
    });
    _pollTimer?.cancel();
    _hideTimer?.cancel();
    if (_controlEnabled) {
      // The session is about to be torn down; lift what this page still holds
      // so the short window before Stop() does not leave a key down.
      _releaseRemoteInput();
    }
    await _starting;
    _teardown = null;
    await _stopSession();
    _clearCursor();
    if (!mounted) {
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
      _wheelX = 0;
      _wheelY = 0;
      _heldButtons = 0;
      _heldKeys.clear();
    });
    _setKeyCapture(false);
    _startPolling();
    _starting = _start();
    await _starting;
  }

  Future<void> _shutdown() {
    return _shutdownFuture ??= _shutdownOnce();
  }

  Future<void> _shutdownOnce() async {
    _closing = true;
    await _starting;
    _teardown = null;
    await _stopSession();
    await _tunnel?.stop();
    _tunnel = null;
    _tunnelStarting = null;
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
    Navigator.of(context).pop();
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
    if (_inputActive) {
      if (event is KeyRepeatEvent) {
        // The host generates its own auto-repeat; forwarding these would
        // double the rate.
        return KeyEventResult.handled;
      }
      if (event is KeyDownEvent || event is KeyUpEvent) {
        // Flutter's usbHidUsage carries the usage page in the upper half
        // (0x00070004 for A); the protocol wants the page-0x07 id alone.
        final int raw = event.physicalKey.usbHidUsage;
        final int usage = raw & 0xFFFF;
        if ((raw >> 16) == 0x07 && usage > 0) {
          if (event is KeyDownEvent) {
            _heldKeys.add(usage);
          } else {
            _heldKeys.remove(usage);
          }
          unawaited(_sendKey(usage, event is KeyDownEvent));
          return KeyEventResult.handled;
        }
      }
      return KeyEventResult.ignored;
    }
    if (event is! KeyDownEvent) {
      return KeyEventResult.ignored;
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
    }
    setState(() {
      _controlEnabled = !_controlEnabled;
      _wheelX = 0;
      _wheelY = 0;
    });
    _setKeyCapture(_inputActive);
    _syncCursor();
    if (_controlEnabled) {
      // The toolbar button may claim focus while it handles the tap; take it
      // back once the frame settles so keys go to the host immediately.
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted && _inputActive) {
          _focus.requestFocus();
        }
      });
    }
  }

  void _releaseRemoteInput() {
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
        _sendMouse(ViewerMouse.buttonUp, buttons: bit);
      }
    }
    _heldButtons = 0;
    // Even if an Up above was lost, or the host saw a press this page never
    // recorded, this lifts whatever it still holds.
    unawaited(_guardSend(ViewerBridge.releaseInput()));
  }

  void _setKeyCapture(bool enable) {
    unawaited(_guardSend(ViewerBridge.setKeyCapture(enable)));
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
    unawaited(_guardSend(ViewerBridge.sendMouse(
      action: action,
      buttons: buttons,
      x: x,
      y: y,
      modifiers: _inputState(),
    )));
  }

  Future<void> _sendKey(int usage, bool down) {
    return _guardSend(ViewerBridge.sendKey(
      usage: usage,
      down: down,
      modifiers: _inputState(),
    ));
  }

  /// Like [_sendKey], but with an explicit bitmap so a synthetic chord can
  /// describe the modifiers it is holding instead of what this machine's
  /// keyboard happens to have down.
  Future<void> _sendKeyWithBits(int usage, bool down, int modifiers) {
    return _guardSend(ViewerBridge.sendKey(
      usage: usage,
      down: down,
      modifiers: modifiers,
    ));
  }

  /// Sends one toolbar shortcut as a press chord: modifiers down, the key
  /// down and up, then the modifiers up in reverse order. Every event carries
  /// the bitmap at that instant, matching a physical chord on the host.
  Future<void> _sendChord(List<int> modifierUsages, int keyUsage) async {
    for (final RemoteKeyEvent event
        in buildRemoteChord(modifierUsages, keyUsage, _inputState())) {
      await _sendKeyWithBits(event.usage, event.down, event.modifiers);
    }
  }

  /// Win is itself a modifier, so a tap clears its bit before the key-up event
  /// rather than sending a second, duplicate down.
  Future<void> _sendWinTap() async {
    for (final RemoteKeyEvent event
        in buildRemoteWinTap(_inputState(), ViewerModifier.leftGui)) {
      await _sendKeyWithBits(event.usage, event.down, event.modifiers);
    }
  }

  Future<void> _sendSpecialAction(RemoteSpecialAction action) async {
    if (!_inputActive) {
      return;
    }
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
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
          content: Text('Ctrl+Alt+Del 未能排队：远程键鼠尚未就绪'),
          duration: Duration(seconds: 3),
        ));
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
      return '主机未授予输入权限';
    }
    if (code == ViewerSasCode.unsupported.code) {
      return '主机不支持发送 Ctrl+Alt+Del（托盘主机需改用服务运行）';
    }
    if (code == ViewerSasCode.failed.code) {
      return '主机调用 SendSAS 失败';
    }
    return '主机未发送 Ctrl+Alt+Del';
  }

  // Input travels in captured-desktop pixels; the encoded picture may be
  // smaller, so the clamp follows the source size.
  int get _sourceWidth =>
      _status.sourceWidth > 0 ? _status.sourceWidth : _status.width;

  int get _sourceHeight =>
      _status.sourceHeight > 0 ? _status.sourceHeight : _status.height;

  int _desktopX(Offset frame) {
    final int left = _status.originX;
    final int width = _sourceWidth > 0 ? _sourceWidth : 1;
    return (left + frame.dx).round().clamp(left, left + width - 1).toInt();
  }

  int _desktopY(Offset frame) {
    final int top = _status.originY;
    final int height = _sourceHeight > 0 ? _sourceHeight : 1;
    return (top + frame.dy).round().clamp(top, top + height - 1).toInt();
  }

  void _onPointerHover(Offset frame, int buttons) {
    _sendMouse(ViewerMouse.move, buttons: buttons, x: _desktopX(frame), y: _desktopY(frame));
  }

  void _onPointerMove(Offset frame, int buttons) {
    _sendMouse(ViewerMouse.move, buttons: buttons, x: _desktopX(frame), y: _desktopY(frame));
  }

  void _onPointerButton(Offset frame, int button, bool down) {
    if (down) {
      _heldButtons |= button;
    } else {
      _heldButtons &= ~button;
    }
    _sendMouse(
      down ? ViewerMouse.buttonDown : ViewerMouse.buttonUp,
      buttons: button,
    );
  }

  void _onPointerScroll(Offset frame, Offset delta) {
    _wheelY += delta.dy;
    _wheelX += delta.dx;
    final int stepsY =
        (_wheelY / kViewerScrollPixelsPerNotch).truncate().clamp(-10, 10).toInt();
    if (stepsY != 0) {
      _wheelY -= stepsY * kViewerScrollPixelsPerNotch;
      // Flutter's positive dy scrolls down; Windows WHEEL_DELTA is positive
      // when the wheel turns up (away from the user).
      _sendMouse(ViewerMouse.wheel, y: -stepsY * kViewerWheelDelta);
    }
    final int stepsX =
        (_wheelX / kViewerScrollPixelsPerNotch).truncate().clamp(-10, 10).toInt();
    if (stepsX != 0) {
      _wheelX -= stepsX * kViewerScrollPixelsPerNotch;
      _sendMouse(ViewerMouse.wheelHorizontal, x: stepsX * kViewerWheelDelta);
    }
  }

  /// Path selected by the embedded agent, empty for a direct connection that
  /// does not use the connection service.
  String get _tunnelPath {
    if (!widget.target.viaServer) {
      return '';
    }
    return _tunnel?.pathLabel ?? '连接中';
  }

  String get _stateText {
    switch (_status.state) {
      case ViewerState.idle:
      case ViewerState.connecting:
        return '正在建立 QUIC 连接…';
      case ViewerState.handshaking:
        return '正在协商视频流…';
      case ViewerState.streaming:
        return '等待远端画面…';
      case ViewerState.ended:
        return '会话已结束';
      case ViewerState.failed:
        return '连接失败';
    }
  }

  /// True when the connection was refused by the host's access control. The
  /// token lives on the home page, so the card sends the user back to edit it
  /// instead of offering another attempt with the same one.
  bool get _denied => _startError == null && _status.denial != 0;

  String get _overTitle {
    if (_startError != null) {
      return '无法发起连接';
    }
    if (_denied) {
      return '服务端拒绝了连接';
    }
    if (_status.state == ViewerState.failed) {
      return '连接中断';
    }
    return '会话已结束';
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
      final String reason =
          _status.error.startsWith(prefix) ? _status.error.substring(prefix.length) : '';
      return reason.isEmpty ? denial : '$denial\n\n服务端说明：$reason';
    }
    if (_status.error.isNotEmpty) {
      return _status.error;
    }
    return '远端已结束视频流。';
  }

  @override
  Widget build(BuildContext context) {
    final bool hasFrame = _status.hasFrame;
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
                if (hasFrame && _textureId >= 0)
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
                      hideCursor: _inputActive && _cursorInstalled,
                      onScaleChanged: _onViewScale,
                      onHover: _inputActive ? _onPointerHover : null,
                      onMove: _inputActive ? _onPointerMove : null,
                      onButton: _inputActive ? _onPointerButton : null,
                      onScroll: _inputActive ? _onPointerScroll : null,
                      onFocusRequested: _focus.requestFocus,
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
                        clipboardEnabled: _clipboardSync,
                        clipboardAvailable: _status.acceptsClipboard,
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
                        privacyAvailable: _status.acceptsPrivacy,
                        privacyEnabled: _status.privacyEnabled,
                        displays: _status.displays,
                        activeDisplay: _activeDisplay,
                        onSelectDisplay: (int index) => unawaited(_switchDisplay(index)),
                        modes: _status.displayModesDisplay ==
                                    _status.streamedDisplayIndex &&
                                _status.displayModesCode == 0
                            ? _status.displayModes
                            : const <ViewerDisplayMode>[],
                        modeChangeAvailable: _status.acceptsInput &&
                            !_status.mergedLayout,
                        onSelectMode: (ViewerDisplayMode mode) =>
                            unawaited(_changeDisplayMode(mode)),
                        streamOptionsAvailable: _status.isLive,
                        streamCodec: _streamCodec,
                        streamQuality: _streamQuality,
                        streamMaxFps: _streamMaxFps,
                        onSelectStreamCodec: (ViewerStreamCodec codec) =>
                            unawaited(_changeStreamOptions(codec: codec)),
                        onSelectStreamQuality: (ViewerStreamQuality quality) =>
                            unawaited(_changeStreamOptions(quality: quality)),
                        onSelectStreamFps: (int fps) =>
                            unawaited(_changeStreamOptions(maxFps: fps)),
                        onToggleControl: _toggleControl,
                        specialKeysAvailable: _inputActive,
                        onSpecialAction: (RemoteSpecialAction action) =>
                            unawaited(_sendSpecialAction(action)),
                        onToggleClipboard: _toggleClipboardSync,
                        onSendFile: () => unawaited(_sendFile()),
                        onToggleAudio: () => unawaited(_toggleAudioMute()),
                        onToggleMicrophone: () => unawaited(_toggleMicrophone()),
                        onTogglePrivacy: () => unawaited(_togglePrivacy()),
                        onTogglePin: _togglePin,
                        onToggleStats: () => setState(() => _statsVisible = !_statsVisible),
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
                              horizontal: 12, vertical: 7),
                          decoration: BoxDecoration(
                            color: Colors.black.withValues(alpha: 0.68),
                            borderRadius: BorderRadius.circular(6),
                            border: Border.all(color: Colors.amber.withValues(alpha: 0.45)),
                          ),
                          child: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: <Widget>[
                              const Icon(Icons.lock_rounded,
                                  size: 14, color: Colors.amberAccent),
                              const SizedBox(width: 7),
                              Text(
                                _status.desktop == ViewerDesktopKind.secure
                                    ? '主机已进入安全桌面（锁屏或 UAC），画面暂停'
                                    : '主机暂时无法采集桌面（锁屏或 UAC），恢复后自动继续',
                                style: const TextStyle(
                                    color: Colors.white70, fontSize: 11),
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
                if (hasFrame && !showOverlay && _inputActive)
                  Positioned(
                    left: 10,
                    bottom: 10,
                    child: IgnorePointer(
                      child: Container(
                        padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 5),
                        decoration: BoxDecoration(
                          color: Colors.black.withValues(alpha: 0.55),
                          borderRadius: BorderRadius.circular(6),
                          border: Border.all(color: Colors.white12),
                        ),
                        child: const Text(
                          '远程键鼠已开启 · Esc 与 F11 已转发，工具栏可退出',
                          style: TextStyle(color: Colors.white54, fontSize: 10.5),
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
                    (_status.privacyEnabled ||
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
                            _statusBadge(Icons.shield_rounded, '隐私模式'),
                          if (_status.privacyEnabled &&
                              _status.acceptsAudio &&
                              _status.audioMuted)
                            const SizedBox(height: 6),
                          if (_status.acceptsAudio && _status.audioMuted)
                            _statusBadge(Icons.volume_off_rounded, '已静音'),
                        ],
                      ),
                    ),
                  ),
              ],
            ),
          ),
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
      ViewerFilePhase.hashing => '正在校验',
      ViewerFilePhase.offered => '等待主机接收',
      ViewerFilePhase.sending => '正在发送',
      _ => '准备发送',
    };
    final String detail = status.fileSize > 0
        ? '${_formatBytes(status.fileSent)} / ${_formatBytes(status.fileSize)}'
        : '';
    final String path = status.fileRelativePath.isEmpty
        ? status.fileName
        : '${status.fileRelativePath}/${status.fileName}';
    // fileQueue counts the entry on screen too; the note is for the rest.
    final String waiting = status.fileQueue > 1
        ? '队列剩余 ${status.fileQueue - 1} 个'
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
                const Icon(Icons.upload_file_rounded, size: 13, color: Colors.white70),
                const SizedBox(width: 6),
                Expanded(
                  child: Text(
                    path,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(color: Colors.white70, fontSize: 10.5),
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
                      style: const TextStyle(color: Colors.white38, fontSize: 9.5),
                    ),
                  if (waiting.isNotEmpty) ...<Widget>[
                    const Spacer(),
                    Text(
                      waiting,
                      style: const TextStyle(color: Colors.white38, fontSize: 9.5),
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
                const Icon(Icons.download_rounded, size: 13, color: Colors.white70),
                const SizedBox(width: 6),
                Expanded(
                  child: Text(
                    status.downloadName,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(color: Colors.white70, fontSize: 10.5),
                  ),
                ),
                const SizedBox(width: 8),
                const Text(
                  '正在接收',
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
                '隧道 · $_tunnelPath',
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
              label: const Text('取消连接'),
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
              BoxShadow(color: Color(0x80000000), blurRadius: 30, offset: Offset(0, 12)),
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
              const Text(
                '连接中断',
                style: TextStyle(
                  color: Colors.white,
                  fontSize: 17,
                  fontWeight: FontWeight.w600,
                ),
              ),
              const SizedBox(height: 8),
              Text(
                '正在自动重连（第 ${_autoPolicy.attempt}/${AutoReconnectPolicy.limit} 次）',
                style: const TextStyle(color: Colors.white54, fontSize: 12.5),
              ),
              if (detail.isNotEmpty) ...<Widget>[
                const SizedBox(height: 8),
                Text(
                  detail,
                  textAlign: TextAlign.center,
                  style: const TextStyle(
                      color: Colors.white38, fontSize: 11.5, height: 1.4),
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
                label: const Text('取消连接'),
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildDisconnected(bool hasFrame) {
    final int anomalies =
        _status.crcErrors + _status.decodeErrors + _status.protocolErrors + _status.gaps;
    final bool failed = _startError != null || _status.state == ViewerState.failed;
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
              BoxShadow(color: Color(0x80000000), blurRadius: 30, offset: Offset(0, 12)),
            ],
          ),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              Container(
                width: 46,
                height: 46,
                decoration: BoxDecoration(
                  color: (failed ? Colors.redAccent : kAccent).withValues(alpha: 0.14),
                  shape: BoxShape.circle,
                ),
                child: Icon(
                  _denied
                      ? Icons.lock_outline_rounded
                      : (failed ? Icons.link_off_rounded : Icons.videocam_off_rounded),
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
                style: const TextStyle(color: Colors.white54, fontSize: 12.5, height: 1.5),
              ),
              if (_identityChanged) ...<Widget>[
                const SizedBox(height: 14),
                Container(
                  padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
                  decoration: BoxDecoration(
                    color: const Color(0xFFFFB74D).withValues(alpha: 0.08),
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(color: const Color(0xFFFFB74D).withValues(alpha: 0.32)),
                  ),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      const Row(
                        children: <Widget>[
                          Icon(Icons.shield_outlined, size: 14, color: Color(0xFFFFB74D)),
                          SizedBox(width: 6),
                          Text(
                            '服务器证书指纹已变化',
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
                        '已固定 ${shortenFingerprint(_pinnedFingerprint)} · '
                        '当前 ${shortenFingerprint(_status.serverFingerprint)}',
                        style: const TextStyle(color: Colors.white54, fontSize: 11, height: 1.4),
                      ),
                      const SizedBox(height: 4),
                      const Text(
                        '对端可能重装或换了证书。确认这是同一台机器后再信任；'
                        '否则请不要继续，避免连到冒充的主机。',
                        style: TextStyle(color: Colors.white38, fontSize: 11, height: 1.4),
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
                  '接收 ${_status.received} 帧 · 解码 ${_status.decoded} 帧'
                  '${anomalies > 0 ? ' · 异常 $anomalies' : ''}',
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
                      textStyle: const TextStyle(fontSize: 13, fontWeight: FontWeight.w600),
                    ),
                    child: const Text('返回并修改令牌'),
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
                          child: const Text('返回'),
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
                            disabledBackgroundColor: kAccent.withValues(alpha: 0.5),
                            elevation: 0,
                            shape: RoundedRectangleBorder(
                              borderRadius: BorderRadius.circular(8),
                            ),
                            textStyle: const TextStyle(fontSize: 13, fontWeight: FontWeight.w600),
                          ),
                          child: _reconnecting
                              ? const SizedBox(
                                  width: 15,
                                  height: 15,
                                  child: CircularProgressIndicator(
                                    strokeWidth: 2,
                                    valueColor: AlwaysStoppedAnimation<Color>(Colors.white),
                                  ),
                                )
                              : Text(_identityChanged ? '信任并重新连接' : '重新连接'),
                        ),
                      ),
                    ),
                  ],
                ),
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
  late final TextEditingController _text =
      TextEditingController(text: widget.passwordStep ? widget.initialPassword : '');
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
      setState(() => _error = widget.passwordStep ? '请输入密码' : '请输入两步验证码');
      return;
    }
    Navigator.of(context).pop(_CredentialResult(value, _remember));
  }

  @override
  Widget build(BuildContext context) {
    final bool passwordStep = widget.passwordStep;
    return AlertDialog(
      title: Text(passwordStep ? '输入连接密码' : '输入两步验证码'),
      content: SizedBox(
        width: 320,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            if (widget.detail.isNotEmpty) ...<Widget>[
              Text(
                widget.detail,
                style: const TextStyle(color: Colors.white54, fontSize: 12, height: 1.4),
              ),
              const SizedBox(height: 14),
            ],
            TextField(
              controller: _text,
              autofocus: true,
              obscureText: passwordStep,
              keyboardType:
                  passwordStep ? TextInputType.visiblePassword : TextInputType.number,
              maxLength: passwordStep ? 128 : 16,
              onSubmitted: (_) => _submit(),
              decoration: InputDecoration(
                labelText: passwordStep ? '密码' : '6 位验证码',
                errorText: _error,
                counterText: '',
              ),
            ),
            const SizedBox(height: 8),
            CheckboxListTile(
              value: _remember,
              onChanged: (bool? value) => setState(() => _remember = value ?? false),
              contentPadding: EdgeInsets.zero,
              controlAffinity: ListTileControlAffinity.leading,
              dense: true,
              title: Text(
                passwordStep ? '在本机加密保存密码' : '信任此设备，下次跳过两步验证',
                style: const TextStyle(fontSize: 12.5),
              ),
            ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(onPressed: () => Navigator.of(context).pop(), child: const Text('取消')),
        FilledButton(onPressed: _submit, child: const Text('连接')),
      ],
    );
  }
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
  const _StreamOptionSnapshot(this.codec, this.quality, this.maxFps);

  final ViewerStreamCodec codec;
  final ViewerStreamQuality quality;
  final int maxFps;
}

ViewerStreamQuality _streamQualityFor(int code) {
  for (final ViewerStreamQuality quality in ViewerStreamQuality.values) {
    if (quality.code == code) {
      return quality;
    }
  }
  return ViewerStreamQuality.high;
}
