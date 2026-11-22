import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/pages/session_page.dart';
import 'package:chast_viewer/recording_upload_queue.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_identity.dart';
import 'package:chast_viewer/server_settings.dart';
import 'package:chast_viewer/stream_prefs.dart';
import 'package:chast_viewer/target.dart';
import 'package:chast_viewer/trust_store.dart';
import 'package:chast_viewer/tunnel_agent.dart';
import 'package:chast_viewer/view_prefs.dart';
import 'package:chast_viewer/viewer_strategy.dart';
import 'package:chast_viewer/widgets/remote_toolbar.dart';
import 'package:chast_viewer/widgets/remote_view.dart';

const String _serverKey = 'AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8';
const String _clientKey = 'A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg';

List<int> _recordingFixtureBytes() => <int>[
  ..._recordingFixtureBox('ftyp', <int>[0x69, 0x73, 0x6f, 0x6d, 0, 0, 0, 0]),
  ..._recordingFixtureBox('mdat', <int>[1, 2, 3]),
  ..._recordingFixtureBox('moov', <int>[4, 5]),
];

List<int> _recordingFixtureBox(String type, List<int> payload) {
  final ByteData size = ByteData(4)..setUint32(0, payload.length + 8);
  return <int>[...size.buffer.asUint8List(), ...type.codeUnits, ...payload];
}

const String _fingerprint =
    '630DCD2966C4336691125448BBB25B4FF412A49C732DB2C8ABC1B8581BD710DD';
const String _digest =
    '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef';

ViewerStrategyDelivery _delivery({
  String context = 'viewer-context',
  TunnelMode tunnelMode = TunnelMode.relay,
  String settingMode = 'locked',
  bool useWebSocket = true,
  bool uploadRecordings = true,
  bool syncInitialClipboard = true,
  int viewFit = 1,
  bool defaultViewOnly = true,
  int revision = 4,
  String digest = _digest,
  bool includeUnknown = true,
  String peerId = 'devbox',
}) {
  return ViewerStrategyDelivery(
    context: context,
    expiresAtMs: DateTime.now()
        .toUtc()
        .add(const Duration(minutes: 5))
        .millisecondsSinceEpoch,
    clientPublicKey: _clientKey,
    snapshot: ViewerStrategySnapshot(
      peerId: peerId,
      target: 'viewer',
      revision: revision,
      digest: digest,
      settings: <String, ViewerStrategySetting>{
        viewerTunnelModeKey: ViewerStrategySetting(
          mode: settingMode,
          value: tunnelMode.name,
        ),
        viewerUseWebSocketKey: ViewerStrategySetting(
          mode: settingMode,
          value: useWebSocket,
        ),
        viewerUploadRecordingsKey: ViewerStrategySetting(
          mode: settingMode,
          value: uploadRecordings,
        ),
        viewerSyncInitialClipboardKey: ViewerStrategySetting(
          mode: settingMode,
          value: syncInitialClipboard,
        ),
        viewerViewFitKey: ViewerStrategySetting(
          mode: settingMode,
          value: viewFit,
        ),
        viewerDefaultViewOnlyKey: ViewerStrategySetting(
          mode: settingMode,
          value: defaultViewOnly,
        ),
        if (includeUnknown)
          'viewer.future_setting': const ViewerStrategySetting(
            mode: 'default',
            value: 'new',
          ),
      },
    ),
  );
}

AccountSession _account({
  String username = 'alice',
  String server = 'http://127.0.0.1:1',
}) {
  return AccountSession(
    server: server,
    username: username,
    token: '$username-token',
    expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
  );
}

HostInfo _hostInfo(
  ViewerStrategyDelivery delivery, {
  String peerId = 'devbox',
}) {
  return HostInfo(
    id: peerId,
    online: true,
    accessTicket: HostAccessTicket(
      ticket: 'signed-ticket',
      expiresAtMs: DateTime.now()
          .add(const Duration(minutes: 1))
          .millisecondsSinceEpoch,
      expiresInMs: 60000,
    ),
    viewerStrategy: delivery,
  );
}

class _FakeTunnel implements TunnelConnection {
  _FakeTunnel(this.mode);

  final TunnelMode mode;
  bool stopped = false;
  final StreamController<TunnelAccessTicket> tickets =
      StreamController<TunnelAccessTicket>.broadcast();

  @override
  String get accessTicket => '';
  @override
  String get carrierHost => '';
  @override
  int get carrierPort => 0;
  @override
  String get carrierToken => '';
  @override
  String get host => '127.0.0.1';
  @override
  String get path => 'direct';
  @override
  String get pathLabel => 'direct';
  @override
  int get port => 9;
  @override
  String get relayHost => '';
  @override
  int get relayPort => 0;
  @override
  bool get running => !stopped;
  @override
  Stream<TunnelAccessTicket> get accessTickets => tickets.stream;

  @override
  Future<void> stop() async {
    stopped = true;
    await tickets.close();
  }
}

class _MemoryIdentity extends ServerIdentityStore {
  _MemoryIdentity(this.pins);

  final Map<String, ServerIdentityPin> pins;

  @override
  Future<Map<String, ServerIdentityPin>> load() async =>
      Map<String, ServerIdentityPin>.of(pins);

  @override
  Future<ServerIdentityPin?> pinFor(String serverUrl) async =>
      pins[normalizeServerUrl(serverUrl)];

  @override
  Future<ServerIdentityPin> pin(
    String serverUrl,
    ServerIdentity identity, {
    ServerIdentityPin? previous,
  }) async {
    final ServerIdentityPin record = ServerIdentityPin(
      identity: identity,
      pinnedAt: DateTime.now(),
    );
    pins[normalizeServerUrl(serverUrl)] = record;
    return record;
  }
}

class _MemoryTrust extends TrustStore {
  _MemoryTrust(this.entries);

  final Map<String, String> entries;

  @override
  Future<Map<String, String>> load() async => Map<String, String>.of(entries);
}

void main() {
  const MethodChannel native = MethodChannel('chast/viewer');
  const MethodChannel gpu = MethodChannel('flutter_gpu_texture_renderer');

  testWidgets(
    'viewer strategy is fixed before routing and reaches the native session',
    (WidgetTester tester) async {
      final Directory dir = Directory.systemTemp.createTempSync(
        'chast-viewer-strategy-',
      );
      const String serverUrl = 'http://127.0.0.1:9';
      StreamPreferences.configPathOverride = '${dir.path}/prefs.json';
      ViewPreferences.configPathOverride = '${dir.path}/prefs.json';
      final RecordingUploadQueueStore recordingUploadQueue =
          RecordingUploadQueueStore(
            file: File('${dir.path}/recording-uploads.json'),
          );
      final ServerIdentity identity = ServerIdentity.parse(_serverKey)!;
      final ServerIdentityStore identities =
          _MemoryIdentity(<String, ServerIdentityPin>{
            normalizeServerUrl(serverUrl): ServerIdentityPin(
              identity: identity,
              pinnedAt: DateTime.now().toUtc(),
            ),
          });
      final ConnectTarget viaServer = ConnectTarget(
        '',
        0,
        server: serverUrl,
        hostId: 'devbox',
        mode: TunnelMode.direct,
      );
      final File recording = File('${dir.path}/recording.mp4')
        ..writeAsBytesSync(_recordingFixtureBytes());
      final AccountSession account = _account(
        server: serverUrl,
      ).copyWith(token: 'alice-token');
      final List<ServerSettings> tunnelSettings = <ServerSettings>[];
      final List<Map<Object?, Object?>> starts = <Map<Object?, Object?>>[];
      final List<String> uploads = <String>[];
      bool recordingLive = true;
      bool inputEnabled = false;
      final Completer<void> receiptSent = Completer<void>();
      final messenger = tester.binding.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(gpu, (MethodCall call) async => 1);
      messenger.setMockMethodCallHandler(native, (MethodCall call) async {
        switch (call.method) {
          case 'ticketIdentity':
            return _clientKey;
          case 'loadPassword':
            return '';
          case 'start':
            starts.add(call.arguments as Map<Object?, Object?>);
            return null;
          case 'status':
            return <String, Object>{
              'state': 'streaming',
              'recording': recordingLive,
              'recordPath': recording.path,
              'width': 640,
              'height': 480,
              'sourceWidth': 640,
              'sourceHeight': 480,
              'published': 1,
              'inputEnabled': inputEnabled,
              'elevationCode': 1,
              'serverFingerprint': _fingerprint,
            };
          case 'setClipboardSync':
            return true;
          case 'takeCompletedFiles':
            return <Object?>[];
          case 'refreshRemoteCursor':
            return false;
          default:
            return null;
        }
      });
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        await _waitForUploadWorkerIdle(tester, recordingUploadQueue);
        messenger.setMockMethodCallHandler(native, null);
        messenger.setMockMethodCallHandler(gpu, null);
        StreamPreferences.configPathOverride = null;
        ViewPreferences.configPathOverride = null;
        dir.deleteSync(recursive: true);
      });

      await tester.pumpWidget(
        MaterialApp(
          home: SessionPage(
            target: viaServer,
            account: account,
            settings: const ServerSettings(
              mode: TunnelMode.direct,
              useWebSocket: false,
              uploadRecordings: false,
            ),
            recordingUploadQueue: recordingUploadQueue,
            identityStore: identities,
            trustStore: _MemoryTrust(<String, String>{
              viaServer.trustKey: _fingerprint,
            }),
            serverIdentityLoader: (ServerSettings settings) async {
              expect(settings.url, serverUrl);
              return identity;
            },
            accountResolver: () async => account,
            hostLookup:
                ({
                  required ServerSettings settings,
                  required String hostId,
                  required String accountToken,
                  required String clientPublicKey,
                }) async {
                  expect(settings.url, serverUrl);
                  expect(hostId, 'devbox');
                  expect(accountToken, 'alice-token');
                  expect(clientPublicKey, _clientKey);
                  return _hostInfo(_delivery(defaultViewOnly: false));
                },
            tunnelStarter:
                ({
                  required ServerSettings server,
                  required String hostId,
                  required String accountToken,
                  required String serverPublicKey,
                  required EmergencyAccessRequest? emergency,
                }) async {
                  expect(serverPublicKey, _serverKey);
                  expect(accountToken, 'alice-token');
                  expect(emergency, isNull);
                  tunnelSettings.add(server);
                  return _FakeTunnel(server.mode);
                },
            viewerStrategyReceiptReporter:
                ({
                  required ServerSettings settings,
                  required String accountToken,
                  required String peerId,
                  required String context,
                  required String clientPublicKey,
                  required Map<String, Object?> receipt,
                }) async {
                  expect(settings.url, serverUrl);
                  expect(accountToken, 'alice-token');
                  expect(peerId, 'devbox');
                  expect(context, 'viewer-context');
                  expect(clientPublicKey, _clientKey);
                  expect(receipt['status'], 'unsupported');
                  final Map<Object?, Object?> results =
                      receipt['results']! as Map<Object?, Object?>;
                  expect(
                    (results['viewer.future_setting']!
                        as Map<Object?, Object?>)['status'],
                    'unsupported',
                  );
                  receiptSent.complete();
                },
            recordingUploader:
                (
                  AccountSession uploadedBy,
                  File file,
                  DateTime startedAt,
                  DateTime endedAt,
                ) async {
                  uploads.add(uploadedBy.token);
                },
          ),
        ),
      );

      await _waitFor(tester, () => starts.isNotEmpty);
      await tester.pump(const Duration(milliseconds: 250));
      expect(tunnelSettings, hasLength(1));
      expect(tunnelSettings.single.mode, TunnelMode.relay);
      expect(starts.single['transport'], 'ws');
      expect(starts.single['syncInitClipboard'], isTrue);
      expect(
        tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).fit,
        ViewFit.original,
      );
      expect(
        tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).controlEnabled,
        isFalse,
      );
      inputEnabled = true;
      await tester.pump(const Duration(milliseconds: 250));
      expect(
        tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).controlEnabled,
        isTrue,
      );
      await tester.tap(find.byTooltip('关闭远程键鼠'));
      await tester.pump();
      expect(
        tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).controlEnabled,
        isFalse,
      );
      inputEnabled = false;
      await tester.pump(const Duration(milliseconds: 250));
      inputEnabled = true;
      await tester.pump(const Duration(milliseconds: 250));
      expect(
        tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).controlEnabled,
        isFalse,
      );
      await receiptSent.future.timeout(const Duration(seconds: 2));

      recordingLive = false;
      await tester.pump(const Duration(milliseconds: 250));
      await _waitFor(tester, () => uploads.isNotEmpty);
      expect(uploads, <String>['alice-token']);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    },
  );

  testWidgets('a late lookup from the previous account is discarded', (
    WidgetTester tester,
  ) async {
    final Directory dir = Directory.systemTemp.createTempSync(
      'chast-viewer-strategy-race-',
    );
    StreamPreferences.configPathOverride = '${dir.path}/prefs.json';
    ViewPreferences.configPathOverride = '${dir.path}/prefs.json';
    final AccountSession first = _account();
    final AccountSession second = _account(username: 'bob');
    int resolves = 0;
    int tunnelStarts = 0;
    bool started = false;
    final messenger = tester.binding.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(native, (MethodCall call) async {
      switch (call.method) {
        case 'ticketIdentity':
          return _clientKey;
        case 'loadPassword':
          return '';
        case 'start':
          started = true;
          return null;
        default:
          return null;
      }
    });
    addTearDown(() {
      messenger.setMockMethodCallHandler(native, null);
      StreamPreferences.configPathOverride = null;
      ViewPreferences.configPathOverride = null;
      dir.deleteSync(recursive: true);
    });

    await tester.pumpWidget(
      MaterialApp(
        home: SessionPage(
          target: const ConnectTarget(
            '',
            0,
            server: 'https://example.test',
            hostId: 'devbox',
          ),
          account: first,
          settings: const ServerSettings(),
          trustStore: TrustStore(file: File('${dir.path}/trust.json')),
          accountResolver: () async {
            resolves++;
            return resolves == 1 ? first : second;
          },
          hostLookup:
              ({
                required ServerSettings settings,
                required String hostId,
                required String accountToken,
                required String clientPublicKey,
              }) async => _hostInfo(_delivery()),
          tunnelStarter:
              ({
                required ServerSettings server,
                required String hostId,
                required String accountToken,
                required String serverPublicKey,
                required EmergencyAccessRequest? emergency,
              }) async {
                tunnelStarts++;
                return _FakeTunnel(server.mode);
              },
        ),
      ),
    );

    await _waitFor(
      tester,
      () => find.textContaining('账号或连接目标已切换').evaluate().isNotEmpty,
    );
    expect(tunnelStarts, 0);
    expect(started, isFalse);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets(
    'a widget identity change stops the old session before using the new strategy',
    (WidgetTester tester) async {
      final Directory dir = Directory.systemTemp.createTempSync(
        'chast-viewer-strategy-widget-',
      );
      StreamPreferences.configPathOverride = '${dir.path}/prefs.json';
      ViewPreferences.configPathOverride = '${dir.path}/prefs.json';
      final AccountSession firstForExample = _account(
        server: 'https://example.test',
      ).copyWith(token: 'alice-token');
      final AccountSession secondForExample = _account(
        username: 'bob',
        server: 'https://example.test',
      );
      final ServerIdentity identity = ServerIdentity.parse(_serverKey)!;
      final ServerIdentityStore identities =
          _MemoryIdentity(<String, ServerIdentityPin>{
            'https://example.test': ServerIdentityPin(
              identity: identity,
              pinnedAt: DateTime.now().toUtc(),
            ),
          });
      final stamps = <String>[];
      final tunnels = <_FakeTunnel>[];
      final messenger = tester.binding.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(gpu, (MethodCall call) async => 1);
      messenger.setMockMethodCallHandler(native, (MethodCall call) async {
        switch (call.method) {
          case 'ticketIdentity':
            return _clientKey;
          case 'loadPassword':
            return '';
          case 'start':
            stamps.add('start');
            return null;
          case 'stop':
            stamps.add('stop');
            return null;
          case 'takeCompletedFiles':
            return <Object?>[];
          case 'refreshRemoteCursor':
            return false;
          case 'status':
            return <String, Object>{
              'state': 'streaming',
              'width': 640,
              'height': 480,
              'sourceWidth': 640,
              'sourceHeight': 480,
              'published': 1,
              'inputEnabled': true,
              'serverFingerprint': _fingerprint,
            };
          default:
            return null;
        }
      });
      addTearDown(() {
        messenger.setMockMethodCallHandler(native, null);
        messenger.setMockMethodCallHandler(gpu, null);
        StreamPreferences.configPathOverride = null;
        ViewPreferences.configPathOverride = null;
        dir.deleteSync(recursive: true);
      });

      Widget page({
        required ConnectTarget target,
        required AccountSession account,
      }) {
        return MaterialApp(
          home: SessionPage(
            key: const ValueKey<String>('session'),
            target: target,
            account: account,
            settings: const ServerSettings(),
            trustStore: TrustStore(file: File('${dir.path}/trust.json')),
            identityStore: identities,
            serverIdentityLoader: (_) async => identity,
            accountResolver: () async => account,
            hostLookup:
                ({
                  required ServerSettings settings,
                  required String hostId,
                  required String accountToken,
                  required String clientPublicKey,
                }) async => _hostInfo(
                  _delivery(
                    context: 'context-$hostId',
                    peerId: hostId,
                    tunnelMode: hostId == 'first'
                        ? TunnelMode.direct
                        : TunnelMode.relay,
                  ),
                  peerId: hostId,
                ),
            tunnelStarter:
                ({
                  required ServerSettings server,
                  required String hostId,
                  required String accountToken,
                  required String serverPublicKey,
                  required EmergencyAccessRequest? emergency,
                }) async {
                  expect(accountToken, account.token);
                  stamps.add('tunnel-$hostId-${server.mode.name}');
                  final tunnel = _FakeTunnel(server.mode);
                  tunnels.add(tunnel);
                  return tunnel;
                },
          ),
        );
      }

      const firstTarget = ConnectTarget(
        '',
        0,
        server: 'https://example.test',
        hostId: 'first',
      );
      const secondTarget = ConnectTarget(
        '',
        0,
        server: 'https://example.test',
        hostId: 'second',
      );
      await tester.pumpWidget(
        page(target: firstTarget, account: firstForExample),
      );
      await _waitFor(tester, () => stamps.contains('start'));
      expect(tunnels, hasLength(1));
      expect(tunnels.single.stopped, isFalse);

      await tester.pumpWidget(
        page(target: secondTarget, account: secondForExample),
      );
      await tester.pump(const Duration(milliseconds: 20));
      await _waitFor(
        tester,
        () =>
            tunnels.length == 2 &&
            tunnels.first.stopped &&
            stamps.where((stamp) => stamp == 'start').length == 2,
      );
      expect(stamps.indexOf('stop'), greaterThan(stamps.indexOf('start')));
      expect(stamps, contains('tunnel-first-direct'));
      expect(stamps, contains('tunnel-second-relay'));
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    },
  );

  testWidgets('a failed receipt does not tear down a live session', (
    WidgetTester tester,
  ) async {
    final Directory dir = Directory.systemTemp.createTempSync(
      'chast-viewer-strategy-receipt-',
    );
    StreamPreferences.configPathOverride = '${dir.path}/prefs.json';
    ViewPreferences.configPathOverride = '${dir.path}/prefs.json';
    final AccountSession account = _account(server: 'https://example.test');
    final ServerIdentity identity = ServerIdentity.parse(_serverKey)!;
    final ServerIdentityStore identities =
        _MemoryIdentity(<String, ServerIdentityPin>{
          'https://example.test': ServerIdentityPin(
            identity: identity,
            pinnedAt: DateTime.now().toUtc(),
          ),
        });
    bool started = false;
    bool stopped = false;
    final receiptAttempt = Completer<void>();
    final messenger = tester.binding.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(gpu, (MethodCall call) async => 1);
    messenger.setMockMethodCallHandler(native, (MethodCall call) async {
      switch (call.method) {
        case 'ticketIdentity':
          return _clientKey;
        case 'loadPassword':
          return '';
        case 'start':
          started = true;
          return null;
        case 'stop':
          stopped = true;
          return null;
        case 'status':
          return <String, Object>{
            'state': 'streaming',
            'width': 640,
            'height': 480,
            'sourceWidth': 640,
            'sourceHeight': 480,
            'published': 1,
            'inputEnabled': true,
            'elevationCode': 1,
            'serverFingerprint': _fingerprint,
          };
        case 'takeCompletedFiles':
          return <Object?>[];
        case 'refreshRemoteCursor':
          return false;
        default:
          return null;
      }
    });
    addTearDown(() {
      messenger.setMockMethodCallHandler(native, null);
      messenger.setMockMethodCallHandler(gpu, null);
      StreamPreferences.configPathOverride = null;
      ViewPreferences.configPathOverride = null;
      dir.deleteSync(recursive: true);
    });

    await tester.pumpWidget(
      MaterialApp(
        home: SessionPage(
          target: const ConnectTarget(
            '',
            0,
            server: 'https://example.test',
            hostId: 'devbox',
          ),
          account: account,
          settings: const ServerSettings(),
          trustStore: TrustStore(file: File('${dir.path}/trust.json')),
          identityStore: identities,
          serverIdentityLoader: (_) async => identity,
          accountResolver: () async => account,
          hostLookup:
              ({
                required ServerSettings settings,
                required String hostId,
                required String accountToken,
                required String clientPublicKey,
              }) async => _hostInfo(_delivery()),
          tunnelStarter:
              ({
                required ServerSettings server,
                required String hostId,
                required String accountToken,
                required String serverPublicKey,
                required EmergencyAccessRequest? emergency,
              }) async => _FakeTunnel(server.mode),
          viewerStrategyReceiptReporter:
              ({
                required ServerSettings settings,
                required String accountToken,
                required String peerId,
                required String context,
                required String clientPublicKey,
                required Map<String, Object?> receipt,
              }) async {
                receiptAttempt.complete();
                throw const ServerApiException('receipt failed');
              },
        ),
      ),
    );
    await _waitFor(tester, () => started);
    await receiptAttempt.future.timeout(const Duration(seconds: 2));
    await _waitFor(
      tester,
      () => find.byType(RemoteToolbar).evaluate().isNotEmpty,
    );
    expect(
      tester
          .widget<RemoteToolbar>(find.byType(RemoteToolbar))
          .elevationAvailable,
      isTrue,
    );
    expect(stopped, isFalse);
    expect(find.textContaining('receipt failed'), findsNothing);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets('viewer defaults are session snapshots and explicit prefs win', (
    WidgetTester tester,
  ) async {
    final Directory dir = Directory.systemTemp.createTempSync(
      'chast-viewer-strategy-renew-',
    );
    final RecordingUploadQueueStore recordingUploadQueue =
        RecordingUploadQueueStore(
          file: File('${dir.path}/recording-uploads.json'),
        );
    StreamPreferences.configPathOverride = '${dir.path}/prefs.json';
    ViewPreferences.configPathOverride = '${dir.path}/prefs.json';
    final AccountSession account = _account(server: 'https://example.test');
    final ServerIdentity identity = ServerIdentity.parse(_serverKey)!;
    final ServerIdentityStore identities =
        _MemoryIdentity(<String, ServerIdentityPin>{
          'https://example.test': ServerIdentityPin(
            identity: identity,
            pinnedAt: DateTime.now().toUtc(),
          ),
        });
    const ConnectTarget target = ConnectTarget(
      '',
      0,
      server: 'https://example.test',
      hostId: 'devbox',
    );
    const ConnectTarget explicitTarget = ConnectTarget(
      '',
      0,
      server: 'https://example.test',
      hostId: 'devbox',
      mode: TunnelMode.direct,
    );
    final tunnelSettings = <ServerSettings>[];
    final starts = <Map<Object?, Object?>>[];
    final uploads = <String>[];
    final File recording = File('${dir.path}/recording.mp4')
      ..writeAsBytesSync(_recordingFixtureBytes());
    bool recordingLive = true;
    int stops = 0;
    int lookups = 0;
    final messenger = tester.binding.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(gpu, (MethodCall call) async => 1);
    messenger.setMockMethodCallHandler(native, (MethodCall call) async {
      switch (call.method) {
        case 'ticketIdentity':
          return _clientKey;
        case 'loadPassword':
          return '';
        case 'start':
          starts.add(call.arguments as Map<Object?, Object?>);
          return null;
        case 'stop':
          stops++;
          return null;
        case 'renewAccessTicket':
          return true;
        case 'status':
          return <String, Object>{
            'state': 'streaming',
            'recording': recordingLive,
            'recordPath': recording.path,
            'width': 640,
            'height': 480,
            'sourceWidth': 640,
            'sourceHeight': 480,
            'published': 1,
            'inputEnabled': true,
            'elevationCode': 1,
            'serverFingerprint': _fingerprint,
          };
        case 'takeCompletedFiles':
          return <Object?>[];
        case 'refreshRemoteCursor':
          return false;
        default:
          return null;
      }
    });
    addTearDown(() async {
      await tester.pumpWidget(const SizedBox.shrink());
      await _waitForUploadWorkerIdle(tester, recordingUploadQueue);
      messenger.setMockMethodCallHandler(native, null);
      messenger.setMockMethodCallHandler(gpu, null);
      StreamPreferences.configPathOverride = null;
      ViewPreferences.configPathOverride = null;
      dir.deleteSync(recursive: true);
    });

    Widget managedSession({
      ConnectTarget sessionTarget = target,
      ServerSettings settings = const ServerSettings(),
    }) => MaterialApp(
      home: SessionPage(
        target: sessionTarget,
        account: account,
        settings: settings,
        recordingUploadQueue: recordingUploadQueue,
        trustStore: TrustStore(file: File('${dir.path}/trust.json')),
        identityStore: identities,
        serverIdentityLoader: (_) async => identity,
        accountResolver: () async => account,
        hostLookup:
            ({
              required ServerSettings settings,
              required String hostId,
              required String accountToken,
              required String clientPublicKey,
            }) async {
              final bool firstPolicy = ++lookups == 1;
              return _hostInfo(
                _delivery(
                  context: 'context-$lookups',
                  settingMode: 'default',
                  tunnelMode: firstPolicy
                      ? TunnelMode.direct
                      : TunnelMode.relay,
                  useWebSocket: firstPolicy,
                  uploadRecordings: !firstPolicy,
                  syncInitialClipboard: firstPolicy,
                  viewFit: firstPolicy ? 1 : 0,
                  defaultViewOnly: !firstPolicy,
                  revision: firstPolicy ? 4 : 5,
                  digest: firstPolicy
                      ? _digest
                      : '1123456789abcdef1123456789abcdef1123456789abcdef1123456789abcdef',
                  includeUnknown: false,
                ),
              );
            },
        tunnelStarter:
            ({
              required ServerSettings server,
              required String hostId,
              required String accountToken,
              required String serverPublicKey,
              required EmergencyAccessRequest? emergency,
            }) async {
              tunnelSettings.add(server);
              return _FakeTunnel(server.mode);
            },
        viewerStrategyReceiptReporter:
            ({
              required ServerSettings settings,
              required String accountToken,
              required String peerId,
              required String context,
              required String clientPublicKey,
              required Map<String, Object?> receipt,
            }) async {},
        recordingUploader:
            (
              AccountSession uploadedBy,
              File file,
              DateTime startedAt,
              DateTime endedAt,
            ) async {
              uploads.add(uploadedBy.token);
            },
      ),
    );

    await tester.pumpWidget(managedSession());
    await _waitFor(tester, () => starts.isNotEmpty);
    expect(tunnelSettings.single.mode, TunnelMode.direct);
    expect(starts.single['transport'], 'ws');
    expect(starts.single['syncInitClipboard'], isTrue);
    await _waitFor(
      tester,
      () => find.byType(RemoteToolbar).evaluate().isNotEmpty,
    );
    expect(
      tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).fit,
      ViewFit.original,
    );
    expect(
      tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).controlEnabled,
      isTrue,
    );
    await tester.pump(const Duration(milliseconds: 250));
    recordingLive = false;
    await tester.pump(const Duration(milliseconds: 250));
    expect(uploads, isEmpty);

    // A newer delivery arrives during ticket renewal, but cannot mutate this
    // session's already captured routing and display settings.
    await tester.pump(const Duration(seconds: 20));
    await _waitFor(tester, () => lookups >= 2);
    expect(tunnelSettings, hasLength(1));
    expect(starts, hasLength(1));
    expect(tunnelSettings.single.mode, TunnelMode.direct);
    expect(starts.single['transport'], 'ws');
    expect(starts.single['syncInitClipboard'], isTrue);
    expect(
      tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).fit,
      ViewFit.original,
    );

    await tester.pumpWidget(const SizedBox.shrink());
    await _waitFor(tester, () => stops >= 1);

    // A new SessionPage consumes the latest default strategy snapshot.
    recordingLive = true;
    await tester.pumpWidget(managedSession());
    await _waitFor(tester, () => starts.length >= 2);
    expect(tunnelSettings[1].mode, TunnelMode.relay);
    expect(starts[1]['transport'], 'auto');
    expect(starts[1]['syncInitClipboard'], isFalse);
    await _waitFor(
      tester,
      () => find.byType(RemoteToolbar).evaluate().isNotEmpty,
    );
    expect(
      tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).fit,
      ViewFit.adaptive,
    );
    expect(
      tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).controlEnabled,
      isFalse,
    );
    await tester.pump(const Duration(milliseconds: 250));
    recordingLive = false;
    await tester.pump(const Duration(milliseconds: 250));
    await _waitFor(tester, () => uploads.length == 1);
    expect(uploads, <String>['alice-token']);

    // Persisted explicit local choices take precedence over default-mode
    // recommendations on the following session.
    ViewPreferences.update(
      peerId: target.preferencesKey,
      fit: 1,
      syncInitClipboard: true,
      defaultViewOnly: false,
    );
    recordingLive = true;
    await tester.pumpWidget(const SizedBox.shrink());
    await _waitFor(tester, () => stops >= 2);
    await tester.pumpWidget(
      managedSession(
        sessionTarget: explicitTarget,
        settings: const ServerSettings(
          modeConfigured: true,
          useWebSocket: true,
          uploadRecordingsConfigured: true,
          useWebSocketConfigured: true,
        ),
      ),
    );
    await _waitFor(tester, () => starts.length >= 3);
    expect(tunnelSettings[2].mode, TunnelMode.direct);
    expect(starts[2]['transport'], 'ws');
    expect(starts[2]['syncInitClipboard'], isTrue);
    await _waitFor(
      tester,
      () => find.byType(RemoteToolbar).evaluate().isNotEmpty,
    );
    expect(
      tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).fit,
      ViewFit.original,
    );
    expect(
      tester.widget<RemoteToolbar>(find.byType(RemoteToolbar)).controlEnabled,
      isTrue,
    );
    await tester.pump(const Duration(milliseconds: 250));
    recordingLive = false;
    await tester.pump(const Duration(milliseconds: 250));
    expect(uploads, <String>['alice-token']);

    await tester.pumpWidget(const SizedBox.shrink());
    await _waitFor(tester, () => stops >= 3);
  });
}

Future<void> _waitFor(WidgetTester tester, bool Function() condition) async {
  for (int i = 0; i < 120 && !condition(); i++) {
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 10)),
    );
    await tester.pump(const Duration(milliseconds: 50));
  }
  expect(condition(), isTrue);
}

Future<void> _waitForUploadWorkerIdle(
  WidgetTester tester,
  RecordingUploadQueueStore queue,
) async {
  for (int i = 0; i < 120; i++) {
    final _RecordingWorkerProbe? probe = await tester.runAsync(
      () async => _RecordingWorkerProbe(await queue.tryAcquireWorker()),
    );
    final RecordingUploadWorkerLease? lease = probe?.lease;
    if (lease != null) {
      await tester.runAsync(lease.release);
      return;
    }
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 10)),
    );
    await tester.pump(const Duration(milliseconds: 50));
  }
  fail('recording upload worker did not release its lock');
}

class _RecordingWorkerProbe {
  const _RecordingWorkerProbe(this.lease);

  final RecordingUploadWorkerLease? lease;
}
