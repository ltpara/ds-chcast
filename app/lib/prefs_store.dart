import 'dart:convert';
import 'dart:io';

import 'store_lock.dart';

/// Shared `ui.json` storage. Root keys are the defaults every host starts
/// with; [peersKey] holds per-host overrides keyed by the peer ID, the way
/// RustDesk keeps a PeerConfig for each remote device instead of one global
/// picture.
class PrefsStore {
  static const String peersKey = 'peers';
  static const int _maxPeerId = 256;

  /// Redirects the settings file away from the real user profile in tests.
  static String? configPathOverride;

  static Map<String, Object?> read() {
    final File? file = _file();
    if (file == null) {
      return <String, Object?>{};
    }
    return withStoreLockSync(file, () {
      try {
        if (!file.existsSync()) {
          return <String, Object?>{};
        }
        return _decode(file.readAsStringSync());
      } catch (_) {
        // A corrupt settings file must not stop the viewer from starting.
      }
      return <String, Object?>{};
    }, onFileError: () => <String, Object?>{});
  }

  /// Root defaults with [peerId]'s overrides layered on top.
  static Map<String, Object?> readMerged(String peerId) {
    final Map<String, Object?> root = read();
    final Map<String, Object?> peer = peerSection(root, peerId);
    if (peer.isEmpty) {
      return root;
    }
    return <String, Object?>{...root, ...peer};
  }

  /// The override section for [peerId], or an empty map. A malformed section
  /// is ignored the same way a corrupt root file is.
  static Map<String, Object?> peerSection(
    Map<String, Object?> root,
    String peerId,
  ) {
    if (peerId.isEmpty) {
      return <String, Object?>{};
    }
    final Object? peers = root[peersKey];
    if (peers is! Map) {
      return <String, Object?>{};
    }
    return _section(peers, peerId);
  }

  /// Updates root keys, keeping everything else in the file.
  static void update(void Function(Map<String, Object?> root) change) {
    final File? file = _file();
    if (file == null) {
      return;
    }
    withStoreLockSync(file, () {
      final Map<String, Object?> root = _readFile(file);
      change(root);
      _write(file, root);
    }, onFileError: () {});
  }

  /// Updates [peerId]'s section and optional shared panel fields in one lock,
  /// keeping the other peer overrides and unspecified root keys untouched.
  static void updatePeer(
    String peerId,
    void Function(Map<String, Object?> peer) change, {
    Map<String, Object?> shared = const <String, Object?>{},
  }) {
    if (!_validPeerId(peerId)) {
      return;
    }
    update((Map<String, Object?> root) {
      final Map<String, Object?> peers = _peers(root);
      final Map<String, Object?> section = _section(peers, peerId);
      change(section);
      root.addAll(shared);
      peers[peerId] = section;
      root[peersKey] = peers;
    });
  }

  /// ConnectTarget namespaces server IDs and normalizes direct addresses.
  /// The length guard also matches the native child-window argument limit.
  static bool _validPeerId(String peerId) =>
      peerId.isNotEmpty && peerId.length <= _maxPeerId;

  static Map<String, Object?> _peers(Map<String, Object?> root) {
    final Map<String, Object?> peers = <String, Object?>{};
    final Object? raw = root[peersKey];
    if (raw is Map) {
      for (final MapEntry<Object?, Object?> entry in raw.entries) {
        final Object? key = entry.key;
        if (key is String) {
          peers[key] = entry.value;
        }
      }
    }
    return peers;
  }

  static Map<String, Object?> _section(Object? peers, String peerId) {
    if (peers is! Map) {
      return <String, Object?>{};
    }
    final Object? raw = peers[peerId];
    final Map<String, Object?> section = <String, Object?>{};
    if (raw is Map) {
      for (final MapEntry<Object?, Object?> entry in raw.entries) {
        final Object? key = entry.key;
        if (key is String) {
          section[key] = entry.value;
        }
      }
    }
    return section;
  }

  static Map<String, Object?> _readFile(File file) {
    try {
      if (!file.existsSync()) {
        return <String, Object?>{};
      }
      return _decode(file.readAsStringSync());
    } catch (_) {
      return <String, Object?>{};
    }
  }

  static Map<String, Object?> _decode(String text) {
    final Object? decoded = jsonDecode(text);
    if (decoded is Map<String, Object?>) {
      return Map<String, Object?>.of(decoded);
    }
    return <String, Object?>{};
  }

  static void _write(File file, Map<String, Object?> payload) {
    try {
      file.parent.createSync(recursive: true);
      file.writeAsStringSync(jsonEncode(payload));
    } catch (_) {
      // A preference; failing to persist it is not fatal.
    }
  }

  static File? _file() {
    final String? override = configPathOverride;
    if (override != null && override.isNotEmpty) {
      return File(override);
    }
    final String? appData = Platform.environment['APPDATA'];
    if (appData == null || appData.isEmpty) {
      return null;
    }
    return File('$appData\\chast\\ui.json');
  }
}
