import 'dart:convert';
import 'dart:io';

import 'package:chast_viewer/l10n.dart';
import 'package:chast_viewer/l10n_en.dart';
import 'package:flutter_test/flutter_test.dart';

final RegExp _cjk = RegExp(r'[\u3400-\u9fff]');
final RegExp _placeholder = RegExp(r'\{[^{}]*\}');
final RegExp _trLiteral = RegExp(r"\btr\(\s*'((?:[^'\\]|\\.)*)'");

/// Turns a raw Dart literal body into the runtime string, so `\n` in sources
/// compares equal to the newline in the dictionary key.
String _decode(String literal) {
  final StringBuffer out = StringBuffer();
  for (int i = 0; i < literal.length; i++) {
    final String ch = literal[i];
    if (ch != r'\' || i + 1 >= literal.length) {
      out.write(ch);
      continue;
    }
    final String next = literal[++i];
    switch (next) {
      case 'n':
        out.write('\n');
      case 'r':
        out.write('\r');
      case 't':
        out.write('\t');
      case '\\':
        out.write(r'\');
      case "'":
        out.write("'");
      case '"':
        out.write('"');
      case r'$':
        out.write(r'$');
      default:
        out.write(next);
    }
  }
  return out.toString();
}

void main() {
  tearDown(() {
    AppLanguage.configPathOverride = null;
    AppLanguage.current.value = kLanguageChinese;
  });

  group('tr', () {
    test('keeps Chinese as the source language', () {
      expect(tr('关闭'), '关闭');
      expect(tr('最高 {cap} 帧/秒', <String, Object?>{'cap': 60}), '最高 60 帧/秒');
    });

    test('renders English and fills placeholders', () {
      AppLanguage.current.value = kLanguageEnglish;
      expect(tr('关闭'), 'Close');
      expect(
        tr('最高 {cap} 帧/秒', <String, Object?>{'cap': 60}),
        'Max 60 fps',
      );
      expect(
        tr('显示器 {p1}', <String, Object?>{'p1': 2}),
        'Display 2',
      );
    });

    test('falls back to Chinese for an unknown key', () {
      AppLanguage.current.value = kLanguageEnglish;
      expect(tr('没有这个键'), '没有这个键');
    });

    test('persists the choice and loads it back', () {
      final Directory dir = Directory.systemTemp.createTempSync('chast_l10n_');
      addTearDown(() => dir.deleteSync(recursive: true));
      final String path = '${dir.path}${Platform.pathSeparator}ui.json';
      AppLanguage.configPathOverride = path;

      AppLanguage.set(kLanguageEnglish);
      expect(AppLanguage.code, kLanguageEnglish);
      expect(jsonDecode(File(path).readAsStringSync()), <String, Object?>{
        'language': 'en',
      });

      AppLanguage.current.value = kLanguageChinese;
      AppLanguage.load();
      expect(AppLanguage.code, kLanguageEnglish);

      AppLanguage.set(kLanguageChinese);
      expect(jsonDecode(File(path).readAsStringSync()), <String, Object?>{
        'language': 'zh',
      });
    });

    test('ignores a corrupt settings file', () {
      final Directory dir = Directory.systemTemp.createTempSync('chast_l10n_');
      addTearDown(() => dir.deleteSync(recursive: true));
      final File file = File('${dir.path}${Platform.pathSeparator}ui.json')
        ..writeAsStringSync('not json');
      AppLanguage.configPathOverride = file.path;

      AppLanguage.load();
      expect(AppLanguage.code, kLanguageChinese);
    });
  });

  group('dictionary', () {
    test('every Chinese string passed to tr has an English value', () {
      final Set<String> used = <String>{};
      final List<File> sources = Directory('lib')
          .listSync(recursive: true)
          .whereType<File>()
          .where((File file) => file.path.endsWith('.dart'))
          .where(
            (File file) =>
                !file.path.endsWith('l10n.dart') &&
                !file.path.endsWith('l10n_en.dart') &&
                !file.path.contains('${Platform.pathSeparator}l10n'
                    '${Platform.pathSeparator}'),
          )
          .toList();
      for (final File file in sources) {
        for (final RegExpMatch match
            in _trLiteral.allMatches(file.readAsStringSync())) {
          final String key = _decode(match.group(1)!);
          if (_cjk.hasMatch(key)) {
            used.add(key);
          }
        }
      }

      expect(used.length, greaterThan(700));
      final List<String> missing = used
          .where((String key) => !kEnglishMessages.containsKey(key))
          .toList()
        ..sort();
      expect(missing, isEmpty, reason: 'untranslated: $missing');
    });

    test('placeholders are identical in every key and value', () {
      final List<String> bad = <String>[];
      for (final MapEntry<String, String> entry in kEnglishMessages.entries) {
        final List<String> source =
            _placeholder.allMatches(entry.key).map((m) => m.group(0)!).toList()
              ..sort();
        final List<String> value =
            _placeholder.allMatches(entry.value).map((m) => m.group(0)!).toList()
              ..sort();
        if (source.join('|') != value.join('|') || entry.value.isEmpty) {
          bad.add('${entry.key} -> ${entry.value}');
        }
      }
      expect(bad, isEmpty, reason: 'placeholder mismatch: $bad');
    });
  });
}
