import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

/// Persists small pieces of address-book UI state so a viewer comes back the
/// way it was left: the tag filter applied to "我的设备", the collapsed state
/// of address-book groups, and the search text of each address-book view. The
/// two searches use separate keys so typing in one never hides entries in the
/// other; clearing a search removes its key entirely.
class ViewFilter {
  ViewFilter._();

  static const String _tagsKey = 'tags';
  static const String _collapsedKey = 'collapsed';
  static const String _deviceSearchKey = 'search_devices';
  static const String _sharedSearchKey = 'search_shared';

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
    _writeList(_tagsKey, _clean(tags, dropEmpty: true).toList()..sort());
  }

  /// Group IDs collapsed on the device page. The empty string is the
  /// "未分组" bucket, so blank keys are meaningful here.
  static Set<String> loadCollapsed() {
    return _clean(_readMap()[_collapsedKey], dropEmpty: false);
  }

  static void saveCollapsed(Iterable<String> groups) {
    _writeList(_collapsedKey, _clean(groups, dropEmpty: false).toList()..sort());
  }

  /// The last search typed in [view]. Empty when none was saved; a locked or
  /// corrupt file falls back to the same state.
  static String loadSearch(SearchView view) {
    final Object? raw = _readMap()[_searchKey(view)];
    return raw is String ? raw.trim() : '';
  }

  /// Remembers [query] for [view] until it is cleared. An empty query drops
  /// the key so the file does not carry a filter that hides everything.
  static void saveSearch(SearchView view, String query) {
    final String value = query.trim();
    _writeValue(_searchKey(view), value.isEmpty ? null : value);
  }

  static String _searchKey(SearchView view) {
    return switch (view) {
      SearchView.devices => _deviceSearchKey,
      SearchView.shared => _sharedSearchKey,
    };
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

  static void _writeList(String key, List<String> values) {
    _writeValue(key, values);
  }

  static void _writeValue(String key, Object? value) {
    final File? file = _file();
    if (file == null) {
      return;
    }
    final Map<String, Object?> payload = _readMap();
    if (value == null) {
      payload.remove(key);
    } else {
      payload[key] = value;
    }
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

/// The two address-book views with independent search state.
enum SearchView { devices, shared }
