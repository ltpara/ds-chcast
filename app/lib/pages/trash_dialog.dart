import 'package:flutter/material.dart';

import '../server_api.dart';
import '../server_settings.dart';
import '../theme.dart';
import 'package:chast_viewer/l10n.dart';

typedef TrashFetcher =
    Future<List<AddressBookPeer>> Function(
      ServerSettings settings,
      String token,
    );
typedef TrashRestorer =
    Future<AddressBookPeer> Function(
      ServerSettings settings,
      String token,
      String peerId,
    );
typedef TrashPurger =
    Future<void> Function(
      ServerSettings settings,
      String token,
      String peerId,
    );
typedef TrashEmptier = Future<int> Function(ServerSettings settings, String token);

/// Lists the deleted entries of the signed-in user's own address book and
/// lets the user put one back or delete it for good. Tombstones keep their
/// name, tags and note, so the rows read like the live device cards.
///
/// The dialog returns true when the book changed so the caller can resync.
class TrashDialog extends StatefulWidget {
  const TrashDialog({
    super.key,
    required this.server,
    required this.token,
    this.fetch,
    this.restore,
    this.purge,
    this.empty,
  });

  final ServerSettings server;
  final String token;
  final TrashFetcher? fetch;
  final TrashRestorer? restore;
  final TrashPurger? purge;
  final TrashEmptier? empty;

  @override
  State<TrashDialog> createState() => _TrashDialogState();
}

class _TrashDialogState extends State<TrashDialog> {
  List<AddressBookPeer> _peers = <AddressBookPeer>[];
  bool _loading = true;
  bool _busy = false;
  bool _changed = false;
  String _error = '';
  String _notice = '';

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    try {
      final List<AddressBookPeer> peers =
          await (widget.fetch ?? ServerApi.addressBookTrash)(
            widget.server,
            widget.token,
          );
      if (!mounted) {
        return;
      }
      setState(() => _peers = peers);
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
        setState(() => _loading = false);
      }
    }
  }

  String _label(AddressBookPeer peer) =>
      peer.name.isEmpty ? peer.id : peer.name;

  Future<void> _restore(AddressBookPeer peer) async {
    await _run(() async {
      try {
        await (widget.restore ?? ServerApi.restoreAddressBookPeer)(
          widget.server,
          widget.token,
          peer.id,
        );
      } on ServerApiException catch (error) {
        if (error.statusCode == 404) {
          // Restored or purged elsewhere; show what the server has now.
          await _reload();
          return;
        }
        rethrow;
      }
      if (!mounted) {
        return;
      }
      setState(() {
        _peers = <AddressBookPeer>[
          for (final AddressBookPeer item in _peers)
            if (item.id != peer.id) item,
        ];
        _changed = true;
        _notice = tr('已恢复“{p1}”', <String, Object?>{'p1': _label(peer)});
      });
    });
  }

  Future<void> _purge(AddressBookPeer peer) async {
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) => AlertDialog(
        backgroundColor: Theme.of(context).colorScheme.surface,
        title: Text(tr('永久删除')),
        content: Text(
          tr('永久删除“{p1}”？此操作无法撤销。', <String, Object?>{
            'p1': _label(peer),
          }),
        ),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: Text(tr('取消')),
          ),
          FilledButton(
            key: const Key('trash-purge-confirm'),
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
        await (widget.purge ?? ServerApi.purgeAddressBookPeer)(
          widget.server,
          widget.token,
          peer.id,
        );
      } on ServerApiException catch (error) {
        if (error.statusCode == 404) {
          await _reload();
          return;
        }
        rethrow;
      }
      if (!mounted) {
        return;
      }
      setState(() {
        _peers = <AddressBookPeer>[
          for (final AddressBookPeer item in _peers)
            if (item.id != peer.id) item,
        ];
      });
    });
  }

  Future<void> _emptyTrash() async {
    final int count = _peers.length;
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) => AlertDialog(
        backgroundColor: Theme.of(context).colorScheme.surface,
        title: Text(tr('清空回收站')),
        content: Text(
          tr('永久删除回收站中的 {count} 个条目？此操作无法撤销。', <String, Object?>{
            'count': count,
          }),
        ),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: Text(tr('取消')),
          ),
          FilledButton(
            key: const Key('trash-empty-confirm'),
            onPressed: () => Navigator.of(context).pop(true),
            style: FilledButton.styleFrom(
              backgroundColor: Theme.of(context).colorScheme.error,
            ),
            child: Text(tr('清空')),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) {
      return;
    }
    await _run(() async {
      final int removed = await (widget.empty ?? ServerApi.emptyAddressBookTrash)(
        widget.server,
        widget.token,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _peers = <AddressBookPeer>[];
        _changed = true;
        _notice = tr('已清空回收站（{count} 个条目）', <String, Object?>{
          'count': removed,
        });
      });
    });
  }

  /// Re-reads the trash because the server no longer had the entry we tried
  /// to change. The result may still be the same list; the error message
  /// explains why nothing moved.
  Future<void> _reload() async {
    final List<AddressBookPeer> peers =
        await (widget.fetch ?? ServerApi.addressBookTrash)(
          widget.server,
          widget.token,
        );
    if (!mounted) {
      return;
    }
    setState(() {
      _peers = peers;
      _error = tr('回收站已在别处修改，已刷新列表');
    });
  }

  Future<void> _run(Future<void> Function() action) async {
    if (_busy) {
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
      _notice = '';
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

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    return AlertDialog(
      backgroundColor: theme.colorScheme.surface,
      title: Text(tr('地址簿回收站')),
      content: SizedBox(
        width: 440,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            if (_error.isNotEmpty) ...<Widget>[
              Text(
                _error,
                key: const Key('trash-error'),
                style: TextStyle(color: theme.colorScheme.error, fontSize: 12),
              ),
              const SizedBox(height: 8),
            ],
            if (_notice.isNotEmpty) ...<Widget>[
              Text(
                _notice,
                key: const Key('trash-notice'),
                style: const TextStyle(color: kAccent, fontSize: 12),
              ),
              const SizedBox(height: 8),
            ],
            if (_loading)
              const Padding(
                padding: EdgeInsets.symmetric(vertical: 22),
                child: Center(
                  child: SizedBox(
                    width: 18,
                    height: 18,
                    child: CircularProgressIndicator(
                      strokeWidth: 2,
                      color: kAccent,
                    ),
                  ),
                ),
              )
            else if (_peers.isEmpty)
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 18),
                child: Text(
                  tr('回收站是空的。删除的设备会先放到这里，可随时恢复。'),
                  style: mutedStyle(context, size: 12),
                ),
              )
            else
              ConstrainedBox(
                constraints: const BoxConstraints(maxHeight: 320),
                child: ListView.separated(
                  shrinkWrap: true,
                  itemCount: _peers.length,
                  separatorBuilder: (_, _) => const SizedBox(height: 2),
                  itemBuilder: (BuildContext context, int index) {
                    final AddressBookPeer peer = _peers[index];
                    final String meta = <String>[
                      peer.id,
                      if (peer.tags.isNotEmpty) peer.tags.join(' · '),
                    ].join('  ·  ');
                    return ListTile(
                      key: Key('trash-row-${peer.id}'),
                      dense: true,
                      contentPadding: EdgeInsets.zero,
                      leading: Icon(
                        Icons.restore_from_trash_outlined,
                        size: 18,
                        color: theme.colorScheme.onSurface.withValues(
                          alpha: 0.45,
                        ),
                      ),
                      title: Text(
                        _label(peer),
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: const TextStyle(fontSize: 13),
                      ),
                      subtitle: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: <Widget>[
                          Text(
                            meta,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: mutedStyle(context, size: 10),
                          ),
                          if (peer.note.isNotEmpty)
                            Text(
                              peer.note,
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                              style: mutedStyle(context, size: 10),
                            ),
                        ],
                      ),
                      trailing: Row(
                        mainAxisSize: MainAxisSize.min,
                        children: <Widget>[
                          IconButton(
                            tooltip: tr('恢复'),
                            key: Key('trash-restore-${peer.id}'),
                            visualDensity: VisualDensity.compact,
                            onPressed: _busy ? null : () => _restore(peer),
                            icon: const Icon(
                              Icons.settings_backup_restore_rounded,
                              size: 17,
                            ),
                            color: kAccent,
                          ),
                          IconButton(
                            tooltip: tr('永久删除'),
                            key: Key('trash-purge-${peer.id}'),
                            visualDensity: VisualDensity.compact,
                            onPressed: _busy ? null : () => _purge(peer),
                            icon: Icon(
                              Icons.delete_forever_outlined,
                              size: 17,
                              color: theme.colorScheme.error,
                            ),
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
        if (_peers.isNotEmpty)
          TextButton(
            key: const Key('trash-empty-all'),
            onPressed: _busy ? null : _emptyTrash,
            style: TextButton.styleFrom(
              foregroundColor: theme.colorScheme.error,
            ),
            child: Text(tr('清空回收站')),
          ),
        TextButton(
          onPressed: () => Navigator.of(context).pop(_changed),
          child: Text(tr('关闭')),
        ),
      ],
    );
  }
}
