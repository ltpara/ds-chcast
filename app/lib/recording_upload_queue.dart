import 'dart:convert';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'server_url.dart';
import 'store_lock.dart';

final Set<String> _heldRecordingWorkers = <String>{};

class RecordingUploadJob {
  const RecordingUploadJob({
    required this.id,
    required this.path,
    required this.server,
    required this.username,
    required this.hostId,
    required this.peerId,
    required this.startedAtMs,
    required this.endedAtMs,
    required this.fileSize,
    required this.fileModifiedAtMs,
    this.uploadId = '',
  });

  final String id;
  final String path;
  final String server;
  final String username;
  final String hostId;
  final String peerId;
  final int startedAtMs;
  final int endedAtMs;
  final int fileSize;
  final int fileModifiedAtMs;
  final String uploadId;

  String get name => _fileName(path);
  int get durationMs => (endedAtMs - startedAtMs).clamp(0, 1 << 62).toInt();

  bool belongsTo({required String server, required String username}) =>
      normalizeServerUrl(this.server) == normalizeServerUrl(server) &&
      this.username == username.trim();

  RecordingUploadJob copyWith({String? uploadId}) => RecordingUploadJob(
    id: id,
    path: path,
    server: server,
    username: username,
    hostId: hostId,
    peerId: peerId,
    startedAtMs: startedAtMs,
    endedAtMs: endedAtMs,
    fileSize: fileSize,
    fileModifiedAtMs: fileModifiedAtMs,
    uploadId: uploadId ?? this.uploadId,
  );

  Map<String, Object?> toJson() => <String, Object?>{
    'id': id,
    'path': path,
    'server': server,
    'username': username,
    'host_id': hostId,
    'peer_id': peerId,
    'started_at_ms': startedAtMs,
    'ended_at_ms': endedAtMs,
    'file_size': fileSize,
    'file_modified_at_ms': fileModifiedAtMs,
    if (uploadId.isNotEmpty) 'upload_id': uploadId,
  };

  static RecordingUploadJob? fromJson(Object? value) {
    if (value is! Map) return null;
    String string(String key) => value[key]?.toString().trim() ?? '';
    int? integer(String key) =>
        value[key] is num ? (value[key] as num).toInt() : null;
    final String id = string('id');
    final String path = string('path');
    final String server = normalizeServerUrl(string('server'));
    final String username = string('username');
    final int? startedAtMs = integer('started_at_ms');
    final int? endedAtMs = integer('ended_at_ms');
    final int? fileSize = integer('file_size');
    final int? fileModifiedAtMs = integer('file_modified_at_ms');
    if (!_validId(id) ||
        path.isEmpty ||
        server.isEmpty ||
        username.isEmpty ||
        startedAtMs == null ||
        endedAtMs == null ||
        fileSize == null ||
        fileSize <= 0 ||
        fileModifiedAtMs == null) {
      return null;
    }
    final String uploadId = string('upload_id');
    if (uploadId.isNotEmpty && !_validId(uploadId)) return null;
    return RecordingUploadJob(
      id: id,
      path: path,
      server: server,
      username: username,
      hostId: string('host_id'),
      peerId: string('peer_id'),
      startedAtMs: startedAtMs,
      endedAtMs: endedAtMs,
      fileSize: fileSize,
      fileModifiedAtMs: fileModifiedAtMs,
      uploadId: uploadId,
    );
  }
}

class RecordingUploadIntent {
  const RecordingUploadIntent({
    required this.id,
    required this.path,
    required this.server,
    required this.username,
    required this.hostId,
    required this.peerId,
    required this.startedAtMs,
  });

  final String id;
  final String path;
  final String server;
  final String username;
  final String hostId;
  final String peerId;
  final int startedAtMs;

  bool belongsTo({required String server, required String username}) =>
      normalizeServerUrl(this.server) == normalizeServerUrl(server) &&
      this.username == username.trim();

  Map<String, Object?> toJson() => <String, Object?>{
    'id': id,
    'path': path,
    'server': server,
    'username': username,
    'host_id': hostId,
    'peer_id': peerId,
    'started_at_ms': startedAtMs,
  };

  static RecordingUploadIntent? fromJson(Object? value) {
    if (value is! Map) return null;
    String string(String key) => value[key]?.toString().trim() ?? '';
    final Object? startedAtValue = value['started_at_ms'];
    final String id = string('id');
    final String path = string('path');
    final String server = normalizeServerUrl(string('server'));
    final String username = string('username');
    if (!_validId(id) ||
        path.isEmpty ||
        server.isEmpty ||
        username.isEmpty ||
        startedAtValue is! num ||
        startedAtValue <= 0) {
      return null;
    }
    return RecordingUploadIntent(
      id: id,
      path: path,
      server: server,
      username: username,
      hostId: string('host_id'),
      peerId: string('peer_id'),
      startedAtMs: startedAtValue.toInt(),
    );
  }
}

class RecordingUploadQueueStore {
  RecordingUploadQueueStore({File? file, this._appData}) : _injected = file;

  final File? _injected;
  final String? _appData;

  File? get _file {
    if (_injected != null) return _injected;
    final String? appData = _appData ?? Platform.environment['APPDATA'];
    if (appData == null || appData.isEmpty) return null;
    return File('$appData\\chast\\recording-uploads.json');
  }

  File? get _intentFile {
    final File? file = _file;
    return file == null ? null : File('${file.path}.recording-intents.json');
  }

  Future<List<RecordingUploadJob>> load() async {
    final File? file = _file;
    if (file == null) return <RecordingUploadJob>[];
    final List<RecordingUploadJob> result = await withStoreLock(
      file,
      () => _readLocked(file),
    );
    return result;
  }

  Future<RecordingUploadJob> enqueue(RecordingUploadJob job) async {
    if (!_validId(job.id) || job.fileSize <= 0) {
      throw const FormatException('recording upload job is invalid');
    }
    final File file = _requiredFile();
    return withStoreLock(file, () async {
      final List<RecordingUploadJob> jobs = await _readLocked(file);
      final int existingIndex = jobs.indexWhere(
        (RecordingUploadJob item) =>
            _samePath(item.path, job.path) &&
            normalizeServerUrl(item.server) == normalizeServerUrl(job.server) &&
            item.username == job.username,
      );
      final RecordingUploadJob? existing = existingIndex < 0
          ? null
          : jobs[existingIndex];
      if (existing != null) return existing;
      jobs.add(job);
      await _writeLocked(file, jobs);
      return job;
    });
  }

  Future<void> persistIntent(RecordingUploadIntent intent) async {
    if (!_validId(intent.id) ||
        intent.path.trim().isEmpty ||
        intent.server.isEmpty ||
        intent.username.trim().isEmpty ||
        intent.startedAtMs <= 0) {
      throw const FormatException('recording upload intent is invalid');
    }
    final File queue = _requiredFile();
    final File file = _intentFile!;
    await withStoreLock(queue, () async {
      final List<RecordingUploadIntent> intents = await _readIntentsLocked(
        file,
      );
      final int index = intents.indexWhere(
        (RecordingUploadIntent item) => item.id == intent.id,
      );
      if (index < 0) {
        intents.add(intent);
      } else {
        intents[index] = intent;
      }
      await _writeIntentsLocked(file, intents);
    });
  }

  Future<RecordingUploadWorkerLease> beginRecording(
    RecordingUploadIntent intent,
  ) async {
    if (!_validId(intent.id) ||
        intent.path.trim().isEmpty ||
        intent.server.isEmpty ||
        intent.username.trim().isEmpty ||
        intent.startedAtMs <= 0) {
      throw const FormatException('recording upload intent is invalid');
    }
    final File queue = _requiredFile();
    final String key =
        '${queue.absolute.path.toLowerCase()}|recording|${intent.id}';
    if (_heldRecordingWorkers.contains(key)) {
      throw const FileSystemException('recording intent is already leased');
    }
    await queue.parent.create(recursive: true);
    final RandomAccessFile lock = await File(
      '${queue.path}.recording.${intent.id}.lock',
    ).open(mode: FileMode.append);
    try {
      await lock.lock(FileLock.blockingExclusive);
      _heldRecordingWorkers.add(key);
      final File file = _intentFile!;
      await withStoreLock(queue, () async {
        final List<RecordingUploadIntent> intents = await _readIntentsLocked(
          file,
        );
        intents.removeWhere(
          (RecordingUploadIntent item) => item.id == intent.id,
        );
        intents.add(intent);
        await _writeIntentsLocked(file, intents);
      });
      return RecordingUploadWorkerLease._(lock, key);
    } catch (_) {
      _heldRecordingWorkers.remove(key);
      await lock.close();
      rethrow;
    }
  }

  Future<List<RecordingUploadIntent>> loadIntents() async {
    final File? queue = _file;
    final File? file = _intentFile;
    if (queue == null || file == null) return <RecordingUploadIntent>[];
    return withStoreLock(queue, () => _readIntentsLocked(file));
  }

  Future<int?> tryCountUnfinalizedMp4Segments() async {
    final File? queue = _file;
    final File? intentFile = _intentFile;
    if (queue == null || intentFile == null || !await intentFile.exists()) {
      return 0;
    }
    RandomAccessFile? lock;
    List<RecordingUploadIntent> intents;
    try {
      lock = await File('${queue.path}.lock').open(mode: FileMode.append);
      await lock.lock(FileLock.exclusive);
      intents = await _readIntentsLocked(intentFile);
    } on FileSystemException {
      return null;
    } finally {
      await lock?.close();
    }
    final Set<String> unfinalized = <String>{};
    for (final RecordingUploadIntent intent in intents) {
      final List<File> files = await _recordingFilesForIntent(
        File(intent.path),
      );
      for (final File file in files) {
        if (!await isFinalizedMp4(file)) {
          unfinalized.add(file.absolute.path.toLowerCase());
        }
      }
    }
    return unfinalized.length;
  }

  Future<RecordingUploadIntent?> findIntentForPath(String path) async {
    final String candidate = File(path).absolute.path;
    for (final RecordingUploadIntent intent in await loadIntents()) {
      if (_samePath(intent.path, candidate)) return intent;
    }
    return null;
  }

  Future<void> removeIntent(String intentId) async {
    final File queue = _requiredFile();
    final File file = _intentFile!;
    await withStoreLock(queue, () async {
      final List<RecordingUploadIntent> intents = await _readIntentsLocked(
        file,
      );
      final int before = intents.length;
      intents.removeWhere((RecordingUploadIntent item) => item.id == intentId);
      if (intents.length == before) return;
      if (intents.isEmpty) {
        if (await file.exists()) await file.delete();
      } else {
        await _writeIntentsLocked(file, intents);
      }
    });
  }

  Future<void> recoverIntents({
    Future<bool> Function(RecordingUploadIntent intent)? shouldRecover,
  }) async {
    for (final RecordingUploadIntent intent in await loadIntents()) {
      if (shouldRecover != null && !await shouldRecover(intent)) continue;
      final RecordingUploadWorkerLease? lease = await tryAcquireIntentLease(
        intent.id,
      );
      if (lease == null) continue;
      try {
        final File baseFile = File(intent.path);
        final List<File> files = await _recordingFilesForIntent(baseFile);
        if (files.isEmpty) {
          await removeIntent(intent.id);
          continue;
        }
        bool incompleteSegment = false;
        for (final File segment in files) {
          if (!await isFinalizedMp4(segment)) {
            incompleteSegment = true;
            continue;
          }
          final FileStat stat = await segment.stat();
          if (stat.type != FileSystemEntityType.file || stat.size <= 0) {
            incompleteSegment = true;
            continue;
          }
          final int endedAtMs = stat.modified.millisecondsSinceEpoch;
          final bool isBasePath = _samePath(
            segment.absolute.path,
            baseFile.absolute.path,
          );
          await enqueue(
            RecordingUploadJob(
              id: isBasePath ? intent.id : newRecordingUploadJobId(),
              path: segment.absolute.path,
              server: intent.server,
              username: intent.username,
              hostId: intent.hostId,
              peerId: intent.peerId,
              startedAtMs: intent.startedAtMs,
              endedAtMs: endedAtMs < intent.startedAtMs
                  ? intent.startedAtMs
                  : endedAtMs,
              fileSize: stat.size,
              fileModifiedAtMs: stat.modified.millisecondsSinceEpoch,
            ),
          );
        }
        if (!incompleteSegment) await removeIntent(intent.id);
      } finally {
        await lease.release();
      }
    }
  }

  Future<RecordingUploadWorkerLease?> tryAcquireIntentLease(
    String intentId,
  ) async {
    if (!_validId(intentId)) {
      throw const FormatException('recording upload intent id is invalid');
    }
    final File file = _requiredFile();
    final String key =
        '${file.absolute.path.toLowerCase()}|recording|$intentId';
    if (_heldRecordingWorkers.contains(key)) return null;
    await file.parent.create(recursive: true);
    final RandomAccessFile lock = await File(
      '${file.path}.recording.$intentId.lock',
    ).open(mode: FileMode.append);
    try {
      await lock.lock(FileLock.exclusive);
      _heldRecordingWorkers.add(key);
      return RecordingUploadWorkerLease._(lock, key);
    } on FileSystemException {
      await lock.close();
      return null;
    } catch (_) {
      await lock.close();
      rethrow;
    }
  }

  Future<void> updateUploadId(String jobId, String uploadId) async {
    if (!_validId(uploadId)) {
      throw const FormatException('recording upload session id is invalid');
    }
    final File file = _requiredFile();
    await withStoreLock(file, () async {
      final List<RecordingUploadJob> jobs = await _readLocked(file);
      final int index = jobs.indexWhere(
        (RecordingUploadJob job) => job.id == jobId,
      );
      if (index < 0) return;
      jobs[index] = jobs[index].copyWith(uploadId: uploadId);
      await _writeLocked(file, jobs);
    });
  }

  Future<void> remove(String jobId) async {
    final File file = _requiredFile();
    await withStoreLock(file, () async {
      final List<RecordingUploadJob> jobs = await _readLocked(file);
      final int before = jobs.length;
      jobs.removeWhere((RecordingUploadJob job) => job.id == jobId);
      if (jobs.length == before) return;
      await _writeLocked(file, jobs);
    });
  }

  Future<Set<String>> protectedPaths() async => <String>{
    for (final RecordingUploadJob job in await load()) job.path,
  };

  Future<RecordingUploadWorkerLease> acquireWorker() async {
    final File file = _requiredFile();
    final String key = file.absolute.path.toLowerCase();
    await file.parent.create(recursive: true);
    final RandomAccessFile lock = await File(
      '${file.path}.worker.lock',
    ).open(mode: FileMode.append);
    try {
      await lock.lock(FileLock.blockingExclusive);
      _heldRecordingWorkers.add(key);
      return RecordingUploadWorkerLease._(lock, key);
    } catch (_) {
      await lock.close();
      rethrow;
    }
  }

  Future<RecordingUploadWorkerLease?> tryAcquireWorker() async {
    final File file = _requiredFile();
    final String key = file.absolute.path.toLowerCase();
    if (_heldRecordingWorkers.contains(key)) return null;
    await file.parent.create(recursive: true);
    final RandomAccessFile lock = await File(
      '${file.path}.worker.lock',
    ).open(mode: FileMode.append);
    try {
      await lock.lock(FileLock.exclusive);
      _heldRecordingWorkers.add(key);
      return RecordingUploadWorkerLease._(lock, key);
    } on FileSystemException {
      await lock.close();
      return null;
    } catch (_) {
      await lock.close();
      rethrow;
    }
  }

  File _requiredFile() =>
      _file ?? (throw const FileSystemException('APPDATA is unavailable'));

  Future<List<RecordingUploadJob>> _readLocked(File file) async {
    if (!await file.exists()) return <RecordingUploadJob>[];
    final String contents = await file.readAsString();
    final Object? decoded = jsonDecode(contents);
    if (decoded is! Map ||
        decoded['version'] != 1 ||
        decoded['jobs'] is! List) {
      throw const FormatException('recording upload queue is invalid');
    }
    final List<RecordingUploadJob> jobs = <RecordingUploadJob>[];
    for (final Object? item in decoded['jobs'] as List<Object?>) {
      final RecordingUploadJob? job = RecordingUploadJob.fromJson(item);
      if (job == null) {
        throw const FormatException('recording upload queue entry is invalid');
      }
      jobs.add(job);
    }
    return jobs;
  }

  Future<void> _writeLocked(File file, List<RecordingUploadJob> jobs) async {
    final File temp = File(
      '${file.path}.$pid.${DateTime.now().microsecondsSinceEpoch}.tmp',
    );
    try {
      await file.parent.create(recursive: true);
      await temp.writeAsString(
        jsonEncode(<String, Object?>{
          'version': 1,
          'jobs': <Object?>[
            for (final RecordingUploadJob job in jobs) job.toJson(),
          ],
        }),
        flush: true,
      );
      await temp.rename(file.path);
    } catch (_) {
      try {
        if (await temp.exists()) await temp.delete();
      } catch (_) {}
      rethrow;
    }
  }

  Future<List<RecordingUploadIntent>> _readIntentsLocked(File file) async {
    if (!await file.exists()) return <RecordingUploadIntent>[];
    final Object? decoded = jsonDecode(await file.readAsString());
    if (decoded is! Map ||
        decoded['version'] != 1 ||
        decoded['intents'] is! List) {
      throw const FormatException('recording upload intents are invalid');
    }
    final List<RecordingUploadIntent> intents = <RecordingUploadIntent>[];
    for (final Object? item in decoded['intents'] as List<Object?>) {
      final RecordingUploadIntent? intent = RecordingUploadIntent.fromJson(
        item,
      );
      if (intent == null) {
        throw const FormatException('recording upload intent is invalid');
      }
      intents.add(intent);
    }
    return intents;
  }

  Future<void> _writeIntentsLocked(
    File file,
    List<RecordingUploadIntent> intents,
  ) async {
    final File temp = File(
      '${file.path}.$pid.${DateTime.now().microsecondsSinceEpoch}.tmp',
    );
    try {
      await file.parent.create(recursive: true);
      await temp.writeAsString(
        jsonEncode(<String, Object?>{
          'version': 1,
          'intents': <Object?>[
            for (final RecordingUploadIntent intent in intents) intent.toJson(),
          ],
        }),
        flush: true,
      );
      await temp.rename(file.path);
    } catch (_) {
      try {
        if (await temp.exists()) await temp.delete();
      } catch (_) {}
      rethrow;
    }
  }
}

class RecordingUploadWorkerLease {
  RecordingUploadWorkerLease._(this._lock, this._key);

  final RandomAccessFile _lock;
  final String _key;
  bool _released = false;

  Future<void> release() async {
    if (_released) return;
    _released = true;
    try {
      await _lock.close();
    } finally {
      _heldRecordingWorkers.remove(_key);
    }
  }
}

String newRecordingUploadJobId() {
  final Random random = Random.secure();
  return List<String>.generate(
    16,
    (_) => random.nextInt(256).toRadixString(16).padLeft(2, '0'),
  ).join();
}

Future<bool> isFinalizedMp4(File file) async {
  if (!await file.exists()) return false;
  final RandomAccessFile input = await file.open();
  try {
    final int length = await input.length();
    int offset = 0;
    bool hasFileType = false;
    bool hasMediaData = false;
    bool hasMovieMetadata = false;
    int boxes = 0;
    while (offset + 8 <= length && boxes++ < 100000) {
      await input.setPosition(offset);
      final List<int> header = await input.read(8);
      if (header.length != 8) return false;
      final ByteData data = ByteData.sublistView(Uint8List.fromList(header));
      final int size32 = data.getUint32(0, Endian.big);
      final String type = String.fromCharCodes(header.sublist(4, 8));
      int headerSize = 8;
      int boxSize = size32;
      if (size32 == 1) {
        final List<int> extended = await input.read(8);
        if (extended.length != 8) return false;
        boxSize = ByteData.sublistView(
          Uint8List.fromList(extended),
        ).getUint64(0, Endian.big);
        headerSize = 16;
      } else if (size32 == 0) {
        boxSize = length - offset;
      }
      if (boxSize < headerSize || boxSize > length - offset) return false;
      hasFileType |= type == 'ftyp';
      hasMediaData |= type == 'mdat' && boxSize > headerSize;
      hasMovieMetadata |= type == 'moov' && boxSize > headerSize;
      offset += boxSize;
    }
    return offset == length && hasFileType && hasMediaData && hasMovieMetadata;
  } on FileSystemException {
    return false;
  } finally {
    await input.close();
  }
}

bool _validId(String value) => RegExp(r'^[0-9a-f]{32}$').hasMatch(value);

bool _samePath(String left, String right) =>
    left.replaceAll('/', '\\').toLowerCase() ==
    right.replaceAll('/', '\\').toLowerCase();

String _fileName(String path) {
  final String normalized = path.replaceAll('\\', '/');
  final int slash = normalized.lastIndexOf('/');
  final String name = slash < 0 ? normalized : normalized.substring(slash + 1);
  return name.trim().isEmpty ? 'recording.mp4' : name.trim();
}

Future<List<File>> _recordingFilesForIntent(File baseFile) async {
  final List<File> files = <File>[];
  if (await baseFile.exists()) files.add(baseFile);
  final String name = _fileName(baseFile.path);
  final int dot = name.lastIndexOf('.');
  final String stem = dot > 0 ? name.substring(0, dot) : name;
  final String extension = dot > 0 ? name.substring(dot) : '';
  final RegExp numberedSegment = RegExp(
    '^${RegExp.escape(stem)}-\\d+${RegExp.escape(extension)}\$',
    caseSensitive: false,
  );
  final Directory directory = baseFile.parent;
  if (await directory.exists()) {
    await for (final FileSystemEntity entry in directory.list(
      followLinks: false,
    )) {
      if (entry is File && numberedSegment.hasMatch(_fileName(entry.path))) {
        files.add(entry);
      }
    }
  }
  files.sort((File left, File right) => left.path.compareTo(right.path));
  return files;
}
