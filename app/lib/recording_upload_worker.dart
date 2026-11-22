import 'dart:async';
import 'dart:io';

import 'account_store.dart';
import 'recording_upload_queue.dart';
import 'server_api.dart';
import 'server_settings.dart';
import 'viewer_strategy.dart';

typedef RecordingUploadAuthorization =
    Future<bool> Function(AccountSession account, String hostId);
typedef RecordingUploadTransport =
    Future<void> Function(
      AccountSession account,
      RecordingUploadJob job,
      File file,
    );
typedef RecordingUploadHostLookup =
    Future<HostInfo> Function({
      required ServerSettings settings,
      required String hostId,
      required String accountToken,
      required String clientPublicKey,
    });

class RecordingUploadDrainResult {
  const RecordingUploadDrainResult({
    this.uploadedCount = 0,
    this.retryRequired = false,
    this.accountUnavailable = false,
  });

  final int uploadedCount;
  final bool retryRequired;
  final bool accountUnavailable;
}

class RecordingUploadWorker {
  const RecordingUploadWorker({required this.queue});

  final RecordingUploadQueueStore queue;

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
    if (!enabled() || shouldStop()) return const RecordingUploadDrainResult();
    int uploadedCount = 0;
    bool retryRequired = false;
    RecordingUploadWorkerLease? worker;
    try {
      worker = await queue.acquireWorker();
      if (!enabled() || shouldStop()) {
        return const RecordingUploadDrainResult();
      }
      final AccountSession? initialAccount = await accountResolver();
      if (!_isActive(initialAccount)) {
        return const RecordingUploadDrainResult(accountUnavailable: true);
      }
      final AccountSession recoveryAccount = initialAccount!;
      await queue.recoverIntents(
        shouldRecover: (RecordingUploadIntent intent) async {
          if (!enabled() ||
              shouldStop() ||
              !intent.belongsTo(
                server: recoveryAccount.server,
                username: recoveryAccount.username,
              )) {
            return false;
          }
          final AccountSession? current = await accountResolver();
          if (!_sameIdentity(recoveryAccount, current) || !_isActive(current)) {
            return false;
          }
          return authorize(current!, intent.hostId);
        },
      );
      await beforeDrain?.call();

      final Set<String> attempted = <String>{};
      while (enabled() && !shouldStop()) {
        final AccountSession? account = await accountResolver();
        if (!_isActive(account)) {
          return RecordingUploadDrainResult(
            uploadedCount: uploadedCount,
            retryRequired: retryRequired,
            accountUnavailable: true,
          );
        }
        final AccountSession activeAccount = account!;
        final List<RecordingUploadJob> jobs = await queue.load();
        RecordingUploadJob? job;
        for (final RecordingUploadJob candidate in jobs) {
          if (!attempted.contains(candidate.id) &&
              candidate.belongsTo(
                server: activeAccount.server,
                username: activeAccount.username,
              )) {
            job = candidate;
            break;
          }
        }
        if (job == null) break;
        attempted.add(job.id);

        bool started = false;
        try {
          if (!await authorize(activeAccount, job.hostId)) continue;
          final AccountSession? current = await accountResolver();
          if (!_isActive(current)) {
            return RecordingUploadDrainResult(
              uploadedCount: uploadedCount,
              retryRequired: retryRequired,
              accountUnavailable: true,
            );
          }
          final AccountSession liveAccount = current!;
          if (!_sameIdentity(activeAccount, liveAccount) ||
              !job.belongsTo(
                server: liveAccount.server,
                username: liveAccount.username,
              )) {
            continue;
          }

          final File file = File(job.path);
          if (!await file.exists()) {
            await queue.remove(job.id);
            continue;
          }
          final FileStat stat = await file.stat();
          if (stat.type != FileSystemEntityType.file) {
            await queue.remove(job.id);
            continue;
          }
          if (stat.size != job.fileSize ||
              stat.modified.millisecondsSinceEpoch != job.fileModifiedAtMs) {
            throw FileSystemException(
              'recording file changed after it was queued',
              job.path,
            );
          }

          onStarted?.call(job);
          started = true;
          await upload(liveAccount, job, file);
          await queue.remove(job.id);
          uploadedCount++;
          await onUploaded?.call(job);
        } catch (error) {
          final bool retryable = isRetryableRecordingUploadError(error);
          retryRequired |= retryable;
          onJobFailed?.call(job, error, retryable);
          if (retryable) {
            return RecordingUploadDrainResult(
              uploadedCount: uploadedCount,
              retryRequired: true,
            );
          }
        } finally {
          if (started) onFinished?.call(job);
        }
      }
    } catch (error) {
      retryRequired = true;
      onError?.call(error);
    } finally {
      await worker?.release();
    }
    return RecordingUploadDrainResult(
      uploadedCount: uploadedCount,
      retryRequired: retryRequired,
    );
  }
}

Future<bool> resolveViewerRecordingUploadPolicy({
  required ServerSettings settings,
  required AccountSession account,
  required String hostId,
  required bool localEnabled,
  required bool localIsSet,
  required Future<String> Function() clientPublicKey,
  RecordingUploadHostLookup? lookup,
}) async {
  if (hostId.trim().isEmpty) return localEnabled;
  final String publicKey = (await clientPublicKey()).trim();
  if (publicKey.isEmpty) return localEnabled;
  final ServerSettings accountSettings = settings.copyWith(url: account.server);
  final HostInfo info = await (lookup ?? _lookupRecordingHost)(
    settings: accountSettings,
    hostId: hostId.trim(),
    accountToken: account.token,
    clientPublicKey: publicKey,
  );
  final ViewerStrategyDelivery? delivery = info.viewerStrategy;
  if (delivery != null && !delivery.isActiveAt(DateTime.now())) {
    throw const FormatException('viewer upload policy has expired');
  }
  final ViewerLocalSettings local = ViewerLocalSettings(
    tunnelMode: settings.mode,
    useWebSocket: settings.useWebSocket,
    uploadRecordings: localEnabled,
    syncInitialClipboard: false,
    viewFit: 0,
    defaultViewOnly: false,
    useWebSocketSet: settings.useWebSocketConfigured,
    uploadRecordingsSet: localIsSet,
  );
  return ViewerEffectiveSettings.resolve(
    local,
    delivery: delivery,
  ).uploadRecordings;
}

Future<HostInfo> _lookupRecordingHost({
  required ServerSettings settings,
  required String hostId,
  required String accountToken,
  required String clientPublicKey,
}) => ServerApi.lookup(
  settings,
  hostId,
  accountToken: accountToken,
  clientPublicKey: clientPublicKey,
);

bool isRetryableRecordingUploadError(Object error) =>
    error is SocketException ||
    error is TimeoutException ||
    (error is ServerApiException &&
        (error.statusCode == null ||
            error.statusCode == 0 ||
            error.statusCode == 401 ||
            error.statusCode == 408 ||
            error.statusCode == 429 ||
            error.retryable));

bool _isActive(AccountSession? account) =>
    account != null && account.isActiveAt(DateTime.now().toUtc());

bool _sameIdentity(AccountSession left, AccountSession? right) =>
    right != null &&
    normalizeServerUrl(left.server) == normalizeServerUrl(right.server) &&
    left.username.trim() == right.username.trim();
