import 'device_store.dart';
import 'server_api.dart';

/// A local entry that should be written back to the account address book.
///
/// A `null` field is omitted from the request, so a name-only edit never
/// clears server tags (and vice versa). A missing peer always carries a name,
/// possibly empty, to create the row.
class AddressBookPush {
  const AddressBookPush({
    required this.id,
    this.name,
    this.tags,
    this.groupId,
    this.revision = 0,
  });

  final String id;
  final String? name;
  final List<String>? tags;
  final String? groupId;

  /// Server revision the edit is based on; zero means the peer is new locally
  /// and the upload has no concurrency token.
  final int revision;
}

class AddressBookPlan {
  const AddressBookPlan({
    required this.devices,
    required this.pushes,
    required this.added,
    required this.updated,
    required this.removed,
  });

  final List<Device> devices;
  final List<AddressBookPush> pushes;
  final int added;
  final int updated;
  final int removed;
}

/// Merges the server address book into the local device list.
///
/// Pull rules: a server peer missing locally becomes a saved device; a saved
/// device with no local name takes the server name, and empty local tags take
/// the server tags; an unsaved recent entry is promoted to the address book.
///
/// Deletion rules: a tombstone from [tombstones] removes the matching saved
/// device and blocks it from the push list, so a peer deleted on another
/// viewer is not resurrected by this device's stale local copy. A local
/// deletion that has not reached the server is passed in
/// [pendingDeletions]; it blocks the peer even while the server still reports
/// it alive, so a failed delete is not undone by the next pull. Neither kind
/// touches unsaved recents, which were never part of the account book.
///
/// Push rules: every saved device of [serverUrl] missing from the server is
/// uploaded, and a non-empty local name or tag list wins over a different
/// server value. Empty tags or group normally mean "no preference" and are
/// never uploaded; [Device.tagsCleared] and [Device.groupCleared] record an
/// explicit offline clear and push the empty value until it succeeds. A clear
/// whose server row already matches is settled without an upload.
///
/// Group rules: the server group wins while the local entry is ungrouped, and
/// a local group id that no longer exists on the server is dropped. A live
/// local group assignment wins over a different server value and is pushed.
AddressBookPlan planAddressBookSync({
  required List<Device> devices,
  required List<AddressBookPeer> peers,
  List<AddressBookGroup> groups = const <AddressBookGroup>[],
  List<AddressBookPeer> tombstones = const <AddressBookPeer>[],
  Set<String> pendingDeletions = const <String>{},
  required String serverUrl,
  required DateTime now,
}) {
  final Set<String> liveGroups = <String>{
    for (final AddressBookGroup group in groups)
      if (!group.deleted) group.id,
  };
  final List<Device> normalized = <Device>[
    for (final Device device in devices)
      _normalizeDevice(device, serverUrl, liveGroups),
  ];
  final Map<String, AddressBookPeer> peerById = <String, AddressBookPeer>{
    for (final AddressBookPeer peer in peers) peer.id: peer,
  };
  final Set<String> tombstoned = <String>{
    for (final AddressBookPeer peer in tombstones)
      if (peer.deleted && !peerById.containsKey(peer.id)) peer.id,
  };
  final Set<String> blocked = <String>{...tombstoned, ...pendingDeletions};
  final List<Device> merged = List<Device>.of(normalized);
  int added = 0;
  int updated = 0;
  int removed = 0;

  merged.removeWhere((Device device) {
    final bool remove = device.saved &&
        device.server == serverUrl &&
        blocked.contains(device.id);
    if (remove) {
      removed++;
    }
    return remove;
  });

  for (final AddressBookPeer peer in peers) {
    if (pendingDeletions.contains(peer.id)) {
      continue;
    }
    final String key =
        Device(id: peer.id, server: serverUrl, addedAt: now).key;
    final int index = merged.indexWhere((Device device) => device.key == key);
    if (index < 0) {
      merged.add(Device(
        id: peer.id,
        name: peer.name,
        server: serverUrl,
        tags: peer.tags,
        groupId: peer.groupId,
        revision: peer.revision,
        saved: true,
        addedAt: now,
      ));
      added++;
      continue;
    }
    final Device old = merged[index];
    bool changed = false;
    bool promoted = false;
    String name = old.name;
    List<String> tags = old.tags;
    String groupId = old.groupId;
    if (peer.name.isNotEmpty && old.name.isEmpty) {
      name = peer.name;
      changed = true;
    }
    if (peer.tags.isNotEmpty && old.tags.isEmpty && !old.tagsCleared) {
      tags = peer.tags;
      changed = true;
    }
    if (peer.groupId.isNotEmpty && old.groupId.isEmpty && !old.groupCleared) {
      groupId = peer.groupId;
      changed = true;
    }
    if (!old.saved) {
      promoted = true;
    }
    if (changed || promoted || old.revision != peer.revision) {
      merged[index] = old.copyWith(
        name: name,
        tags: tags,
        groupId: groupId,
        revision: peer.revision,
        saved: true,
      );
      if (changed || promoted) {
        updated++;
      }
    }
  }

  final List<AddressBookPush> pushes = <AddressBookPush>[];
  for (final Device device in normalized) {
    if (!device.saved || device.server != serverUrl) {
      continue;
    }
    if (blocked.contains(device.id)) {
      continue;
    }
    final AddressBookPeer? peer = peerById[device.id];
    if (peer == null) {
      pushes.add(AddressBookPush(
        id: device.id,
        name: device.name,
        tags: device.tags.isEmpty ? null : device.tags,
        groupId: device.groupId.isEmpty ? null : device.groupId,
      ));
      continue;
    }
    final String? name =
        device.name.isNotEmpty && device.name != peer.name ? device.name : null;
    final List<String>? tags;
    if (device.tagsCleared && peer.tags.isNotEmpty) {
      tags = const <String>[];
    } else if (device.tags.isNotEmpty && !_sameTags(device.tags, peer.tags)) {
      tags = device.tags;
    } else {
      tags = null;
    }
    final String? groupId;
    if (device.groupCleared && peer.groupId.isNotEmpty) {
      groupId = '';
    } else if (device.groupId.isNotEmpty && device.groupId != peer.groupId) {
      groupId = device.groupId;
    } else {
      groupId = null;
    }
    if (name != null || tags != null || groupId != null) {
      pushes.add(AddressBookPush(
        id: device.id,
        name: name,
        tags: tags,
        groupId: groupId,
        revision: peer.revision,
      ));
    }
  }

  final List<Device> settled = <Device>[
    for (final Device device in merged)
      _settleConfirmedClear(device, serverUrl, peerById[device.id]),
  ];
  settled.sort((Device a, Device b) => b.sortTime.compareTo(a.sortTime));
  return AddressBookPlan(
    devices: settled,
    pushes: pushes,
    added: added,
    updated: updated,
    removed: removed,
  );
}

Device _normalizeDevice(
  Device device,
  String serverUrl,
  Set<String> liveGroups,
) {
  Device result = device;
  if (result.tags.isNotEmpty && result.tagsCleared) {
    result = result.copyWith(tagsCleared: false);
  }
  if (result.groupId.isNotEmpty && result.groupCleared) {
    result = result.copyWith(groupCleared: false);
  }
  if (result.saved &&
      result.server == serverUrl &&
      result.groupId.isNotEmpty &&
      !liveGroups.contains(result.groupId)) {
    result = result.copyWith(groupId: '');
  }
  return result;
}

/// Drops an offline clear marker once the server row already matches it. A
/// marker whose clear still needs an upload stays set until the caller reports
/// success, so a failed request is retried on the next sync.
Device _settleConfirmedClear(
  Device device,
  String serverUrl,
  AddressBookPeer? peer,
) {
  if (!device.saved || device.server != serverUrl) {
    return device;
  }
  Device result = device;
  if (result.tagsCleared && (peer == null || peer.tags.isEmpty)) {
    result = result.copyWith(tagsCleared: false);
  }
  if (result.groupCleared && (peer == null || peer.groupId.isEmpty)) {
    result = result.copyWith(groupCleared: false);
  }
  return result;
}

bool _sameTags(List<String> local, List<String> server) {
  if (local.length != server.length) {
    return false;
  }
  for (int i = 0; i < local.length; i++) {
    if (local[i] != server[i]) {
      return false;
    }
  }
  return true;
}

/// Applies one address-book read to the cached group list. A full read (or a
/// server revision behind the cached cursor, which means the database was
/// recreated) replaces the list; an incremental read applies group changes.
List<AddressBookGroup> mergeAddressBookGroups({
  required List<AddressBookGroup> current,
  required AddressBookSnapshot snapshot,
  required int since,
}) {
  if (since <= 0 || since > snapshot.revision) {
    return List<AddressBookGroup>.of(snapshot.groups);
  }
  final Map<String, AddressBookGroup> byId = <String, AddressBookGroup>{
    for (final AddressBookGroup group in current) group.id: group,
  };
  for (final AddressBookGroup change in snapshot.groupChanges) {
    if (change.deleted) {
      byId.remove(change.id);
    } else {
      byId[change.id] = change;
    }
  }
  final List<AddressBookGroup> merged = byId.values.toList()
    ..sort((AddressBookGroup a, AddressBookGroup b) {
      final int byName = a.name.compareTo(b.name);
      return byName != 0 ? byName : a.id.compareTo(b.id);
    });
  return merged;
}
