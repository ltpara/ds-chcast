import 'package:flutter/services.dart';

enum ViewerState { idle, connecting, handshaking, streaming, ended, failed }

/// Pointer actions, mirroring chast::MouseAction on the wire.
enum RemoteMouseAction { move, moveRelative, buttonDown, buttonUp, wheel, wheelHorizontal }

/// Mouse button bits, mirroring chast::MouseButtonBit.
class RemoteMouseButton {
  static const int left = 1;
  static const int right = 2;
  static const int middle = 4;
  static const int back = 8;
  static const int forward = 16;
}

/// One pointer event in remote virtual-desktop pixels.
class RemoteMouseEvent {
  const RemoteMouseEvent(this.action, {this.buttons = 0, this.x = 0, this.y = 0});

  final RemoteMouseAction action;
  final int buttons;
  final int x;
  final int y;
}

/// One keyboard event carrying a USB HID usage id (usage page 0x07).
class RemoteKeyEvent {
  const RemoteKeyEvent(this.down, this.hidUsage);

  final bool down;
  final int hidUsage;
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
    this.framerate = 0,
    this.serverBitrate = 0,
    this.decoder = '',
    this.serverEncoder = '',
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
    this.elapsedMs = 0,
    this.videoFinished = false,
    this.fullscreen = false,
    this.inputEnabled = false,
    this.originX = 0,
    this.originY = 0,
    this.inputSent = 0,
    this.inputDropped = 0,
  });

  final ViewerState state;
  final String error;
  final String host;
  final int port;
  final String codec;
  final int width;
  final int height;
  final int framerate;
  final int serverBitrate;
  final String decoder;
  final String serverEncoder;
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
  final int elapsedMs;
  final bool videoFinished;
  final bool fullscreen;

  /// The server accepts input events (it was started with input enabled).
  final bool inputEnabled;

  /// The captured output's top-left in the host's virtual-desktop pixels.
  /// Pointers are sent in that space, so the viewer adds these to positions
  /// measured inside the video picture.
  final int originX;
  final int originY;

  /// Events this viewer handed to the transport, and how many it dropped.
  final int inputSent;
  final int inputDropped;

  bool get hasFrame => published > 0 && width > 0 && height > 0;
  bool get isLive => state == ViewerState.streaming;
  bool get isOver =>
      state == ViewerState.ended || state == ViewerState.failed || error.isNotEmpty;
  String get codecLabel => codec == 'hevc' ? 'HEVC' : 'H.264';

  static ViewerStatus fromMap(Map<Object?, Object?> map) {
    int asInt(Object? value) => (value as num?)?.toInt() ?? 0;
    double asDouble(Object? value) => (value as num?)?.toDouble() ?? 0.0;
    String asString(Object? value) => value?.toString() ?? '';
    return ViewerStatus(
      state: _parseState(map['state']),
      error: asString(map['error']),
      host: asString(map['host']),
      port: asInt(map['port']),
      codec: asString(map['codec']),
      width: asInt(map['width']),
      height: asInt(map['height']),
      framerate: asInt(map['framerate']),
      serverBitrate: asInt(map['serverBitrate']),
      decoder: asString(map['decoder']),
      serverEncoder: asString(map['serverEncoder']),
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
      elapsedMs: asInt(map['elapsedMs']),
      videoFinished: map['videoFinished'] == true,
      fullscreen: map['fullscreen'] == true,
      inputEnabled: map['inputEnabled'] == true,
      originX: asInt(map['originX']),
      originY: asInt(map['originY']),
      inputSent: asInt(map['inputSent']),
      inputDropped: asInt(map['inputDropped']),
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
  static Future<void> start({
    required String host,
    required int port,
    required int gpuOutput,
  }) async {
    await _channel.invokeMethod<void>(
      'start',
      <String, Object>{'host': host, 'port': port, 'gpuOutput': gpuOutput},
    );
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

  /// Queues one pointer event. Returns false when the session has ended or the
  /// queue is full, which is not an error: the event is simply stale.
  static Future<bool> sendMouse(RemoteMouseEvent event) async {
    final bool? accepted = await _channel.invokeMethod<bool>('input', <String, Object>{
      'kind': 'mouse',
      'action': event.action.index,
      'buttons': event.buttons,
      'x': event.x,
      'y': event.y,
    });
    return accepted ?? false;
  }

  static Future<bool> sendKey(RemoteKeyEvent event) async {
    final bool? accepted = await _channel.invokeMethod<bool>('input', <String, Object>{
      'kind': 'key',
      'action': event.down ? 0 : 1,
      'hidUsage': event.hidUsage,
    });
    return accepted ?? false;
  }
}
