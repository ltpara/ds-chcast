import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/pages/shared_books_dialog.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';

class _Selection {
  AddressBookPeer? peer;
}

void main() {
  const ServerSettings settings = ServerSettings(url: 'http://127.0.0.1:18643');
  final AccountSession account = AccountSession(
    server: settings.url,
    username: 'alice',
    token: 'account-token',
    expiresAt: DateTime.utc(2030),
  );

  Future<_Selection> open(WidgetTester tester, SharedBooksDialog dialog) async {
    final _Selection selection = _Selection();
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (BuildContext context) {
            return TextButton(
              onPressed: () async {
                selection.peer = await showDialog<AddressBookPeer>(
                  context: context,
                  builder: (BuildContext context) => dialog,
                );
              },
              child: const Text('打开'),
            );
          },
        ),
      ),
    );
    await tester.tap(find.text('打开'));
    await tester.pumpAndSettle();
    return selection;
  }

  testWidgets('a shared peer can be selected for a connection', (tester) async {
    final SharedBooksDialog dialog = SharedBooksDialog(
      server: settings,
      account: account,
      loadGrants: (ServerSettings server, String token) async =>
          const AddressBookGrants(
            incoming: <AddressBookGrant>[AddressBookGrant(username: 'carol')],
          ),
      loadBooks: (ServerSettings server, String token) async =>
          const <SharedAddressBook>[
            SharedAddressBook(
              owner: 'carol',
              revision: 9,
              peers: <AddressBookPeer>[
                AddressBookPeer(
                  id: 'sharedbox',
                  name: '共享设备',
                  tags: <String>['shared'],
                  revision: 8,
                ),
              ],
            ),
          ],
    );
    final _Selection selection = await open(tester, dialog);

    expect(find.text('carol'), findsOneWidget);
    expect(find.text('共享设备'), findsOneWidget);
    expect(find.text('sharedbox · #shared'), findsOneWidget);
    expect(
      find.byKey(const Key('shared-permission-chip-carol')),
      findsOneWidget,
    );
    expect(
      find.descendant(
        of: find.byKey(const Key('shared-permission-chip-carol')),
        matching: find.text('只读'),
      ),
      findsOneWidget,
    );
    expect(
      find.byKey(const Key('shared-peer-menu-carol-sharedbox')),
      findsNothing,
    );
    expect(find.byKey(const Key('shared-manage-carol')), findsNothing);
    expect(find.byKey(const Key('shared-add-carol')), findsNothing);
    await tester.tap(find.byKey(const Key('shared-connect-carol-sharedbox')));
    await tester.pumpAndSettle();

    expect(selection.peer?.id, 'sharedbox');
    expect(selection.peer?.name, '共享设备');
    expect(selection.peer?.tags, <String>['shared']);
  });

  testWidgets('shares can be granted with a permission and revoked', (
    tester,
  ) async {
    final List<AddressBookGrant> outgoing = <AddressBookGrant>[
      const AddressBookGrant(username: 'bob'),
    ];
    final List<String> shared = <String>[];
    final List<String> permissions = <String>[];
    final List<String> revoked = <String>[];
    final SharedBooksDialog dialog = SharedBooksDialog(
      server: settings,
      account: account,
      loadGrants: (ServerSettings server, String token) async =>
          AddressBookGrants(outgoing: List<AddressBookGrant>.of(outgoing)),
      loadBooks: (ServerSettings server, String token) async =>
          const <SharedAddressBook>[],
      share:
          (
            ServerSettings server,
            String token,
            String username,
            String permission, {
            String? groupId,
          }) async {
            shared.add('$username@${groupId ?? ''}');
            permissions.add(permission);
            outgoing.removeWhere(
              (AddressBookGrant grant) => grant.username == username,
            );
            outgoing.add(
              AddressBookGrant(username: username, permission: permission),
            );
            return AddressBookGrant(username: username, permission: permission);
          },
      revoke:
          (ServerSettings server, String token, String username, {String? groupId}) async {
        revoked.add(username);
        outgoing.removeWhere(
          (AddressBookGrant grant) => grant.username == username,
        );
      },
    );
    await open(tester, dialog);

    expect(find.text('bob'), findsOneWidget);
    await tester.tap(find.byKey(const Key('shared-permission')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('读写').last);
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('shared-username')), ' dave ');
    await tester.tap(find.byKey(const Key('shared-grant')));
    await tester.pumpAndSettle();
    expect(shared, <String>['dave@']);
    expect(permissions, <String>['write']);
    expect(find.text('dave'), findsOneWidget);
    expect(find.text('读写'), findsWidgets);

    await tester.tap(find.byKey(const Key('shared-revoke-bob')));
    await tester.pumpAndSettle();
    expect(revoked, <String>['bob']);
    expect(find.text('bob'), findsNothing);
    expect(find.text('dave'), findsOneWidget);
  });

  testWidgets('an existing grant permission can be changed', (tester) async {
    final List<AddressBookGrant> outgoing = <AddressBookGrant>[
      const AddressBookGrant(username: 'bob'),
    ];
    final List<String> permissions = <String>[];
    final SharedBooksDialog dialog = SharedBooksDialog(
      server: settings,
      account: account,
      loadGrants: (ServerSettings server, String token) async =>
          AddressBookGrants(outgoing: List<AddressBookGrant>.of(outgoing)),
      loadBooks: (ServerSettings server, String token) async =>
          const <SharedAddressBook>[],
      share:
          (
            ServerSettings server,
            String token,
            String username,
            String permission, {
            String? groupId,
          }) async {
            permissions.add('$username=$permission@${groupId ?? ''}');
            outgoing.removeWhere(
              (AddressBookGrant grant) => grant.username == username,
            );
            outgoing.add(
              AddressBookGrant(username: username, permission: permission),
            );
            return AddressBookGrant(username: username, permission: permission);
          },
    );
    await open(tester, dialog);

    await tester.tap(find.byKey(const Key('shared-permission-bob')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('完全控制').last);
    await tester.pumpAndSettle();

    expect(permissions, <String>['bob=control@']);
    expect(find.text('完全控制'), findsWidgets);
  });

  testWidgets('writable shared entries can be edited and deleted', (
    tester,
  ) async {
    final List<String> updates = <String>[];
    final List<String> deletes = <String>[];
    final SharedBooksDialog dialog = SharedBooksDialog(
      server: settings,
      account: account,
      loadGrants: (ServerSettings server, String token) async =>
          const AddressBookGrants(),
      loadBooks: (ServerSettings server, String token) async =>
          const <SharedAddressBook>[
            SharedAddressBook(
              owner: 'carol',
              permission: 'write',
              peers: <AddressBookPeer>[
                AddressBookPeer(
                  id: 'sharedbox',
                  name: '共享设备',
                  revision: 5,
                ),
              ],
            ),
          ],
      updatePeer:
          (
            ServerSettings server,
            String token,
            String owner,
            String peerId, {
            String? name,
            List<String>? tags,
            int? ifRevision,
          }) async {
            updates.add('$owner/$peerId/$name/${tags?.join('+')}/$ifRevision');
            return AddressBookPeer(
              id: peerId,
              name: name ?? '',
              tags: tags ?? const <String>[],
            );
          },
      deletePeer:
          (
            ServerSettings server,
            String token,
            String owner,
            String peerId, {
            int? ifRevision,
          }) async {
            deletes.add('$owner/$peerId/$ifRevision');
          },
    );
    await open(tester, dialog);

    expect(find.text('读写'), findsOneWidget);
    await tester.tap(find.byKey(const Key('shared-peer-menu-carol-sharedbox')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('编辑名称/标签'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('shared-peer-name')), '会议室');
    await tester.enterText(
      find.byKey(const Key('shared-peer-tags')),
      'lab, vip, lab',
    );
    await tester.tap(find.byKey(const Key('shared-peer-save')));
    await tester.pumpAndSettle();
    expect(updates, <String>['carol/sharedbox/会议室/lab+vip/5']);

    await tester.tap(find.byKey(const Key('shared-peer-menu-carol-sharedbox')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('删除'));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('shared-peer-delete-confirm')));
    await tester.pumpAndSettle();
    expect(deletes, <String>['carol/sharedbox/5']);
  });

  testWidgets('writable shared books accept new peers', (tester) async {
    final List<String> added = <String>[];
    final SharedBooksDialog dialog = SharedBooksDialog(
      server: settings,
      account: account,
      loadGrants: (ServerSettings server, String token) async =>
          const AddressBookGrants(),
      loadBooks: (ServerSettings server, String token) async =>
          const <SharedAddressBook>[
            SharedAddressBook(
              owner: 'carol',
              permission: 'write',
              peers: <AddressBookPeer>[
                AddressBookPeer(id: 'sharedbox', name: '共享设备'),
              ],
            ),
          ],
      addPeer:
          (
            ServerSettings server,
            String token,
            String owner,
            String peerId, {
            String? name,
            List<String>? tags,
            String? groupId,
          }) async {
            added.add('$owner/$peerId/$name/${tags?.join('+')}@${groupId ?? ''}');
            return AddressBookPeer(
              id: peerId,
              name: name ?? '',
              tags: tags ?? const <String>[],
            );
          },
    );
    await open(tester, dialog);

    await tester.tap(find.byKey(const Key('shared-add-carol')));
    await tester.pumpAndSettle();
    expect(find.text('添加共享条目'), findsOneWidget);
    await tester.tap(find.byKey(const Key('shared-add-save')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('shared-add-error')), findsOneWidget);
    expect(added, isEmpty);

    await tester.enterText(find.byKey(const Key('shared-add-id')), ' dlbox ');
    await tester.enterText(find.byKey(const Key('shared-add-name')), '下载机');
    await tester.enterText(
      find.byKey(const Key('shared-add-tags')),
      'lab， vip, lab',
    );
    await tester.tap(find.byKey(const Key('shared-add-save')));
    await tester.pumpAndSettle();

    expect(added, <String>['carol/dlbox/下载机/lab+vip@']);
    expect(find.byKey(const Key('shared-add-id')), findsNothing);
    expect(find.byKey(const Key('shared-books-error')), findsNothing);
  });

  testWidgets('control permission exposes shared grant management', (
    tester,
  ) async {
    final List<AddressBookGrant> controlled = <AddressBookGrant>[
      const AddressBookGrant(username: 'dave'),
    ];
    final List<String> shared = <String>[];
    final List<String> revoked = <String>[];
    final SharedBooksDialog dialog = SharedBooksDialog(
      server: settings,
      account: account,
      loadGrants: (ServerSettings server, String token) async =>
          const AddressBookGrants(),
      loadBooks: (ServerSettings server, String token) async =>
          const <SharedAddressBook>[
            SharedAddressBook(
              owner: 'carol',
              permission: 'control',
              peers: <AddressBookPeer>[AddressBookPeer(id: 'sharedbox')],
            ),
          ],
      loadSharedGrants:
          (ServerSettings server, String token, String owner) async =>
              AddressBookGrants(
                outgoing: List<AddressBookGrant>.of(controlled),
              ),
      shareShared:
          (
            ServerSettings server,
            String token,
            String owner,
            String username,
            String permission, {
            String? groupId,
          }) async {
            shared.add('$owner/$username/$permission@${groupId ?? ''}');
            controlled.add(
              AddressBookGrant(
                username: username,
                permission: permission,
                groupId: groupId ?? '',
              ),
            );
            return AddressBookGrant(
              username: username,
              permission: permission,
              groupId: groupId ?? '',
            );
          },
      revokeShared:
          (
            ServerSettings server,
            String token,
            String owner,
            String username, {
            String? groupId,
          }) async {
            revoked.add('$owner/$username@${groupId ?? ''}');
            controlled.removeWhere(
              (AddressBookGrant grant) =>
                  grant.username == username &&
                  grant.groupId == (groupId ?? ''),
            );
          },
    );
    await open(tester, dialog);

    expect(find.text('完全控制'), findsOneWidget);
    await tester.tap(find.byKey(const Key('shared-manage-carol')));
    await tester.pumpAndSettle();
    expect(find.text('管理「carol」的共享'), findsOneWidget);
    expect(find.text('dave'), findsOneWidget);

    await tester.enterText(
      find.byKey(const Key('shared-book-username')),
      'erin',
    );
    await tester.tap(find.byKey(const Key('shared-book-permission')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('读写').last);
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('shared-book-grant')));
    await tester.pumpAndSettle();
    expect(shared, <String>['carol/erin/write@']);
    expect(find.text('erin'), findsOneWidget);

    await tester.tap(find.byKey(const Key('shared-book-revoke-dave')));
    await tester.pumpAndSettle();
    expect(revoked, <String>['carol/dave@']);
    expect(find.text('dave'), findsNothing);
  });

  testWidgets('group scoped shares and writes follow the granted group', (
    tester,
  ) async {
    final List<String> shared = <String>[];
    final List<String> added = <String>[];
    final List<AddressBookGrant> outgoing = <AddressBookGrant>[];
    final SharedBooksDialog dialog = SharedBooksDialog(
      server: settings,
      account: account,
      groups: const <AddressBookGroup>[
        AddressBookGroup(id: 'lab', name: '实验室'),
        AddressBookGroup(id: 'work', name: '办公室'),
      ],
      loadGrants: (ServerSettings server, String token) async =>
          AddressBookGrants(outgoing: List<AddressBookGrant>.of(outgoing)),
      loadBooks: (ServerSettings server, String token) async =>
          const <SharedAddressBook>[
            SharedAddressBook(
              owner: 'carol',
              permission: 'write',
              groups: <AddressBookGroup>[
                AddressBookGroup(id: 'lab', name: '实验室'),
                AddressBookGroup(id: 'work', name: '办公室'),
              ],
              scopes: <AddressBookScope>[
                AddressBookScope(permission: 'read'),
                AddressBookScope(groupId: 'lab', permission: 'write'),
              ],
              peers: <AddressBookPeer>[
                AddressBookPeer(id: 'devbox', groupId: 'lab'),
                AddressBookPeer(id: 'office', groupId: 'work'),
                AddressBookPeer(id: 'loose'),
              ],
            ),
          ],
      share:
          (
            ServerSettings server,
            String token,
            String username,
            String permission, {
            String? groupId,
          }) async {
            shared.add('$username/$permission@${groupId ?? ''}');
            final AddressBookGrant grant = AddressBookGrant(
              username: username,
              permission: permission,
              groupId: groupId ?? '',
            );
            outgoing
              ..removeWhere(
                (AddressBookGrant item) =>
                    item.username == grant.username &&
                    item.groupId == grant.groupId,
              )
              ..add(grant);
            return grant;
          },
      addPeer:
          (
            ServerSettings server,
            String token,
            String owner,
            String peerId, {
            String? name,
            List<String>? tags,
            String? groupId,
          }) async {
            added.add('$owner/$peerId@${groupId ?? ''}');
            return AddressBookPeer(id: peerId, groupId: groupId ?? '');
          },
    );
    await open(tester, dialog);

    expect(
      find.byKey(const Key('shared-peer-menu-carol-devbox')),
      findsOneWidget,
    );
    expect(find.byKey(const Key('shared-peer-menu-carol-office')), findsNothing);
    expect(find.byKey(const Key('shared-peer-menu-carol-loose')), findsNothing);
    expect(find.byKey(const Key('shared-add-carol')), findsOneWidget);

    await tester.tap(find.byKey(const Key('shared-scope')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('实验室').last);
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('shared-username')), 'erin');
    await tester.ensureVisible(find.byKey(const Key('shared-grant')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('shared-grant')));
    await tester.pumpAndSettle();

    expect(shared, <String>['erin/read@lab']);
    expect(find.textContaining('分组 实验室'), findsOneWidget);
    expect(
      find.byKey(const Key('shared-revoke-erin-lab')),
      findsOneWidget,
    );

    await tester.ensureVisible(find.byKey(const Key('shared-add-carol')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('shared-add-carol')));
    await tester.pumpAndSettle();
    expect(find.text('未分组'), findsNothing);
    await tester.enterText(find.byKey(const Key('shared-add-id')), 'newbox');
    await tester.tap(find.byKey(const Key('shared-add-save')));
    await tester.pumpAndSettle();
    expect(added, <String>['carol/newbox@lab']);
  });

  testWidgets('controlled shared books manage grants per group', (
    tester,
  ) async {
    final List<AddressBookGrant> controlled = <AddressBookGrant>[
      const AddressBookGrant(
        username: 'dave',
        permission: 'write',
        groupId: 'lab',
      ),
    ];
    final List<String> shared = <String>[];
    final List<String> revoked = <String>[];
    final SharedBooksDialog dialog = SharedBooksDialog(
      server: settings,
      account: account,
      loadGrants: (ServerSettings server, String token) async =>
          const AddressBookGrants(),
      loadBooks: (ServerSettings server, String token) async =>
          const <SharedAddressBook>[
            SharedAddressBook(
              owner: 'carol',
              permission: 'control',
              groups: <AddressBookGroup>[
                AddressBookGroup(id: 'lab', name: '实验室'),
              ],
              peers: <AddressBookPeer>[AddressBookPeer(id: 'sharedbox')],
            ),
          ],
      loadSharedGrants:
          (ServerSettings server, String token, String owner) async =>
              AddressBookGrants(
                outgoing: List<AddressBookGrant>.of(controlled),
              ),
      shareShared:
          (
            ServerSettings server,
            String token,
            String owner,
            String username,
            String permission, {
            String? groupId,
          }) async {
            shared.add('$owner/$username/$permission@${groupId ?? ''}');
            final AddressBookGrant grant = AddressBookGrant(
              username: username,
              permission: permission,
              groupId: groupId ?? '',
            );
            controlled.add(grant);
            return grant;
          },
      revokeShared:
          (
            ServerSettings server,
            String token,
            String owner,
            String username, {
            String? groupId,
          }) async {
            revoked.add('$owner/$username@${groupId ?? ''}');
            controlled.removeWhere(
              (AddressBookGrant grant) =>
                  grant.username == username &&
                  grant.groupId == (groupId ?? ''),
            );
          },
    );
    await open(tester, dialog);

    await tester.tap(find.byKey(const Key('shared-manage-carol')));
    await tester.pumpAndSettle();
    expect(find.textContaining('分组 实验室'), findsOneWidget);
    expect(
      find.byKey(const Key('shared-book-revoke-dave-lab')),
      findsOneWidget,
    );

    await tester.tap(find.byKey(const Key('shared-book-scope')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('实验室').last);
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const Key('shared-book-username')),
      'erin',
    );
    await tester.tap(find.byKey(const Key('shared-book-grant')));
    await tester.pumpAndSettle();
    expect(shared, <String>['carol/erin/read@lab']);

    await tester.tap(find.byKey(const Key('shared-book-revoke-dave-lab')));
    await tester.pumpAndSettle();
    expect(revoked, <String>['carol/dave@lab']);
    expect(find.text('dave'), findsNothing);
  });

  testWidgets('a forbidden write shows the server message', (tester) async {
    final SharedBooksDialog dialog = SharedBooksDialog(
      server: settings,
      account: account,
      loadGrants: (ServerSettings server, String token) async =>
          const AddressBookGrants(),
      loadBooks: (ServerSettings server, String token) async =>
          const <SharedAddressBook>[
            SharedAddressBook(
              owner: 'carol',
              permission: 'write',
              peers: <AddressBookPeer>[
                AddressBookPeer(id: 'sharedbox', name: '共享设备'),
              ],
            ),
          ],
      updatePeer:
          (
            ServerSettings server,
            String token,
            String owner,
            String peerId, {
            String? name,
            List<String>? tags,
            int? ifRevision,
          }) async {
            throw const ServerApiException('共享地址簿权限不足', statusCode: 403);
          },
    );
    await open(tester, dialog);

    await tester.tap(find.byKey(const Key('shared-peer-menu-carol-sharedbox')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('编辑名称/标签'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('shared-peer-name')), '会议室');
    await tester.tap(find.byKey(const Key('shared-peer-save')));
    await tester.pumpAndSettle();

    expect(find.byKey(const Key('shared-books-error')), findsOneWidget);
    expect(find.text('共享地址簿权限不足'), findsOneWidget);
  });

  testWidgets('a stale shared edit reloads the server row', (tester) async {
    String serverName = '旧名';
    int revision = 3;
    final SharedBooksDialog dialog = SharedBooksDialog(
      server: settings,
      account: account,
      loadGrants: (ServerSettings server, String token) async =>
          const AddressBookGrants(),
      loadBooks: (ServerSettings server, String token) async =>
          <SharedAddressBook>[
            SharedAddressBook(
              owner: 'carol',
              permission: 'write',
              peers: <AddressBookPeer>[
                AddressBookPeer(
                  id: 'sharedbox',
                  name: serverName,
                  revision: revision,
                ),
              ],
            ),
          ],
      updatePeer:
          (
            ServerSettings server,
            String token,
            String owner,
            String peerId, {
            String? name,
            List<String>? tags,
            int? ifRevision,
          }) async {
            if (ifRevision == 3) {
              serverName = '服务器名';
              revision = 8;
              throw const ServerApiException(
                'address book peer changed elsewhere',
                statusCode: 409,
              );
            }
            return AddressBookPeer(id: peerId, name: name ?? '');
          },
    );
    await open(tester, dialog);

    await tester.tap(find.byKey(const Key('shared-peer-menu-carol-sharedbox')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('编辑名称/标签'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('shared-peer-name')), '本地名');
    await tester.tap(find.byKey(const Key('shared-peer-save')));
    await tester.pumpAndSettle();

    expect(find.textContaining('已刷新为服务器版本'), findsOneWidget);
    expect(find.text('服务器名'), findsOneWidget);
    expect(find.text('旧名'), findsNothing);
  });

  testWidgets('a load failure is shown with a retry action', (tester) async {
    int attempts = 0;
    final SharedBooksDialog dialog = SharedBooksDialog(
      server: settings,
      account: account,
      loadGrants: (ServerSettings server, String token) async {
        attempts++;
        if (attempts == 1) {
          throw const ServerApiException('连接服务返回 HTTP 500', statusCode: 500);
        }
        return const AddressBookGrants();
      },
      loadBooks: (ServerSettings server, String token) async =>
          const <SharedAddressBook>[],
    );
    await open(tester, dialog);

    expect(find.byKey(const Key('shared-books-error')), findsOneWidget);
    expect(find.textContaining('HTTP 500'), findsOneWidget);
    await tester.tap(find.byKey(const Key('shared-books-retry')));
    await tester.pumpAndSettle();
    expect(attempts, 2);
    expect(find.byKey(const Key('shared-books-error')), findsNothing);
    expect(find.text('尚未共享给其他用户'), findsOneWidget);
  });
}
