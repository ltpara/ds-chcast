import 'dart:convert';
import 'dart:io';

import 'server_settings.dart';

/// A logged-in viewer account tied to one connection service.
///
/// The Windows viewer persists tokens inside a DPAPI envelope (see
/// [AccountSessionStore]); standalone Dart tooling without the native bridge
/// falls back to the legacy plaintext file, protected only by the profile
/// directory ACL.
class AccountSession {
  const AccountSession({
    required this.server,
    required this.username,
    required this.token,
    required this.expiresAt,
    this.refreshToken = '',
    this.refreshExpiresAt,
    this.syncedAt,
    this.bookRevision = 0,
  });

  final String server;
  final String username;
  final String token;
  final DateTime expiresAt;

  /// Renews [token] without a password until [refreshExpiresAt]. Empty on
  /// sessions created by servers that only issue long-lived access tokens.
  final String refreshToken;
  final DateTime? refreshExpiresAt;
  final DateTime? syncedAt;

  /// Newest server address-book revision applied on this device. The next
  /// sync sends it as `since` so deletions that happened offline arrive as
  /// tombstones instead of being overwritten by a local re-upload.
  final int bookRevision;

  bool get hasToken => token.isNotEmpty;

  bool isActiveAt(DateTime now) =>
      hasToken && expiresAt.isAfter(now.toUtc());

  bool canRefreshAt(DateTime now) {
    final DateTime? until = refreshExpiresAt;
    return refreshToken.isNotEmpty && until != null && until.isAfter(now.toUtc());
  }

  /// The session is worth keeping while either the access token still works or
  /// a refresh token can renew it; only then is a password login required.
  bool isUsableAt(DateTime now) => isActiveAt(now) || canRefreshAt(now);

  AccountSession copyWith({
    String? token,
    DateTime? expiresAt,
    String? refreshToken,
    DateTime? refreshExpiresAt,
    DateTime? syncedAt,
    int? bookRevision,
  }) {
    return AccountSession(
      server: server,
      username: username,
      token: token ?? this.token,
      expiresAt: expiresAt ?? this.expiresAt,
      refreshToken: refreshToken ?? this.refreshToken,
      refreshExpiresAt: refreshExpiresAt ?? this.refreshExpiresAt,
      syncedAt: syncedAt ?? this.syncedAt,
      bookRevision: bookRevision ?? this.bookRevision,
    );
  }

  Map<String, Object?> toJson() => <String, Object?>{
        'server': server,
        'username': username,
        'token': token,
        'expiresAt': expiresAt.toUtc().millisecondsSinceEpoch,
        if (refreshToken.isNotEmpty) 'refreshToken': refreshToken,
        if (refreshExpiresAt != null)
          'refreshExpiresAt': refreshExpiresAt!.toUtc().millisecondsSinceEpoch,
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
    final String refreshToken =
        value['refreshToken']?.toString().trim() ?? '';
    final Object? rawRefreshExpires = value['refreshExpiresAt'];
    return AccountSession(
      server: server,
      username: username,
      token: token,
      expiresAt: DateTime.fromMillisecondsSinceEpoch(
        rawExpires.toInt(),
        isUtc: true,
      ),
      refreshToken: refreshToken,
      refreshExpiresAt: refreshToken.isNotEmpty &&
              rawRefreshExpires is num &&
              rawRefreshExpires.toInt() > 0
          ? DateTime.fromMillisecondsSinceEpoch(
              rawRefreshExpires.toInt(),
              isUtc: true,
            )
          : null,
      syncedAt: rawSynced is num && rawSynced.toInt() > 0
          ? DateTime.fromMillisecondsSinceEpoch(rawSynced.toInt(), isUtc: true)
          : null,
      bookRevision:
          rawRevision is num && rawRevision.toInt() > 0 ? rawRevision.toInt() : 0,
    );
  }
}

/// Encrypts a session payload before it is written to disk.
typedef SecretProtector = Future<String> Function(String plaintext);

/// Decrypts a payload produced by [SecretProtector].
typedef SecretUnprotector = Future<String> Function(String protectedBase64);

/// Persists one account session in `%APPDATA%\chast\account.json`.
///
/// With a [SecretProtector] the file holds
/// `{"version":1,"protected":"<DPAPI blob>"}`; the Windows viewer passes
/// `ViewerBridge.protectSecret` so the bearer and refresh tokens never touch
/// the disk in the clear. Without one (standalone `dart run` tools) the file
/// keeps the legacy plaintext shape, and a plaintext file is re-encrypted the
/// first time it is loaded with a protector.
///
/// A session is only used when its [AccountSession.server] matches the
/// configured connection service; switching services does not silently reuse
/// a token against a different server.
class AccountSessionStore {
  AccountSessionStore({
    File? file,
    SecretProtector? protectSecret,
    SecretUnprotector? unprotectSecret,
  })  : _injected = file,
        _protector = protectSecret,
        _unprotector = unprotectSecret;

  final File? _injected;
  final SecretProtector? _protector;
  final SecretUnprotector? _unprotector;

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
      final Object? decoded = jsonDecode(await file.readAsString());
      if (decoded is Map && decoded['protected'] is String) {
        final SecretUnprotector? unprotect = _unprotector;
        if (unprotect == null) {
          return null;
        }
        final String plaintext =
            await unprotect(decoded['protected'] as String);
        return AccountSession.fromJson(jsonDecode(plaintext));
      }
      final AccountSession? session = AccountSession.fromJson(decoded);
      if (session != null && _protector != null) {
        // Replace a legacy plaintext token with its protected envelope.
        await save(session);
      }
      return session;
    } catch (_) {
      return null;
    }
  }

  Future<void> save(AccountSession session) async {
    final File? file = _file();
    if (file == null) {
      return;
    }
    final String payload = jsonEncode(session.toJson());
    String text = payload;
    final SecretProtector? protect = _protector;
    if (protect != null) {
      try {
        final String blob = await protect(payload);
        text =
            jsonEncode(<String, Object?>{'version': 1, 'protected': blob});
      } catch (_) {
        // Losing the session costs a login; refusing to persist it costs the
        // same. Keep the legacy shape rather than failing the sign-in.
      }
    }
    try {
      await file.parent.create(recursive: true);
      await file.writeAsString(text);
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
