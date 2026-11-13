import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../address_book_sync.dart';
import '../server_api.dart';
import '../server_settings.dart';
import '../theme.dart';
import 'package:chast_viewer/l10n.dart';

typedef GroupWriter =
    Future<AddressBookGroup> Function(
      ServerSettings settings,
      String token,
      String groupId,
      String? name, {
      String? parentId,
      int? position,
      int? ifRevision,
    });
typedef GroupDeleter =
    Future<void> Function(
      ServerSettings settings,
      String token,
      String groupId, {
      int? ifRevision,
    });
typedef GroupReloader = Future<List<AddressBookGroup>> Function();
typedef GroupExporter =
    Future<AddressBookGroupTransferDocument> Function(
      ServerSettings settings,
      String token,
    );
typedef GroupImporter =
    Future<AddressBookGroupImportResult> Function(
      ServerSettings settings,
      String token,
      AddressBookGroupTransferDocument document, {
      required String conflict,
    });

/// Creates, edits, reorders and deletes groups in the signed-in account's
/// address book. Groups form a shallow tree: an edit dialog picks the parent,
/// the arrow buttons move a group among its siblings, and dragging the handle
/// reorders siblings (never across parents). The dialog returns true when any
/// group was changed so the caller can resync peers (deleting a group moves
/// its peers back to ungrouped and promotes its child groups).
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
    this.exportGroups,
    this.importGroups,
  });

  final ServerSettings server;
  final String token;
  final List<AddressBookGroup> groups;
  final Map<String, int> peerCounts;
  final GroupWriter? create;
  final GroupWriter? rename;
  final GroupDeleter? remove;
  final GroupExporter? exportGroups;
  final GroupImporter? importGroups;

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
      builder: (BuildContext context) =>
          _GroupEditorDialog(title: tr('新建分组'), groups: _groups),
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
    await _applyOrder(reordered);
  }

  /// Moves [moved] into [target]'s slot. Only siblings (same parent) can be
  /// reordered, so a drag never changes the tree shape.
  Future<void> _moveGroupOnto(
    AddressBookGroup moved,
    AddressBookGroup target,
  ) async {
    if (moved.id == target.id || moved.parentId != target.parentId) {
      return;
    }
    final List<AddressBookGroup> siblings = <AddressBookGroup>[
      for (final AddressBookGroup item in _groups)
        if (item.parentId == moved.parentId) item,
    ]..sort(compareAddressBookGroups);
    final int from = siblings.indexWhere(
      (AddressBookGroup item) => item.id == moved.id,
    );
    final int to = siblings.indexWhere(
      (AddressBookGroup item) => item.id == target.id,
    );
    if (from < 0 || to < 0 || from == to) {
      return;
    }
    siblings
      ..removeAt(from)
      ..insert(to, moved);
    await _applyOrder(siblings);
  }

  /// Writes sequential positions for [reordered], skipping rows that already
  /// sit at the right spot so a one-step move only touches two rows.
  Future<void> _applyOrder(List<AddressBookGroup> reordered) async {
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
          tr('删除分组“{p2}”？组内设备会移到未分组，子分组会上移一层，都不会被删除。', <String, Object?>{
            'p2': group.name,
          }),
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

  Future<void> _transferGroups() async {
    final AddressBookGroupImportResult?
    imported = await showDialog<AddressBookGroupImportResult>(
      context: context,
      barrierDismissible: false,
      builder: (BuildContext context) => _GroupTransferDialog(
        server: widget.server,
        token: widget.token,
        exportGroups: widget.exportGroups ?? ServerApi.exportAddressBookGroups,
        importGroups: widget.importGroups ?? ServerApi.importAddressBookGroups,
      ),
    );
    if (imported == null || !imported.hasChanges || !mounted) {
      return;
    }
    await _run(() async {
      if (widget.reload != null) {
        final List<AddressBookGroup> latest = await widget.reload!();
        if (!mounted) {
          return;
        }
        setState(() {
          _groups = List<AddressBookGroup>.of(latest)
            ..sort(compareAddressBookGroups);
          _changed = true;
        });
        return;
      }
      final Map<String, AddressBookGroup> byID = <String, AddressBookGroup>{
        for (final AddressBookGroup group in _groups) group.id: group,
      };
      for (final AddressBookGroup group in <AddressBookGroup>[
        ...imported.created,
        ...imported.updated,
      ]) {
        byID[group.id] = group;
      }
      setState(() {
        _groups = byID.values.toList()..sort(compareAddressBookGroups);
        _changed = true;
      });
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
          IconButton(
            tooltip: tr('分组导入/导出'),
            key: const Key('group-transfer'),
            onPressed: _busy ? null : _transferGroups,
            icon: const Icon(Icons.swap_vert_rounded, size: 19),
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
                    return DragTarget<AddressBookGroup>(
                      onWillAcceptWithDetails:
                          (DragTargetDetails<AddressBookGroup> details) =>
                              !_busy &&
                              details.data.id != group.id &&
                              details.data.parentId == group.parentId,
                      onAcceptWithDetails:
                          (DragTargetDetails<AddressBookGroup> details) =>
                              _moveGroupOnto(details.data, group),
                      builder:
                          (
                            BuildContext context,
                            List<AddressBookGroup?> candidate,
                            List<dynamic> rejected,
                          ) {
                            return DecoratedBox(
                              decoration: candidate.isEmpty
                                  ? const BoxDecoration()
                                  : BoxDecoration(
                                      color: kAccent.withValues(alpha: 0.08),
                                      borderRadius: BorderRadius.circular(6),
                                    ),
                              child: ListTile(
                                key: Key('group-row-${group.id}'),
                                dense: true,
                                contentPadding: EdgeInsets.only(
                                  left: node.depth * 18.0,
                                ),
                                leading: Row(
                                  mainAxisSize: MainAxisSize.min,
                                  children: <Widget>[
                                    Draggable<AddressBookGroup>(
                                      key: Key('group-drag-${group.id}'),
                                      data: group,
                                      maxSimultaneousDrags: _busy ? 0 : 1,
                                      feedback: Material(
                                        color: Colors.transparent,
                                        child: Container(
                                          padding: const EdgeInsets.symmetric(
                                            horizontal: 10,
                                            vertical: 6,
                                          ),
                                          decoration: BoxDecoration(
                                            color: theme.colorScheme.surface,
                                            borderRadius: BorderRadius.circular(
                                              6,
                                            ),
                                            border: Border.all(color: kAccent),
                                          ),
                                          child: Text(
                                            group.name,
                                            style: const TextStyle(
                                              fontSize: 12,
                                            ),
                                          ),
                                        ),
                                      ),
                                      child: MouseRegion(
                                        cursor: _busy
                                            ? SystemMouseCursors.basic
                                            : SystemMouseCursors.grab,
                                        child: Icon(
                                          Icons.drag_indicator_rounded,
                                          size: 16,
                                          color: theme.colorScheme.onSurface
                                              .withValues(alpha: 0.45),
                                        ),
                                      ),
                                    ),
                                    const SizedBox(width: 4),
                                    Icon(
                                      node.depth == 0
                                          ? Icons.folder_outlined
                                          : Icons
                                                .subdirectory_arrow_right_rounded,
                                      size: 18,
                                      color: kAccent,
                                    ),
                                  ],
                                ),
                                title: Text(
                                  group.name,
                                  maxLines: 1,
                                  overflow: TextOverflow.ellipsis,
                                  style: const TextStyle(fontSize: 13),
                                ),
                                subtitle: Text(
                                  tr('{count} 台设备', <String, Object?>{
                                    'count': count,
                                  }),
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
                                      onPressed:
                                          _busy ||
                                              siblingIndex < 0 ||
                                              siblingIndex >=
                                                  siblings.length - 1
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
                                      onPressed: _busy
                                          ? null
                                          : () => _editGroup(group),
                                      icon: const Icon(
                                        Icons.edit_outlined,
                                        size: 16,
                                      ),
                                    ),
                                    IconButton(
                                      tooltip: tr('删除分组'),
                                      key: Key('group-delete-${group.id}'),
                                      visualDensity: VisualDensity.compact,
                                      onPressed: _busy
                                          ? null
                                          : () => _deleteGroup(group),
                                      icon: Icon(
                                        Icons.delete_outline_rounded,
                                        size: 16,
                                        color: theme.colorScheme.error,
                                      ),
                                    ),
                                  ],
                                ),
                              ),
                            );
                          },
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

class _GroupTransferDialog extends StatefulWidget {
  const _GroupTransferDialog({
    required this.server,
    required this.token,
    required this.exportGroups,
    required this.importGroups,
  });

  final ServerSettings server;
  final String token;
  final GroupExporter exportGroups;
  final GroupImporter importGroups;

  @override
  State<_GroupTransferDialog> createState() => _GroupTransferDialogState();
}

class _GroupTransferDialogState extends State<_GroupTransferDialog> {
  final TextEditingController _exportText = TextEditingController();
  final TextEditingController _importText = TextEditingController();
  String _conflict = 'reject';
  String _error = '';
  String _notice = '';
  bool _busy = false;
  AddressBookGroupImportResult? _lastImport;

  @override
  void initState() {
    super.initState();
    _loadExport();
  }

  @override
  void dispose() {
    _exportText.dispose();
    _importText.dispose();
    super.dispose();
  }

  Future<void> _loadExport() async {
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      final AddressBookGroupTransferDocument document = await widget
          .exportGroups(widget.server, widget.token);
      if (mounted) {
        setState(() => _exportText.text = document.toJsonText());
      }
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(
          () => _error = tr('读取分组导出失败：{error}', <String, Object?>{
            'error': error.message,
          }),
        );
      }
    } catch (error) {
      if (mounted) {
        setState(
          () => _error = tr('读取分组导出失败：{error}', <String, Object?>{
            'error': '$error',
          }),
        );
      }
    } finally {
      if (mounted) {
        setState(() => _busy = false);
      }
    }
  }

  Future<void> _copyExport() async {
    try {
      await Clipboard.setData(ClipboardData(text: _exportText.text));
      if (mounted) {
        setState(() {
          _error = '';
          _notice = tr('已复制分组 JSON');
        });
      }
    } on PlatformException catch (error) {
      if (mounted) {
        setState(
          () => _error = tr('复制分组 JSON 失败：{error}', <String, Object?>{
            'error': error.message ?? error.code,
          }),
        );
      }
    }
  }

  Future<void> _pasteImport() async {
    try {
      final ClipboardData? data = await Clipboard.getData(Clipboard.kTextPlain);
      if (!mounted) {
        return;
      }
      if (data?.text == null || data!.text!.trim().isEmpty) {
        setState(() => _error = tr('剪贴板中没有文本'));
        return;
      }
      setState(() {
        _importText.text = data.text!;
        _error = '';
      });
    } on PlatformException catch (error) {
      if (mounted) {
        setState(
          () => _error = tr('读取剪贴板失败：{error}', <String, Object?>{
            'error': error.message ?? error.code,
          }),
        );
      }
    }
  }

  Future<void> _submitImport() async {
    if (_busy) {
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
      _notice = '';
    });
    try {
      final AddressBookGroupTransferDocument document =
          AddressBookGroupTransferDocument.parse(_importText.text);
      final AddressBookGroupImportResult result = await widget.importGroups(
        widget.server,
        widget.token,
        document,
        conflict: _conflict,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _lastImport = result;
        _notice = tr(
          '导入结果：新建 {created}，更新 {updated}，跳过 {skipped}，未变 {unchanged}',
          <String, Object?>{
            'created': result.created.length,
            'updated': result.updated.length,
            'skipped': result.skipped.length,
            'unchanged': result.unchanged.length,
          },
        );
      });
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() => _error = error.message);
      }
    } catch (error) {
      if (mounted) {
        setState(
          () => _error = tr('导入分组失败：{error}', <String, Object?>{
            'error': '$error',
          }),
        );
      }
    } finally {
      if (mounted) {
        setState(() => _busy = false);
      }
    }
  }

  void _close() {
    Navigator.of(context).pop(_lastImport);
  }

  String _conflictLabel(String mode) {
    switch (mode) {
      case 'skip':
        return tr('跳过已有分组');
      case 'update':
        return tr('更新已有分组');
      default:
        return tr('拒绝整个导入');
    }
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    return AlertDialog(
      backgroundColor: theme.colorScheme.surface,
      title: Text(tr('分组导入/导出')),
      content: SizedBox(
        width: 600,
        child: SingleChildScrollView(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: <Widget>[
              Row(
                children: <Widget>[
                  Expanded(
                    child: Text(
                      tr('导出 JSON'),
                      style: const TextStyle(fontWeight: FontWeight.w600),
                    ),
                  ),
                  OutlinedButton.icon(
                    key: const Key('group-export-copy'),
                    onPressed: _busy || _exportText.text.isEmpty
                        ? null
                        : _copyExport,
                    icon: const Icon(Icons.copy_rounded, size: 16),
                    label: Text(tr('复制 JSON')),
                  ),
                ],
              ),
              const SizedBox(height: 8),
              TextField(
                key: const Key('group-export-text'),
                controller: _exportText,
                readOnly: true,
                maxLines: 6,
                style: const TextStyle(fontFamily: 'Consolas', fontSize: 12),
                decoration: InputDecoration(
                  border: const OutlineInputBorder(),
                  hintText: _busy ? tr('正在读取…') : null,
                ),
              ),
              const SizedBox(height: 14),
              Row(
                children: <Widget>[
                  Expanded(
                    child: Text(
                      tr('导入 JSON'),
                      style: const TextStyle(fontWeight: FontWeight.w600),
                    ),
                  ),
                  TextButton.icon(
                    key: const Key('group-import-paste'),
                    onPressed: _busy ? null : _pasteImport,
                    icon: const Icon(Icons.content_paste_rounded, size: 16),
                    label: Text(tr('粘贴')),
                  ),
                ],
              ),
              Text(
                tr('导入只迁移分组结构，不包含设备、成员关系或连接信息。'),
                style: mutedStyle(context, size: 11),
              ),
              const SizedBox(height: 8),
              TextField(
                key: const Key('group-import-text'),
                controller: _importText,
                minLines: 6,
                maxLines: 10,
                style: const TextStyle(fontFamily: 'Consolas', fontSize: 12),
                decoration: InputDecoration(
                  border: const OutlineInputBorder(),
                  hintText: tr('粘贴分组 JSON'),
                ),
              ),
              const SizedBox(height: 12),
              DropdownButtonFormField<String>(
                key: const Key('group-import-conflict'),
                initialValue: _conflict,
                decoration: InputDecoration(labelText: tr('冲突模式')),
                items: <DropdownMenuItem<String>>[
                  for (final String mode in addressBookGroupImportConflictModes)
                    DropdownMenuItem<String>(
                      key: Key('group-import-conflict-$mode'),
                      value: mode,
                      child: Text(_conflictLabel(mode)),
                    ),
                ],
                onChanged: _busy
                    ? null
                    : (String? value) {
                        if (value != null) {
                          setState(() => _conflict = value);
                        }
                      },
              ),
              if (_error.isNotEmpty) ...<Widget>[
                const SizedBox(height: 10),
                Text(
                  _error,
                  key: const Key('group-transfer-error'),
                  style: TextStyle(
                    color: theme.colorScheme.error,
                    fontSize: 12,
                  ),
                ),
              ],
              if (_notice.isNotEmpty) ...<Widget>[
                const SizedBox(height: 10),
                Text(
                  _notice,
                  key: const Key('group-transfer-notice'),
                  style: TextStyle(color: kAccent, fontSize: 12),
                ),
              ],
            ],
          ),
        ),
      ),
      actions: <Widget>[
        TextButton(
          key: const Key('group-transfer-close'),
          onPressed: _busy ? null : _close,
          child: Text(_lastImport == null ? tr('取消') : tr('完成')),
        ),
        FilledButton(
          key: const Key('group-import-confirm'),
          onPressed: _busy ? null : _submitImport,
          child: Text(tr('导入分组')),
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
  late final TextEditingController _name = TextEditingController(
    text: widget.editing?.name ?? '',
  );
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
      for (final AddressBookGroupNode node in addressBookGroupTreeOrder(
        widget.groups,
      ))
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
