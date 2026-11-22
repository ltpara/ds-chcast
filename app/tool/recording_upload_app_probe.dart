import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/device_store.dart';
import 'package:chast_viewer/pages/home_page.dart';
import 'package:chast_viewer/recording_upload_queue.dart';
import 'package:chast_viewer/recording_upload_worker.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_identity.dart';
import 'package:chast_viewer/server_settings.dart';
import 'package:chast_viewer/trust_store.dart';
import 'package:chast_viewer/theme.dart';

const String _configPath = String.fromEnvironment(
  'CHAST_UPLOAD_ACCEPTANCE_CONFIG',
);

void _traceProbe(String event) {
  stderr.writeln('[upload-probe] $event');
}

Future<void> _signalFirstFrame(String serverUrl) async {
  final HttpClient client = HttpClient();
  try {
    final HttpClientRequest request = await client.getUrl(
      Uri.parse('$serverUrl/__probe/ready'),
    );
    request.headers.set(
      HttpHeaders.authorizationHeader,
      'Bearer test-upload-token',
    );
    final HttpClientResponse response = await request.close();
    await response.drain<void>();
    if (response.statusCode != HttpStatus.noContent) {
      throw HttpException('Ready signal returned ${response.statusCode}.');
    }
    _traceProbe('first-frame-signaled');
  } on Object catch (error) {
    _traceProbe('first-frame-signal-error $error');
  } finally {
    client.close(force: true);
  }
}

Future<void> main() async {
  _traceProbe('main-enter');
  _traceProbe('binding-start');
  WidgetsFlutterBinding.ensureInitialized();
  _traceProbe('binding-ready');
  Map<String, Object?> config = <String, Object?>{};
  try {
    final Object? decoded = jsonDecode(File(_configPath).readAsStringSync());
    if (decoded is Map) config = Map<String, Object?>.from(decoded);
  } on Object catch (error) {
    _traceProbe('config-read-error $error');
  }
  final String serverUrl = (config['server']?.toString() ?? '').trim();
  final String queuePath = (config['queue']?.toString() ?? '').trim();
  _traceProbe(
    'config-loaded server=${serverUrl.isNotEmpty} queue=${queuePath.isNotEmpty}',
  );
  if (serverUrl.isEmpty || queuePath.isEmpty) {
    _traceProbe('missing-required-config');
    stderr.writeln('The acceptance probe requires server and queue config.');
    exitCode = 64;
    return;
  }
  final File queueFile = File(queuePath);
  final AccountSession account = AccountSession(
    server: serverUrl,
    username: 'alice',
    token: 'test-upload-token',
    expiresAt: DateTime.now().toUtc().add(const Duration(hours: 1)),
  );
  final ServerSettings settings = ServerSettings(
    url: serverUrl,
    uploadRecordings: true,
    uploadRecordingsConfigured: true,
  );
  final RecordingUploadQueueStore queue = RecordingUploadQueueStore(
    file: queueFile,
  );

  _traceProbe('run-app');
  runApp(
    MaterialApp(
      title: 'Chast Upload Recovery Probe',
      debugShowCheckedModeBanner: false,
      supportedLocales: const <Locale>[Locale('zh', 'CN'), Locale('en')],
      localizationsDelegates: const <LocalizationsDelegate<Object>>[
        GlobalMaterialLocalizations.delegate,
        GlobalWidgetsLocalizations.delegate,
        GlobalCupertinoLocalizations.delegate,
      ],
      theme: buildChastTheme(Brightness.light),
      darkTheme: buildChastTheme(Brightness.dark),
      home: HomePage(
        deviceStore: _EmptyDevices(),
        serverStore: _FixedServers(settings),
        trustStore: _EmptyTrust(),
        identityStore: _EmptyIdentity(),
        accountStore: _FixedAccount(account),
        recordingUploadQueue: queue,
        recordingUploadWorker: _ProbeWorker(queue),
        addressBookFetcher: (ServerSettings _, String _, int _) async =>
            const AddressBookSnapshot(revision: 0, peers: <AddressBookPeer>[]),
        lookupBatch: (ServerSettings _, List<String> _) async =>
            const BatchLookupResult(hosts: <String, HostInfo>{}),
      ),
    ),
  );
  _traceProbe('run-app-returned');

  WidgetsBinding.instance.addPostFrameCallback((_) {
    unawaited(_signalFirstFrame(serverUrl));
  });
}

class _ProbeWorker extends RecordingUploadWorker {
  _ProbeWorker(RecordingUploadQueueStore queue) : super(queue: queue);

  @override
  Future<RecordingUploadDrainResult> drain({
    required Future<AccountSession?> Function() accountResolver,
    required RecordingUploadAuthorization authorize,
    required RecordingUploadTransport upload,
    required bool Function() enabled,
    required bool Function() shouldStop,
    Future<void> Function()? beforeDrain,
    void Function(RecordingUploadJob job)? onStarted,
    void Function(RecordingUploadJob job)? onFinished,
    Future<void> Function(RecordingUploadJob job)? onUploaded,
    void Function(RecordingUploadJob job, Object error, bool retryable)?
    onJobFailed,
    void Function(Object error)? onError,
  }) async {
    _traceProbe('worker-drain');
    try {
      final RecordingUploadDrainResult result = await super.drain(
        accountResolver: accountResolver,
        authorize: authorize,
        upload: upload,
        enabled: enabled,
        shouldStop: shouldStop,
        beforeDrain: beforeDrain,
        onStarted: onStarted,
        onFinished: onFinished,
        onUploaded: onUploaded,
        onJobFailed: (RecordingUploadJob job, Object error, bool retryable) {
          _traceProbe(
            'job-failed id=${job.id} retryable=$retryable error=$error',
          );
          onJobFailed?.call(job, error, retryable);
        },
        onError: (Object error) {
          _traceProbe('drain-error $error');
          onError?.call(error);
        },
      );
      _traceProbe(
        'worker-result uploaded=${result.uploadedCount} retry=${result.retryRequired} account=${result.accountUnavailable}',
      );
      return result;
    } catch (error) {
      _traceProbe('worker-error $error');
      rethrow;
    }
  }
}

class _EmptyDevices extends DeviceStore {
  @override
  Future<List<Device>> load() async => <Device>[];

  @override
  Future<List<PendingDeletion>> loadPendingDeletions() async =>
      <PendingDeletion>[];
}

class _FixedServers extends ServerSettingsStore {
  _FixedServers(this.settings);

  final ServerSettings settings;

  @override
  Future<ServerSettings> load() async => settings;
}

class _FixedAccount extends AccountSessionStore {
  _FixedAccount(this.account);

  final AccountSession account;

  @override
  Future<AccountSession?> load() async => account;
}

class _EmptyTrust extends TrustStore {
  @override
  Future<Map<String, String>> load() async => <String, String>{};
}

class _EmptyIdentity extends ServerIdentityStore {
  @override
  Future<Map<String, ServerIdentityPin>> load() async =>
      <String, ServerIdentityPin>{};
}
