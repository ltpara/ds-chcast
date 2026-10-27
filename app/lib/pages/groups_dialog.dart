import 'package:flutter/material.dart';

import '../server_api.dart';
import '../server_settings.dart';
import '../theme.dart';

typedef GroupWriter = Future<AddressBookGroup> Function(
  ServerSettings settings,
  String token,
  String groupId,
  String name, {
  int? ifRevision,
});
typedef GroupDeleter = Future<void> Function(
  ServerSettings settings,
  String token,
  String groupId, {
  int? ifRevision,
});
typedef GroupReloader = Future<List<AddressBookGroup>> Function();

/// Creates, renames and deletes groups in the signed-in account's address
/// book. The dialog returns true when any group was changed so the caller can
/// resync peers (deleting a group moves its peers back to ungrouped).
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
    final String? name = await showDialog<String>(
      context: context,
      builder: (BuildContext context) =>
          const _GroupNameDialog(title: '新建分组'),
    );
    if (name == null || !mounted) {
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
        name,
      );
      _upsert(group);
    });
  }

  Future<void> _renameGroup(AddressBookGroup group) async {
    final String? name = await showDialog<String>(
      context: context,
      builder: (BuildContext context) =>
          _GroupNameDialog(title: '重命名分组', initial: group.name),
    );
    if (name == null || !mounted || name == group.name) {
      return;
    }
    await _run(() async {
      try {
        final AddressBookGroup updated =
            await (widget.rename ?? ServerApi.updateAddressBookGroup)(
          widget.server,
          widget.token,
          group.id,
          name,
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

  Future<void> _deleteGroup(AddressBookGroup group) async {
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) => AlertDialog(
        backgroundColor: Theme.of(context).colorScheme.surface,
        title: const Text('删除分组'),
        content: Text('删除分组“${group.name}”？组内设备会移到未分组，不会被删除。'),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: const Text('取消'),
          ),
          FilledButton(
            key: const Key('group-delete-confirm'),
            onPressed: () => Navigator.of(context).pop(true),
            style: FilledButton.styleFrom(
              backgroundColor: Theme.of(context).colorScheme.error,
            ),
            child: const Text('删除'),
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
      _error = '分组已在别处修改，已刷新列表，请重试';
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

  void _upsert(AddressBookGroup group) {
    final List<AddressBookGroup> next = <AddressBookGroup>[
      for (final AddressBookGroup item in _groups)
        if (item.id != group.id) item,
      group,
    ]..sort((AddressBookGroup a, AddressBookGroup b) {
        final int byName = a.name.compareTo(b.name);
        return byName != 0 ? byName : a.id.compareTo(b.id);
      });
    _groups = next;
    _changed = true;
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    return AlertDialog(
      backgroundColor: theme.colorScheme.surface,
      title: Row(
        children: <Widget>[
          const Expanded(child: Text('地址簿分组')),
          IconButton(
            tooltip: '新建分组',
            key: const Key('group-create'),
            onPressed: _busy ? null : _create,
            icon: const Icon(Icons.create_new_folder_outlined, size: 19),
            color: kAccent,
          ),
        ],
      ),
      content: SizedBox(
        width: 380,
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
            if (_groups.isEmpty)
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 18),
                child: Text(
                  '还没有分组。创建后可在“我的设备”里归类，登录同一账号的查看器会同步。',
                  style: mutedStyle(context, size: 12),
                ),
              )
            else
              ConstrainedBox(
                constraints: const BoxConstraints(maxHeight: 320),
                child: ListView.separated(
                  shrinkWrap: true,
                  itemCount: _groups.length,
                  separatorBuilder: (_, _) => const SizedBox(height: 2),
                  itemBuilder: (BuildContext context, int index) {
                    final AddressBookGroup group = _groups[index];
                    final int count = widget.peerCounts[group.id] ?? 0;
                    return ListTile(
                      key: Key('group-row-${group.id}'),
                      dense: true,
                      contentPadding: EdgeInsets.zero,
                      leading: Icon(Icons.folder_outlined,
                          size: 18, color: kAccent),
                      title: Text(
                        group.name,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: const TextStyle(fontSize: 13),
                      ),
                      subtitle: Text(
                        '$count 台设备',
                        style: mutedStyle(context, size: 10),
                      ),
                      trailing: Row(
                        mainAxisSize: MainAxisSize.min,
                        children: <Widget>[
                          IconButton(
                            tooltip: '重命名',
                            key: Key('group-rename-${group.id}'),
                            visualDensity: VisualDensity.compact,
                            onPressed:
                                _busy ? null : () => _renameGroup(group),
                            icon: const Icon(Icons.edit_outlined, size: 16),
                          ),
                          IconButton(
                            tooltip: '删除分组',
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
          child: const Text('关闭'),
        ),
      ],
    );
  }
}

class _GroupNameDialog extends StatefulWidget {
  const _GroupNameDialog({required this.title, this.initial = ''});

  final String title;
  final String initial;

  @override
  State<_GroupNameDialog> createState() => _GroupNameDialogState();
}

class _GroupNameDialogState extends State<_GroupNameDialog> {
  late final TextEditingController _name =
      TextEditingController(text: widget.initial);
  String _error = '';

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  void _save() {
    final String name = _name.text.trim();
    if (name.isEmpty) {
      setState(() => _error = '请输入分组名称');
      return;
    }
    if (name.length > 64) {
      setState(() => _error = '分组名称最多 64 个字符');
      return;
    }
    Navigator.of(context).pop(name);
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      backgroundColor: Theme.of(context).colorScheme.surface,
      title: Text(widget.title),
      content: SizedBox(
        width: 320,
        child: TextField(
          key: const Key('group-name'),
          controller: _name,
          autofocus: true,
          maxLength: 64,
          decoration: InputDecoration(
            labelText: '分组名称',
            errorText: _error.isEmpty ? null : _error,
          ),
          onSubmitted: (_) => _save(),
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('取消'),
        ),
        FilledButton(
          key: const Key('group-name-save'),
          onPressed: _save,
          child: const Text('保存'),
        ),
      ],
    );
  }
}
