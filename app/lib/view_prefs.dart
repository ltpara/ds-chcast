import 'store_lock.dart';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

/// View state the viewer remembers across launches: how the picture is fitted
/// (RustDesk's view style), whether the stats overlay is shown, and how the
/// file browser sorts. All live in `ui.json` next to the language and picture
/// preferences.
class ViewPreferences {
  const ViewPreferences({
    this.fit = 0,
    this.stats = false,
    this.fileSort = 0,
    this.fileSortAscending = true,
    this.browserDirectory = '',
    this.terminalFontSize = 13,
  });

  /// 0 = adaptive (fit the window), 1 = original pixels.
  final int fit;
  final bool stats;

  /// 0 = name, 1 = size, 2 = modified (`FileBrowserSort` order).
  final int fileSort;
  final bool fileSortAscending;

  /// Share-relative folder last shown, with `/` separators. Empty is the root.
  final String browserDirectory;

  /// Terminal font size, clamped to the panel's 10–20 range.
  final int terminalFontSize;

  static const String _fitKey = 'view_fit';
  static const String _statsKey = 'view_stats';
  static const String _fileSortKey = 'file_sort';
  static const String _fileSortAscendingKey = 'file_sort_asc';
  static const String _browserDirectoryKey = 'browser_dir';
  static const String _terminalFontSizeKey = 'terminal_font_size';

  /// Redirects the settings file away from the real user profile in tests.
  @visibleForTesting
  static String? configPathOverride;

  /// A missing, unreadable or corrupt file yields the session defaults.
  /// Each key is validated alone, so one bad value does not discard the other.
  static ViewPreferences load() {
    final Map<String, Object?> map = _readMap();
    final Object? rawFit = map[_fitKey];
    final Object? rawStats = map[_statsKey];
    final Object? rawSort = map[_fileSortKey];
    final Object? rawAscending = map[_fileSortAscendingKey];
    final Object? rawFontSize = map[_terminalFontSizeKey];
    return ViewPreferences(
      fit: rawFit is int && (rawFit == 0 || rawFit == 1) ? rawFit : 0,
      stats: rawStats is bool ? rawStats : false,
      fileSort: rawSort is int && rawSort >= 0 && rawSort <= 2 ? rawSort : 0,
      fileSortAscending: rawAscending is bool ? rawAscending : true,
      browserDirectory: normalizeDirectory(map[_browserDirectoryKey]),
      terminalFontSize:
          rawFontSize is int && rawFontSize >= 10 && rawFontSize <= 20
          ? rawFontSize
          : 13,
    );
  }

  /// Keeps only real path segments so a stored value can never climb above
  /// the host share root or carry separators the listing request rejects.
  @visibleForTesting
  static String normalizeDirectory(Object? raw) {
    if (raw is! String) {
      return '';
    }
    final List<String> parts = <String>[];
    for (final String segment in raw.split('/')) {
      final String part = segment.trim();
      if (part.isEmpty || part == '.' || part == '..') {
        continue;
      }
      parts.add(part);
    }
    return parts.join('/');
  }

  /// Merges into `ui.json` instead of replacing it, because the language and
  /// picture choices live in the same file.
  static void save(ViewPreferences prefs) {
    final File? file = _file();
    if (file == null) {
      return;
    }
    try {
      withStoreLockSync(file, () {
        final Map<String, Object?> payload = _readMap()
          ..[_fitKey] = prefs.fit == 1 ? 1 : 0
          ..[_statsKey] = prefs.stats
          ..[_fileSortKey] = prefs.fileSort >= 0 && prefs.fileSort <= 2
              ? prefs.fileSort
              : 0
          ..[_fileSortAscendingKey] = prefs.fileSortAscending
          ..[_browserDirectoryKey] = normalizeDirectory(prefs.browserDirectory)
          ..[_terminalFontSizeKey] =
              prefs.terminalFontSize >= 10 && prefs.terminalFontSize <= 20
              ? prefs.terminalFontSize
              : 13;
        try {
          file.parent.createSync(recursive: true);
          file.writeAsStringSync(jsonEncode(payload));
        } catch (_) {
          // A view preference; failing to persist it is not fatal.
        }
      });
    } on FileSystemException {
      // Preferences are best effort when the directory cannot be locked.
    }
  }

  static Map<String, Object?> _readMap() {
    final File? file = _file();
    if (file == null) {
      return <String, Object?>{};
    }
    try {
      return withStoreLockSync(file, () {
        if (!file.existsSync()) {
          return <String, Object?>{};
        }
        final Object? decoded = jsonDecode(file.readAsStringSync());
        if (decoded is Map<String, Object?>) {
          return Map<String, Object?>.of(decoded);
        }
        return <String, Object?>{};
      });
    } catch (_) {
      // A corrupt settings file must not stop the viewer from starting.
    }
    return <String, Object?>{};
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
