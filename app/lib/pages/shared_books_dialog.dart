import 'dart:io';

import 'package:flutter/material.dart';

import '../account_store.dart';
import '../server_api.dart';
import '../server_settings.dart';
import '../theme.dart';

typedef SharedGrantsLoader =
    Future<AddressBookGrants> Function(ServerSettings settings, String token);
typedef SharedGrantAction =
    Future<AddressBookGrant> Function(
      ServerSettings settings,
      String token,
      String username,
      String permission,
    );
typedef SharedRevokeAction =
    Future<void> Function(
      ServerSettings settings,
      String token,
      String username,
    );
typedef SharedBooksLoader =
    Future<List<SharedAddressBook>> Function(
      ServerSettings settings,
      String token,
    );
typedef SharedPeerUpdater =
    Future<AddressBookPeer> Function(
      ServerSettings settings,
      String token,
      String owner,
      String peerId, {
      String? name,
      List<String>? tags,
      int? ifRevision,
    });
typedef SharedPeerDeleter =
    Future<void> Function(
      ServerSettings settings,
      String token,
      String owner,
      String peerId, {
      int? ifRevision,
    });
typedef SharedPeerAdder =
    Future<AddressBookPeer> Function(
      ServerSettings settings,
      String token,
      String owner,
      String peerId, {
      String? name,
      List<String>? tags,
    });
typedef SharedBookGrantsLoader =
    Future<AddressBookGrants> Function(
      ServerSettings settings,
      String token,
      String owner,
    );
typedef SharedBookGrantAction =
    Future<AddressBookGrant> Function(
      ServerSettings settings,
      String token,
      String owner,
      String username,
      String permission,
    );
typedef SharedBookRevokeAction =
    Future<void> Function(
      ServerSettings settings,
      String token,
      String owner,
      String username,
    );

String sharedPermissionLabel(String permission) {
  switch (permission) {
    case 'write':
      return '读写';
    case 'control':
      return '完全控制';
  }
  return '只读';
}

/// Address books shared by other accounts, plus management of the shares this
/// account hands out. Selecting a shared peer closes the dialog with that peer
/// so the home page can start a normal session.
class SharedBooksDialog extends StatefulWidget {
  const SharedBooksDialog({
    super.key,
    required this.server,
    required this.account,
    this.loadGrants,
    this.share,
    this.revoke,
    this.loadBooks,
    this.updatePeer,
    this.deletePeer,
    this.addPeer,
    this.loadSharedGrants,
    this.shareShared,
    this.revokeShared,
  });

  final ServerSettings server;
  final AccountSession account;
  final SharedGrantsLoader? loadGrants;
  final SharedGrantAction? share;
  final SharedRevokeAction? revoke;
  final SharedBooksLoader? loadBooks;
  final SharedPeerUpdater? updatePeer;
  final SharedPeerDeleter? deletePeer;
  final SharedPeerAdder? addPeer;
  final SharedBookGrantsLoader? loadSharedGrants;
  final SharedBookGrantAction? shareShared;
  final SharedBookRevokeAction? revokeShared;

  @override
  State<SharedBooksDialog> createState() => _SharedBooksDialogState();
}

class _SharedBooksDialogState extends State<SharedBooksDialog> {
  final TextEditingController _username = TextEditingController();
  AddressBookGrants _grants = const AddressBookGrants();
  List<SharedAddressBook> _books = const <SharedAddressBook>[];
  String _permission = 'read';
  bool _loading = true;
  bool _busy = false;
  String _error = '';

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void dispose() {
    _username.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    setState(() {
      _loading = true;
      _error = '';
    });
    try {
      final AddressBookGrants grants =
          await (widget.loadGrants ?? ServerApi.addressBookGrants)(
            widget.server,
            widget.account.token,
          );
      final List<SharedAddressBook> books =
          await (widget.loadBooks ?? ServerApi.sharedAddressBooks)(
            widget.server,
            widget.account.token,
          );
      if (!mounted) {
        return;
      }
      setState(() {
        _grants = grants;
        _books = books;
        _loading = false;
      });
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _error = '读取共享地址簿失败：$error';
        });
      }
    }
  }

  Future<void> _perform(Future<void> Function() action, String failure) async {
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      await action();
      await _load();
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.conflict) {
        await _load();
        if (mounted) {
          setState(() {
            _busy = false;
            if (_error.isEmpty) {
              _error = '该条目已在其他设备修改，已刷新为服务器版本';
            }
          });
        }
        return;
      }
      if (mounted) {
        setState(() {
          _busy = false;
          _error = error.message;
        });
      }
      return;
    } catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = '$failure：$error';
        });
      }
      return;
    }
    if (mounted) {
      setState(() => _busy = false);
    }
  }

  Future<void> _share() async {
    final String username = _username.text.trim();
    if (username.isEmpty) {
      setState(() => _error = '请输入要共享的用户名');
      return;
    }
    await _perform(() async {
      await (widget.share ?? ServerApi.shareAddressBook)(
        widget.server,
        widget.account.token,
        username,
        _permission,
      );
      _username.clear();
    }, '共享失败');
  }

  Future<void> _changePermission(
    AddressBookGrant grant,
    String permission,
  ) async {
    if (permission == grant.permission) {
      return;
    }
    await _perform(() async {
      await (widget.share ?? ServerApi.shareAddressBook)(
        widget.server,
        widget.account.token,
        grant.username,
        permission,
      );
    }, '修改权限失败');
  }

  Future<void> _revoke(String username) async {
    await _perform(() async {
      await (widget.revoke ?? ServerApi.unshareAddressBook)(
        widget.server,
        widget.account.token,
        username,
      );
    }, '撤销共享失败');
  }

  Future<void> _editPeer(SharedAddressBook book, AddressBookPeer peer) async {
    final _PeerEdit? edit = await showDialog<_PeerEdit>(
      context: context,
      builder: (BuildContext context) => _EditSharedPeerDialog(peer: peer),
    );
    if (edit == null || !mounted) {
      return;
    }
    await _perform(() async {
      await (widget.updatePeer ?? ServerApi.updateSharedAddressBookPeer)(
        widget.server,
        widget.account.token,
        book.owner,
        peer.id,
        name: edit.name,
        tags: edit.tags,
        ifRevision: peer.revision > 0 ? peer.revision : null,
      );
    }, '更新共享条目失败');
  }

  Future<void> _addPeer(SharedAddressBook book) async {
    final _PeerDraft? draft = await showDialog<_PeerDraft>(
      context: context,
      builder: (BuildContext context) => const _AddSharedPeerDialog(),
    );
    if (draft == null || !mounted) {
      return;
    }
    await _perform(() async {
      await (widget.addPeer ?? ServerApi.addSharedAddressBookPeer)(
        widget.server,
        widget.account.token,
        book.owner,
        draft.id,
        name: draft.name,
        tags: draft.tags,
      );
    }, '添加共享条目失败');
  }

  Future<void> _deletePeer(SharedAddressBook book, AddressBookPeer peer) async {
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) => AlertDialog(
        title: const Text('删除共享条目'),
        content: Text(
          '从 ${book.owner} 的地址簿删除 ${peer.name.isEmpty ? peer.id : peer.name}？',
        ),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: const Text('取消'),
          ),
          FilledButton(
            key: const Key('shared-peer-delete-confirm'),
            onPressed: () => Navigator.of(context).pop(true),
            child: const Text('删除'),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) {
      return;
    }
    await _perform(() async {
      await (widget.deletePeer ?? ServerApi.deleteSharedAddressBookPeer)(
        widget.server,
        widget.account.token,
        book.owner,
        peer.id,
        ifRevision: peer.revision > 0 ? peer.revision : null,
      );
    }, '删除共享条目失败');
  }

  Future<void> _manageGrants(SharedAddressBook book) async {
    await showDialog<void>(
      context: context,
      builder: (BuildContext context) => SharedBookGrantsDialog(
        server: widget.server,
        account: widget.account,
        owner: book.owner,
        loadGrants: widget.loadSharedGrants,
        share: widget.shareShared,
        revoke: widget.revokeShared,
      ),
    );
    if (mounted) {
      await _load();
    }
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    return AlertDialog(
      backgroundColor: theme.colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: const Text('共享地址簿'),
      content: SizedBox(
        width: 480,
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxHeight: 540),
          child: _loading
              ? const Padding(
                  key: Key('shared-books-loading'),
                  padding: EdgeInsets.symmetric(vertical: 32),
                  child: Center(child: CircularProgressIndicator()),
                )
              : SingleChildScrollView(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Text(
                        '共享条目不会复制到对方自己的地址簿；'
                        '读写与完全控制允许对方直接修改本账号的条目。',
                        style: mutedStyle(context, size: 11.5),
                      ),
                      if (_error.isNotEmpty) ...<Widget>[
                        const SizedBox(height: 10),
                        Row(
                          children: <Widget>[
                            Expanded(
                              child: Text(
                                _error,
                                key: const Key('shared-books-error'),
                                style: TextStyle(
                                  color: theme.colorScheme.error,
                                  fontSize: 12,
                                ),
                              ),
                            ),
                            TextButton(
                              key: const Key('shared-books-retry'),
                              onPressed: _busy ? null : _load,
                              child: const Text('重试'),
                            ),
                          ],
                        ),
                      ],
                      const SizedBox(height: 14),
                      _sectionTitle(context, '共享给我的地址簿'),
                      if (_books.isEmpty)
                        Text('暂无共享', style: mutedStyle(context, size: 12))
                      else
                        for (final SharedAddressBook book
                            in _books) ...<Widget>[
                          Padding(
                            padding: const EdgeInsets.only(top: 8, bottom: 2),
                            child: Row(
                              children: <Widget>[
                                Icon(
                                  Icons.folder_shared_outlined,
                                  size: 15,
                                  color: theme.colorScheme.primary,
                                ),
                                const SizedBox(width: 6),
                                Expanded(
                                  child: Text(
                                    book.owner,
                                    style: TextStyle(
                                      fontSize: 12,
                                      fontWeight: FontWeight.w600,
                                      color: theme.colorScheme.onSurface,
                                    ),
                                  ),
                                ),
                                _PermissionChip(
                                  permission: book.permission,
                                  key: Key(
                                    'shared-permission-chip-${book.owner}',
                                  ),
                                ),
                                const SizedBox(width: 6),
                                Text(
                                  '${book.peers.length} 台',
                                  style: mutedStyle(context, size: 10.5),
                                ),
                                if (addressBookPermissionAllows(
                                  book.permission,
                                  'write',
                                ))
                                  IconButton(
                                    key: Key('shared-add-${book.owner}'),
                                    tooltip: '添加共享条目',
                                    visualDensity: VisualDensity.compact,
                                    onPressed: _busy
                                        ? null
                                        : () => _addPeer(book),
                                    icon: Icon(
                                      Icons.add_circle_outline_rounded,
                                      size: 17,
                                      color: theme.colorScheme.primary,
                                    ),
                                  ),
                                if (book.permission == 'control')
                                  TextButton(
                                    key: Key('shared-manage-${book.owner}'),
                                    onPressed: _busy
                                        ? null
                                        : () => _manageGrants(book),
                                    child: const Text('管理授权'),
                                  ),
                              ],
                            ),
                          ),
                          if (book.peers.isEmpty)
                            Padding(
                              padding: const EdgeInsets.only(
                                left: 21,
                                bottom: 4,
                              ),
                              child: Text(
                                '该地址簿为空',
                                style: mutedStyle(context, size: 11),
                              ),
                            )
                          else
                            for (final AddressBookPeer peer in book.peers)
                              ListTile(
                                dense: true,
                                contentPadding: const EdgeInsets.only(
                                  left: 21,
                                  right: 0,
                                ),
                                title: Text(
                                  peer.name.isEmpty ? peer.id : peer.name,
                                  style: const TextStyle(fontSize: 12.5),
                                ),
                                subtitle: Text(
                                  <String>[
                                    peer.id,
                                    if (peer.tags.isNotEmpty)
                                      peer.tags
                                          .map((String tag) => '#$tag')
                                          .join(' '),
                                  ].join(' · '),
                                  style: mutedStyle(context, size: 10.5),
                                ),
                                trailing: Row(
                                  mainAxisSize: MainAxisSize.min,
                                  children: <Widget>[
                                    if (addressBookPermissionAllows(
                                      book.permission,
                                      'write',
                                    ))
                                      PopupMenuButton<String>(
                                        key: Key(
                                          'shared-peer-menu-${book.owner}-${peer.id}',
                                        ),
                                        enabled: !_busy,
                                        tooltip: '共享条目操作',
                                        onSelected: (String action) {
                                          if (action == 'edit') {
                                            _editPeer(book, peer);
                                          } else if (action == 'delete') {
                                            _deletePeer(book, peer);
                                          }
                                        },
                                        itemBuilder: (_) =>
                                            const <PopupMenuEntry<String>>[
                                              PopupMenuItem<String>(
                                                value: 'edit',
                                                child: Text('编辑名称/标签'),
                                              ),
                                              PopupMenuItem<String>(
                                                value: 'delete',
                                                child: Text('删除'),
                                              ),
                                            ],
                                      ),
                                    TextButton(
                                      key: Key(
                                        'shared-connect-${book.owner}-${peer.id}',
                                      ),
                                      onPressed: _busy
                                          ? null
                                          : () =>
                                                Navigator.of(context).pop(peer),
                                      child: const Text('连接'),
                                    ),
                                  ],
                                ),
                              ),
                        ],
                      const SizedBox(height: 14),
                      _sectionTitle(context, '我共享的地址簿'),
                      if (_grants.outgoing.isEmpty)
                        Text('尚未共享给其他用户', style: mutedStyle(context, size: 12))
                      else
                        for (final AddressBookGrant grant in _grants.outgoing)
                          ListTile(
                            dense: true,
                            contentPadding: EdgeInsets.zero,
                            leading: const Icon(
                              Icons.person_outline_rounded,
                              size: 18,
                            ),
                            title: Text(
                              grant.username,
                              style: const TextStyle(fontSize: 12.5),
                            ),
                            subtitle: Text(
                              sharedPermissionLabel(grant.permission),
                              style: mutedStyle(context, size: 10.5),
                            ),
                            trailing: Row(
                              mainAxisSize: MainAxisSize.min,
                              children: <Widget>[
                                DropdownButton<String>(
                                  key: Key(
                                    'shared-permission-${grant.username}',
                                  ),
                                  value: grant.permission,
                                  underline: const SizedBox.shrink(),
                                  style: TextStyle(
                                    fontSize: 11,
                                    color: theme.colorScheme.onSurface,
                                  ),
                                  items: <DropdownMenuItem<String>>[
                                    for (final String permission
                                        in addressBookPermissions)
                                      DropdownMenuItem<String>(
                                        value: permission,
                                        child: Text(
                                          sharedPermissionLabel(permission),
                                        ),
                                      ),
                                  ],
                                  onChanged: _busy
                                      ? null
                                      : (String? value) {
                                          if (value != null) {
                                            _changePermission(grant, value);
                                          }
                                        },
                                ),
                                IconButton(
                                  key: Key('shared-revoke-${grant.username}'),
                                  tooltip: '撤销共享',
                                  onPressed: _busy
                                      ? null
                                      : () => _revoke(grant.username),
                                  icon: Icon(
                                    Icons.link_off_rounded,
                                    size: 18,
                                    color: theme.colorScheme.error,
                                  ),
                                ),
                              ],
                            ),
                          ),
                      const SizedBox(height: 8),
                      Row(
                        children: <Widget>[
                          Expanded(
                            child: TextField(
                              key: const Key('shared-username'),
                              controller: _username,
                              autocorrect: false,
                              enableSuggestions: false,
                              decoration: const InputDecoration(
                                isDense: true,
                                labelText: '共享给用户名',
                                prefixIcon: Icon(
                                  Icons.person_add_alt_1,
                                  size: 17,
                                ),
                              ),
                              onSubmitted: (_) {
                                if (!_busy) {
                                  _share();
                                }
                              },
                            ),
                          ),
                          const SizedBox(width: 8),
                          DropdownButton<String>(
                            key: const Key('shared-permission'),
                            value: _permission,
                            underline: const SizedBox.shrink(),
                            style: TextStyle(
                              fontSize: 12,
                              color: theme.colorScheme.onSurface,
                            ),
                            items: <DropdownMenuItem<String>>[
                              for (final String permission
                                  in addressBookPermissions)
                                DropdownMenuItem<String>(
                                  value: permission,
                                  child: Text(
                                    sharedPermissionLabel(permission),
                                  ),
                                ),
                            ],
                            onChanged: _busy
                                ? null
                                : (String? value) {
                                    if (value != null) {
                                      setState(() => _permission = value);
                                    }
                                  },
                          ),
                          const SizedBox(width: 8),
                          FilledButton(
                            key: const Key('shared-grant'),
                            onPressed: _busy ? null : _share,
                            child: const Text('共享'),
                          ),
                        ],
                      ),
                    ],
                  ),
                ),
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('关闭'),
        ),
      ],
    );
  }

  Widget _sectionTitle(BuildContext context, String text) {
    return Text(
      text,
      style: TextStyle(
        fontSize: 12,
        fontWeight: FontWeight.w600,
        color: Theme.of(context).colorScheme.onSurface,
      ),
    );
  }
}

/// Grant management for a shared book the signed-in user controls.
class SharedBookGrantsDialog extends StatefulWidget {
  const SharedBookGrantsDialog({
    super.key,
    required this.server,
    required this.account,
    required this.owner,
    this.loadGrants,
    this.share,
    this.revoke,
  });

  final ServerSettings server;
  final AccountSession account;
  final String owner;
  final SharedBookGrantsLoader? loadGrants;
  final SharedBookGrantAction? share;
  final SharedBookRevokeAction? revoke;

  @override
  State<SharedBookGrantsDialog> createState() => _SharedBookGrantsDialogState();
}

class _SharedBookGrantsDialogState extends State<SharedBookGrantsDialog> {
  final TextEditingController _username = TextEditingController();
  AddressBookGrants _grants = const AddressBookGrants();
  String _permission = 'read';
  bool _loading = true;
  bool _busy = false;
  String _error = '';

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void dispose() {
    _username.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    setState(() {
      _loading = true;
      _error = '';
    });
    try {
      final AddressBookGrants grants =
          await (widget.loadGrants ?? ServerApi.sharedAddressBookGrants)(
            widget.server,
            widget.account.token,
            widget.owner,
          );
      if (!mounted) {
        return;
      }
      setState(() {
        _grants = grants;
        _loading = false;
      });
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _error = '读取授权失败：$error';
        });
      }
    }
  }

  Future<void> _share() async {
    final String username = _username.text.trim();
    if (username.isEmpty) {
      setState(() => _error = '请输入要共享的用户名');
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      await (widget.share ?? ServerApi.shareSharedAddressBook)(
        widget.server,
        widget.account.token,
        widget.owner,
        username,
        _permission,
      );
      _username.clear();
      await _load();
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = error.message;
        });
      }
      return;
    } catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = '共享失败：$error';
        });
      }
      return;
    }
    if (mounted) {
      setState(() => _busy = false);
    }
  }

  Future<void> _revoke(String username) async {
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      await (widget.revoke ?? ServerApi.unshareSharedAddressBook)(
        widget.server,
        widget.account.token,
        widget.owner,
        username,
      );
      await _load();
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = error.message;
        });
      }
      return;
    } catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = '撤销共享失败：$error';
        });
      }
      return;
    }
    if (mounted) {
      setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    return AlertDialog(
      backgroundColor: theme.colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: Text('管理「${widget.owner}」的共享'),
      content: SizedBox(
        width: 400,
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxHeight: 420),
          child: _loading
              ? const Padding(
                  key: Key('shared-book-grants-loading'),
                  padding: EdgeInsets.symmetric(vertical: 28),
                  child: Center(child: CircularProgressIndicator()),
                )
              : SingleChildScrollView(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      if (_error.isNotEmpty) ...<Widget>[
                        Row(
                          children: <Widget>[
                            Expanded(
                              child: Text(
                                _error,
                                key: const Key('shared-book-grants-error'),
                                style: TextStyle(
                                  color: theme.colorScheme.error,
                                  fontSize: 12,
                                ),
                              ),
                            ),
                            TextButton(
                              key: const Key('shared-book-grants-retry'),
                              onPressed: _busy ? null : _load,
                              child: const Text('重试'),
                            ),
                          ],
                        ),
                        const SizedBox(height: 10),
                      ],
                      if (_grants.outgoing.isEmpty)
                        Text(
                          '该地址簿尚未共享给其他用户',
                          style: mutedStyle(context, size: 12),
                        )
                      else
                        for (final AddressBookGrant grant in _grants.outgoing)
                          ListTile(
                            dense: true,
                            contentPadding: EdgeInsets.zero,
                            leading: const Icon(
                              Icons.person_outline_rounded,
                              size: 18,
                            ),
                            title: Text(
                              grant.username,
                              style: const TextStyle(fontSize: 12.5),
                            ),
                            subtitle: Text(
                              sharedPermissionLabel(grant.permission),
                              style: mutedStyle(context, size: 10.5),
                            ),
                            trailing: IconButton(
                              key: Key('shared-book-revoke-${grant.username}'),
                              tooltip: '撤销共享',
                              onPressed: _busy
                                  ? null
                                  : () => _revoke(grant.username),
                              icon: Icon(
                                Icons.link_off_rounded,
                                size: 18,
                                color: theme.colorScheme.error,
                              ),
                            ),
                          ),
                      const SizedBox(height: 8),
                      Row(
                        children: <Widget>[
                          Expanded(
                            child: TextField(
                              key: const Key('shared-book-username'),
                              controller: _username,
                              autocorrect: false,
                              enableSuggestions: false,
                              decoration: const InputDecoration(
                                isDense: true,
                                labelText: '共享给用户名',
                                prefixIcon: Icon(
                                  Icons.person_add_alt_1,
                                  size: 17,
                                ),
                              ),
                              onSubmitted: (_) {
                                if (!_busy) {
                                  _share();
                                }
                              },
                            ),
                          ),
                          const SizedBox(width: 8),
                          DropdownButton<String>(
                            key: const Key('shared-book-permission'),
                            value: _permission,
                            underline: const SizedBox.shrink(),
                            style: TextStyle(
                              fontSize: 12,
                              color: theme.colorScheme.onSurface,
                            ),
                            items: <DropdownMenuItem<String>>[
                              for (final String permission
                                  in addressBookPermissions)
                                DropdownMenuItem<String>(
                                  value: permission,
                                  child: Text(
                                    sharedPermissionLabel(permission),
                                  ),
                                ),
                            ],
                            onChanged: _busy
                                ? null
                                : (String? value) {
                                    if (value != null) {
                                      setState(() => _permission = value);
                                    }
                                  },
                          ),
                          const SizedBox(width: 8),
                          FilledButton(
                            key: const Key('shared-book-grant'),
                            onPressed: _busy ? null : _share,
                            child: const Text('共享'),
                          ),
                        ],
                      ),
                    ],
                  ),
                ),
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('关闭'),
        ),
      ],
    );
  }
}

class _PeerEdit {
  const _PeerEdit(this.name, this.tags);

  final String name;
  final List<String> tags;
}

class _PeerDraft {
  const _PeerDraft(this.id, this.name, this.tags);

  final String id;
  final String name;
  final List<String> tags;
}

List<String> _splitTags(String text) {
  final List<String> tags = <String>[];
  final Set<String> seen = <String>{};
  for (final String raw in text.split(RegExp('[,，]'))) {
    final String tag = raw.trim();
    if (tag.isNotEmpty && seen.add(tag)) {
      tags.add(tag);
    }
  }
  return tags;
}

class _AddSharedPeerDialog extends StatefulWidget {
  const _AddSharedPeerDialog();

  @override
  State<_AddSharedPeerDialog> createState() => _AddSharedPeerDialogState();
}

class _AddSharedPeerDialogState extends State<_AddSharedPeerDialog> {
  final TextEditingController _id = TextEditingController();
  final TextEditingController _name = TextEditingController();
  final TextEditingController _tags = TextEditingController();
  String _error = '';

  @override
  void dispose() {
    _id.dispose();
    _name.dispose();
    _tags.dispose();
    super.dispose();
  }

  void _save() {
    final String id = _id.text.trim();
    if (id.isEmpty) {
      setState(() => _error = '请输入设备 ID');
      return;
    }
    Navigator.of(
      context,
    ).pop(_PeerDraft(id, _name.text.trim(), _splitTags(_tags.text)));
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: const Text('添加共享条目'),
      content: Column(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          TextField(
            key: const Key('shared-add-id'),
            controller: _id,
            autocorrect: false,
            enableSuggestions: false,
            decoration: const InputDecoration(
              isDense: true,
              labelText: '设备 ID',
            ),
          ),
          const SizedBox(height: 12),
          TextField(
            key: const Key('shared-add-name'),
            controller: _name,
            decoration: const InputDecoration(isDense: true, labelText: '名称'),
          ),
          const SizedBox(height: 12),
          TextField(
            key: const Key('shared-add-tags'),
            controller: _tags,
            decoration: const InputDecoration(
              isDense: true,
              labelText: '标签（逗号分隔）',
            ),
          ),
          if (_error.isNotEmpty) ...<Widget>[
            const SizedBox(height: 10),
            Align(
              alignment: Alignment.centerLeft,
              child: Text(
                _error,
                key: const Key('shared-add-error'),
                style: TextStyle(
                  color: Theme.of(context).colorScheme.error,
                  fontSize: 12,
                ),
              ),
            ),
          ],
        ],
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('取消'),
        ),
        FilledButton(
          key: const Key('shared-add-save'),
          onPressed: _save,
          child: const Text('添加'),
        ),
      ],
    );
  }
}

class _EditSharedPeerDialog extends StatefulWidget {
  const _EditSharedPeerDialog({required this.peer});

  final AddressBookPeer peer;

  @override
  State<_EditSharedPeerDialog> createState() => _EditSharedPeerDialogState();
}

class _EditSharedPeerDialogState extends State<_EditSharedPeerDialog> {
  late final TextEditingController _name = TextEditingController(
    text: widget.peer.name,
  );
  late final TextEditingController _tags = TextEditingController(
    text: widget.peer.tags.join(', '),
  );

  @override
  void dispose() {
    _name.dispose();
    _tags.dispose();
    super.dispose();
  }

  void _save() {
    Navigator.of(
      context,
    ).pop(_PeerEdit(_name.text.trim(), _splitTags(_tags.text)));
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: const Text('编辑共享条目'),
      content: Column(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          TextField(
            key: const Key('shared-peer-name'),
            controller: _name,
            decoration: const InputDecoration(isDense: true, labelText: '名称'),
          ),
          const SizedBox(height: 12),
          TextField(
            key: const Key('shared-peer-tags'),
            controller: _tags,
            decoration: const InputDecoration(
              isDense: true,
              labelText: '标签（逗号分隔）',
            ),
          ),
        ],
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('取消'),
        ),
        FilledButton(
          key: const Key('shared-peer-save'),
          onPressed: _save,
          child: const Text('保存'),
        ),
      ],
    );
  }
}

class _PermissionChip extends StatelessWidget {
  const _PermissionChip({super.key, required this.permission});

  final String permission;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
      decoration: BoxDecoration(
        color: theme.colorScheme.primaryContainer,
        borderRadius: BorderRadius.circular(6),
      ),
      child: Text(
        sharedPermissionLabel(permission),
        style: TextStyle(
          fontSize: 9.5,
          color: theme.colorScheme.onPrimaryContainer,
        ),
      ),
    );
  }
}
