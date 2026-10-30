import 'dart:convert';
import 'dart:io';

import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/viewer_bridge.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

/// Live probe for the DPAPI envelope behind `account.json`.
///
/// Build and run it in the real Windows runner:
///
///   flutter build windows --release --target=tool/dpapi_envelope_probe.dart
///   build\windows\x64\runner\Release\chast_viewer.exe
///
/// It writes the report to `%CHAST_DPAPI_RESULT%` (default
/// `dpapi-envelope.txt`), exits 0 when every check passed, then the app must
/// be rebuilt with the default target.
Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final StringBuffer report = StringBuffer();
  int failed = 0;
  void check(bool ok, String message) {
    report.writeln('${ok ? 'ok' : 'FAIL'} $message');
    if (!ok) {
      failed++;
    }
  }

  try {
    final String plaintext = jsonEncode(<String, Object?>{
      'server': 'http://127.0.0.1:8443',
      'username': 'alice',
      'token': 'token-value',
      'refreshToken': 'refresh-value',
    });
    final String blob = await ViewerBridge.protectSecret(plaintext);
    check(blob.isNotEmpty, 'protectSecret returns a blob');
    check(!blob.contains('token-value'), 'the blob carries no plaintext token');
    check(
      await ViewerBridge.unprotectSecret(blob) == plaintext,
      'unprotectSecret returns the exact payload',
    );

    final Directory temp =
        await Directory.systemTemp.createTemp('chast-dpapi-');
    try {
      final File file = File('${temp.path}/account.json');
      final AccountSessionStore store = AccountSessionStore(
        file: file,
        protectSecret: ViewerBridge.protectSecret,
        unprotectSecret: ViewerBridge.unprotectSecret,
      );
      await store.save(AccountSession(
        server: 'http://127.0.0.1:8443',
        username: 'alice',
        token: 'token-value',
        expiresAt: DateTime.utc(2027),
        refreshToken: 'refresh-value',
      ));
      final String raw = await file.readAsString();
      final Object? envelope = jsonDecode(raw);
      check(
        envelope is Map &&
            envelope['version'] == 1 &&
            envelope['protected'] is String,
        'save writes the version-1 envelope',
      );
      check(
        !raw.contains('token-value') && !raw.contains('refresh-value'),
        'the file never holds the tokens in the clear',
      );
      final AccountSession? loaded = await store.load();
      check(
        loaded?.username == 'alice' &&
            loaded?.token == 'token-value' &&
            loaded?.refreshToken == 'refresh-value',
        'load restores the session through DPAPI',
      );

      await file.writeAsString(jsonEncode(<String, Object?>{
        'server': 'http://127.0.0.1:8443',
        'username': 'alice',
        'token': 'legacy-value',
        'expiresAt': DateTime.utc(2027).millisecondsSinceEpoch,
      }));
      final AccountSession? legacy = await store.load();
      final String migrated = await file.readAsString();
      check(legacy?.token == 'legacy-value', 'a plaintext session still loads');
      check(
        !migrated.contains('legacy-value') &&
            (jsonDecode(migrated) as Map<String, Object?>)['protected']
                is String,
        'loading rewrote it as an envelope',
      );

      await file.writeAsString(jsonEncode(<String, Object?>{
        'version': 1,
        'protected': base64Encode(<int>[1, 2, 3, 4]),
      }));
      check(
        await store.load() == null,
        'a corrupt or foreign blob has no session',
      );
    } finally {
      await temp.delete(recursive: true);
    }

    try {
      await ViewerBridge.protectSecret('x' * (64 * 1024 + 1));
      check(false, 'the bridge refuses an oversized secret');
    } on PlatformException catch (error) {
      check(error.code == 'bad_args', 'the bridge refuses an oversized secret');
    }
  } catch (error, stack) {
    check(false, 'unexpected failure: $error\n$stack');
  }

  report.writeln(failed == 0
      ? 'dpapi-envelope-probe: all checks passed'
      : 'dpapi-envelope-probe: $failed check(s) failed');
  final String path =
      Platform.environment['CHAST_DPAPI_RESULT'] ?? 'dpapi-envelope.txt';
  await File(path).writeAsString(report.toString());
  exit(failed == 0 ? 0 : 1);
}
