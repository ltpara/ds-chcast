import 'dart:async';
import 'dart:io';

final Object _heldStores = Object();
final Map<String, Future<void>> _pendingStores = <String, Future<void>>{};

/// Serializes a complete read/modify/write transaction across viewer windows.
/// A separate lock file remains stable when the data file is replaced.
Future<T> withStoreLock<T>(File? file, Future<T> Function() body) async {
  if (file == null) return body();
  final String key = file.absolute.path.toLowerCase();
  final Set<String> held =
      Zone.current[_heldStores] as Set<String>? ?? <String>{};
  if (held.contains(key)) return body();
  final Future<void> previous = _pendingStores[key] ?? Future<void>.value();
  final Completer<void> complete = Completer<void>();
  _pendingStores[key] = complete.future;
  await previous;
  RandomAccessFile? lock;
  try {
    await file.parent.create(recursive: true);
    lock = await File('${file.path}.lock').open(mode: FileMode.append);
    await lock.lock(FileLock.blockingExclusive);
    return await runZoned(
      body,
      zoneValues: <Object, Object>{
        _heldStores: <String>{...held, key},
      },
    );
  } finally {
    try {
      if (lock != null) await lock.close();
    } finally {
      complete.complete();
      if (identical(_pendingStores[key], complete.future)) {
        _pendingStores.remove(key);
      }
    }
  }
}

T withStoreLockSync<T>(File file, T Function() body) {
  final String key = file.absolute.path.toLowerCase();
  final Set<String> held =
      Zone.current[_heldStores] as Set<String>? ?? <String>{};
  if (held.contains(key)) return body();
  file.parent.createSync(recursive: true);
  final RandomAccessFile lock = File(
    '${file.path}.lock',
  ).openSync(mode: FileMode.append);
  try {
    lock.lockSync(FileLock.blockingExclusive);
    return runZoned(
      body,
      zoneValues: <Object, Object>{
        _heldStores: <String>{...held, key},
      },
    );
  } finally {
    lock.closeSync();
  }
}
