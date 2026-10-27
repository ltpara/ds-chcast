import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';

import 'server_settings.dart';

/// The rendezvous server's long-term Ed25519 public key.
///
/// A host lookup is only as trustworthy as the server that answers it, so the
/// viewer pins this key per server URL. [fingerprint] is the SHA-256 of the 32
/// key bytes, uppercase hex — the same form `--server-pubkey` users compare.
class ServerIdentity {
  const ServerIdentity({required this.publicKey, required this.fingerprint});

  final String publicKey;
  final String fingerprint;

  /// Parses the base64url key text returned by `/v1/server`. Padded input is
  /// accepted and canonicalised, so a key copied from a config file works.
  static ServerIdentity? parse(String text) {
    final String key = text.trim();
    if (key.isEmpty) {
      return null;
    }
    try {
      final List<int> raw = base64Url.decode(base64Url.normalize(key));
      if (raw.length != 32) {
        return null;
      }
      final String canonical = base64Url.encode(raw).replaceAll('=', '');
      return ServerIdentity(
        publicKey: canonical,
        fingerprint: sha256.convert(raw).toString().toUpperCase(),
      );
    } on FormatException {
      return null;
    }
  }
}

/// Groups a hex fingerprint for display: "AB12CD34 EF56…". Short or invalid
/// input is returned unchanged so a stored value is never hidden.
String formatServerFingerprint(String fingerprint) {
  final String hex = fingerprint.trim().toUpperCase();
  if (hex.length != 64) {
    return fingerprint;
  }
  final List<String> chunks = <String>[];
  for (int i = 0; i < hex.length; i += 8) {
    chunks.add(hex.substring(i, i + 8));
  }
  return chunks.join(' ');
}

String shortenServerFingerprint(String fingerprint) {
  final String hex = fingerprint.trim().toUpperCase();
  return hex.length <= 20
      ? hex
      : '${hex.substring(0, 8)}…${hex.substring(hex.length - 8)}';
}

class ServerIdentityPin {
  const ServerIdentityPin({required this.identity, required this.pinnedAt});

  final ServerIdentity identity;
  final DateTime pinnedAt;

  Map<String, Object?> toJson() => <String, Object?>{
        'public_key': identity.publicKey,
        'fingerprint': identity.fingerprint,
        'pinned_at_ms': pinnedAt.toUtc().millisecondsSinceEpoch,
      };

  static ServerIdentityPin? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final ServerIdentity? identity =
        ServerIdentity.parse(value['public_key']?.toString() ?? '');
    if (identity == null) {
      return null;
    }
    final Object? rawAt = value['pinned_at_ms'];
    return ServerIdentityPin(
      identity: identity,
      pinnedAt: DateTime.fromMillisecondsSinceEpoch(
        rawAt is num ? rawAt.toInt() : 0,
        isUtc: true,
      ),
    );
  }
}

/// Remembers the server key the user confirmed, keyed by normalised server
/// URL, and appends every trust decision to a local JSON-lines audit.
///
/// The first explicit connection stores the key it saw (trust on first use);
/// a later connection whose key differs is refused until the user confirms the
/// replacement. Rotations are rare, so the audit is append-only rather than
/// capped.
class ServerIdentityStore {
  ServerIdentityStore({File? file, File? auditFile})
      : _injected = file,
        _auditInjected = auditFile;

  final File? _injected;
  final File? _auditInjected;

  File? _file() {
    if (_injected != null) {
      return _injected;
    }
    final String? appData = Platform.environment['APPDATA'];
    if (appData == null || appData.isEmpty) {
      return null;
    }
    return File('$appData\\chast\\server_identities.json');
  }

  File? _auditFile() {
    if (_auditInjected != null) {
      return _auditInjected;
    }
    final String? appData = Platform.environment['APPDATA'];
    if (appData == null || appData.isEmpty) {
      return null;
    }
    return File('$appData\\chast\\server_identity_audit.jsonl');
  }

  Future<Map<String, ServerIdentityPin>> load() async {
    final File? file = _file();
    if (file == null || !await file.exists()) {
      return <String, ServerIdentityPin>{};
    }
    try {
      final Object? decoded = jsonDecode(await file.readAsString());
      if (decoded is! Map) {
        return <String, ServerIdentityPin>{};
      }
      final Map<String, ServerIdentityPin> pins = <String, ServerIdentityPin>{};
      decoded.forEach((Object? key, Object? value) {
        if (key is! String) {
          return;
        }
        final String url = normalizeServerUrl(key);
        final ServerIdentityPin? pin = ServerIdentityPin.fromJson(value);
        if (url.isNotEmpty && pin != null) {
          pins[url] = pin;
        }
      });
      return pins;
    } catch (_) {
      return <String, ServerIdentityPin>{};
    }
  }

  Future<ServerIdentityPin?> pinFor(String serverUrl) async {
    final String url = normalizeServerUrl(serverUrl);
    if (url.isEmpty) {
      return null;
    }
    return (await load())[url];
  }

  /// Stores [identity] for [serverUrl]. [previous] is the pin being replaced,
  /// if any; it only feeds the audit record and the confirmation wording.
  Future<ServerIdentityPin> pin(
    String serverUrl,
    ServerIdentity identity, {
    ServerIdentityPin? previous,
  }) async {
    final String url = normalizeServerUrl(serverUrl);
    if (url.isEmpty) {
      return ServerIdentityPin(identity: identity, pinnedAt: DateTime.now());
    }
    final ServerIdentityPin record = ServerIdentityPin(
      identity: identity,
      pinnedAt: DateTime.now().toUtc(),
    );
    final Map<String, ServerIdentityPin> pins = await load();
    pins[url] = record;
    await _save(pins);
    await _audit(
      server: url,
      action: previous == null ? 'tofu' : 'rotate',
      identity: identity,
      previousFingerprint: previous?.identity.fingerprint ?? '',
    );
    return record;
  }

  /// Removes the pin so the next connection asks again unconditionally.
  Future<void> forget(String serverUrl) async {
    final String url = normalizeServerUrl(serverUrl);
    if (url.isEmpty) {
      return;
    }
    final Map<String, ServerIdentityPin> pins = await load();
    final ServerIdentityPin? previous = pins.remove(url);
    if (previous == null) {
      return;
    }
    await _save(pins);
    await _audit(
      server: url,
      action: 'forget',
      previousFingerprint: previous.identity.fingerprint,
    );
  }

  Future<void> _save(Map<String, ServerIdentityPin> pins) async {
    final File? file = _file();
    if (file == null) {
      return;
    }
    try {
      await file.parent.create(recursive: true);
      await file.writeAsString(jsonEncode(<String, Object?>{
        for (final MapEntry<String, ServerIdentityPin> entry in pins.entries)
          entry.key: entry.value.toJson(),
      }));
    } catch (_) {
      // Losing the pin only costs a future confirmation; it must not block
      // the connection.
    }
  }

  Future<void> _audit({
    required String server,
    required String action,
    ServerIdentity? identity,
    String previousFingerprint = '',
  }) async {
    final File? file = _auditFile();
    if (file == null) {
      return;
    }
    final String line = jsonEncode(<String, Object?>{
      'time_ms': DateTime.now().toUtc().millisecondsSinceEpoch,
      'server': server,
      'action': action,
      if (identity != null) 'public_key': identity.publicKey,
      if (identity != null) 'fingerprint': identity.fingerprint,
      if (previousFingerprint.isNotEmpty)
        'previous_fingerprint': previousFingerprint,
    });
    try {
      await file.parent.create(recursive: true);
      await file.writeAsString('$line\n', mode: FileMode.append);
    } catch (_) {
      // Audit delivery is best effort; the pin already protects the session.
    }
  }
}
