import 'dart:convert';
import 'dart:io';

class RecentEntry {
  const RecentEntry(this.target, this.lastUsed);

  final String target;
  final DateTime lastUsed;

  Map<String, Object?> toJson() => <String, Object?>{
        'target': target,
        'lastUsed': lastUsed.millisecondsSinceEpoch,
      };

  static RecentEntry? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final Object? target = value['target'];
    final Object? seconds = value['lastUsed'];
    if (target is! String || target.isEmpty || seconds is! num) {
      return null;
    }
    return RecentEntry(
      target,
      DateTime.fromMillisecondsSinceEpoch(seconds.toInt()),
    );
  }
}

class RecentStore {
  static const int maxEntries = 8;

  File? _file() {
    final String? appData = Platform.environment['APPDATA'];
    if (appData == null || appData.isEmpty) {
      return null;
    }
    return File('$appData\\chast\\recent.json');
  }

  Future<List<RecentEntry>> load() async {
    final File? file = _file();
    if (file == null || !await file.exists()) {
      return <RecentEntry>[];
    }
    try {
      final Object? decoded = jsonDecode(await file.readAsString());
      if (decoded is! List) {
        return <RecentEntry>[];
      }
      final List<RecentEntry> entries = <RecentEntry>[
        for (final Object? item in decoded)
          if (RecentEntry.fromJson(item) case final RecentEntry entry) entry,
      ];
      entries.sort((RecentEntry a, RecentEntry b) => b.lastUsed.compareTo(a.lastUsed));
      return entries.take(maxEntries).toList();
    } catch (_) {
      return <RecentEntry>[];
    }
  }

  Future<List<RecentEntry>> add(String target) async {
    final List<RecentEntry> entries = await load();
    entries.removeWhere((RecentEntry entry) => entry.target == target);
    entries.insert(0, RecentEntry(target, DateTime.now()));
    final List<RecentEntry> trimmed = entries.take(maxEntries).toList();
    await _save(trimmed);
    return trimmed;
  }

  Future<List<RecentEntry>> clear() async {
    await _save(<RecentEntry>[]);
    return <RecentEntry>[];
  }

  Future<void> _save(List<RecentEntry> entries) async {
    final File? file = _file();
    if (file == null) {
      return;
    }
    try {
      await file.parent.create(recursive: true);
      await file.writeAsString(
        jsonEncode(<Object?>[for (final RecentEntry entry in entries) entry.toJson()]),
      );
    } catch (_) {
      // Recent connections are a convenience; failing to persist them must not
      // block connecting.
    }
  }
}
