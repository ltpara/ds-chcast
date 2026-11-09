import 'dart:async';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../display_stream_controller.dart';
import '../l10n.dart';
import '../remote_pointer.dart';
import '../session_window.dart';
import '../viewer_bridge.dart';
import '../view_prefs.dart';
import '../widgets/keyboard_mode_button.dart';
import '../widgets/display_stream_view.dart';
import '../widgets/remote_view.dart';

/// The runner supplies the existing session. This engine owns only its texture
/// and local input state; authentication and reconnect stay in the parent.
class DisplayWindowPage extends StatefulWidget {
  const DisplayWindowPage({
    super.key,
    required this.streamId,
    this.peerId = '',
  });
  final int streamId;

  /// Owner's per-device preference key, passed by the window that opened this
  /// one so input handling matches the main session.
  final String peerId;

  @override
  State<DisplayWindowPage> createState() => _DisplayWindowPageState();
}

class _DisplayWindowPageState extends State<DisplayWindowPage>
    with WidgetsBindingObserver {
  final _displays = DisplayStreamController();
  final _focus = FocusNode();
  final _contacts = RemoteContactTracker();
  ViewerStatus _status = const ViewerStatus();
  Timer? _timer;
  bool _polling = false, _closing = false;
  bool _control = false, _nativeKeys = false, _capture = false;
  int _captureGeneration = 0;
  bool _localKeyboardUnavailable = false;
  bool _fullscreen = false;
  ViewerKeyboardMode _keyboardMode = ViewerKeyboardMode.physical;
  bool _keyboardChanging = false;
  ViewFit _fit = ViewFit.adaptive;
  MouseMapping _mouseMapping = const MouseMapping();
  final Map<int, int> _remoteButtons = <int, int>{};
  String _error = '';
  double _wheelX = 0, _wheelY = 0;
  int _buttons = 0;
  int? _pen;
  DisplayTexture? get _texture => _displays.textures
      .where((t) => t.stream.streamId == widget.streamId)
      .firstOrNull;
  bool get _input =>
      !_closing &&
      !_keyboardChanging &&
      _control &&
      _status.acceptsInput &&
      _texture?.stream.ready == true;

  @override
  void initState() {
    super.initState();
    final prefs = ViewPreferences.load(peerId: widget.peerId);
    _fit = prefs.fit == 1 ? ViewFit.original : ViewFit.adaptive;
    _mouseMapping = MouseMapping(
      reverseWheel: prefs.reverseWheel,
      swapButtons: prefs.swapButtons,
    );
    _keyboardMode = viewerKeyboardModeFromName(prefs.keyboardMode);
    ViewerBridge.setKeyboardTextRejectedHandler(() {
      if (mounted && !_closing) {
        setState(() => _error = tr('文字未发送，请检查控制权限或连接状态'));
      }
    });
    WidgetsBinding.instance.addObserver(this);
    _displays.addListener(_changed);
    _focus.addListener(_onFocusChanged);
    _timer = Timer.periodic(const Duration(milliseconds: 200), (_) => _poll());
    unawaited(_poll());
  }

  void _changed() {
    if (!mounted || _closing) return;
    setState(() {});
    _syncCapture();
  }

  Future<void> _poll() async {
    if (_polling || _closing) return;
    _polling = true;
    try {
      final status = await ViewerBridge.status();
      if (!mounted || _closing) return;
      _status = status;
      await _displays.refresh();
      if (!mounted || _closing) return;
      if (!status.isLive ||
          !_displays.state.active ||
          !_displays.state.streams.any((s) => s.streamId == widget.streamId)) {
        await _close();
        return;
      }
      _error = _displays.error;
      _changed();
    } on PlatformException catch (e) {
      if (e.code == 'session_ended') {
        await _close();
      } else if (mounted && !_closing) {
        setState(() => _error = e.message ?? e.code);
      }
    } finally {
      _polling = false;
    }
  }

  Future<void> _quiet(Future<Object?> action) async {
    try {
      await action;
    } on PlatformException {
      /* Session is closing. */
    } on MissingPluginException {
      /* Engine has already detached. */
    }
  }

  void _release() {
    _contacts.clear();
    _pen = null;
    _buttons = 0;
    _remoteButtons.clear();
    _wheelX = _wheelY = 0;
    unawaited(_quiet(ViewerBridge.releaseInput()));
  }

  void _syncCapture() {
    final lifecycle = WidgetsBinding.instance.lifecycleState;
    final enabled =
        _input &&
        _focus.hasPrimaryFocus &&
        (lifecycle == null || lifecycle == AppLifecycleState.resumed);
    if (_capture == enabled) return;
    _capture = enabled;
    if (!enabled) _release();
    if (!enabled) _nativeKeys = false;
    unawaited(_setCapture(enabled, ++_captureGeneration));
  }

  void _onFocusChanged() {
    if (_focus.hasPrimaryFocus) _reloadPreferences();
    _syncCapture();
  }

  void _reloadPreferences() {
    if (!mounted || _closing) return;
    final prefs = ViewPreferences.load(peerId: widget.peerId);
    setState(() {
      _fit = prefs.fit == 1 ? ViewFit.original : ViewFit.adaptive;
      _mouseMapping = MouseMapping(
        reverseWheel: prefs.reverseWheel,
        swapButtons: prefs.swapButtons,
      );
    });
    final mode = viewerKeyboardModeFromName(prefs.keyboardMode);
    if (mode != _keyboardMode) {
      unawaited(_changeKeyboardMode(mode, persist: false));
    }
  }

  Future<void> _setCapture(bool enabled, int generation) async {
    var captured = false;
    try {
      if (enabled) await ViewerBridge.setKeyboardMode(_keyboardMode);
      if (!mounted || generation != _captureGeneration) return;
      captured = await ViewerBridge.setKeyCapture(enabled);
    } on PlatformException {
      // The local mode must not silently route text as physical keys.
    } on MissingPluginException {
      // Other embedders may lack native keyboard capture.
    }
    if (!mounted || generation != _captureGeneration) return;
    _nativeKeys = enabled && captured;
    if (enabled && _keyboardMode == ViewerKeyboardMode.local && !captured) {
      if (!_localKeyboardUnavailable) {
        _localKeyboardUnavailable = true;
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text(tr('本地输入法未启用，请重新聚焦画面或切换物理按键'))));
      }
    } else if (captured || _keyboardMode != ViewerKeyboardMode.local) {
      _localKeyboardUnavailable = false;
    }
  }

  Future<void> _changeKeyboardMode(
    ViewerKeyboardMode mode, {
    bool persist = true,
  }) async {
    if (_keyboardChanging || mode == _keyboardMode) return;
    _keyboardChanging = true;
    _release();
    _syncCapture();
    try {
      await ViewerBridge.setKeyboardMode(mode);
      if (mounted && !_closing) {
        setState(() => _keyboardMode = mode);
        if (persist) {
          ViewPreferences.update(
            peerId: widget.peerId,
            keyboardMode: mode.name,
          );
        }
      }
    } on PlatformException catch (error) {
      if (mounted && !_closing) {
        setState(() => _error = error.message ?? tr('无法切换键盘模式'));
      }
    } finally {
      _keyboardChanging = false;
      if (mounted && !_closing) {
        _focus.requestFocus();
        _syncCapture();
      }
    }
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state != AppLifecycleState.resumed) _release();
    if (state == AppLifecycleState.resumed) _reloadPreferences();
    _syncCapture();
  }

  Future<void> _close() async {
    if (_closing) return;
    setState(() => _closing = true);
    _timer?.cancel();
    _syncCapture();
    _release();
    // Native Shutdown also detaches synchronously if the window X destroys
    // this engine before Dart cleanup can finish.
    try {
      await _quiet(_displays.close());
    } finally {
      await _quiet(SessionWindowRequest.close());
    }
  }

  @override
  void dispose() {
    _closing = true;
    _timer?.cancel();
    ViewerBridge.setKeyboardTextRejectedHandler(null);
    WidgetsBinding.instance.removeObserver(this);
    _displays.removeListener(_changed);
    _focus.removeListener(_onFocusChanged);
    _focus.dispose();
    unawaited(_quiet(ViewerBridge.setKeyCapture(false)));
    _release();
    unawaited(_quiet(_displays.close().whenComplete(_displays.dispose)));
    super.dispose();
  }

  int get _modifiers =>
      modifierBitsFor(HardwareKeyboard.instance.physicalKeysPressed) |
      lockBitsFor(HardwareKeyboard.instance.lockModesEnabled);

  KeyEventResult _key(FocusNode node, KeyEvent event) {
    if (!_input || !node.hasPrimaryFocus) return KeyEventResult.ignored;
    if (_nativeKeys || _keyboardMode == ViewerKeyboardMode.local) {
      return KeyEventResult.handled;
    }
    final raw = event.physicalKey.usbHidUsage;
    if ((raw >> 16) != 7 || (raw & 0xffff) == 0) return KeyEventResult.ignored;
    unawaited(
      _quiet(
        ViewerBridge.sendKey(
          usage: raw & 0xffff,
          down: event is! KeyUpEvent,
          physical: true,
          modifiers: _modifiers,
        ),
      ),
    );
    return KeyEventResult.handled;
  }

  void _mouse(int action, {int buttons = 0, int x = 0, int y = 0}) {
    if (!_input) return;
    unawaited(
      _quiet(
        ViewerBridge.sendMouse(
          action: action,
          buttons: buttons,
          x: x,
          y: y,
          modifiers: _modifiers,
        ),
      ),
    );
  }

  (int, int) _point(Offset p) {
    final s = _texture!.stream;
    return frameToDesktop(
      p,
      originX: s.originX,
      originY: s.originY,
      width: s.sourceWidth > 0 ? s.sourceWidth : s.width,
      height: s.sourceHeight > 0 ? s.sourceHeight : s.height,
    );
  }

  void _move(Offset p, int buttons) {
    if (!_input) return;
    final (x, y) = _point(p);
    var mapped = 0;
    for (var bit = 1; bit <= ViewerMouse.forward; bit <<= 1) {
      if (buttons & bit != 0) {
        mapped |= _remoteButtons[bit] ?? _mouseMapping.buttons(bit);
      }
    }
    _mouse(ViewerMouse.move, buttons: mapped, x: x, y: y);
  }

  void _button(Offset p, int button, bool down) {
    if (!_input) return;
    _move(p, _buttons);
    final mapped = down
        ? _remoteButtons.putIfAbsent(
            button,
            () => _mouseMapping.buttons(button),
          )
        : _remoteButtons.remove(button);
    if (down) {
      _buttons |= button;
    } else {
      _buttons &= ~button;
    }
    if (mapped == null) return;
    _mouse(
      down ? ViewerMouse.buttonDown : ViewerMouse.buttonUp,
      buttons: mapped,
    );
  }

  void _scroll(Offset p, Offset delta) {
    if (!_input) return;
    _move(p, _buttons);
    _wheelX += delta.dx;
    _wheelY += delta.dy;
    final x = (_wheelX / kViewerScrollPixelsPerNotch).truncate().clamp(-10, 10);
    final y = (_wheelY / kViewerScrollPixelsPerNotch).truncate().clamp(-10, 10);
    _wheelX -= x * kViewerScrollPixelsPerNotch;
    _wheelY -= y * kViewerScrollPixelsPerNotch;
    if (x != 0) {
      _mouse(
        ViewerMouse.wheelHorizontal,
        x: _mouseMapping.wheelX(x * kViewerWheelDelta),
      );
    }
    if (y != 0) {
      _mouse(
        ViewerMouse.wheel,
        y: _mouseMapping.wheelY(-y * kViewerWheelDelta),
      );
    }
  }

  void _gesture(Offset p, bool zoom, int x, int y) {
    if (!_input) return;
    _move(p, _buttons);
    unawaited(
      _quiet(
        ViewerBridge.sendGesture(zoom: zoom, x: x, y: y, modifiers: _modifiers),
      ),
    );
  }

  void _device(PointerEvent event, Offset p) {
    if (!_input) return;
    final (x, y) = _point(p);
    final pressure = windowsPointerPressure(
      event.pressure,
      event.pressureMin,
      event.pressureMax,
    );
    final action = event is PointerDownEvent
        ? ViewerTouchAction.down
        : event is PointerMoveEvent
        ? ViewerTouchAction.move
        : event is PointerCancelEvent
        ? ViewerTouchAction.cancel
        : ViewerTouchAction.up;
    if (event.kind == PointerDeviceKind.touch) {
      final contact = event is PointerDownEvent
          ? _contacts.acquire(event.pointer)
          : event is PointerMoveEvent
          ? _contacts.slotFor(event.pointer)
          : _contacts.release(event.pointer);
      if (contact == null) return;
      unawaited(
        _quiet(
          ViewerBridge.sendTouch(
            action: action,
            contact: contact,
            x: x,
            y: y,
            pressure: pressure,
            modifiers: _modifiers,
          ),
        ),
      );
    } else {
      if (event is PointerDownEvent) _pen = event.pointer;
      if (_pen != event.pointer) return;
      if (event is PointerUpEvent || event is PointerCancelEvent) _pen = null;
      final (tiltX, tiltY) = windowsPenTilt(event.tilt, event.orientation);
      unawaited(
        _quiet(
          ViewerBridge.sendPen(
            action: action,
            x: x,
            y: y,
            pressure: pressure,
            flags: windowsPenFlags(event.kind, event.buttons),
            tiltX: tiltX,
            tiltY: tiltY,
            modifiers: _modifiers,
          ),
        ),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    final texture = _texture;
    return Scaffold(
      backgroundColor: const Color(0xff101114),
      appBar: AppBar(
        title: Text(
          texture == null
              ? tr('显示器窗口')
              : tr('显示器 {n}', {'n': '${texture.stream.displayIndex + 1}'}),
        ),
        actions: [
          KeyboardModeButton(
            mode: _keyboardMode,
            onChanged: (mode) => unawaited(_changeKeyboardMode(mode)),
          ),
          IconButton(
            tooltip: _control ? tr('关闭远程键鼠') : tr('开启远程键鼠'),
            isSelected: _control,
            icon: const Icon(Icons.mouse_outlined),
            onPressed: !_closing && _status.acceptsInput
                ? () {
                    setState(() => _control = !_control);
                    _focus.requestFocus();
                    _syncCapture();
                  }
                : null,
          ),
          IconButton(
            tooltip: tr('缩放'),
            icon: const Icon(Icons.fit_screen),
            onPressed: () {
              setState(
                () => _fit = _fit == ViewFit.adaptive
                    ? ViewFit.original
                    : ViewFit.adaptive,
              );
              ViewPreferences.update(
                peerId: widget.peerId,
                fit: _fit == ViewFit.original ? 1 : 0,
              );
            },
          ),
          IconButton(
            tooltip: tr('全屏'),
            icon: Icon(_fullscreen ? Icons.fullscreen_exit : Icons.fullscreen),
            onPressed: () async {
              final value = await ViewerBridge.setFullscreen(!_fullscreen);
              if (mounted) setState(() => _fullscreen = value);
            },
          ),
          IconButton(
            tooltip: tr('关闭窗口'),
            icon: const Icon(Icons.close),
            onPressed: _close,
          ),
        ],
      ),
      body: Column(
        children: [
          if (_error.isNotEmpty)
            Text(_error, style: const TextStyle(color: Colors.orangeAccent)),
          Expanded(
            child: Focus(
              focusNode: _focus,
              onKeyEvent: _key,
              autofocus: true,
              child: texture == null || !texture.stream.ready
                  ? Center(
                      child: Text(
                        tr('等待远端画面…'),
                        style: const TextStyle(color: Colors.white70),
                      ),
                    )
                  : ListenableBuilder(
                      listenable: texture,
                      builder: (context, _) {
                        final s = texture.stream;
                        return RemoteView(
                          textureId: texture.textureId,
                          videoWidth: s.width,
                          videoHeight: s.height,
                          sourceWidth: s.sourceWidth,
                          sourceHeight: s.sourceHeight,
                          fit: _fit,
                          captureInput: _input,
                          hideCursor:
                              texture.pointer?.visible == true &&
                              texture.cursor != null &&
                              texture.cursorShape == texture.pointer?.shapeId,
                          overlayBuilder: (size) =>
                              displayCursorOverlay(texture, size),
                          onFocusRequested: () {
                            _focus.requestFocus();
                            unawaited(_displays.focus(s.streamId));
                          },
                          onHover: _move,
                          onMove: _move,
                          onButton: _button,
                          onScroll: _scroll,
                          onGesture: _gesture,
                          onDevicePointer: _device,
                          onDeviceCancel: _device,
                        );
                      },
                    ),
            ),
          ),
        ],
      ),
    );
  }
}
