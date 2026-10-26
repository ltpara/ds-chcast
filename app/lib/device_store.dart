import 'dart:convert';
import 'dart:io';

import 'server_settings.dart';
import 'wake_on_lan.dart';

/// One address-book or recent-connection entry.
///
/// [server] empty means a direct `host:port` in [id]. Otherwise [id] is the
/// host id registered with [server] and the tunnel agent resolves the actual
/// candidate and relay addresses.
class Device {
  const Device({
    required this.id,
    required this.addedAt,
    this.name = '',
    this.server = '',
    this.tags = const <String>[],
    this.groupId = '',
    this.tagsCleared = false,
    this.groupCleared = false,
    this.saved = false,
    this.mac = '',
    this.revision = 0,
    this.lastUsed,
  });

  final String id;
  final String name;
  final String server;

  /// Normalized Wake-on-LAN address. Stored only on this device; the account
  /// server has no MAC field, so it is not synced.
  final String mac;

  /// Account address-book tags. Kept locally so they survive offline use and
  /// can be merged with the server copy on the next sync.
  final List<String> tags;

  /// Account address-book group. Empty means ungrouped. The group name lives
  /// on the server; only the id is cached locally for sync.
  final String groupId;

  /// The user emptied the tags while offline; an empty list is an explicit
  /// intent to clear the server copy, not "no local preference". Cleared once
  /// the upload succeeds.
  final bool tagsCleared;

  /// The user moved the peer out of its group while offline. Same lifecycle
  /// as [tagsCleared].
  final bool groupCleared;
  final bool saved;

  /// Server revision of the address-book row this entry was last synced from.
  /// Zero means unknown, so the next upload omits the concurrency token.
  final int revision;
  final DateTime? lastUsed;
  final DateTime addedAt;

  bool get viaServer => server.isNotEmpty;

  /// Stable identity inside the store; a host id is only unique per server.
  String get key => viaServer ? '$server|$id' : 'direct|$id';

  String get trustKey => viaServer ? 'server:$server/$id' : id;

  String get displayName => name.isEmpty ? id : name;

  String get detail {
    if (!viaServer) {
      return id;
    }
    return name.isEmpty ? id : '$id · $server';
  }

  DateTime get sortTime => lastUsed ?? addedAt;

  Device copyWith({
    String? id,
    String? name,
    String? server,
    List<String>? tags,
    String? groupId,
    bool? tagsCleared,
    bool? groupCleared,
    bool? saved,
    String? mac,
    int? revision,
    DateTime? lastUsed,
    DateTime? addedAt,
  }) {
    return Device(
      id: id ?? this.id,
      name: name ?? this.name,
      server: server ?? this.server,
      tags: tags ?? this.tags,
      groupId: groupId ?? this.groupId,
      tagsCleared: tagsCleared ?? this.tagsCleared,
      groupCleared: groupCleared ?? this.groupCleared,
      saved: saved ?? this.saved,
      mac: mac ?? this.mac,
      revision: revision ?? this.revision,
      lastUsed: lastUsed ?? this.lastUsed,
      addedAt: addedAt ?? this.addedAt,
    );
  }

  Map<String, Object?> toJson() => <String, Object?>{
        'id': id,
        'name': name,
        'server': server,
        'tags': tags,
        'groupId': groupId,
        'tagsCleared': tagsCleared,
        'groupCleared': groupCleared,
        'saved': saved,
        'mac': mac,
        'revision': revision,
        'lastUsed': lastUsed?.millisecondsSinceEpoch,
        'addedAt': addedAt.millisecondsSinceEpoch,
      };

  /// Trims and de-duplicates tags, matching the server's limits so a local
  /// edit cannot create a row the server would silently rewrite.
  static List<String> cleanTags(Iterable<String> value) {
    final List<String> tags = <String>[];
    final Set<String> seen = <String>{};
    for (final String raw in value) {
      final String tag = raw.trim();
      if (tag.isEmpty || !seen.add(tag)) {
        continue;
      }
      if (tags.length == 16) {
        break;
      }
      tags.add(tag);
    }
    return tags;
  }

  static Device? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final Object? rawId = value['id'];
    if (rawId is! String || rawId.trim().isEmpty) {
      return null;
    }
    final String id = rawId.trim();
    final String rawServer = value['server']?.toString() ?? '';
    final String server = rawServer.isEmpty ? '' : normalizeServerUrl(rawServer);
    if (rawServer.isNotEmpty && server.isEmpty) {
      // A broken server field must not silently turn a host id into a direct
      // address (which would be parsed as a hostname and dialed wrong).
      return null;
    }
    final Object? rawName = value['name'];
    final Object? rawTags = value['tags'];
    final Object? rawRevision = value['revision'];
    final Object? rawLastUsed = value['lastUsed'];
    final Object? rawAddedAt = value['addedAt'];
    return Device(
      id: id,
      name: rawName is String ? rawName.trim() : '',
      server: server,
      tags: cleanTags(<String>[
        if (rawTags is List)
          for (final Object? tag in rawTags)
            if (tag is String) tag,
      ]),
      groupId: value['groupId']?.toString().trim() ?? '',
      tagsCleared: value['tagsCleared'] == true,
      groupCleared: value['groupCleared'] == true,
      saved: value['saved'] == true,
      mac: normalizeMac(value['mac']?.toString() ?? '') ?? '',
      revision: rawRevision is num && rawRevision >= 0
          ? rawRevision.toInt()
          : 0,
      lastUsed: rawLastUsed is num
          ? DateTime.fromMillisecondsSinceEpoch(rawLastUsed.toInt())
          : null,
      addedAt: rawAddedAt is num
          ? DateTime.fromMillisecondsSinceEpoch(rawAddedAt.toInt())
          : DateTime.fromMillisecondsSinceEpoch(0),
    );
  }
}

/// A deletion that could not reach the account server yet.
///
/// The server and username are part of the identity: retrying with a different
/// account could delete a peer that account still owns.
class PendingDeletion {
  const PendingDeletion({
    required this.server,
    required this.username,
    required this.id,
    required this.deletedAt,
  });

  final String server;
  final String username;
  final String id;
  final DateTime deletedAt;

  String get key => '$server|$username|$id';

  Map<String, Object?> toJson() => <String, Object?>{
        'server': server,
        'username': username,
        'id': id,
        'deletedAt': deletedAt.millisecondsSinceEpoch,
      };

  static PendingDeletion? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final Object? rawServer = value['server'];
    final Object? rawUsername = value['username'];
    final Object? rawId = value['id'];
    if (rawServer is! String || rawUsername is! String || rawId is! String) {
      return null;
    }
    final String server = normalizeServerUrl(rawServer);
    final String username = rawUsername.trim();
    final String id = rawId.trim();
    if (server.isEmpty || username.isEmpty || id.isEmpty) {
      return null;
    }
    final Object? rawDeletedAt = value['deletedAt'];
    return PendingDeletion(
      server: server,
      username: username,
      id: id,
      deletedAt: rawDeletedAt is num
          ? DateTime.fromMillisecondsSinceEpoch(rawDeletedAt.toInt())
          : DateTime.fromMillisecondsSinceEpoch(0),
    );
  }
}

/// Address book plus recent connections in one file.
///
/// Saved entries are the device list; unsaved entries with a lastUsed time are
/// the recent list and are capped at [maxRecent]. Keeping both in one store
/// means saving a recent host is a flag change, not a copy between files.
/// Unfinished account deletions live in a sibling outbox so an offline delete
/// is retried instead of being pulled back on the next sync.
class DeviceStore {
  DeviceStore({File? file, File? deletionsFile})
      : _injected = file,
        _injectedDeletions = deletionsFile;

  static const int maxRecent = 8;

  final File? _injected;
  final File? _injectedDeletions;

  File? _file() {
    if (_injected != null) {
      return _injected;
    }
    final String? appData = Platform.environment['APPDATA'];
    if (appData == null || appData.isEmpty) {
      return null;
    }
    return File('$appData\\chast\\devices.json');
  }

  File? _deletionsFile() {
    if (_injectedDeletions != null) {
      return _injectedDeletions;
    }
    final File? devices = _file();
    if (devices == null) {
      return null;
    }
    return File(
      '${devices.parent.path}${Platform.pathSeparator}pending_deletions.json',
    );
  }

  Future<List<Device>> load() async {
    final File? file = _file();
    if (file == null || !await file.exists()) {
      return <Device>[];
    }
    try {
      final Object? decoded = jsonDecode(await file.readAsString());
      if (decoded is! List) {
        return <Device>[];
      }
      final List<Device> devices = <Device>[
        for (final Object? item in decoded)
          if (Device.fromJson(item) case final Device device) device,
      ];
      devices.sort((Device a, Device b) => b.sortTime.compareTo(a.sortTime));
      return devices;
    } catch (_) {
      return <Device>[];
    }
  }

  /// Inserts or updates [device] and marks it used now. A saved entry keeps
  /// its saved flag and name when an ad-hoc connection touches it.
  Future<List<Device>> touch(Device device) async {
    final List<Device> devices = await load();
    final int index = devices.indexWhere((Device item) => item.key == device.key);
    final DateTime now = DateTime.now();
    if (index >= 0) {
      final Device old = devices[index];
      final List<String> tags = device.tags.isEmpty ? old.tags : device.tags;
      devices[index] = Device(
        id: device.id,
        name: device.name.isEmpty ? old.name : device.name,
        server: device.server,
        tags: tags,
        groupId: old.groupId,
        tagsCleared: tags.isEmpty ? old.tagsCleared : false,
        groupCleared: old.groupCleared,
        saved: old.saved || device.saved,
        mac: device.mac.isNotEmpty ? device.mac : old.mac,
        revision: old.revision,
        lastUsed: now,
        addedAt: old.addedAt,
      );
    } else {
      devices.add(Device(
        id: device.id,
        name: device.name,
        server: device.server,
        tags: Device.cleanTags(device.tags),
        saved: false,
        mac: device.mac,
        lastUsed: now,
        addedAt: now,
      ));
    }
    return _save(devices);
  }

  /// Adds or updates an address-book entry without marking it as recently
  /// used; a newly saved host has never been connected from this app.
  Future<List<Device>> save(Device device) async {
    final List<Device> devices = await load();
    final int index = devices.indexWhere((Device item) => item.key == device.key);
    final DateTime now = DateTime.now();
    if (index >= 0) {
      final Device old = devices[index];
      final List<String> tags =
          device.tags.isEmpty ? old.tags : Device.cleanTags(device.tags);
      final String groupId =
          old.groupId.isEmpty ? device.groupId.trim() : old.groupId;
      devices[index] = Device(
        id: device.id,
        name: device.name.isEmpty ? old.name : device.name,
        server: device.server,
        tags: tags,
        groupId: groupId,
        tagsCleared: tags.isEmpty ? old.tagsCleared : false,
        groupCleared: groupId.isEmpty ? old.groupCleared : false,
        saved: true,
        mac: device.mac.isNotEmpty ? device.mac : old.mac,
        revision: old.revision,
        lastUsed: old.lastUsed,
        addedAt: old.addedAt,
      );
    } else {
      devices.add(Device(
        id: device.id,
        name: device.name,
        server: device.server,
        tags: Device.cleanTags(device.tags),
        groupId: device.groupId.trim(),
        saved: true,
        mac: device.mac,
        revision: device.revision,
        addedAt: now,
      ));
    }
    return _save(devices);
  }

  Future<List<Device>> promote(String key) async {
    final List<Device> devices = await load();
    final int index = devices.indexWhere((Device item) => item.key == key);
    if (index >= 0) {
      devices[index] = devices[index].copyWith(saved: true);
    }
    return _save(devices);
  }

  Future<List<Device>> rename(String key, String name) async {
    final List<Device> devices = await load();
    final int index = devices.indexWhere((Device item) => item.key == key);
    if (index >= 0) {
      devices[index] = devices[index].copyWith(name: name.trim());
    }
    return _save(devices);
  }

  /// Stores a Wake-on-LAN address locally; an invalid value clears it.
  Future<List<Device>> updateMac(String key, String mac) async {
    final List<Device> devices = await load();
    final int index = devices.indexWhere((Device item) => item.key == key);
    if (index >= 0) {
      devices[index] = devices[index].copyWith(mac: normalizeMac(mac) ?? '');
    }
    return _save(devices);
  }

  Future<List<Device>> updateTags(String key, List<String> tags) async {
    final List<Device> devices = await load();
    final int index = devices.indexWhere((Device item) => item.key == key);
    if (index >= 0) {
      final List<String> clean = Device.cleanTags(tags);
      devices[index] = devices[index].copyWith(
        tags: clean,
        tagsCleared: clean.isEmpty,
      );
    }
    return _save(devices);
  }

  /// Moves one saved entry into [groupId]; an empty id removes it from its
  /// group. Group names are resolved from the server, so only the id is kept.
  Future<List<Device>> updateGroup(String key, String groupId) async {
    final List<Device> devices = await load();
    final int index = devices.indexWhere((Device item) => item.key == key);
    if (index >= 0) {
      final String value = groupId.trim();
      devices[index] = devices[index].copyWith(
        groupId: value,
        groupCleared: value.isEmpty,
      );
    }
    return _save(devices);
  }

  /// Records that a clear intent reached the server, so the next sync does not
  /// replay it over a newer edit from another device.
  Future<List<Device>> markPushSettled(
    String key, {
    bool tags = false,
    bool group = false,
  }) async {
    if (!tags && !group) {
      return load();
    }
    final List<Device> devices = await load();
    final int index = devices.indexWhere((Device item) => item.key == key);
    if (index >= 0) {
      devices[index] = devices[index].copyWith(
        tagsCleared: tags ? false : null,
        groupCleared: group ? false : null,
      );
    }
    return _save(devices);
  }

  /// Records the revision returned by a successful address-book upload so a
  /// follow-up edit carries the new concurrency token instead of the old one.
  Future<List<Device>> setPeerRevision(String key, int revision) async {
    if (revision <= 0) {
      return load();
    }
    final List<Device> devices = await load();
    final int index = devices.indexWhere((Device item) => item.key == key);
    if (index >= 0) {
      devices[index] = devices[index].copyWith(revision: revision);
    }
    return _save(devices);
  }

  Future<List<Device>> remove(String key) async {
    final List<Device> devices = await load();
    devices.removeWhere((Device item) => item.key == key);
    return _save(devices);
  }

  /// Removes ad-hoc entries but keeps the address book.
  Future<List<Device>> clearRecent() async {
    final List<Device> devices = await load();
    devices.removeWhere((Device item) => !item.saved);
    return _save(devices);
  }

  /// Replaces the whole store; account sync builds the merged list itself.
  Future<List<Device>> replaceAll(List<Device> devices) {
    return _save(List<Device>.of(devices));
  }

  Future<List<PendingDeletion>> loadPendingDeletions() async {
    final File? file = _deletionsFile();
    if (file == null || !await file.exists()) {
      return <PendingDeletion>[];
    }
    try {
      final Object? decoded = jsonDecode(await file.readAsString());
      if (decoded is! List) {
        return <PendingDeletion>[];
      }
      final List<PendingDeletion> deletions = <PendingDeletion>[
        for (final Object? item in decoded)
          if (PendingDeletion.fromJson(item) case final PendingDeletion item)
            item,
      ];
      deletions.sort(
        (PendingDeletion a, PendingDeletion b) =>
            a.deletedAt.compareTo(b.deletedAt),
      );
      return deletions;
    } catch (_) {
      return <PendingDeletion>[];
    }
  }

  Future<List<PendingDeletion>> queueDeletion(PendingDeletion deletion) async {
    final List<PendingDeletion> deletions = await loadPendingDeletions();
    if (deletions.every((PendingDeletion item) => item.key != deletion.key)) {
      deletions.add(deletion);
    }
    return _savePendingDeletions(deletions);
  }

  Future<List<PendingDeletion>> removePendingDeletion(String key) async {
    final List<PendingDeletion> deletions = await loadPendingDeletions();
    deletions.removeWhere((PendingDeletion item) => item.key == key);
    return _savePendingDeletions(deletions);
  }

  Future<List<PendingDeletion>> _savePendingDeletions(
      List<PendingDeletion> deletions) async {
    final File? file = _deletionsFile();
    if (file != null) {
      try {
        await file.parent.create(recursive: true);
        await file.writeAsString(jsonEncode(<Object?>[
          for (final PendingDeletion deletion in deletions) deletion.toJson(),
        ]));
      } catch (_) {
        // Retrying next sync is best effort, like the address book itself.
      }
    }
    return deletions;
  }

  Future<List<Device>> _save(List<Device> devices) async {
    // Only recent entries are capped; the address book is user-managed.
    final List<Device> recent = devices.where((Device item) => !item.saved).toList()
      ..sort((Device a, Device b) => b.sortTime.compareTo(a.sortTime));
    if (recent.length > maxRecent) {
      final Set<String> drop = recent
          .skip(maxRecent)
          .map((Device device) => device.key)
          .toSet();
      devices.removeWhere((Device device) => drop.contains(device.key));
    }
    devices.sort((Device a, Device b) => b.sortTime.compareTo(a.sortTime));
    final File? file = _file();
    if (file != null) {
      try {
        await file.parent.create(recursive: true);
        await file.writeAsString(jsonEncode(<Object?>[
          for (final Device device in devices) device.toJson(),
        ]));
      } catch (_) {
        // The address book is a convenience; failing to persist it must not
        // block a connection.
      }
    }
    return devices;
  }
}
