import 'dart:convert';
import 'dart:io';

import 'package:chast_viewer/l10n.dart';
import 'package:chast_viewer/stream_prefs.dart';
import 'package:chast_viewer/view_prefs.dart';
import 'package:chast_viewer/target.dart';
import 'package:chast_viewer/viewer_bridge.dart';
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

  test(
    'same host id on different servers has isolated view and stream preferences',
    () {
      const a = ConnectTarget(
        '',
        0,
        server: 'https://one.example',
        hostId: 'desk',
      );
      const b = ConnectTarget(
        '',
        0,
        server: 'https://two.example',
        hostId: 'desk',
      );
      ViewPreferences.save(
        const ViewPreferences(fit: 1, keyboardMode: 'local'),
        peerId: 'desk',
      );
      StreamPreferences.save(const StreamPreferences(codec: 3), peerId: 'desk');
      expect(ViewPreferences.load(peerId: a.preferencesKey).fit, 0);
      expect(
        ViewPreferences.load(peerId: b.preferencesKey).keyboardMode,
        'physical',
      );
      expect(StreamPreferences.load(peerId: b.preferencesKey).codec, 0);
      ViewPreferences.update(
        peerId: a.preferencesKey,
        fit: 1,
        swapButtons: true,
      );
      StreamPreferences.save(
        const StreamPreferences(codec: 3),
        peerId: a.preferencesKey,
      );
      expect(
        ViewPreferences.load(peerId: a.preferencesKey).swapButtons,
        isTrue,
      );
      expect(StreamPreferences.load(peerId: a.preferencesKey).codec, 3);
      expect(ViewPreferences.load(peerId: b.preferencesKey).fit, 0);
      expect(StreamPreferences.load(peerId: b.preferencesKey).codec, 0);
      expect(ViewPreferences.load(peerId: 'desk').keyboardMode, 'local');
    },
  );

  test('window patches keep concurrently updated peer and shared fields', () {
    ViewPreferences.update(peerId: 'desk', fit: 1, keyboardMode: 'local');
    ViewPreferences.update(
      peerId: 'other',
      terminalFontSize: 18,
      browserDirectory: 'docs',
    );
    ViewPreferences.update(
      peerId: 'desk',
      swapButtons: true,
      reverseWheel: true,
      browserShowHidden: true,
    );
    ViewPreferences.update(peerId: 'desk', fit: 0);
    final prefs = ViewPreferences.load(peerId: 'desk');
    expect(prefs.fit, 0);
    expect(prefs.keyboardMode, 'local');
    expect(prefs.swapButtons, isTrue);
    expect(prefs.reverseWheel, isTrue);
    expect(prefs.browserShowHidden, isTrue);
    expect(ViewPreferences.load(peerId: 'other').browserShowHidden, isFalse);
    expect(prefs.terminalFontSize, 18);
    expect(prefs.browserDirectory, 'docs');
  });

  test('keyboard mode keeps legacy and rejects unknown values', () {
    ViewPreferences.update(peerId: 'desk', keyboardMode: 'legacy');
    expect(ViewPreferences.load(peerId: 'desk').keyboardMode, 'legacy');
    ViewPreferences.update(peerId: 'desk', keyboardMode: 'remote-ime');
    expect(ViewPreferences.load(peerId: 'desk').keyboardMode, 'physical');
    expect(viewerKeyboardModeFromName('legacy'), ViewerKeyboardMode.legacy);
    expect(viewerKeyboardModeFromName('local'), ViewerKeyboardMode.local);
    expect(viewerKeyboardModeFromName(null), ViewerKeyboardMode.physical);
  });

  test('a missing file keeps the session defaults', () {
    final ViewPreferences prefs = ViewPreferences.load();
    expect(prefs.fit, 0);
    expect(prefs.stats, isFalse);
    expect(prefs.fileSort, 0);
    expect(prefs.fileSortAscending, isTrue);
    expect(prefs.browserDirectory, '');
    expect(prefs.browserShowHidden, isFalse);
    expect(prefs.terminalFontSize, 13);
    expect(prefs.reverseWheel, isFalse);
    expect(prefs.swapButtons, isFalse);
    expect(prefs.keyboardMode, 'physical');
    expect(prefs.autoRecord, isFalse);
    expect(prefs.recordKeep, 0);
  });

  test('saves and loads the choices', () {
    ViewPreferences.save(
      const ViewPreferences(
        fit: 1,
        stats: true,
        fileSort: 2,
        fileSortAscending: false,
        browserDirectory: 'photos/2026',
        browserShowHidden: true,
        terminalFontSize: 17,
        reverseWheel: true,
        swapButtons: true,
        keyboardMode: 'local',
        autoRecord: true,
        recordKeep: 10,
        showMyCursor: true,
      ),
    );
    final ViewPreferences prefs = ViewPreferences.load();
    expect(prefs.fit, 1);
    expect(prefs.stats, isTrue);
    expect(prefs.fileSort, 2);
    expect(prefs.fileSortAscending, isFalse);
    expect(prefs.browserDirectory, 'photos/2026');
    expect(prefs.browserShowHidden, isTrue);
    expect(prefs.terminalFontSize, 17);
    expect(prefs.reverseWheel, isTrue);
    expect(prefs.swapButtons, isTrue);
    expect(prefs.keyboardMode, 'local');
    expect(prefs.autoRecord, isTrue);
    expect(prefs.recordKeep, 10);
    expect(prefs.showMyCursor, isTrue);
    expect(readFile(), <String, Object?>{
      'view_fit': 1,
      'view_stats': true,
      'file_sort': 2,
      'file_sort_asc': false,
      'browser_dir': 'photos/2026',
      'browser_show_hidden': true,
      'terminal_font_size': 17,
      'view_reverse_wheel': true,
      'view_swap_buttons': true,
      'view_keyboard_mode': 'local',
      'view_auto_record': true,
      'view_record_keep': 10,
      'view_show_my_cursor': true,
    });
  });

  test('show my cursor follows the device and defaults to off', () {
    expect(ViewPreferences.load().showMyCursor, isFalse);
    ViewPreferences.update(peerId: 'desk', showMyCursor: true);
    expect(ViewPreferences.load(peerId: 'desk').showMyCursor, isTrue);
    expect(ViewPreferences.load(peerId: 'other').showMyCursor, isFalse);
    expect(readFile().containsKey('view_show_my_cursor'), isFalse);
  });

  test('auto record and retention stay shared and reject bad values', () {
    ViewPreferences.update(peerId: 'desk', autoRecord: true, recordKeep: 30);
    final ViewPreferences prefs = ViewPreferences.load(peerId: 'other');
    expect(prefs.autoRecord, isTrue);
    expect(prefs.recordKeep, 30);
    expect(readFile()['view_auto_record'], isTrue);
    expect(readFile()['view_record_keep'], 30);
    ViewPreferences.update(recordKeep: -5);
    expect(ViewPreferences.load().recordKeep, 0);
    ViewPreferences.update(recordKeep: 5000);
    expect(ViewPreferences.load().recordKeep, 0);
    File(path).writeAsStringSync(
      jsonEncode(<String, Object?>{
        'view_auto_record': 'yes',
        'view_record_keep': 3.5,
      }),
    );
    expect(ViewPreferences.load().autoRecord, isFalse);
    expect(ViewPreferences.load().recordKeep, 0);
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
        'view_reverse_wheel': 'yes',
        'view_swap_buttons': 1,
      }),
    );
    final ViewPreferences prefs = ViewPreferences.load();
    expect(prefs.fit, 1);
    expect(prefs.stats, isFalse);
    expect(prefs.fileSort, 0);
    expect(prefs.fileSortAscending, isTrue);
    expect(prefs.browserDirectory, '');
    expect(prefs.terminalFontSize, 13);
    expect(prefs.reverseWheel, isFalse);
    expect(prefs.swapButtons, isFalse);
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
      'browser_show_hidden': false,
      'terminal_font_size': 13,
      'view_reverse_wheel': false,
      'view_swap_buttons': false,
      'view_keyboard_mode': 'physical',
      'view_auto_record': false,
      'view_record_keep': 0,
      'view_show_my_cursor': false,
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

  test('view, mouse and keyboard choices follow the device', () {
    ViewPreferences.save(
      const ViewPreferences(fit: 0, stats: false),
      peerId: '222222222',
    );
    ViewPreferences.save(
      const ViewPreferences(
        fit: 1,
        stats: true,
        fileSort: 2,
        fileSortAscending: false,
        browserDirectory: 'photos',
        terminalFontSize: 17,
        reverseWheel: true,
        swapButtons: true,
        keyboardMode: 'local',
      ),
      peerId: '111111111',
    );
    final ViewPreferences first = ViewPreferences.load(peerId: '111111111');
    expect(first.fit, 1);
    expect(first.stats, isTrue);
    expect(first.reverseWheel, isTrue);
    expect(first.swapButtons, isTrue);
    expect(first.keyboardMode, 'local');
    final ViewPreferences second = ViewPreferences.load(peerId: '222222222');
    expect(second.fit, 0);
    expect(second.stats, isFalse);
    expect(second.keyboardMode, 'physical');
    // Local browser and terminal state stays shared.
    expect(second.browserDirectory, 'photos');
    expect(second.fileSort, 2);
    expect(second.terminalFontSize, 17);
    // A device choice must not become every other host's default.
    expect(readFile().containsKey('view_fit'), isFalse);
  });

  test('device overrides stay independent of each other', () {
    ViewPreferences.save(const ViewPreferences(fit: 1), peerId: '111111111');
    StreamPreferences.save(
      const StreamPreferences(codec: 3),
      peerId: '111111111',
    );
    ViewPreferences.save(
      const ViewPreferences(fit: 1, stats: true),
      peerId: '111111111',
    );
    ViewPreferences.save(const ViewPreferences(fit: 0), peerId: '222222222');
    final ViewPreferences first = ViewPreferences.load(peerId: '111111111');
    expect(first.fit, 1);
    expect(first.stats, isTrue);
    expect(StreamPreferences.load(peerId: '111111111').codec, 3);
    expect(ViewPreferences.load(peerId: '222222222').fit, 0);
    expect(ViewPreferences.load(peerId: '222222222').stats, isFalse);
  });

  test('a device without overrides uses the shared defaults', () {
    ViewPreferences.save(
      const ViewPreferences(fit: 1, stats: true, keyboardMode: 'local'),
    );
    final ViewPreferences fallback = ViewPreferences.load(peerId: '333333333');
    expect(fallback.fit, 1);
    expect(fallback.stats, isTrue);
    expect(fallback.keyboardMode, 'local');
  });
}
