import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:chast_viewer/recording_upload_queue.dart';

const String _jobId = '0123456789abcdef0123456789abcdef';
const String _uploadId = 'abcdef0123456789abcdef0123456789';
bool _uploadStarted = false;
int _uploadSize = 0;
int _uploadOffset = 0;
String _idempotencyKey = '';
final List<int> _storedBytes = <int>[];

Future<void> main(List<String> args) async {
  if (args.isEmpty) {
    stderr.writeln('usage: <seed|serve> ...');
    exitCode = 64;
    return;
  }
  switch (args.first) {
    case 'seed':
      if (args.length != 4) {
        stderr.writeln('seed requires queue-file, recording-file and server');
        exitCode = 64;
        return;
      }
      await _seed(File(args[1]), File(args[2]), args[3]);
      return;
    case 'serve':
      if (args.length != 9) {
        stderr.writeln(
          'serve requires recording, port, patch-started, release, released, complete, trace and ready files',
        );
        exitCode = 64;
        return;
      }
      await _serve(
        recording: File(args[1]),
        portFile: File(args[2]),
        patchStartedFile: File(args[3]),
        releasePatchFile: File(args[4]),
        patchReleasedFile: File(args[5]),
        completeFile: File(args[6]),
        traceFile: File(args[7]),
        readyFile: File(args[8]),
      );
      return;
    default:
      stderr.writeln('unknown mode: ${args.first}');
      exitCode = 64;
  }
}

Future<void> _seed(File queueFile, File recording, String server) async {
  await recording.parent.create(recursive: true);
  await recording.writeAsBytes(_fixtureMp4(), flush: true);
  final FileStat stat = await recording.stat();
  final int startedAtMs = DateTime.now()
      .toUtc()
      .subtract(const Duration(seconds: 5))
      .millisecondsSinceEpoch;
  await RecordingUploadQueueStore(file: queueFile).enqueue(
    RecordingUploadJob(
      id: _jobId,
      path: recording.absolute.path,
      server: server,
      username: 'alice',
      hostId: '',
      peerId: 'process-restart-fixture',
      startedAtMs: startedAtMs,
      endedAtMs: startedAtMs + 5000,
      fileSize: stat.size,
      fileModifiedAtMs: stat.modified.millisecondsSinceEpoch,
    ),
  );
}

Future<void> _serve({
  required File recording,
  required File portFile,
  required File patchStartedFile,
  required File releasePatchFile,
  required File patchReleasedFile,
  required File completeFile,
  required File traceFile,
  required File readyFile,
}) async {
  final HttpServer server = await HttpServer.bind(
    InternetAddress.loopbackIPv4,
    0,
  );
  await portFile.parent.create(recursive: true);
  await portFile.writeAsString('${server.port}', flush: true);

  late final Timer stopTimer;
  final File stopFile = File('${portFile.path}.stop');
  stopTimer = Timer.periodic(const Duration(milliseconds: 50), (Timer timer) {
    if (!stopFile.existsSync()) return;
    timer.cancel();
    unawaited(server.close(force: true));
  });

  await for (final HttpRequest request in server) {
    unawaited(
      _handle(
        request,
        recording: recording,
        patchStartedFile: patchStartedFile,
        releasePatchFile: releasePatchFile,
        patchReleasedFile: patchReleasedFile,
        completeFile: completeFile,
        traceFile: traceFile,
        readyFile: readyFile,
      ),
    );
  }
  stopTimer.cancel();
}

Future<void> _handle(
  HttpRequest request, {
  required File recording,
  required File patchStartedFile,
  required File releasePatchFile,
  required File patchReleasedFile,
  required File completeFile,
  required File traceFile,
  required File readyFile,
}) async {
  final String path = request.uri.path;
  await _trace(traceFile, <String, Object?>{
    'method': request.method,
    'path': path,
  });
  if (request.headers.value(HttpHeaders.authorizationHeader) !=
      'Bearer test-upload-token') {
    await _writeJson(
      request.response,
      HttpStatus.unauthorized,
      <String, Object?>{},
    );
    return;
  }

  if (request.method == 'GET' && path == '/__probe/ready') {
    await readyFile.parent.create(recursive: true);
    await readyFile.writeAsString('ready\n', flush: true);
    request.response.statusCode = HttpStatus.noContent;
    await request.response.close();
    return;
  }

  if (request.method == 'POST' && path == '/v1/account/recordings/uploads') {
    final Object? decoded = jsonDecode(await utf8.decoder.bind(request).join());
    if (decoded is! Map || decoded['size'] is! num) {
      await _writeJson(
        request.response,
        HttpStatus.badRequest,
        <String, Object?>{},
      );
      return;
    }
    final int size = (decoded['size'] as num).toInt();
    final String idempotencyKey = decoded['idempotency_key']?.toString() ?? '';
    if (size <= 0 ||
        (_uploadStarted &&
            (_uploadSize != size || _idempotencyKey != idempotencyKey))) {
      await _writeJson(
        request.response,
        HttpStatus.conflict,
        <String, Object?>{},
      );
      return;
    }
    _uploadStarted = true;
    _uploadSize = size;
    _idempotencyKey = idempotencyKey;
    await _writeJson(request.response, HttpStatus.created, <String, Object?>{
      'upload': <String, Object?>{
        'id': _uploadId,
        'offset': _uploadOffset,
        'size': _uploadSize,
        'idempotency_key': _idempotencyKey,
      },
    });
    return;
  }

  final List<String> parts = path.split('/');
  if (parts.length == 6 &&
      parts.take(5).join('/') == '/v1/account/recordings/uploads' &&
      parts.last == _uploadId) {
    if (request.method == 'GET') {
      if (!_uploadStarted) {
        await _writeJson(
          request.response,
          HttpStatus.notFound,
          <String, Object?>{},
        );
        return;
      }
      await _writeJson(request.response, HttpStatus.ok, <String, Object?>{
        'upload': <String, Object?>{
          'id': _uploadId,
          'offset': _uploadOffset,
          'size': _uploadSize,
          'idempotency_key': _idempotencyKey,
        },
      });
      return;
    }
    if (request.method == 'PATCH') {
      final List<int> bytes = await request.fold<List<int>>(
        <int>[],
        (List<int> all, List<int> chunk) => all..addAll(chunk),
      );
      final List<int> expected = await recording.readAsBytes();
      final int offset =
          int.tryParse(request.headers.value('X-Upload-Offset') ?? '') ?? -1;
      if (!_uploadStarted ||
          offset != _uploadOffset ||
          offset < 0 ||
          offset + bytes.length > _uploadSize ||
          !_sameBytes(bytes, expected.sublist(offset, offset + bytes.length))) {
        await _writeJson(
          request.response,
          HttpStatus.conflict,
          <String, Object?>{},
        );
        return;
      }
      _storedBytes.addAll(bytes);
      _uploadOffset += bytes.length;
      await patchStartedFile.writeAsString('held', flush: true);
      await _waitForFile(releasePatchFile);
      await patchReleasedFile.writeAsString('released', flush: true);
      await _writeJson(request.response, HttpStatus.accepted, <String, Object?>{
        'upload': <String, Object?>{
          'id': _uploadId,
          'offset': _uploadOffset,
          'size': _uploadSize,
          'idempotency_key': _idempotencyKey,
        },
      });
      return;
    }
  }

  if (parts.length == 7 &&
      parts.take(5).join('/') == '/v1/account/recordings/uploads' &&
      parts[5] == _uploadId &&
      parts[6] == 'complete' &&
      request.method == 'POST') {
    final List<int> expected = await recording.readAsBytes();
    if (!_uploadStarted ||
        _uploadOffset != _uploadSize ||
        !_sameBytes(_storedBytes, expected)) {
      await _writeJson(
        request.response,
        HttpStatus.conflict,
        <String, Object?>{},
      );
      return;
    }
    await _trace(traceFile, <String, Object?>{
      'completed_sha256': sha256.convert(expected).toString(),
      'completed_size': expected.length,
    });
    await completeFile.writeAsString('complete', flush: true);
    await _writeJson(request.response, HttpStatus.created, <String, Object?>{
      'recording': <String, Object?>{
        'id': 'fixture-recording-1',
        'name': recording.uri.pathSegments.last,
        'size': expected.length,
        'sha256': sha256.convert(expected).toString(),
      },
    });
    return;
  }

  await _writeJson(request.response, HttpStatus.notFound, <String, Object?>{});
}

Future<void> _writeJson(HttpResponse response, int status, Object value) async {
  response.statusCode = status;
  response.headers.contentType = ContentType.json;
  try {
    response.write(jsonEncode(value));
    await response.close();
  } on HttpException {
    // A killed client can close the held first chunk response.
  } on SocketException {
    // The server state is retained for the restarted client.
  }
}

Future<void> _trace(File file, Map<String, Object?> entry) async {
  await file.parent.create(recursive: true);
  await file.writeAsString(
    '${jsonEncode(entry)}\n',
    mode: FileMode.append,
    flush: true,
  );
}

Future<void> _waitForFile(File file) async {
  final DateTime deadline = DateTime.now().add(const Duration(minutes: 2));
  while (!await file.exists()) {
    if (DateTime.now().isAfter(deadline)) {
      throw TimeoutException('timed out waiting for ${file.path}');
    }
    await Future<void>.delayed(const Duration(milliseconds: 50));
  }
}

bool _sameBytes(List<int> left, List<int> right) {
  if (left.length != right.length) return false;
  for (int i = 0; i < left.length; i++) {
    if (left[i] != right[i]) return false;
  }
  return true;
}

List<int> _fixtureMp4() => <int>[
  ..._box('ftyp', <int>[0x69, 0x73, 0x6f, 0x6d, 0, 0, 0, 0]),
  ..._box('mdat', <int>[1, 2, 3, 4, 5, 6, 7, 8]),
  ..._box('moov', <int>[0, 0, 0, 0]),
];

List<int> _box(String type, List<int> payload) {
  final ByteData size = ByteData(4)..setUint32(0, payload.length + 8);
  return <int>[...size.buffer.asUint8List(), ...type.codeUnits, ...payload];
}
