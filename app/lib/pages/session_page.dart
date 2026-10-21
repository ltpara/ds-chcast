import 'dart:async';
import 'dart:collection';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_gpu_texture_renderer/flutter_gpu_texture_renderer.dart';

import '../target.dart';
import '../theme.dart';
import '../trust_store.dart';
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
  // choice to the server.
  int _display = kViewerDisplayAuto;
  // The identity this page pins for the current connection. Empty only until
  // the first contact, whose certificate is then remembered by the store.
  String _pinnedFingerprint = '';

  bool get _inputActive => _controlEnabled && _status.acceptsInput && _startError == null;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _startPolling();
    _starting = _start();
    unawaited(_starting);
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _pollTimer?.cancel();
    _hideTimer?.cancel();
    _focus.dispose();
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
        if (mounted) {
          setState(() => _startError = error.message ?? error.code);
        }
        return;
      }
    }
  }

  Future<void> _startOnce() async {
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
    final String pin = await _trustStore.fingerprintFor(widget.target.label);
    if (!mounted) {
      await _stopSession();
      return;
    }
    _pinnedFingerprint = pin;
    try {
      await ViewerBridge.start(
        host: widget.target.host,
        port: widget.target.port,
        gpuOutput: output,
        display: _display,
        fingerprint: pin,
        allowUnverified: pin.isEmpty,
        accessToken: widget.target.token,
      );
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
      unawaited(_trustStore.trust(widget.target.label, status.serverFingerprint));
    }
    final bool nowOver = status.isOver;
    if (status.hasFrame && !_sawFrame) {
      _sawFrame = true;
      _collapsed = false;
      _scheduleHide();
    }
    if (nowOver && !_over) {
      _over = true;
      _hideTimer?.cancel();
      _pollTimer?.cancel();
      _collapsed = false;
    }
    if (nowOver || !status.inputEnabled) {
      _controlEnabled = false;
    }
    setState(() {
      _status = status;
      _fullscreen = status.fullscreen;
    });
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

  /// The output the toolbar should mark. An explicit request is authoritative;
  /// after "server chooses" the list is matched against the stream's origin
  /// and size, which is how HelloAck identifies the captured output.
  int get _activeDisplay {
    if (_display != kViewerDisplayAuto) {
      return _display;
    }
    return _status.streamedDisplayIndex ?? kViewerDisplayAuto;
  }

  Future<void> _switchDisplay(int display) async {
    if (_reconnecting || display == _display) {
      return;
    }
    _display = display;
    await _reconnect();
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
    await _trustStore.trust(widget.target.label, fresh);
    if (!mounted) {
      return;
    }
    _pinnedFingerprint = fresh;
    await _reconnect();
  }

  Future<void> _reconnect() async {
    if (_reconnecting) {
      return;
    }
    setState(() {
      _reconnecting = true;
      _startError = null;
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
      _wheelX = 0;
      _wheelY = 0;
      _heldButtons = 0;
      _heldKeys.clear();
    });
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
  }

  Future<void> _leave() async {
    if (_closing) {
      return;
    }
    _closing = true;
    _pollTimer?.cancel();
    _hideTimer?.cancel();
    if (_fullscreen) {
      unawaited(ViewerBridge.setFullscreen(false));
    }
    await _shutdown();
    if (!mounted) {
      return;
    }
    Navigator.of(context).pop();
  }

  void _cleanupAfterPop() {
    _pollTimer?.cancel();
    _hideTimer?.cancel();
    if (_fullscreen) {
      unawaited(ViewerBridge.setFullscreen(false));
    }
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

  int _desktopX(Offset frame) {
    final int left = _status.originX;
    final int width = _status.width > 0 ? _status.width : 1;
    return (left + frame.dx).round().clamp(left, left + width - 1).toInt();
  }

  int _desktopY(Offset frame) {
    final int top = _status.originY;
    final int height = _status.height > 0 ? _status.height : 1;
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
                      fit: _fit,
                      captureInput: _inputActive,
                      onHover: _inputActive ? _onPointerHover : null,
                      onMove: _inputActive ? _onPointerMove : null,
                      onButton: _inputActive ? _onPointerButton : null,
                      onScroll: _inputActive ? _onPointerScroll : null,
                      onFocusRequested: _focus.requestFocus,
                    ),
                  )
                else
                  const ColoredBox(color: Colors.black),
                if (showOverlay)
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
                        displays: _status.displays,
                        activeDisplay: _activeDisplay,
                        onSelectDisplay: (int index) => unawaited(_switchDisplay(index)),
                        onToggleControl: _toggleControl,
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
                if (hasFrame && !showOverlay && _statsVisible)
                  Positioned(
                    top: 10,
                    right: 10,
                    child: StatsOverlay(status: _status, target: widget.target),
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
              ],
            ),
          ),
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
