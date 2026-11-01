import 'dart:convert';
import 'dart:io';

import 'package:chast_viewer/l10n.dart';
import 'package:chast_viewer/stream_prefs.dart';
import 'package:chast_viewer/view_prefs.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  late Directory dir;
  late String path;

  setUp(() {
    dir = Directory.systemTemp.createTempSync('chast_view_prefs_');
    path = '${dir.path}${Platform.pathSeparator}ui.json';
    ViewPreferences.configPathOverride = path;
    AppLanguage.configPathOverride = path;
    StreamPreferences.configPathOverride = path;
  });

  tearDown(() {
    ViewPreferences.configPathOverride = null;
    AppLanguage.configPathOverride = null;
    StreamPreferences.configPathOverride = null;
    AppLanguage.current.value = kLanguageChinese;
    dir.deleteSync(recursive: true);
  });

  Map<String, Object?> readFile() =>
      jsonDecode(File(path).readAsStringSync()) as Map<String, Object?>;

  test('a missing file keeps the session defaults', () {
    final ViewPreferences prefs = ViewPreferences.load();
    expect(prefs.fit, 0);
    expect(prefs.stats, isFalse);
    expect(prefs.fileSort, 0);
    expect(prefs.fileSortAscending, isTrue);
    expect(prefs.browserDirectory, '');
    expect(prefs.terminalFontSize, 13);
  });

  test('saves and loads the choices', () {
    ViewPreferences.save(
      const ViewPreferences(
        fit: 1,
        stats: true,
        fileSort: 2,
        fileSortAscending: false,
        browserDirectory: 'photos/2026',
        terminalFontSize: 17,
      ),
    );
    final ViewPreferences prefs = ViewPreferences.load();
    expect(prefs.fit, 1);
    expect(prefs.stats, isTrue);
    expect(prefs.fileSort, 2);
    expect(prefs.fileSortAscending, isFalse);
    expect(prefs.browserDirectory, 'photos/2026');
    expect(prefs.terminalFontSize, 17);
    expect(readFile(), <String, Object?>{
      'view_fit': 1,
      'view_stats': true,
      'file_sort': 2,
      'file_sort_asc': false,
      'browser_dir': 'photos/2026',
      'terminal_font_size': 17,
    });
  });

  test('each invalid value falls back on its own', () {
    File(path).writeAsStringSync(
      jsonEncode(<String, Object?>{
        'view_fit': 1,
        'view_stats': 'yes',
        'file_sort': 9,
        'file_sort_asc': 'up',
        'browser_dir': 7,
        'terminal_font_size': 99,
      }),
    );
    final ViewPreferences prefs = ViewPreferences.load();
    expect(prefs.fit, 1);
    expect(prefs.stats, isFalse);
    expect(prefs.fileSort, 0);
    expect(prefs.fileSortAscending, isTrue);
    expect(prefs.browserDirectory, '');
    expect(prefs.terminalFontSize, 13);
  });

  test('browser directory keeps only real path segments', () {
    expect(ViewPreferences.normalizeDirectory('//a/./b/../c/'), 'a/b/c');
    expect(ViewPreferences.normalizeDirectory('  photos  '), 'photos');
    expect(ViewPreferences.normalizeDirectory(5), '');
  });

  test('a corrupt or wrong-shaped file keeps the defaults', () {
    File(path).writeAsStringSync('not json');
    expect(ViewPreferences.load().fit, 0);
    File(path).writeAsStringSync('[1, 2, 3]');
    expect(ViewPreferences.load().stats, isFalse);
  });

  test('save normalises the fit and keeps foreign keys', () {
    File(path).writeAsStringSync(
      jsonEncode(<String, Object?>{'language': 'en', 'stream_codec': 2}),
    );
    ViewPreferences.save(
      const ViewPreferences(
        fit: 7,
        stats: true,
        fileSort: -1,
        fileSortAscending: false,
        browserDirectory: '///photos//./2026/',
        terminalFontSize: 99,
      ),
    );
    expect(readFile(), <String, Object?>{
      'language': 'en',
      'stream_codec': 2,
      'view_fit': 0,
      'view_stats': true,
      'file_sort': 0,
      'file_sort_asc': false,
      'browser_dir': 'photos/2026',
      'terminal_font_size': 13,
    });
  });

  test('language and picture changes keep the view choices', () {
    ViewPreferences.save(
      const ViewPreferences(
        fit: 1,
        stats: true,
        fileSort: 1,
        fileSortAscending: false,
        browserDirectory: 'photos',
      ),
    );
    AppLanguage.set(kLanguageEnglish);
    StreamPreferences.save(
      const StreamPreferences(codec: 2, quality: 1, maxFps: 30),
    );
    expect(readFile()['view_fit'], 1);
    expect(readFile()['view_stats'], true);
    expect(readFile()['file_sort'], 1);
    expect(readFile()['file_sort_asc'], false);
    expect(readFile()['browser_dir'], 'photos');
    final ViewPreferences prefs = ViewPreferences.load();
    expect(prefs.fit, 1);
    expect(prefs.stats, isTrue);
    expect(prefs.fileSort, 1);
    expect(prefs.fileSortAscending, isFalse);
    expect(prefs.browserDirectory, 'photos');
  });

  test('a view change keeps the language and picture choices', () {
    AppLanguage.set(kLanguageEnglish);
    StreamPreferences.save(
      const StreamPreferences(codec: 1, quality: 2, maxFps: 60),
    );
    ViewPreferences.save(const ViewPreferences(fit: 1, stats: false));
    expect(readFile()['language'], 'en');
    expect(readFile()['stream_codec'], 1);
    expect(readFile()['stream_quality'], 2);
    expect(readFile()['stream_max_fps'], 60);
  });

  test('a sort change keeps the language and picture choices', () {
    AppLanguage.set(kLanguageEnglish);
    StreamPreferences.save(
      const StreamPreferences(codec: 1, quality: 2, maxFps: 60),
    );
    ViewPreferences.save(
      const ViewPreferences(
        fit: 0,
        stats: false,
        fileSort: 1,
        fileSortAscending: false,
        browserDirectory: 'photos',
      ),
    );
    expect(readFile()['language'], 'en');
    expect(readFile()['stream_codec'], 1);
    expect(readFile()['stream_quality'], 2);
    expect(readFile()['stream_max_fps'], 60);
    expect(readFile()['file_sort'], 1);
    expect(readFile()['file_sort_asc'], false);
    expect(readFile()['browser_dir'], 'photos');
  });
}
