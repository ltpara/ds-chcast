import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/recording_retention.dart';

void main() {
  late Directory dir;

  setUp(() async {
    dir = await Directory.systemTemp.createTemp('chast-recordings-');
  });

  tearDown(() async {
    await dir.delete(recursive: true);
  });

  File recording(String name, int minute) {
    final File file = File('${dir.path}${Platform.pathSeparator}$name')
      ..writeAsStringSync('mp4');
    file.setLastModifiedSync(DateTime(2026, 11, 10, 5, minute));
    return file;
  }

  test(
    'keeps the newest recording groups and removes whole old groups',
    () async {
      final File oldest = recording('chast-20261110-053000.mp4', 30);
      final File olderSegment = recording('chast-20261110-053000-2.mp4', 31);
      final File middle = recording('chast-20261110-053100.mp4', 31);
      final File newest = recording('chast-20261110-053200.mp4', 32);
      final File newestSegment = recording('chast-20261110-053200-2.mp4', 33);

      final int removed = await pruneRecordings(dir.path, 2);

      expect(removed, 2);
      expect(oldest.existsSync(), isFalse);
      expect(olderSegment.existsSync(), isFalse);
      expect(middle.existsSync(), isTrue);
      expect(newest.existsSync(), isTrue);
      expect(newestSegment.existsSync(), isTrue);
    },
  );

  test('groups rollover segments by their stamp, not by file order', () async {
    final File oneA = recording('chast-20261110-053000.mp4', 30);
    final File oneB = recording('chast-20261110-053000-2.mp4', 40);
    final File two = recording('chast-20261110-053100.mp4', 35);

    // The first group has the newest segment but the older stamp; ordering by
    // stamp keeps the later session and deletes both segments of the older.
    await pruneRecordings(dir.path, 1);

    expect(oneA.existsSync(), isFalse);
    expect(oneB.existsSync(), isFalse);
    expect(two.existsSync(), isTrue);
  });

  test(
    'a protected file keeps its whole group and other files are untouched',
    () async {
      final File oldest = recording('chast-20261110-053000.mp4', 30);
      final File oldestSegment = recording('chast-20261110-053000-2.mp4', 31);
      final File newest = recording('chast-20261110-053200.mp4', 32);
      final File other = recording('holiday.mp4', 10);
      final File note = File('${dir.path}${Platform.pathSeparator}notes.txt')
        ..writeAsStringSync('keep');

      final int removed = await pruneRecordings(
        dir.path,
        1,
        protect: <String>{oldest.path},
      );

      expect(removed, 0);
      expect(oldest.existsSync(), isTrue);
      expect(oldestSegment.existsSync(), isTrue);
      expect(newest.existsSync(), isTrue);
      expect(other.existsSync(), isTrue);
      expect(note.existsSync(), isTrue);
    },
  );

  test('zero keeps everything and a missing folder is a no-op', () async {
    final File file = recording('chast-20261110-053000.mp4', 30);
    expect(await pruneRecordings(dir.path, 0), 0);
    expect(file.existsSync(), isTrue);

    final Directory missing = Directory(
      '${dir.path}${Platform.pathSeparator}gone',
    );
    expect(await pruneRecordings(missing.path, 1), 0);
  });

  test('names that are not viewer recordings are ignored', () async {
    final File near = recording('chast-2026.mp4', 10);
    final File similar = recording('chast-20261110-053000.mp4.bak', 11);
    final File newest = recording('chast-20261110-053000.mp4', 30);

    expect(await pruneRecordings(dir.path, 1), 0);
    expect(near.existsSync(), isTrue);
    expect(similar.existsSync(), isTrue);
    expect(newest.existsSync(), isTrue);
  });
}
