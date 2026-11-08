import 'dart:async';
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter_gpu_texture_renderer/flutter_gpu_texture_renderer.dart';

import 'display_stream_bridge.dart';

class DisplayTexture extends ChangeNotifier {
  DisplayTexture(this.stream, this.textureId);
  DisplayVideoStream stream;
  final int textureId;
  DisplayPointer? pointer;
  ui.Image? cursor;
  int? cursorShape;
  void updateCursor() => notifyListeners();
}

/// Owns the textures of one native connection. A retired controller never binds
/// an output again, including when registration finishes during reconnect.
class DisplayStreamController extends ChangeNotifier {
  DisplayStreamController({
    Future<int?> Function()? register,
    Future<int?> Function(int)? output,
    Future<void> Function(int)? unregister,
  }) {
    final renderer = FlutterGpuTextureRenderer();
    _register = register ?? renderer.registerTexture;
    _output = output ?? renderer.output;
    _unregister =
        unregister ??
        (id) async {
          await renderer.unregisterTexture(id);
        };
  }

  late final Future<int?> Function() _register;
  late final Future<int?> Function(int) _output;
  late final Future<void> Function(int) _unregister;
  final Map<int, DisplayTexture> _textures = {};
  DisplaySubscriptions state = const DisplaySubscriptions();
  String error = '';
  int? focused;
  int _request = 0;
  int? _pending;
  bool _closed = false;
  Future<void>? _refreshing;
  Future<void>? _selecting;
  Future<void>? _pointers;
  Future<void>? _closing;
  Timer? _cursorTimer;
  bool get busy => _pending != null;
  List<DisplayTexture> get textures => List.unmodifiable(_textures.values);
  DisplayTexture? get selected => _textures[focused];

  Future<bool> select(List<int> displays) async {
    if (_closed || busy) return false;
    final request = ++_request;
    _pending = request;
    error = '';
    notifyListeners();
    final done = Completer<void>();
    _selecting = done.future;
    try {
      final accepted = await DisplayStreamBridge.capture(
        requestId: request,
        set: displays,
      );
      if (!accepted) {
        _pending = null;
        error = '无法提交显示器选择';
      }
      return accepted;
    } catch (e) {
      _pending = null;
      error = '无法提交显示器选择：$e';
      return false;
    } finally {
      done.complete();
      _selecting = null;
      if (!_closed) notifyListeners();
    }
  }

  Future<void> focus(int id) async {
    if (_closed || !_textures.containsKey(id)) return;
    try {
      if (await DisplayStreamBridge.focus(id) &&
          !_closed &&
          _textures.containsKey(id)) {
        focused = id;
        notifyListeners();
      }
    } catch (e) {
      error = '无法选择当前显示器：$e';
      if (!_closed) notifyListeners();
    }
  }

  Future<void> refresh() {
    if (_closed) return Future.value();
    return _refreshing ??= _refresh().whenComplete(() {
      _refreshing = null;
    });
  }

  Future<void> _release(int id, {bool sessionStopped = false}) async {
    final texture = _textures[id];
    if (texture == null) return;
    if (!sessionStopped && !await DisplayStreamBridge.bind(id, 0)) {
      throw StateError('无法释放显示器输出');
    }
    await _unregister(texture.textureId);
    texture.cursor?.dispose();
    texture.dispose();
    _textures.remove(id);
  }

  Future<void> _refresh() async {
    try {
      final next = await DisplayStreamBridge.subscriptions();
      if (_closed) return;
      state = next;
      if (_pending == next.requestId && next.results > 0) {
        _pending = null;
        error = next.error;
      }
      final ids = next.streams.map((s) => s.streamId).toSet();
      for (final id in _textures.keys.toList()) {
        if (!ids.contains(id)) await _release(id);
      }
      for (final stream in next.streams) {
        if (_closed) return;
        final existing = _textures[stream.streamId];
        if (existing != null) {
          existing.stream = stream;
          continue;
        }
        final id = await _register() ?? -1;
        if (id < 0) throw StateError('无法创建显示器纹理');
        bool bound = false;
        try {
          if (_closed) continue;
          final output = await _output(id) ?? 0;
          if (_closed) continue;
          if (output == 0) throw StateError('无法创建显示器输出');
          // Keep ownership even if the channel reports an error after binding.
          _textures[stream.streamId] = DisplayTexture(stream, id);
          bound = true;
          if (!await DisplayStreamBridge.bind(stream.streamId, output)) {
            await _release(stream.streamId);
            throw StateError('显示器订阅已失效');
          }
        } catch (_) {
          if (bound && _textures.containsKey(stream.streamId)) {
            await _release(stream.streamId);
          }
          rethrow;
        } finally {
          if (!bound) await _unregister(id);
        }
      }
      if (_closed) return;
      if (!_textures.containsKey(focused)) {
        focused = _textures.containsKey(next.focused) ? next.focused : null;
      }
      if (next.active && _textures.isNotEmpty) {
        _cursorTimer ??= Timer.periodic(const Duration(milliseconds: 33), (_) {
          unawaited(_refreshPointers());
        });
      } else {
        _cursorTimer?.cancel();
        _cursorTimer = null;
      }
      await _refreshPointers();
    } catch (e) {
      error = '显示器更新失败：$e';
    } finally {
      if (!_closed) notifyListeners();
    }
  }

  Future<void> _refreshPointers() {
    if (_closed) return Future.value();
    return _pointers ??= _takePointers().whenComplete(() {
      _pointers = null;
    });
  }

  Future<void> _takePointers() async {
    var changed = false;
    try {
      for (final pointer in await DisplayStreamBridge.takePointers()) {
        if (_closed) return;
        final texture = _textures[pointer.streamId];
        if (texture == null) continue;
        texture.pointer = pointer;
        if (pointer.bgra.isNotEmpty &&
            pointer.width > 0 &&
            pointer.height > 0 &&
            pointer.bgra.length == pointer.width * pointer.height * 4 &&
            texture.cursorShape != pointer.shapeId) {
          final decoded = Completer<ui.Image>();
          ui.decodeImageFromPixels(
            pointer.bgra,
            pointer.width,
            pointer.height,
            ui.PixelFormat.bgra8888,
            decoded.complete,
          );
          final image = await decoded.future;
          if (_closed || _textures[pointer.streamId] != texture) {
            image.dispose();
            return;
          }
          texture.cursor?.dispose();
          texture.cursor = image;
          texture.cursorShape = pointer.shapeId;
        }
        texture.updateCursor();
      }
    } catch (e) {
      final message = '显示器光标更新失败：$e';
      changed = error != message;
      error = message;
    } finally {
      if (!_closed && changed) notifyListeners();
    }
  }

  Future<void> close({bool sessionStopped = false}) =>
      _closing ??= _close(sessionStopped: sessionStopped).whenComplete(() {
        _closing = null;
      });

  Future<void> _close({required bool sessionStopped}) async {
    _closed = true;
    _cursorTimer?.cancel();
    await _selecting;
    await _refreshing;
    await _pointers;
    for (final id in _textures.keys.toList()) {
      await _release(id, sessionStopped: sessionStopped);
    }
  }
}
