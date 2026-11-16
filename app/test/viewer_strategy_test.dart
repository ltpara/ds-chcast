import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/server_settings.dart';
import 'package:chast_viewer/viewer_strategy.dart';

const String _digest =
    '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef';

ViewerStrategyDelivery _snapshot(Map<String, ViewerStrategySetting> settings) {
  return ViewerStrategyDelivery(
    context: 'context-1',
    expiresAtMs: DateTime.utc(2099).millisecondsSinceEpoch,
    clientPublicKey: 'client-key',
    snapshot: ViewerStrategySnapshot(
      peerId: 'devbox',
      target: 'viewer',
      revision: 7,
      digest: _digest,
      settings: settings,
    ),
  );
}

ViewerLocalSettings _local({
  TunnelMode tunnelMode = TunnelMode.auto,
  bool useWebSocket = false,
  bool uploadRecordings = false,
  bool syncInitialClipboard = false,
  int viewFit = 0,
  bool configured = false,
}) {
  return ViewerLocalSettings(
    tunnelMode: tunnelMode,
    useWebSocket: useWebSocket,
    uploadRecordings: uploadRecordings,
    syncInitialClipboard: syncInitialClipboard,
    viewFit: viewFit,
    tunnelModeSet: configured,
    useWebSocketSet: configured,
    uploadRecordingsSet: configured,
    syncInitialClipboardSet: configured,
    viewFitSet: configured,
  );
}

void main() {
  final Map<String, ViewerStrategySetting> locked =
      <String, ViewerStrategySetting>{
        viewerTunnelModeKey: const ViewerStrategySetting(
          mode: 'locked',
          value: 'relay',
        ),
        viewerUseWebSocketKey: const ViewerStrategySetting(
          mode: 'locked',
          value: true,
        ),
        viewerUploadRecordingsKey: const ViewerStrategySetting(
          mode: 'locked',
          value: true,
        ),
        viewerSyncInitialClipboardKey: const ViewerStrategySetting(
          mode: 'locked',
          value: true,
        ),
        viewerViewFitKey: const ViewerStrategySetting(mode: 'locked', value: 1),
      };

  test('locked values override explicit local viewer preferences', () {
    final ViewerEffectiveSettings effective = ViewerEffectiveSettings.resolve(
      _local(configured: true),
      delivery: _snapshot(locked),
    );
    expect(effective.tunnelMode, TunnelMode.relay);
    expect(effective.useWebSocket, isTrue);
    expect(effective.uploadRecordings, isTrue);
    expect(effective.syncInitialClipboard, isTrue);
    expect(effective.viewFit, 1);
  });

  test('default values apply only without an explicit local preference', () {
    final Map<String, ViewerStrategySetting> defaults =
        <String, ViewerStrategySetting>{
          for (final MapEntry<String, ViewerStrategySetting> entry
              in locked.entries)
            entry.key: ViewerStrategySetting(
              mode: 'default',
              value: entry.value.value,
            ),
        };
    final ViewerEffectiveSettings unconfigured =
        ViewerEffectiveSettings.resolve(
          _local(),
          delivery: _snapshot(defaults),
        );
    expect(unconfigured.tunnelMode, TunnelMode.relay);
    expect(unconfigured.useWebSocket, isTrue);
    expect(unconfigured.uploadRecordings, isTrue);
    expect(unconfigured.syncInitialClipboard, isTrue);
    expect(unconfigured.viewFit, 1);

    final ViewerEffectiveSettings explicit = ViewerEffectiveSettings.resolve(
      _local(configured: true),
      delivery: _snapshot(defaults),
    );
    expect(explicit.tunnelMode, TunnelMode.auto);
    expect(explicit.useWebSocket, isFalse);
    expect(explicit.uploadRecordings, isFalse);
    expect(explicit.syncInitialClipboard, isFalse);
    expect(explicit.viewFit, 0);
  });

  test('missing viewer keys preserve local values', () {
    final ViewerEffectiveSettings effective = ViewerEffectiveSettings.resolve(
      _local(
        tunnelMode: TunnelMode.direct,
        useWebSocket: true,
        uploadRecordings: true,
        syncInitialClipboard: true,
        viewFit: 1,
      ),
      delivery: _snapshot(const <String, ViewerStrategySetting>{}),
    );
    expect(effective.tunnelMode, TunnelMode.direct);
    expect(effective.useWebSocket, isTrue);
    expect(effective.uploadRecordings, isTrue);
    expect(effective.syncInitialClipboard, isTrue);
    expect(effective.viewFit, 1);
  });

  test('each supported key validates its type and range', () {
    for (final MapEntry<String, Object> entry in <String, Object>{
      viewerTunnelModeKey: 'socket',
      viewerUseWebSocketKey: 1,
      viewerUploadRecordingsKey: 'true',
      viewerSyncInitialClipboardKey: 0,
      viewerViewFitKey: 2,
    }.entries) {
      expect(
        ViewerStrategySetting.fromJson(entry.key, <String, Object?>{
          'mode': 'locked',
          'value': entry.value,
        }),
        isNull,
        reason: entry.key,
      );
    }
  });

  test('snapshot rejects a peer mismatch and duplicate parsed keys', () {
    expect(
      ViewerStrategySnapshot.fromJson(<String, Object?>{
        'peer_id': 'other',
        'target': 'viewer',
        'revision': 1,
        'digest': _digest,
        'settings': <String, Object?>{},
      }, expectedPeer: 'devbox'),
      isNull,
    );
    expect(
      ViewerStrategySnapshot.fromJson(<String, Object?>{
        'peer_id': 'devbox',
        'target': 'viewer',
        'revision': 1,
        'digest': _digest,
        'settings': <String, Object?>{
          ' viewer.view_fit ': <String, Object?>{'mode': 'locked', 'value': 1},
          'viewer.view_fit': <String, Object?>{'mode': 'default', 'value': 0},
        },
      }, expectedPeer: 'devbox'),
      isNull,
    );
  });

  test('unknown keys are retained and reported unsupported', () {
    final ViewerStrategyDelivery delivery = _snapshot(
      <String, ViewerStrategySetting>{
        viewerViewFitKey: const ViewerStrategySetting(mode: 'locked', value: 1),
        'viewer.future_setting': const ViewerStrategySetting(
          mode: 'default',
          value: 'new',
        ),
      },
    );
    expect(
      ViewerEffectiveSettings.resolve(
        _local(),
        delivery: delivery,
      ).unsupportedKeys,
      <String>['viewer.future_setting'],
    );
    expect(
      viewerStrategyReceipt(snapshot: delivery.snapshot, sequence: 1),
      <String, Object?>{
        'revision': 7,
        'digest': _digest,
        'sequence': 1,
        'status': 'unsupported',
        'results': <String, Object?>{
          viewerViewFitKey: <String, String>{'status': 'applied'},
          'viewer.future_setting': <String, String>{'status': 'unsupported'},
        },
      },
    );
  });

  test('delivery rejects an expired or missing client key', () {
    final Map<String, Object?> json = <String, Object?>{
      'context': 'context-1',
      'expires_at_ms': DateTime.now()
          .toUtc()
          .add(const Duration(minutes: 1))
          .millisecondsSinceEpoch,
      'snapshot': <String, Object?>{
        'peer_id': 'devbox',
        'target': 'viewer',
        'revision': 1,
        'digest': _digest,
        'settings': <String, Object?>{},
      },
    };
    expect(
      ViewerStrategyDelivery.fromJson(
        json,
        expectedPeer: 'devbox',
        clientPublicKey: '',
      ),
      isNull,
    );
    final ViewerStrategyDelivery? delivery = ViewerStrategyDelivery.fromJson(
      json,
      expectedPeer: 'devbox',
      clientPublicKey: 'client-key',
    );
    expect(delivery, isNotNull);
    expect(delivery!.isActiveAt(DateTime.now()), isTrue);
  });
}
