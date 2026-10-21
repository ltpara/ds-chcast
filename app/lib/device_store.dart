import 'dart:convert';
import 'dart:io';

import 'server_settings.dart';

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
    this.saved = false,
    this.lastUsed,
  });

  final String id;
  final String name;
  final String server;
  final bool saved;
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
    bool? saved,
    DateTime? lastUsed,
    DateTime? addedAt,
  }) {
    return Device(
      id: id ?? this.id,
      name: name ?? this.name,
      server: server ?? this.server,
      saved: saved ?? this.saved,
      lastUsed: lastUsed ?? this.lastUsed,
      addedAt: addedAt ?? this.addedAt,
    );
  }

  Map<String, Object?> toJson() => <String, Object?>{
        'id': id,
        'name': name,
        'server': server,
        'saved': saved,
        'lastUsed': lastUsed?.millisecondsSinceEpoch,
        'addedAt': addedAt.millisecondsSinceEpoch,
      };

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
    final Object? rawLastUsed = value['lastUsed'];
    final Object? rawAddedAt = value['addedAt'];
    return Device(
      id: id,
      name: rawName is String ? rawName.trim() : '',
      server: server,
      saved: value['saved'] == true,
      lastUsed: rawLastUsed is num
          ? DateTime.fromMillisecondsSinceEpoch(rawLastUsed.toInt())
          : null,
      addedAt: rawAddedAt is num
          ? DateTime.fromMillisecondsSinceEpoch(rawAddedAt.toInt())
          : DateTime.fromMillisecondsSinceEpoch(0),
    );
  }
}

/// Address book plus recent connections in one file.
///
/// Saved entries are the device list; unsaved entries with a lastUsed time are
/// the recent list and are capped at [maxRecent]. Keeping both in one store
/// means saving a recent host is a flag change, not a copy between files.
class DeviceStore {
  DeviceStore({File? file}) : _injected = file;

  static const int maxRecent = 8;

  final File? _injected;

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
      devices[index] = Device(
        id: device.id,
        name: device.name.isEmpty ? old.name : device.name,
        server: device.server,
        saved: old.saved || device.saved,
        lastUsed: now,
        addedAt: old.addedAt,
      );
    } else {
      devices.add(Device(
        id: device.id,
        name: device.name,
        server: device.server,
        saved: false,
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
      devices[index] = Device(
        id: device.id,
        name: device.name.isEmpty ? old.name : device.name,
        server: device.server,
        saved: true,
        lastUsed: old.lastUsed,
        addedAt: old.addedAt,
      );
    } else {
      devices.add(Device(
        id: device.id,
        name: device.name,
        server: device.server,
        saved: true,
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
