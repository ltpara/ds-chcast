import 'package:flutter/material.dart';

import '../address_book_sync.dart';
import '../server_api.dart';
import '../server_settings.dart';
import '../theme.dart';
import 'package:chast_viewer/l10n.dart';

typedef GroupWriter = Future<AddressBookGroup> Function(
  ServerSettings settings,
  String token,
  String groupId,
  String? name, {
  String? parentId,
  int? position,
  int? ifRevision,
});
typedef GroupDeleter = Future<void> Function(
  ServerSettings settings,
  String token,
  String groupId, {
  int? ifRevision,
});
typedef GroupReloader = Future<List<AddressBookGroup>> Function();

/// Creates, edits, reorders and deletes groups in the signed-in account's
/// address book. Groups form a shallow tree: an edit dialog picks the parent,
/// and the arrow buttons move a group among its siblings. The dialog returns
/// true when any group was changed so the caller can resync peers (deleting a
/// group moves its peers back to ungrouped and promotes its child groups).
class GroupsDialog extends StatefulWidget {
  const GroupsDialog({
    super.key,
    required this.server,
    required this.token,
    required this.groups,
    this.peerCounts = const <String, int>{},
    this.create,
    this.rename,
    this.remove,
    this.reload,
  });

  final ServerSettings server;
  final String token;
  final List<AddressBookGroup> groups;
  final Map<String, int> peerCounts;
  final GroupWriter? create;
  final GroupWriter? rename;
  final GroupDeleter? remove;

  /// Fetches the latest groups after a 409 so the dialog can show the state
  /// that actually won instead of retrying against stale revisions.
  final GroupReloader? reload;

  @override
  State<GroupsDialog> createState() => _GroupsDialogState();
}

class _GroupEditorResult {
  const _GroupEditorResult(this.name, this.parentId);

  final String name;
  final String parentId;
}

class _GroupsDialogState extends State<GroupsDialog> {
  late List<AddressBookGroup> _groups;
  bool _changed = false;
  bool _busy = false;
  String _error = '';

  @override
  void initState() {
    super.initState();
    _groups = List<AddressBookGroup>.of(widget.groups);
  }

  Future<void> _create() async {
    final _GroupEditorResult? result = await showDialog<_GroupEditorResult>(
      context: context,
      builder: (BuildContext context) => _GroupEditorDialog(
        title: tr('新建分组'),
        groups: _groups,
      ),
    );
    if (result == null || !mounted) {
      return;
    }
    final String id =
        'g${DateTime.now().microsecondsSinceEpoch.toRadixString(36)}';
    await _run(() async {
      final AddressBookGroup group =
          await (widget.create ?? ServerApi.updateAddressBookGroup)(
        widget.server,
        widget.token,
        id,
        result.name,
        parentId: result.parentId.isEmpty ? null : result.parentId,
      );
      _upsert(group);
    });
  }

  Future<void> _editGroup(AddressBookGroup group) async {
    final _GroupEditorResult? result = await showDialog<_GroupEditorResult>(
      context: context,
      builder: (BuildContext context) => _GroupEditorDialog(
        title: tr('编辑分组'),
        groups: _groups,
        editing: group,
      ),
    );
    if (result == null || !mounted) {
      return;
    }
    if (result.name == group.name && result.parentId == group.parentId) {
      return;
    }
    await _run(() async {
      try {
        final AddressBookGroup updated =
            await (widget.rename ?? ServerApi.updateAddressBookGroup)(
          widget.server,
          widget.token,
          group.id,
          result.name == group.name ? null : result.name,
          parentId: result.parentId,
          ifRevision: group.revision > 0 ? group.revision : null,
        );
        _upsert(updated);
      } on ServerApiException catch (error) {
        if (error.statusCode != 409) {
          rethrow;
        }
        await _conflict();
      }
    });
  }

  Future<void> _moveGroup(AddressBookGroup group, int delta) async {
    final List<AddressBookGroup> siblings = <AddressBookGroup>[
      for (final AddressBookGroup item in _groups)
        if (item.parentId == group.parentId) item,
    ]..sort(compareAddressBookGroups);
    final int index = siblings.indexWhere(
      (AddressBookGroup item) => item.id == group.id,
    );
    final int target = index + delta;
    if (index < 0 || target < 0 || target >= siblings.length) {
      return;
    }
    final List<AddressBookGroup> reordered = List<AddressBookGroup>.of(siblings)
      ..removeAt(index)
      ..insert(target, group);
    await _run(() async {
      try {
        for (int i = 0; i < reordered.length; i++) {
          final AddressBookGroup item = reordered[i];
          final int position = i + 1;
          if (item.position == position) {
            continue;
          }
          final AddressBookGroup updated =
              await (widget.rename ?? ServerApi.updateAddressBookGroup)(
            widget.server,
            widget.token,
            item.id,
            null,
            position: position,
            ifRevision: item.revision > 0 ? item.revision : null,
          );
          _upsert(updated, markChanged: false);
        }
        _changed = true;
      } on ServerApiException catch (error) {
        if (error.statusCode != 409) {
          rethrow;
        }
        await _conflict();
      }
    });
  }

  Future<void> _deleteGroup(AddressBookGroup group) async {
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) => AlertDialog(
        backgroundColor: Theme.of(context).colorScheme.surface,
        title: Text(tr('删除分组')),
        content: Text(
          tr('删除分组“{p2}”？组内设备会移到未分组，子分组会上移一层，都不会被删除。', <String, Object?>{'p2': group.name}),
        ),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: Text(tr('取消')),
          ),
          FilledButton(
            key: const Key('group-delete-confirm'),
            onPressed: () => Navigator.of(context).pop(true),
            style: FilledButton.styleFrom(
              backgroundColor: Theme.of(context).colorScheme.error,
            ),
            child: Text(tr('删除')),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) {
      return;
    }
    await _run(() async {
      try {
        await (widget.remove ?? ServerApi.deleteAddressBookGroup)(
          widget.server,
          widget.token,
          group.id,
          ifRevision: group.revision > 0 ? group.revision : null,
        );
      } on ServerApiException catch (error) {
        if (error.statusCode != 409) {
          rethrow;
        }
        await _conflict();
        return;
      }
      _groups = <AddressBookGroup>[
        for (final AddressBookGroup item in _groups)
          if (item.id != group.id) item,
      ];
      _changed = true;
    });
  }

  Future<void> _conflict() async {
    _changed = true;
    List<AddressBookGroup>? latest;
    if (widget.reload != null) {
      try {
        latest = await widget.reload!();
      } catch (_) {
        // Keep the local list when the refresh fails; the error below still
        // tells the user that their edit did not win.
      }
    }
    if (!mounted) {
      return;
    }
    setState(() {
      if (latest != null) {
        _groups = List<AddressBookGroup>.of(latest);
      }
      _error = tr('分组已在别处修改，已刷新列表，请重试');
    });
  }

  Future<void> _run(Future<void> Function() action) async {
    if (_busy) {
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      await action();
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() => _error = error.message);
      }
    } catch (error) {
      if (mounted) {
        setState(() => _error = '$error');
      }
    } finally {
      if (mounted) {
        setState(() => _busy = false);
      }
    }
  }

  void _upsert(AddressBookGroup group, {bool markChanged = true}) {
    final List<AddressBookGroup> next = <AddressBookGroup>[
      for (final AddressBookGroup item in _groups)
        if (item.id != group.id) item,
      group,
    ]..sort(compareAddressBookGroups);
    _groups = next;
    if (markChanged) {
      _changed = true;
    }
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final List<AddressBookGroupNode> nodes = addressBookGroupTreeOrder(_groups);
    return AlertDialog(
      backgroundColor: theme.colorScheme.surface,
      title: Row(
        children: <Widget>[
          Expanded(child: Text(tr('地址簿分组'))),
          IconButton(
            tooltip: tr('新建分组'),
            key: const Key('group-create'),
            onPressed: _busy ? null : _create,
            icon: const Icon(Icons.create_new_folder_outlined, size: 19),
            color: kAccent,
          ),
        ],
      ),
      content: SizedBox(
        width: 440,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            if (_error.isNotEmpty) ...<Widget>[
              Text(
                _error,
                key: const Key('group-error'),
                style: TextStyle(color: theme.colorScheme.error, fontSize: 12),
              ),
              const SizedBox(height: 8),
            ],
            if (nodes.isEmpty)
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 18),
                child: Text(
                  tr('还没有分组。创建后可在“我的设备”里归类，登录同一账号的查看器会同步。'),
                  style: mutedStyle(context, size: 12),
                ),
              )
            else
              ConstrainedBox(
                constraints: const BoxConstraints(maxHeight: 320),
                child: ListView.separated(
                  shrinkWrap: true,
                  itemCount: nodes.length,
                  separatorBuilder: (_, _) => const SizedBox(height: 2),
                  itemBuilder: (BuildContext context, int index) {
                    final AddressBookGroupNode node = nodes[index];
                    final AddressBookGroup group = node.group;
                    final int count = widget.peerCounts[group.id] ?? 0;
                    final List<AddressBookGroup> siblings = <AddressBookGroup>[
                      for (final AddressBookGroupNode other in nodes)
                        if (other.group.parentId == group.parentId) other.group,
                    ];
                    final int siblingIndex = siblings.indexWhere(
                      (AddressBookGroup other) => other.id == group.id,
                    );
                    return ListTile(
                      key: Key('group-row-${group.id}'),
                      dense: true,
                      contentPadding: EdgeInsets.only(left: node.depth * 18.0),
                      leading: Icon(
                        node.depth == 0
                            ? Icons.folder_outlined
                            : Icons.subdirectory_arrow_right_rounded,
                        size: 18,
                        color: kAccent,
                      ),
                      title: Text(
                        group.name,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: const TextStyle(fontSize: 13),
                      ),
                      subtitle: Text(
                        tr('{count} 台设备', <String, Object?>{'count': count}),
                        style: mutedStyle(context, size: 10),
                      ),
                      trailing: Row(
                        mainAxisSize: MainAxisSize.min,
                        children: <Widget>[
                          IconButton(
                            tooltip: tr('上移'),
                            key: Key('group-up-${group.id}'),
                            visualDensity: VisualDensity.compact,
                            onPressed: _busy || siblingIndex <= 0
                                ? null
                                : () => _moveGroup(group, -1),
                            icon: const Icon(
                              Icons.arrow_upward_rounded,
                              size: 16,
                            ),
                          ),
                          IconButton(
                            tooltip: tr('下移'),
                            key: Key('group-down-${group.id}'),
                            visualDensity: VisualDensity.compact,
                            onPressed: _busy ||
                                    siblingIndex < 0 ||
                                    siblingIndex >= siblings.length - 1
                                ? null
                                : () => _moveGroup(group, 1),
                            icon: const Icon(
                              Icons.arrow_downward_rounded,
                              size: 16,
                            ),
                          ),
                          IconButton(
                            tooltip: tr('编辑'),
                            key: Key('group-rename-${group.id}'),
                            visualDensity: VisualDensity.compact,
                            onPressed: _busy ? null : () => _editGroup(group),
                            icon: const Icon(Icons.edit_outlined, size: 16),
                          ),
                          IconButton(
                            tooltip: tr('删除分组'),
                            key: Key('group-delete-${group.id}'),
                            visualDensity: VisualDensity.compact,
                            onPressed:
                                _busy ? null : () => _deleteGroup(group),
                            icon: Icon(Icons.delete_outline_rounded,
                                size: 16, color: theme.colorScheme.error),
                          ),
                        ],
                      ),
                    );
                  },
                ),
              ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(_changed),
          child: Text(tr('关闭')),
        ),
      ],
    );
  }
}

class _GroupEditorDialog extends StatefulWidget {
  const _GroupEditorDialog({
    required this.title,
    required this.groups,
    this.editing,
  });

  final String title;
  final List<AddressBookGroup> groups;
  final AddressBookGroup? editing;

  @override
  State<_GroupEditorDialog> createState() => _GroupEditorDialogState();
}

class _GroupEditorDialogState extends State<_GroupEditorDialog> {
  late final TextEditingController _name =
      TextEditingController(text: widget.editing?.name ?? '');
  late String _parentId = widget.editing?.parentId ?? '';
  String _error = '';

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  /// Ids a group may not be moved under: itself and everything below it.
  Set<String> _blockedParents() {
    final AddressBookGroup? editing = widget.editing;
    if (editing == null) {
      return const <String>{};
    }
    final Set<String> blocked = <String>{editing.id};
    bool grew = true;
    while (grew) {
      grew = false;
      for (final AddressBookGroup group in widget.groups) {
        if (!blocked.contains(group.id) &&
            group.parentId.isNotEmpty &&
            blocked.contains(group.parentId)) {
          blocked.add(group.id);
          grew = true;
        }
      }
    }
    return blocked;
  }

  void _save() {
    final String name = _name.text.trim();
    if (name.isEmpty) {
      setState(() => _error = tr('请输入分组名称'));
      return;
    }
    if (name.length > 64) {
      setState(() => _error = tr('分组名称最多 64 个字符'));
      return;
    }
    Navigator.of(context).pop(_GroupEditorResult(name, _parentId));
  }

  @override
  Widget build(BuildContext context) {
    final Set<String> blocked = _blockedParents();
    final List<AddressBookGroupNode> options = <AddressBookGroupNode>[
      for (final AddressBookGroupNode node
          in addressBookGroupTreeOrder(widget.groups))
        if (!blocked.contains(node.group.id)) node,
    ];
    return AlertDialog(
      backgroundColor: Theme.of(context).colorScheme.surface,
      title: Text(widget.title),
      content: SizedBox(
        width: 340,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            TextField(
              key: const Key('group-name'),
              controller: _name,
              autofocus: true,
              maxLength: 64,
              decoration: InputDecoration(
                labelText: tr('分组名称'),
                errorText: _error.isEmpty ? null : _error,
              ),
              onSubmitted: (_) => _save(),
            ),
            const SizedBox(height: 8),
            DropdownButtonFormField<String>(
              key: const Key('group-parent'),
              initialValue: _parentId,
              decoration: InputDecoration(labelText: tr('上级分组')),
              items: <DropdownMenuItem<String>>[
                DropdownMenuItem<String>(
                  value: '',
                  child: Text(tr('（根分组）'), style: TextStyle(fontSize: 13)),
                ),
                for (final AddressBookGroupNode node in options)
                  DropdownMenuItem<String>(
                    value: node.group.id,
                    child: Text(
                      '${List<String>.filled(node.depth, '　').join()}${node.group.name}',
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(fontSize: 13),
                    ),
                  ),
              ],
              onChanged: (String? value) =>
                  setState(() => _parentId = value ?? ''),
            ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
        FilledButton(
          key: const Key('group-name-save'),
          onPressed: _save,
          child: Text(tr('保存')),
        ),
      ],
    );
  }
}
