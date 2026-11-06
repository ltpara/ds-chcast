import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

import 'store_lock.dart';

/// Picture choices the viewer remembers across launches, the same way
/// RustDesk keeps its image quality and codec selection. Values are the
/// `code` fields of [ViewerStreamCodec] and [ViewerStreamQuality]; the frame
/// cap is one of 0 (uncapped), 15, 30 and 60.
class StreamPreferences {
  const StreamPreferences({
    this.codec = 0,
    this.quality = 0,
    this.maxFps = 0,
  });

  final int codec;
  final int quality;
  final int maxFps;

  static const String _codecKey = 'stream_codec';
  static const String _qualityKey = 'stream_quality';
  static const String _maxFpsKey = 'stream_max_fps';
  static const Set<int> _fpsCaps = <int>{0, 15, 30, 60};
  // Highest value of the matching preference enum: auto, h264, hevc, av1;
  // quality is high, balanced, smooth.
  static const int _codecValues = 3;
  static const int _qualityValues = 2;

  /// Redirects the settings file away from the real user profile in tests.
  @visibleForTesting
  static String? configPathOverride;

  /// A missing, unreadable or corrupt file yields the session defaults that
  /// the page already used before preferences were remembered. Each key is
  /// validated alone, so one bad value does not discard the others.
  static StreamPreferences load() {
    final Map<String, Object?> map = _readMap();
    return StreamPreferences(
      codec: _choice(map[_codecKey], _codecValues),
      quality: _choice(map[_qualityKey], _qualityValues),
      maxFps: _fps(map[_maxFpsKey]),
    );
  }

  /// Merges into `ui.json` instead of replacing it, because the language
  /// choice lives in the same file.
  static void save(StreamPreferences prefs) {
    final File? file = _file();
    if (file == null) {
      return;
    }
    withStoreLockSync(file, () {
      final Map<String, Object?> payload = _readMap()
        ..[_codecKey] = _choice(prefs.codec, _codecValues)
        ..[_qualityKey] = _choice(prefs.quality, _qualityValues)
        ..[_maxFpsKey] = _fps(prefs.maxFps);
      try {
        file.parent.createSync(recursive: true);
        file.writeAsStringSync(jsonEncode(payload));
      } catch (_) {
        // A picture preference; failing to persist it is not fatal.
      }
    });
  }

  static int _choice(Object? raw, int max) {
    if (raw is int && raw >= 0 && raw <= max) {
      return raw;
    }
    return 0;
  }

  static int _fps(Object? raw) {
    if (raw is int && _fpsCaps.contains(raw)) {
      return raw;
    }
    return 0;
  }

  static Map<String, Object?> _readMap() {
    final File? file = _file();
    if (file == null) {
      return <String, Object?>{};
    }
    return withStoreLockSync(file, () {
      try {
        if (!file.existsSync()) {
          return <String, Object?>{};
        }
        final Object? decoded = jsonDecode(file.readAsStringSync());
        if (decoded is Map<String, Object?>) {
          return Map<String, Object?>.of(decoded);
        }
      } catch (_) {
        // A corrupt settings file must not stop the viewer from starting.
      }
      return <String, Object?>{};
    });
  }

  static File? _file() {
    final String? override = configPathOverride;
    if (override != null && override.isNotEmpty) {
      return File(override);
    }
    final String? appData = Platform.environment['APPDATA'];
    if (appData == null || appData.isEmpty) {
      return null;
    }
    return File('$appData\\chast\\ui.json');
  }
}
