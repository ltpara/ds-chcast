import 'package:flutter/foundation.dart';

import 'prefs_store.dart';

/// Picture choices the viewer remembers, the same way RustDesk keeps its
/// image quality and codec selection. Values are the `code` fields of
/// [ViewerStreamCodec] and [ViewerStreamQuality]; the frame cap is 0 (follow
/// the host) or any value in [_minFps]..[_maxFps]. The menu keeps 15/30/60 as
/// shortcuts and accepts a typed custom value like RustDesk's custom FPS.
/// Custom image quality is kept as a percent in [_minQualityPercent]..
/// [_maxQualityPercent], independent of the selected preset, so switching
/// back to Custom reuses the last number.
///
/// Root `ui.json` keys are the default for devices that have no saved
/// choice; a non-empty [peerId] reads and writes that device's override.
class StreamPreferences {
  const StreamPreferences({
    this.codec = 0,
    this.quality = 0,
    this.qualityPercent = 50,
    this.maxFps = 0,
    this.fullColor = false,
  });

  final int codec;
  final int quality;
  final int qualityPercent;
  final int maxFps;
  final bool fullColor;

  static const String _codecKey = 'stream_codec';
  static const String _qualityKey = 'stream_quality';
  static const String _qualityPercentKey = 'stream_quality_percent';
  static const String _maxFpsKey = 'stream_max_fps';
  /// RustDesk's custom FPS range. The host clamps its own ceiling to 120.
  static const int _minFps = 5;
  static const int _maxFps = 120;
  /// RustDesk's custom image-quality range, in percent.
  static const int _minQualityPercent = 10;
  static const int _maxQualityPercent = 100;
  static const int _defaultQualityPercent = 50;
  // Highest value of the matching preference enum: auto, h264, hevc, av1, vp9;
  // quality is high, balanced, smooth, custom.
  static const int _codecValues = 4;
  static const int _qualityValues = 3;

  /// Kept here so existing tests can redirect the shared settings file.
  @visibleForTesting
  static String? get configPathOverride => PrefsStore.configPathOverride;

  @visibleForTesting
  static set configPathOverride(String? value) =>
      PrefsStore.configPathOverride = value;

  /// A missing, unreadable or corrupt file yields the session defaults that
  /// the page already used before preferences were remembered. Each key is
  /// validated alone, so one bad value does not discard the others.
  static StreamPreferences load({String peerId = ''}) {
    final Map<String, Object?> map = PrefsStore.readMerged(peerId);
    return StreamPreferences(
      codec: _choice(map[_codecKey], _codecValues),
      quality: _choice(map[_qualityKey], _qualityValues),
      qualityPercent: _percent(map[_qualityPercentKey]),
      maxFps: _fps(map[_maxFpsKey]),
      fullColor: map['stream_full_color'] == true,
    );
  }

  /// Merges into `ui.json` instead of replacing it, because the language
  /// choice lives in the same file. With [peerId] the choice belongs to that
  /// device and the shared defaults stay as they were.
  static void save(StreamPreferences prefs, {String peerId = ''}) {
    final Map<String, Object?> values = <String, Object?>{
      _codecKey: _choice(prefs.codec, _codecValues),
      _qualityKey: _choice(prefs.quality, _qualityValues),
      _qualityPercentKey: _percent(prefs.qualityPercent),
      _maxFpsKey: _fps(prefs.maxFps),
      'stream_full_color': prefs.fullColor,
    };
    if (peerId.isEmpty) {
      PrefsStore.update((Map<String, Object?> root) => root.addAll(values));
    } else {
      PrefsStore.updatePeer(
        peerId,
        (Map<String, Object?> peer) => peer.addAll(values),
      );
    }
  }

  static int _choice(Object? raw, int max) {
    if (raw is int && raw >= 0 && raw <= max) {
      return raw;
    }
    return 0;
  }

  static int _fps(Object? raw) {
    if (raw is int && (raw == 0 || (raw >= _minFps && raw <= _maxFps))) {
      return raw;
    }
    return 0;
  }

  static int _percent(Object? raw) {
    if (raw is int && raw >= _minQualityPercent && raw <= _maxQualityPercent) {
      return raw;
    }
    return _defaultQualityPercent;
  }
}
