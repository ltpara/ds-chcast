import 'dart:convert';
import 'dart:io';

import 'package:chast_viewer/l10n.dart';
import 'package:chast_viewer/stream_prefs.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  late Directory dir;
  late String path;

  setUp(() {
    dir = Directory.systemTemp.createTempSync('chast_stream_prefs_');
    path = '${dir.path}${Platform.pathSeparator}ui.json';
    StreamPreferences.configPathOverride = path;
    AppLanguage.configPathOverride = path;
  });

  tearDown(() {
    StreamPreferences.configPathOverride = null;
    AppLanguage.configPathOverride = null;
    AppLanguage.current.value = kLanguageChinese;
    dir.deleteSync(recursive: true);
  });

  Map<String, Object?> readFile() =>
      jsonDecode(File(path).readAsStringSync()) as Map<String, Object?>;

  test('a missing file keeps the session defaults', () {
    final StreamPreferences prefs = StreamPreferences.load();
    expect(prefs.codec, 0);
    expect(prefs.quality, 0);
    expect(prefs.maxFps, 0);
  });

  test('saves and loads the choices', () {
    StreamPreferences.save(
        const StreamPreferences(codec: 2, quality: 1, maxFps: 30));
    final StreamPreferences prefs = StreamPreferences.load();
    expect(prefs.codec, 2);
    expect(prefs.quality, 1);
    expect(prefs.maxFps, 30);
    expect(readFile(), <String, Object?>{
      'stream_codec': 2,
      'stream_quality': 1,
      'stream_max_fps': 30,
      'stream_full_color': false,
    });
  });

  test('VP9 full color persists independently of quality', () {
    StreamPreferences.save(const StreamPreferences(codec: 4, fullColor: true));
    expect(StreamPreferences.load().codec, 4);
    expect(StreamPreferences.load().fullColor, isTrue);
  });

  test('each invalid value falls back on its own', () {
    File(path).writeAsStringSync(jsonEncode(<String, Object?>{
      'stream_codec': 2,
      'stream_quality': 'high',
      'stream_max_fps': 45,
    }));
    final StreamPreferences prefs = StreamPreferences.load();
    expect(prefs.codec, 2);
    expect(prefs.quality, 0);
    expect(prefs.maxFps, 0);
  });

  test('a corrupt or wrong-shaped file keeps the defaults', () {
    File(path).writeAsStringSync('not json');
    expect(StreamPreferences.load().codec, 0);
    File(path).writeAsStringSync('[1, 2, 3]');
    expect(StreamPreferences.load().maxFps, 0);
  });

  test('save normalises invalid choices and keeps foreign keys', () {
    File(path).writeAsStringSync(jsonEncode(<String, Object?>{
      'language': 'en',
      'another_key': 7,
    }));
    StreamPreferences.save(
        const StreamPreferences(codec: 9, quality: -1, maxFps: 12));
    expect(readFile(), <String, Object?>{
      'language': 'en',
      'another_key': 7,
      'stream_codec': 0,
      'stream_quality': 0,
      'stream_max_fps': 0,
      'stream_full_color': false,
    });
  });

  test('a language change keeps the picture choices', () {
    StreamPreferences.save(
        const StreamPreferences(codec: 1, quality: 2, maxFps: 60));
    AppLanguage.set(kLanguageEnglish);
    expect(readFile(), <String, Object?>{
      'stream_codec': 1,
      'stream_quality': 2,
      'stream_max_fps': 60,
      'stream_full_color': false,
      'language': 'en',
    });
    expect(StreamPreferences.load().maxFps, 60);
  });

  test('a picture change keeps the language', () {
    AppLanguage.set(kLanguageEnglish);
    StreamPreferences.save(
        const StreamPreferences(codec: 2, quality: 1, maxFps: 15));
    expect(readFile()['language'], 'en');
    AppLanguage.current.value = kLanguageChinese;
    AppLanguage.load();
    expect(AppLanguage.code, kLanguageEnglish);
  });
}
