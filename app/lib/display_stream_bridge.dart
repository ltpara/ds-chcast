import 'package:flutter/services.dart';

class DisplayVideoStream {
  const DisplayVideoStream({
    required this.streamId,
    required this.displayIndex,
    required this.width,
    required this.height,
    required this.sourceWidth,
    required this.sourceHeight,
    required this.originX,
    required this.originY,
    required this.published,
    required this.bound,
    required this.ready,
    required this.error,
  });

  factory DisplayVideoStream.fromMap(Map<Object?, Object?> value) =>
      DisplayVideoStream(
        streamId: (value['streamId'] as num).toInt(),
        displayIndex: (value['displayIndex'] as num).toInt(),
        width: (value['width'] as num).toInt(),
        height: (value['height'] as num).toInt(),
        sourceWidth: (value['sourceWidth'] as num).toInt(),
        sourceHeight: (value['sourceHeight'] as num).toInt(),
        originX: (value['originX'] as num).toInt(),
        originY: (value['originY'] as num).toInt(),
        published: (value['published'] as num).toInt(),
        bound: value['bound'] == true,
        ready: value['ready'] == true,
        error: value['error'] as String? ?? '',
      );

  final int streamId, displayIndex;
  final int width, height, sourceWidth, sourceHeight, originX, originY;
  final int published;
  final bool bound;
  final bool ready;
  final String error;
}

class DisplaySubscriptions {
  const DisplaySubscriptions({
    this.active = false,
    this.requestId = 0,
    this.results = 0,
    this.error = '',
    this.focused = -1,
    this.streams = const <DisplayVideoStream>[],
  });

  factory DisplaySubscriptions.fromMap(Map<Object?, Object?> value) =>
      DisplaySubscriptions(
        active: value['active'] == true,
        requestId: (value['requestId'] as num?)?.toInt() ?? 0,
        results: (value['results'] as num?)?.toInt() ?? 0,
        error: value['error'] as String? ?? '',
        focused: (value['focused'] as num?)?.toInt() ?? -1,
        streams: (value['streams'] as List<Object?>? ?? const <Object?>[])
            .map(
              (item) =>
                  DisplayVideoStream.fromMap(item as Map<Object?, Object?>),
            )
            .toList(growable: false),
      );

  final bool active;
  final int requestId, results;
  final int focused;
  final String error;
  final List<DisplayVideoStream> streams;
}

class DisplayPointer {
  DisplayPointer.fromMap(Map<Object?, Object?> value)
    : streamId = (value['streamId'] as num).toInt(),
      visible = value['visible'] == true,
      x = (value['x'] as num).toInt(),
      y = (value['y'] as num).toInt(),
      width = (value['width'] as num).toInt(),
      height = (value['height'] as num).toInt(),
      hotspotX = (value['hotspotX'] as num).toInt(),
      hotspotY = (value['hotspotY'] as num).toInt(),
      shapeId = (value['shapeId'] as num).toInt(),
      bgra = value['bgra'] as Uint8List;

  final int streamId, x, y, width, height, hotspotX, hotspotY, shapeId;
  final bool visible;
  final Uint8List bgra;
}

/// Desktop subscriptions share one authenticated native session. Stream IDs
/// identify decoder lifetimes; display indices are only selection arguments.
class DisplayStreamBridge {
  static const MethodChannel _channel = MethodChannel('chast/viewer');
  static Future<bool> openWindow(int streamId) async =>
      await _channel.invokeMethod<bool>('openDisplayWindow', streamId) ?? false;

  static Future<bool> focus(int streamId) async =>
      await _channel.invokeMethod<bool>('focusDisplay', streamId) ?? false;

  static Future<bool> capture({
    required int requestId,
    List<int> add = const <int>[],
    List<int> sub = const <int>[],
    List<int> set = const <int>[],
  }) async =>
      await _channel.invokeMethod<bool>('captureDisplays', <String, Object>{
        'requestId': requestId,
        'add': add,
        'sub': sub,
        'set': set,
      }) ??
      false;

  /// Await detaching with output=0 before unregistering the plugin texture.
  static Future<bool> bind(int streamId, int output) async =>
      await _channel.invokeMethod<bool>('bindDisplayOutput', <String, int>{
        'streamId': streamId,
        'output': output,
      }) ??
      false;

  static Future<DisplaySubscriptions> subscriptions() async {
    final value = await _channel.invokeMapMethod<Object?, Object?>(
      'displayStreams',
    );
    return DisplaySubscriptions.fromMap(value ?? const <Object?, Object?>{});
  }

  static Future<List<DisplayPointer>> takePointers() async {
    final values = await _channel.invokeListMethod<Object?>(
      'takeDisplayCursors',
    );
    return (values ?? const <Object?>[])
        .map((item) => DisplayPointer.fromMap(item as Map<Object?, Object?>))
        .toList(growable: false);
  }
}
