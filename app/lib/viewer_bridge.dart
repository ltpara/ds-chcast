import 'package:flutter/services.dart';

enum ViewerState { idle, connecting, handshaking, streaming, ended, failed }

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
}
