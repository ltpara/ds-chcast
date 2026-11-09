import 'dart:io';

/// Applies the viewer's opt-in "keep the newest N recordings" policy.
///
/// A recording is one MP4 plus its rollover segments:
/// `chast-20261110-053000.mp4`, `chast-20261110-053000-2.mp4`, ... Groups are
/// ordered by the timestamp in the name, so a long multi-segment session
/// counts as one recording. Only files the viewer names itself are considered;
/// anything else in the folder is left alone, and [keep] below one keeps
/// everything.
///
/// [protect] lists files that must survive this pass even when their group is
/// old (a recording still open, or an upload in flight). If one file of a
/// group is protected the whole group stays, because its segments belong
/// together. Returns how many files were deleted.
Future<int> pruneRecordings(
  String directory,
  int keep, {
  Set<String> protect = const <String>{},
}) async {
  if (keep < 1) {
    return 0;
  }
  final Directory dir = Directory(directory);
  if (!await dir.exists()) {
    return 0;
  }
  final RegExp name = RegExp(
    r'^chast-(\d{8}-\d{6})(-\d+)?\.mp4$',
    caseSensitive: false,
  );
  final Map<String, List<File>> groups = <String, List<File>>{};
  await for (final FileSystemEntity entity in dir.list(followLinks: false)) {
    if (entity is! File) {
      continue;
    }
    final String fileName = entity.path.replaceAll('\\', '/').split('/').last;
    final Match? match = name.firstMatch(fileName);
    if (match == null) {
      continue;
    }
    groups.putIfAbsent(match.group(1)!, () => <File>[]).add(entity);
  }
  // The stamp is fixed width, so a descending string order is newest first.
  final List<String> order = groups.keys.toList()
    ..sort((String a, String b) => b.compareTo(a));
  final Set<String> protectedPaths = protect.map(_normalizePath).toSet();
  int removed = 0;
  for (int index = keep; index < order.length; index++) {
    final List<File> files = groups[order[index]]!;
    if (files.any(
      (File file) => protectedPaths.contains(_normalizePath(file.path)),
    )) {
      continue;
    }
    for (final File file in files) {
      try {
        await file.delete();
        removed++;
      } catch (_) {
        // A file another process holds open stays; the next pass retries.
      }
    }
  }
  return removed;
}

String _normalizePath(String path) => path.replaceAll('/', '\\').toLowerCase();
