import 'server_settings.dart';

const String viewerTunnelModeKey = 'viewer.tunnel_mode';
const String viewerUseWebSocketKey = 'viewer.use_websocket';
const String viewerUploadRecordingsKey = 'viewer.upload_recordings';
const String viewerSyncInitialClipboardKey = 'viewer.sync_initial_clipboard';
const String viewerViewFitKey = 'viewer.view_fit';
const String viewerDefaultViewOnlyKey = 'viewer.default_view_only';

class ViewerStrategySetting {
  const ViewerStrategySetting({required this.mode, required this.value});

  final String mode;
  final Object value;

  bool get locked => mode == 'locked';

  static ViewerStrategySetting? fromJson(String key, Object? raw) {
    if (raw is! Map) {
      return null;
    }
    final String mode = raw['mode']?.toString().trim() ?? '';
    final Object? value = raw['value'];
    if ((mode != 'locked' && mode != 'default') || value == null) {
      return null;
    }
    switch (key) {
      case viewerUseWebSocketKey:
      case viewerUploadRecordingsKey:
      case viewerSyncInitialClipboardKey:
      case viewerDefaultViewOnlyKey:
        if (value is! bool) {
          return null;
        }
        break;
      case viewerViewFitKey:
        if (value is! int || value < 0 || value > 1) {
          return null;
        }
        break;
      case viewerTunnelModeKey:
        if (value is! String ||
            (value != 'auto' && value != 'direct' && value != 'relay')) {
          return null;
        }
        break;
      default:
        // Unknown keys remain visible so an older client can report
        // `unsupported` instead of silently claiming to have applied them.
        break;
    }
    return ViewerStrategySetting(mode: mode, value: value);
  }
}

class ViewerStrategySnapshot {
  const ViewerStrategySnapshot({
    required this.peerId,
    required this.target,
    required this.revision,
    required this.digest,
    required this.settings,
  });

  final String peerId;
  final String target;
  final int revision;
  final String digest;
  final Map<String, ViewerStrategySetting> settings;

  static ViewerStrategySnapshot? fromJson(
    Object? value, {
    required String expectedPeer,
  }) {
    if (value is! Map) {
      return null;
    }
    final String peerId = value['peer_id']?.toString().trim() ?? '';
    final String target = value['target']?.toString().trim() ?? '';
    final Object? rawRevision = value['revision'];
    final String digest = value['digest']?.toString().trim() ?? '';
    final Object? rawSettings = value['settings'];
    if (peerId != expectedPeer ||
        target != 'viewer' ||
        rawRevision is! num ||
        rawRevision.toInt() <= 0 ||
        !RegExp(r'^[0-9a-f]{64}$').hasMatch(digest) ||
        rawSettings is! Map) {
      return null;
    }
    final Map<String, ViewerStrategySetting> settings =
        <String, ViewerStrategySetting>{};
    for (final MapEntry<Object?, Object?> entry in rawSettings.entries) {
      if (entry.key is! String) return null;
      final String key = (entry.key as String).trim();
      final ViewerStrategySetting? setting = ViewerStrategySetting.fromJson(
        key,
        entry.value,
      );
      if (key.isEmpty || setting == null || settings.containsKey(key)) {
        return null;
      }
      settings[key] = setting;
    }
    return ViewerStrategySnapshot(
      peerId: peerId,
      target: target,
      revision: rawRevision.toInt(),
      digest: digest,
      settings: Map<String, ViewerStrategySetting>.unmodifiable(settings),
    );
  }
}

class ViewerStrategyDelivery {
  const ViewerStrategyDelivery({
    required this.context,
    required this.expiresAtMs,
    required this.clientPublicKey,
    required this.snapshot,
  });

  final String context;
  final int expiresAtMs;
  final String clientPublicKey;
  final ViewerStrategySnapshot snapshot;

  bool isActiveAt(DateTime now) =>
      context.isNotEmpty &&
      clientPublicKey.isNotEmpty &&
      expiresAtMs > now.toUtc().millisecondsSinceEpoch;

  static ViewerStrategyDelivery? fromJson(
    Object? value, {
    required String expectedPeer,
    required String clientPublicKey,
  }) {
    if (value is! Map) {
      return null;
    }
    final String context = value['context']?.toString().trim() ?? '';
    final Object? rawExpires = value['expires_at_ms'];
    final ViewerStrategySnapshot? snapshot = ViewerStrategySnapshot.fromJson(
      value['snapshot'],
      expectedPeer: expectedPeer,
    );
    if (context.isEmpty ||
        context.length > 128 ||
        rawExpires is! num ||
        rawExpires.toInt() <= 0 ||
        snapshot == null ||
        clientPublicKey.trim().isEmpty) {
      return null;
    }
    return ViewerStrategyDelivery(
      context: context,
      expiresAtMs: rawExpires.toInt(),
      clientPublicKey: clientPublicKey.trim(),
      snapshot: snapshot,
    );
  }
}

class ViewerLocalSettings {
  const ViewerLocalSettings({
    required this.tunnelMode,
    required this.useWebSocket,
    required this.uploadRecordings,
    required this.syncInitialClipboard,
    required this.viewFit,
    required this.defaultViewOnly,
    this.tunnelModeSet = false,
    this.useWebSocketSet = false,
    this.uploadRecordingsSet = false,
    this.syncInitialClipboardSet = false,
    this.viewFitSet = false,
    this.defaultViewOnlySet = false,
  });

  final TunnelMode tunnelMode;
  final bool useWebSocket;
  final bool uploadRecordings;
  final bool syncInitialClipboard;
  final int viewFit;
  final bool defaultViewOnly;

  final bool tunnelModeSet;
  final bool useWebSocketSet;
  final bool uploadRecordingsSet;
  final bool syncInitialClipboardSet;
  final bool viewFitSet;
  final bool defaultViewOnlySet;

  bool isSet(String key) => switch (key) {
    viewerTunnelModeKey => tunnelModeSet,
    viewerUseWebSocketKey => useWebSocketSet,
    viewerUploadRecordingsKey => uploadRecordingsSet,
    viewerSyncInitialClipboardKey => syncInitialClipboardSet,
    viewerViewFitKey => viewFitSet,
    viewerDefaultViewOnlyKey => defaultViewOnlySet,
    _ => false,
  };
}

class ViewerEffectiveSettings {
  const ViewerEffectiveSettings({
    required this.tunnelMode,
    required this.useWebSocket,
    required this.uploadRecordings,
    required this.syncInitialClipboard,
    required this.viewFit,
    required this.defaultViewOnly,
    this.unsupportedKeys = const <String>[],
  });

  final TunnelMode tunnelMode;
  final bool useWebSocket;
  final bool uploadRecordings;
  final bool syncInitialClipboard;
  final int viewFit;
  final bool defaultViewOnly;
  final List<String> unsupportedKeys;

  static ViewerEffectiveSettings resolve(
    ViewerLocalSettings local, {
    ViewerStrategyDelivery? delivery,
  }) {
    final Map<String, ViewerStrategySetting> settings =
        delivery?.snapshot.settings ?? const <String, ViewerStrategySetting>{};
    final List<String> unsupported = <String>[
      for (final String key in settings.keys)
        if (!const <String>{
          viewerTunnelModeKey,
          viewerUseWebSocketKey,
          viewerUploadRecordingsKey,
          viewerSyncInitialClipboardKey,
          viewerViewFitKey,
          viewerDefaultViewOnlyKey,
        }.contains(key))
          key,
    ]..sort();

    ViewerStrategySetting? overridden(String key) {
      final ViewerStrategySetting? setting = settings[key];
      if (setting == null) return null;
      if (setting.locked || !local.isSet(key)) return setting;
      return null;
    }

    final ViewerStrategySetting? tunnel = overridden(viewerTunnelModeKey);
    final ViewerStrategySetting? websocket = overridden(viewerUseWebSocketKey);
    final ViewerStrategySetting? upload = overridden(viewerUploadRecordingsKey);
    final ViewerStrategySetting? clipboard = overridden(
      viewerSyncInitialClipboardKey,
    );
    final ViewerStrategySetting? fit = overridden(viewerViewFitKey);
    final ViewerStrategySetting? viewOnly = overridden(
      viewerDefaultViewOnlyKey,
    );
    return ViewerEffectiveSettings(
      tunnelMode: tunnel == null
          ? local.tunnelMode
          : TunnelModeName.parse(tunnel.value),
      useWebSocket: websocket == null
          ? local.useWebSocket
          : websocket.value as bool,
      uploadRecordings: upload == null
          ? local.uploadRecordings
          : upload.value as bool,
      syncInitialClipboard: clipboard == null
          ? local.syncInitialClipboard
          : clipboard.value as bool,
      viewFit: fit == null ? local.viewFit : fit.value as int,
      defaultViewOnly: viewOnly == null
          ? local.defaultViewOnly
          : viewOnly.value as bool,
      unsupportedKeys: List<String>.unmodifiable(unsupported),
    );
  }
}

/// Resolves initial control once per connection. A requested control start
/// waits for the first desktop frame and the host input gate; later permission
/// changes cannot turn control back on.
class ViewerInitialControlPolicy {
  bool _resolved = false;

  void reset() => _resolved = false;

  bool? resolve({
    required bool desktop,
    required bool sessionReady,
    required bool acceptsInput,
    required bool defaultViewOnly,
  }) {
    if (_resolved || !desktop || !sessionReady) return null;
    if (defaultViewOnly) {
      _resolved = true;
      return false;
    }
    if (!acceptsInput) return null;
    _resolved = true;
    return true;
  }
}

Map<String, Object?> viewerStrategyReceipt({
  required ViewerStrategySnapshot snapshot,
  required int sequence,
}) {
  final Map<String, Object?> results = <String, Object?>{};
  const Set<String> supportedKeys = <String>{
    viewerTunnelModeKey,
    viewerUseWebSocketKey,
    viewerUploadRecordingsKey,
    viewerSyncInitialClipboardKey,
    viewerViewFitKey,
    viewerDefaultViewOnlyKey,
  };
  for (final String key in snapshot.settings.keys) {
    results[key] = <String, String>{
      'status': supportedKeys.contains(key) ? 'applied' : 'unsupported',
    };
  }
  final bool unsupported = results.values.any(
    (Object? value) => value is Map && value['status'] == 'unsupported',
  );
  return <String, Object?>{
    'revision': snapshot.revision,
    'digest': snapshot.digest,
    'sequence': sequence,
    'status': unsupported ? 'unsupported' : 'applied',
    'results': results,
  };
}
