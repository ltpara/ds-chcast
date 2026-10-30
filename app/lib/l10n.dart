import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

import 'l10n_en.dart';

/// The two UI languages the viewer ships. Chinese is the source language, so
/// every string in the app is written in Chinese and [tr] looks up the English
/// rendering from [kEnglishMessages]; an untranslated string stays in Chinese
/// rather than disappearing.
const String kLanguageChinese = 'zh';
const String kLanguageEnglish = 'en';

class AppLanguage {
  AppLanguage._();

  static final ValueNotifier<String> current = ValueNotifier<String>(
    kLanguageChinese,
  );

  static String get code => current.value;

  static bool get isEnglish => current.value == kLanguageEnglish;

  /// Native name of the active language, shown in the picker itself.
  static String get label => isEnglish ? 'English' : '简体中文';

  /// Redirects the settings file away from the real user profile in tests.
  @visibleForTesting
  static String? configPathOverride;

  static void set(String code) {
    final String normalized = code == kLanguageEnglish
        ? kLanguageEnglish
        : kLanguageChinese;
    if (current.value == normalized) {
      return;
    }
    current.value = normalized;
    _save(normalized);
  }

  /// Reads the saved choice before the first frame. A missing or unreadable
  /// file keeps Chinese, which is also what every existing installation used.
  static void load() {
    final File? file = _file();
    if (file == null) {
      return;
    }
    try {
      if (!file.existsSync()) {
        return;
      }
      final Object? decoded = jsonDecode(file.readAsStringSync());
      if (decoded is Map<String, Object?>) {
        final Object? language = decoded['language'];
        if (language == kLanguageEnglish) {
          current.value = kLanguageEnglish;
        }
      }
    } catch (_) {
      // A corrupt settings file must not stop the viewer from starting.
    }
  }

  static void _save(String code) {
    final File? file = _file();
    if (file == null) {
      return;
    }
    try {
      file.parent.createSync(recursive: true);
      file.writeAsStringSync(jsonEncode(<String, Object?>{'language': code}));
    } catch (_) {
      // Language is a preference; failing to persist it is not fatal.
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
    return File('$appData\\chast\\ui.json');
  }
}

/// Resolves [source] (Chinese, the language the UI is written in) for the
/// active language. [args] fill `{name}` placeholders that replaced Dart
/// interpolation, so translations can reorder or drop the values.
String tr(String source, [Map<String, Object?>? args]) {
  String text = AppLanguage.isEnglish
      ? (kEnglishMessages[source] ?? source)
      : source;
  if (args == null || args.isEmpty) {
    return text;
  }
  for (final MapEntry<String, Object?> entry in args.entries) {
    text = text.replaceAll('{${entry.key}}', '${entry.value}');
  }
  return text;
}
