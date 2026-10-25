import 'dart:convert';
import 'dart:io';

import 'server_settings.dart';

/// A logged-in viewer account tied to one connection service.
///
/// The token is stored in plaintext under the user profile, like the server
/// key in `server.json`; the app relies on the profile directory ACL rather
/// than a platform keystore.
class AccountSession {
  const AccountSession({
    required this.server,
    required this.username,
    required this.token,
    required this.expiresAt,
    this.syncedAt,
    this.bookRevision = 0,
  });

  final String server;
  final String username;
  final String token;
  final DateTime expiresAt;
  final DateTime? syncedAt;

  /// Newest server address-book revision applied on this device. The next
  /// sync sends it as `since` so deletions that happened offline arrive as
  /// tombstones instead of being overwritten by a local re-upload.
  final int bookRevision;

  bool get hasToken => token.isNotEmpty;

  bool isActiveAt(DateTime now) =>
      hasToken && expiresAt.isAfter(now.toUtc());

  AccountSession copyWith({DateTime? syncedAt, int? bookRevision}) {
    return AccountSession(
      server: server,
      username: username,
      token: token,
      expiresAt: expiresAt,
      syncedAt: syncedAt ?? this.syncedAt,
      bookRevision: bookRevision ?? this.bookRevision,
    );
  }

  Map<String, Object?> toJson() => <String, Object?>{
        'server': server,
        'username': username,
        'token': token,
        'expiresAt': expiresAt.toUtc().millisecondsSinceEpoch,
        'syncedAt': syncedAt?.toUtc().millisecondsSinceEpoch,
        'bookRevision': bookRevision,
      };

  static AccountSession? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final String server =
        normalizeServerUrl(value['server']?.toString() ?? '');
    final String username = value['username']?.toString().trim() ?? '';
    final String token = value['token']?.toString().trim() ?? '';
    final Object? rawExpires = value['expiresAt'];
    final Object? rawSynced = value['syncedAt'];
    final Object? rawRevision = value['bookRevision'];
    if (server.isEmpty ||
        username.isEmpty ||
        token.isEmpty ||
        rawExpires is! num ||
        rawExpires.toInt() <= 0) {
      return null;
    }
    return AccountSession(
      server: server,
      username: username,
      token: token,
      expiresAt: DateTime.fromMillisecondsSinceEpoch(
        rawExpires.toInt(),
        isUtc: true,
      ),
      syncedAt: rawSynced is num && rawSynced.toInt() > 0
          ? DateTime.fromMillisecondsSinceEpoch(rawSynced.toInt(), isUtc: true)
          : null,
      bookRevision:
          rawRevision is num && rawRevision.toInt() > 0 ? rawRevision.toInt() : 0,
    );
  }
}

/// Persists one account session in `%APPDATA%\chast\account.json`.
///
/// A session is only used when its [AccountSession.server] matches the
/// configured connection service; switching services does not silently reuse
/// a token against a different server.
class AccountSessionStore {
  AccountSessionStore({File? file}) : _injected = file;

  final File? _injected;

  File? _file() {
    if (_injected != null) {
      return _injected;
    }
    final String? appData = Platform.environment['APPDATA'];
    if (appData == null || appData.isEmpty) {
      return null;
    }
    return File('$appData\\chast\\account.json');
  }

  Future<AccountSession?> load() async {
    final File? file = _file();
    if (file == null || !await file.exists()) {
      return null;
    }
    try {
      return AccountSession.fromJson(jsonDecode(await file.readAsString()));
    } catch (_) {
      return null;
    }
  }

  Future<void> save(AccountSession session) async {
    final File? file = _file();
    if (file == null) {
      return;
    }
    try {
      await file.parent.create(recursive: true);
      await file.writeAsString(jsonEncode(session.toJson()));
    } catch (_) {
      // A lost session only costs one login; never block the app on it.
    }
  }

  Future<void> clear() async {
    final File? file = _file();
    if (file == null) {
      return;
    }
    try {
      if (await file.exists()) {
        await file.delete();
      }
    } catch (_) {
      // The next load tolerates a malformed or stale file.
    }
  }
}
