import 'dart:convert';
import 'dart:io';

/// Returns the canonical 64 uppercase hex digits of a SHA-256 fingerprint, or
/// an empty string when [text] is not one. Colons, whitespace and lower case
/// are accepted so fingerprints copied from a log or a server banner work.
String normalizeFingerprint(String text) {
  final String hex = text.replaceAll(RegExp(r'[\s:]'), '').toUpperCase();
  return RegExp(r'^[0-9A-F]{64}$').hasMatch(hex) ? hex : '';
}

/// "AB12CD34…" — enough to compare by eye inside a compact overlay.
String shortenFingerprint(String fingerprint) {
  return fingerprint.length <= 16
      ? fingerprint
      : '${fingerprint.substring(0, 8)}…${fingerprint.substring(fingerprint.length - 8)}';
}

/// Remembers the server identity the user has seen, keyed by "host:port".
///
/// The first successful connection stores the fingerprint it learned
/// (trust on first use); every later connection pins it. A server that
/// presents another certificate fails to connect and the session page asks
/// before replacing the stored one.
class TrustStore {
  TrustStore({File? file}) : _injected = file;

  final File? _injected;

  File? _file() {
    if (_injected != null) {
      return _injected;
    }
    final String? appData = Platform.environment['APPDATA'];
    if (appData == null || appData.isEmpty) {
      return null;
    }
    return File('$appData\\chast\\trust.json');
  }

  Future<Map<String, String>> load() async {
    final File? file = _file();
    if (file == null || !await file.exists()) {
      return <String, String>{};
    }
    try {
      final Object? decoded = jsonDecode(await file.readAsString());
      if (decoded is! Map) {
        return <String, String>{};
      }
      final Map<String, String> entries = <String, String>{};
      decoded.forEach((Object? key, Object? value) {
        if (key is! String || value is! String) {
          return;
        }
        final String fingerprint = normalizeFingerprint(value);
        if (key.isNotEmpty && fingerprint.isNotEmpty) {
          entries[key] = fingerprint;
        }
      });
      return entries;
    } catch (_) {
      return <String, String>{};
    }
  }

  Future<String> fingerprintFor(String target) async {
    return (await load())[target] ?? '';
  }

  /// Stores [fingerprint] for [target]; an invalid fingerprint is ignored.
  /// Returns the canonical value that was stored, or an empty string.
  Future<String> trust(String target, String fingerprint) async {
    final String canonical = normalizeFingerprint(fingerprint);
    if (target.isEmpty || canonical.isEmpty) {
      return '';
    }
    final Map<String, String> entries = await load();
    entries[target] = canonical;
    await _save(entries);
    return canonical;
  }

  Future<void> forget(String target) async {
    final Map<String, String> entries = await load();
    if (entries.remove(target) != null) {
      await _save(entries);
    }
  }

  Future<void> _save(Map<String, String> entries) async {
    final File? file = _file();
    if (file == null) {
      return;
    }
    try {
      await file.parent.create(recursive: true);
      await file.writeAsString(jsonEncode(entries));
    } catch (_) {
      // Losing the pin only costs a future confirmation; it must not block
      // the session.
    }
  }
}
