import 'dart:io';

import 'package:chast_viewer/l10n.dart';
import 'package:chast_viewer/store_lock.dart';
import 'package:chast_viewer/stream_prefs.dart';
import 'package:chast_viewer/view_filter.dart';
import 'package:chast_viewer/view_prefs.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  late Directory dir;
  late File file;
  const String original =
      '{"language":"en","stream_codec":1,"view_fit":1,"tags":["saved"]}';

  setUp(() {
    dir = Directory.systemTemp.createTempSync('chast-prefs-lock-');
    file = File('${dir.path}/ui.json')..writeAsStringSync(original);
    AppLanguage.configPathOverride = file.path;
    StreamPreferences.configPathOverride = file.path;
    ViewPreferences.configPathOverride = file.path;
    ViewFilter.configPathOverride = file.path;
    AppLanguage.current.value = kLanguageChinese;
  });

  tearDown(() {
    AppLanguage.configPathOverride = null;
    StreamPreferences.configPathOverride = null;
    ViewPreferences.configPathOverride = null;
    ViewFilter.configPathOverride = null;
    AppLanguage.current.value = kLanguageChinese;
    dir.deleteSync(recursive: true);
  });

  void checkFallbackAndSkippedWrites() {
    AppLanguage.load();
    expect(AppLanguage.code, kLanguageChinese);
    expect(StreamPreferences.load().codec, 0);
    expect(ViewPreferences.load().fit, 0);
    expect(ViewFilter.load(), isEmpty);
    AppLanguage.set(kLanguageEnglish);
    StreamPreferences.save(const StreamPreferences(codec: 2));
    ViewPreferences.save(const ViewPreferences(fit: 1));
    ViewFilter.save(<String>['new']);
    expect(file.readAsStringSync(), original);
  }

  test('unopenable lock falls back without writing unlocked', () {
    Directory('${file.path}.lock').createSync();
    checkFallbackAndSkippedWrites();
    bool ran = false;
    expect(
      () => withStoreLockSync(file, () {
        ran = true;
      }),
      throwsA(isA<FileSystemException>()),
    );
    expect(ran, isFalse);
  });

  test('uncreatable preference directory does not block startup', () {
    final String inaccessible = '${file.path}/nested/ui.json';
    AppLanguage.configPathOverride = inaccessible;
    StreamPreferences.configPathOverride = inaccessible;
    ViewPreferences.configPathOverride = inaccessible;
    ViewFilter.configPathOverride = inaccessible;
    checkFallbackAndSkippedWrites();
  });

  test(
    'Windows read-only lock falls back without changing preferences',
    () {
      final File lock = File('${file.path}.lock')..createSync();
      try {
        expect(
          Process.runSync('attrib', <String>['+R', lock.path]).exitCode,
          0,
        );
        checkFallbackAndSkippedWrites();
      } finally {
        expect(
          Process.runSync('attrib', <String>['-R', lock.path]).exitCode,
          0,
        );
      }
    },
    skip: !Platform.isWindows,
  );
}
