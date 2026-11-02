import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

/// Persists small pieces of address-book UI state so a viewer comes back the
/// way it was left: the tag and favorite filters and search text of
/// "我的设备", the search text of the shared-books dialog, and the collapsed
/// state of address-book groups. The two search keys are separate so one view
/// cannot overwrite the other, and the restored text is visible in its search
/// box.
class ViewFilter {
  ViewFilter._();

  static const String _tagsKey = 'tags';
  static const String _collapsedKey = 'collapsed';
  static const String _deviceSearchKey = 'deviceSearch';
  static const String _sharedSearchKey = 'sharedSearch';
  static const String _favoritesOnlyKey = 'favoritesOnly';

  /// Redirects the filter file away from the real user profile in tests.
  @visibleForTesting
  static String? configPathOverride;

  /// Reads the saved tag selection. A missing, unreadable or corrupt file
  /// yields the default "show everything" state instead of blocking launch.
  static Set<String> load() {
    return _clean(_readMap()[_tagsKey], dropEmpty: true);
  }

  /// Writes [tags], trimming blanks and dropping duplicates so the file stays
  /// stable between saves.
  static void save(Iterable<String> tags) {
    _write(_tagsKey, _clean(tags, dropEmpty: true).toList()..sort());
  }

  /// Group IDs collapsed on the device page. The empty string is the
  /// "未分组" bucket, so blank keys are meaningful here.
  static Set<String> loadCollapsed() {
    return _clean(_readMap()[_collapsedKey], dropEmpty: false);
  }

  static void saveCollapsed(Iterable<String> groups) {
    _write(_collapsedKey, _clean(groups, dropEmpty: false).toList()..sort());
  }

  /// Search text of "我的设备". Clearing the box removes the key again.
  static String loadDeviceSearch() {
    return _readString(_deviceSearchKey);
  }

  static void saveDeviceSearch(String query) {
    _writeString(_deviceSearchKey, query);
  }

  /// Search text of the shared-books dialog, stored apart from the device
  /// page so the two views restore independently.
  static String loadSharedSearch() {
    return _readString(_sharedSearchKey);
  }

  static void saveSharedSearch(String query) {
    _writeString(_sharedSearchKey, query);
  }

  /// Whether "我的设备" shows favorites only. Off removes the key so the
  /// default file stays minimal.
  static bool loadFavoritesOnly() {
    return _readMap()[_favoritesOnlyKey] == true;
  }

  static void saveFavoritesOnly(bool enabled) {
    final File? file = _file();
    if (file == null) {
      return;
    }
    final Map<String, Object?> payload = _readMap();
    if (enabled) {
      payload[_favoritesOnlyKey] = true;
    } else if (payload.remove(_favoritesOnlyKey) == null) {
      return;
    }
    _persist(file, payload);
  }

  static String _readString(String key) {
    final Object? raw = _readMap()[key];
    return raw is String ? raw.trim() : '';
  }

  static void _writeString(String key, String value) {
    final String trimmed = value.trim();
    final File? file = _file();
    if (file == null) {
      return;
    }
    final Map<String, Object?> payload = _readMap();
    if (trimmed.isEmpty) {
      if (!payload.containsKey(key)) {
        return;
      }
      payload.remove(key);
    } else {
      payload[key] = trimmed;
    }
    _persist(file, payload);
  }

  static Set<String> _clean(Object? raw, {required bool dropEmpty}) {
    if (raw is! Iterable) {
      return <String>{};
    }
    return <String>{
      for (final Object? value in raw)
        if (value is String)
          if (dropEmpty ? value.trim().isNotEmpty : true) value.trim(),
    };
  }

  static Map<String, Object?> _readMap() {
    final File? file = _file();
    if (file == null) {
      return <String, Object?>{};
    }
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
  }

  static void _write(String key, List<String> values) {
    final File? file = _file();
    if (file == null) {
      return;
    }
    _persist(file, _readMap()..[key] = values);
  }

  static void _persist(File file, Map<String, Object?> payload) {
    try {
      file.parent.createSync(recursive: true);
      file.writeAsStringSync(jsonEncode(payload));
    } catch (_) {
      // View preferences; failing to persist them is not fatal.
    }
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
    return File('$appData\\chast\\ui-filter.json');
  }
}
