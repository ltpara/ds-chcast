import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_gpu_texture_renderer/flutter_gpu_texture_renderer.dart';

import '../target.dart';
import '../theme.dart';
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

class _SessionPageState extends State<SessionPage> {
  static const Duration _pollInterval = Duration(milliseconds: 200);
  static const Duration _hideDelay = Duration(seconds: 5);

  final FocusNode _focus = FocusNode(debugLabel: 'session');
  final FlutterGpuTextureRenderer _gpuTexture = FlutterGpuTextureRenderer();

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
  ViewFit _fit = ViewFit.adaptive;

  @override
  void initState() {
    super.initState();
    _startPolling();
    _starting = _start();
    unawaited(_starting);
  }

  @override
  void dispose() {
    _pollTimer?.cancel();
    _hideTimer?.cancel();
    _focus.dispose();
    unawaited(_shutdown());
    super.dispose();
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
    try {
      await ViewerBridge.start(
        host: widget.target.host,
        port: widget.target.port,
        gpuOutput: output,
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

  String get _overTitle {
    if (_startError != null) {
      return '无法发起连接';
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
                    onDoubleTap: _toggleFullscreen,
                    child: RemoteView(
                      textureId: _textureId,
                      videoWidth: _status.width,
                      videoHeight: _status.height,
                      fit: _fit,
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
                  failed ? Icons.link_off_rounded : Icons.videocam_off_rounded,
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
                        onPressed: _reconnecting ? null : _reconnect,
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
                            : const Text('重新连接'),
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
