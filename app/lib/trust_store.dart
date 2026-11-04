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
  /// Returns the canonical value once it is on disk, or an empty string when
  /// the store could not be written; callers must not treat the value as
  /// pinned in that case.
  Future<String> trust(String target, String fingerprint) async {
    final String canonical = normalizeFingerprint(fingerprint);
    if (target.isEmpty || canonical.isEmpty) {
      return '';
    }
    final Map<String, String> entries = await load();
    entries[target] = canonical;
    return await _save(entries) ? canonical : '';
  }

  Future<void> forget(String target) async {
    final Map<String, String> entries = await load();
    if (entries.remove(target) != null) {
      await _save(entries);
    }
  }

  /// Writes through a temp file and an atomic rename so a crash or a full disk
  /// cannot leave a truncated pin list. Returns false when nothing could be
  /// persisted.
  Future<bool> _save(Map<String, String> entries) async {
    final File? file = _file();
    if (file == null) {
      return false;
    }
    final File temp = File('${file.path}.$pid.tmp');
    try {
      await file.parent.create(recursive: true);
      if (await file.exists() && !await _isJsonMap(file)) {
        // Keep an unreadable file under a side name instead of destroying the
        // only copy of the pins it may still hold.
        await file.rename(
          '${file.path}.corrupt-${DateTime.now().millisecondsSinceEpoch}',
        );
      }
      await temp.writeAsString(jsonEncode(entries), flush: true);
      await temp.rename(file.path);
      return true;
    } catch (_) {
      try {
        if (await temp.exists()) {
          await temp.delete();
        }
      } catch (_) {
        // A leftover temp file only wastes space; the next save replaces it.
      }
      return false;
    }
  }

  Future<bool> _isJsonMap(File file) async {
    try {
      return jsonDecode(await file.readAsString()) is Map;
    } catch (_) {
      return false;
    }
  }
}
