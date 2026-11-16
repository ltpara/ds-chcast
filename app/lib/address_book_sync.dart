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
    this.note,
    this.groupId,
    this.revision = 0,
  });

  final String id;
  final String? name;
  final List<String>? tags;
  final String? note;
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

/// Merges a complete server snapshot with locally edited fields. Cached
/// fields accept remote changes (including clears). Only explicit local edits
/// or never-synced entries are pushed, using the revision they were based on.
/// Pending edits retain that baseline until a write succeeds or a conflict is
/// resolved; merely reading a newer revision must not authorize a stale write.
AddressBookPlan planAddressBookSync({
  required List<Device> devices,
  required List<AddressBookPeer> peers,
  List<AddressBookGroup> groups = const <AddressBookGroup>[],
  List<AddressBookPeer> tombstones = const <AddressBookPeer>[],
  Set<String> pendingDeletions = const <String>{},
  required String serverUrl,
  required DateTime now,
}) {
  final Set<String> liveGroups = {
    for (final group in groups)
      if (!group.deleted) group.id,
  };
  final Map<String, AddressBookPeer> peerById = {
    for (final peer in peers) peer.id: peer,
  };
  final Set<String> blocked = {
    ...pendingDeletions,
    for (final peer in tombstones)
      if (peer.deleted && !peerById.containsKey(peer.id)) peer.id,
  };
  final List<Device> merged = [];
  final List<AddressBookPush> pushes = [];
  final Set<String> seen = {};
  int added = 0;
  int updated = 0;
  int removed = 0;
  for (final original in devices) {
    if (original.server != serverUrl) {
      merged.add(original);
      continue;
    }
    if (original.saved && blocked.contains(original.id)) {
      removed++;
      continue;
    }
    seen.add(original.id);
    if (pendingDeletions.contains(original.id)) {
      merged.add(original);
      continue;
    }
    Device device = original;
    int dirty =
        device.dirtyFields |
        (device.tagsCleared && device.tags.isEmpty ? Device.tagsField : 0) |
        (device.noteCleared && device.note.isEmpty ? Device.noteField : 0) |
        (device.groupCleared && device.groupId.isEmpty ? Device.groupField : 0);
    // Preserve first-login imports, but never infer edits from a synced cache.
    if (device.revision == 0) {
      dirty |=
          (device.name.isNotEmpty ? Device.nameField : 0) |
          (device.tags.isNotEmpty ? Device.tagsField : 0) |
          (device.note.isNotEmpty ? Device.noteField : 0) |
          (device.groupId.isNotEmpty ? Device.groupField : 0);
    }
    if (device.groupId.isNotEmpty && !liveGroups.contains(device.groupId)) {
      device = device.copyWith(groupId: '');
      dirty &= ~Device.groupField;
    }
    final AddressBookPeer? peer = peerById[device.id];
    if (peer == null) {
      merged.add(device.copyWith(dirtyFields: dirty));
      if (device.saved && !blocked.contains(device.id)) {
        pushes.add(
          AddressBookPush(
            id: device.id,
            name: device.name,
            tags: device.tags.isEmpty ? null : device.tags,
            note: device.note.isEmpty ? null : Device.cleanNote(device.note),
            groupId: device.groupId.isEmpty ? null : device.groupId,
          ),
        );
      }
      continue;
    }
    // A matching server value acknowledges an edit even after a lost response.
    if (device.name == peer.name) dirty &= ~Device.nameField;
    if (Device.sameTags(device.tags, peer.tags)) dirty &= ~Device.tagsField;
    if (Device.cleanNote(device.note) == peer.note) dirty &= ~Device.noteField;
    if (device.groupId == peer.groupId) dirty &= ~Device.groupField;
    bool pending(int field) => dirty & field != 0;
    final Device result = device.copyWith(
      name: pending(Device.nameField) ? device.name : peer.name,
      tags: pending(Device.tagsField) ? device.tags : peer.tags,
      note: pending(Device.noteField)
          ? device.note
          : Device.cleanNote(peer.note),
      groupId: pending(Device.groupField) ? device.groupId : peer.groupId,
      position: peer.position,
      revision: dirty != 0 && device.revision > 0
          ? device.revision
          : peer.revision,
      dirtyFields: dirty,
      tagsCleared: pending(Device.tagsField) && device.tags.isEmpty,
      noteCleared: pending(Device.noteField) && device.note.isEmpty,
      groupCleared: pending(Device.groupField) && device.groupId.isEmpty,
      saved: true,
    );
    merged.add(result);
    if (original.name != result.name ||
        !Device.sameTags(original.tags, result.tags) ||
        original.note != result.note ||
        original.groupId != result.groupId ||
        !original.saved) {
      updated++;
    }
    if (device.saved && dirty != 0) {
      pushes.add(
        AddressBookPush(
          id: device.id,
          name: pending(Device.nameField) ? device.name : null,
          tags: pending(Device.tagsField) ? device.tags : null,
          note: pending(Device.noteField)
              ? Device.cleanNote(device.note)
              : null,
          groupId: pending(Device.groupField) ? device.groupId : null,
          revision: device.revision > 0 ? device.revision : peer.revision,
        ),
      );
    }
  }
  for (final peer in peers) {
    if (seen.contains(peer.id) || pendingDeletions.contains(peer.id)) continue;
    merged.add(
      Device(
        id: peer.id,
        server: serverUrl,
        name: peer.name,
        tags: peer.tags,
        note: Device.cleanNote(peer.note),
        groupId: peer.groupId,
        position: peer.position,
        revision: peer.revision,
        saved: true,
        addedAt: now,
      ),
    );
    added++;
  }
  merged.sort((Device a, Device b) => b.sortTime.compareTo(a.sortTime));
  return AddressBookPlan(
    devices: merged,
    pushes: pushes,
    added: added,
    updated: updated,
    removed: removed,
  );
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
    ..sort(compareAddressBookGroups);
  return merged;
}

/// Orders sibling groups the way the server does: manual position first,
/// then name, then id.
int compareAddressBookGroups(AddressBookGroup a, AddressBookGroup b) {
  if (a.position != b.position) {
    return a.position.compareTo(b.position);
  }
  final int byName = a.name.compareTo(b.name);
  return byName != 0 ? byName : a.id.compareTo(b.id);
}

/// One group in depth-first tree order, with [depth] zero at the root.
class AddressBookGroupNode {
  const AddressBookGroupNode(this.group, this.depth);

  final AddressBookGroup group;
  final int depth;
}

/// Flattens [groups] into depth-first order so a nested list can be rendered
/// with indentation. Groups whose parent is missing or deleted are treated as
/// roots, and a damaged parent cycle is cut after the first visit so the caller
/// always receives every live group exactly once.
List<AddressBookGroupNode> addressBookGroupTreeOrder(
  List<AddressBookGroup> groups,
) {
  final Map<String, AddressBookGroup> byId = <String, AddressBookGroup>{
    for (final AddressBookGroup group in groups)
      if (!group.deleted) group.id: group,
  };
  final Map<String, List<AddressBookGroup>> children =
      <String, List<AddressBookGroup>>{};
  final List<AddressBookGroup> roots = <AddressBookGroup>[];
  for (final AddressBookGroup group in byId.values) {
    final String parent = group.parentId;
    if (parent.isNotEmpty && parent != group.id && byId.containsKey(parent)) {
      children.putIfAbsent(parent, () => <AddressBookGroup>[]).add(group);
    } else {
      roots.add(group);
    }
  }
  for (final List<AddressBookGroup> siblings in children.values) {
    siblings.sort(compareAddressBookGroups);
  }
  roots.sort(compareAddressBookGroups);

  final List<AddressBookGroupNode> ordered = <AddressBookGroupNode>[];
  final Set<String> visited = <String>{};
  void visit(AddressBookGroup group, int depth) {
    if (!visited.add(group.id)) {
      return;
    }
    ordered.add(AddressBookGroupNode(group, depth));
    for (final AddressBookGroup child
        in children[group.id] ?? const <AddressBookGroup>[]) {
      visit(child, depth + 1);
    }
  }

  for (final AddressBookGroup root in roots) {
    visit(root, 0);
  }
  // A cycle has no root; surface its members as roots instead of dropping them.
  final List<AddressBookGroup> leftovers = <AddressBookGroup>[
    for (final AddressBookGroup group in byId.values)
      if (!visited.contains(group.id)) group,
  ]..sort(compareAddressBookGroups);
  for (final AddressBookGroup group in leftovers) {
    visit(group, 0);
  }
  return ordered;
}
