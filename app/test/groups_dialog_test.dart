import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/pages/groups_dialog.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';

class _Result {
  bool? changed;
}

void main() {
  const ServerSettings settings = ServerSettings(url: 'http://127.0.0.1:18643');
  const List<AddressBookGroup> groups = <AddressBookGroup>[
    AddressBookGroup(id: 'lab', name: 'Lab', revision: 5),
    AddressBookGroup(id: 'work', name: 'Work', revision: 7),
  ];

  Future<_Result> open(WidgetTester tester, GroupsDialog dialog) async {
    final _Result result = _Result();
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (BuildContext context) {
            return TextButton(
              onPressed: () async {
                result.changed = await showDialog<bool>(
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
    return result;
  }

  testWidgets('a group can be created with a validated name', (tester) async {
    final List<String> created = <String>[];
    final GroupsDialog dialog = GroupsDialog(
      server: settings,
      token: 'account-token',
      groups: const <AddressBookGroup>[],
      create:
          (ServerSettings server, String token, String groupId, String? name,
              {String? parentId, int? position, int? ifRevision}) async {
        created.add('$groupId/$name');
        return AddressBookGroup(id: groupId, name: name ?? '', revision: 1);
      },
    );
    final _Result result = await open(tester, dialog);

    expect(find.textContaining('还没有分组'), findsOneWidget);
    await tester.tap(find.byKey(const Key('group-create')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('group-name-save')));
    await tester.pumpAndSettle();
    expect(find.text('请输入分组名称'), findsOneWidget);
    expect(created, isEmpty);

    await tester.enterText(find.byKey(const Key('group-name')), ' 办公室 ');
    await tester.tap(find.byKey(const Key('group-name-save')));
    await tester.pumpAndSettle();

    expect(created, hasLength(1));
    expect(created.single, endsWith('/办公室'));
    expect(find.text('办公室'), findsOneWidget);
    expect(find.text('0 台设备'), findsOneWidget);

    await tester.tap(find.text('关闭'));
    await tester.pumpAndSettle();
    expect(result.changed, isTrue);
  });

  testWidgets('a group can be renamed', (tester) async {
    final List<String> renamed = <String>[];
    final GroupsDialog dialog = GroupsDialog(
      server: settings,
      token: 'account-token',
      groups: groups,
      peerCounts: const <String, int>{'lab': 3, 'work': 1},
      rename:
          (ServerSettings server, String token, String groupId, String? name,
              {String? parentId, int? position, int? ifRevision}) async {
        renamed.add('$groupId/$name@$ifRevision');
        return AddressBookGroup(id: groupId, name: name ?? '', revision: 8);
      },
    );
    final _Result result = await open(tester, dialog);

    expect(find.text('3 台设备'), findsOneWidget);
    await tester.tap(find.byKey(const Key('group-rename-work')));
    await tester.pumpAndSettle();
    expect(find.text('编辑分组'), findsOneWidget);
    await tester.enterText(find.byKey(const Key('group-name')), '工作室');
    await tester.tap(find.byKey(const Key('group-name-save')));
    await tester.pumpAndSettle();

    expect(renamed, <String>['work/工作室@7']);
    expect(find.text('工作室'), findsOneWidget);
    expect(find.text('Work'), findsNothing);
    expect(find.text('1 台设备'), findsOneWidget);

    await tester.tap(find.text('关闭'));
    await tester.pumpAndSettle();
    expect(result.changed, isTrue);
  });

  testWidgets('deleting a group warns and ungroups its peers', (tester) async {
    final List<String> deleted = <String>[];
    final GroupsDialog dialog = GroupsDialog(
      server: settings,
      token: 'account-token',
      groups: groups,
      remove:
          (ServerSettings server, String token, String groupId,
              {int? ifRevision}) async {
        deleted.add('$groupId@$ifRevision');
      },
    );
    final _Result result = await open(tester, dialog);

    await tester.tap(find.byKey(const Key('group-delete-lab')));
    await tester.pumpAndSettle();
    expect(find.textContaining('组内设备会移到未分组'), findsOneWidget);
    await tester.tap(find.byKey(const Key('group-delete-confirm')));
    await tester.pumpAndSettle();

    expect(deleted, <String>['lab@5']);
    expect(find.byKey(const Key('group-row-lab')), findsNothing);
    expect(find.byKey(const Key('group-row-work')), findsOneWidget);

    await tester.tap(find.text('关闭'));
    await tester.pumpAndSettle();
    expect(result.changed, isTrue);
  });

  testWidgets('a server rejection keeps the dialog and shows the message', (
    tester,
  ) async {
    int attempts = 0;
    final GroupsDialog dialog = GroupsDialog(
      server: settings,
      token: 'account-token',
      groups: groups,
      rename:
          (ServerSettings server, String token, String groupId, String? name,
              {String? parentId, int? position, int? ifRevision}) async {
        attempts++;
        throw const ServerApiException('地址簿分组名称重复', statusCode: 400);
      },
    );
    final _Result result = await open(tester, dialog);

    await tester.tap(find.byKey(const Key('group-rename-lab')));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('group-name')), 'Lab');
    await tester.tap(find.byKey(const Key('group-name-save')));
    await tester.pumpAndSettle();
    expect(attempts, 0);

    await tester.tap(find.byKey(const Key('group-rename-lab')));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('group-name')), 'Lab 2');
    await tester.tap(find.byKey(const Key('group-name-save')));
    await tester.pumpAndSettle();
    expect(attempts, 1);
    expect(find.byKey(const Key('group-error')), findsOneWidget);
    expect(find.text('地址簿分组名称重复'), findsOneWidget);
    expect(find.byKey(const Key('group-row-lab')), findsOneWidget);

    await tester.tap(find.text('关闭'));
    await tester.pumpAndSettle();
    expect(result.changed, isFalse);
  });

  testWidgets('a stale group edit refreshes instead of retrying', (
    tester,
  ) async {
    int reloads = 0;
    final GroupsDialog dialog = GroupsDialog(
      server: settings,
      token: 'account-token',
      groups: groups,
      rename:
          (ServerSettings server, String token, String groupId, String? name,
              {String? parentId, int? position, int? ifRevision}) async {
        throw const ServerApiException(
          'address book group changed elsewhere',
          statusCode: 409,
        );
      },
      reload: () async {
        reloads++;
        return const <AddressBookGroup>[
          AddressBookGroup(id: 'lab', name: 'Lab', revision: 5),
          AddressBookGroup(id: 'work', name: 'Work 2', revision: 9),
        ];
      },
    );
    final _Result result = await open(tester, dialog);

    await tester.tap(find.byKey(const Key('group-rename-work')));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('group-name')), '工作室');
    await tester.tap(find.byKey(const Key('group-name-save')));
    await tester.pumpAndSettle();

    expect(reloads, 1);
    expect(find.text('分组已在别处修改，已刷新列表，请重试'), findsOneWidget);
    expect(find.text('Work 2'), findsOneWidget);
    expect(find.text('工作室'), findsNothing);

    await tester.tap(find.text('关闭'));
    await tester.pumpAndSettle();
    expect(result.changed, isTrue);
  });

  testWidgets('a group can be created under a parent', (tester) async {
    String? seenParent;
    final GroupsDialog dialog = GroupsDialog(
      server: settings,
      token: 'account-token',
      groups: const <AddressBookGroup>[
        AddressBookGroup(id: 'team', name: 'Team', revision: 2),
      ],
      create:
          (ServerSettings server, String token, String groupId, String? name,
              {String? parentId, int? position, int? ifRevision}) async {
        seenParent = parentId;
        return AddressBookGroup(
          id: groupId,
          name: name ?? '',
          parentId: parentId ?? '',
          revision: 1,
        );
      },
    );
    await open(tester, dialog);

    await tester.tap(find.byKey(const Key('group-create')));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('group-name')), 'Dev');
    await tester.tap(find.byKey(const Key('group-parent')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Team').last);
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('group-name-save')));
    await tester.pumpAndSettle();

    expect(seenParent, 'team');
    expect(find.byKey(const Key('group-row-team')), findsOneWidget);
    expect(find.text('Dev'), findsOneWidget);
  });

  testWidgets('moving a group up renumbers its siblings', (tester) async {
    final List<String> writes = <String>[];
    const List<AddressBookGroup> flat = <AddressBookGroup>[
      AddressBookGroup(id: 'alpha', name: 'Alpha', revision: 2),
      AddressBookGroup(id: 'beta', name: 'Beta', revision: 3),
    ];
    final GroupsDialog dialog = GroupsDialog(
      server: settings,
      token: 'account-token',
      groups: flat,
      rename:
          (ServerSettings server, String token, String groupId, String? name,
              {String? parentId, int? position, int? ifRevision}) async {
        writes.add('$groupId/$name/$position@$ifRevision');
        final AddressBookGroup old = flat.firstWhere(
          (AddressBookGroup group) => group.id == groupId,
        );
        return AddressBookGroup(
          id: groupId,
          name: name ?? old.name,
          parentId: parentId ?? old.parentId,
          position: position ?? old.position,
          revision: (ifRevision ?? old.revision) + 1,
        );
      },
    );
    await open(tester, dialog);

    await tester.tap(find.byKey(const Key('group-up-beta')));
    await tester.pumpAndSettle();

    expect(writes, <String>['beta/null/1@3', 'alpha/null/2@2']);
  });

  testWidgets('a group edit can move it under another group', (tester) async {
    String? seenParent;
    final List<String> writes = <String>[];
    const List<AddressBookGroup> nested = <AddressBookGroup>[
      AddressBookGroup(id: 'team', name: 'Team', revision: 2),
      AddressBookGroup(id: 'ops', name: 'Ops', revision: 4),
    ];
    final GroupsDialog dialog = GroupsDialog(
      server: settings,
      token: 'account-token',
      groups: nested,
      rename:
          (ServerSettings server, String token, String groupId, String? name,
              {String? parentId, int? position, int? ifRevision}) async {
        seenParent = parentId;
        writes.add('$groupId/$name/$parentId@$ifRevision');
        final AddressBookGroup old = nested.firstWhere(
          (AddressBookGroup group) => group.id == groupId,
        );
        return AddressBookGroup(
          id: groupId,
          name: name ?? old.name,
          parentId: parentId ?? old.parentId,
          revision: (ifRevision ?? old.revision) + 1,
        );
      },
    );
    await open(tester, dialog);

    await tester.tap(find.byKey(const Key('group-rename-ops')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('group-parent')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Team').last);
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('group-name-save')));
    await tester.pumpAndSettle();

    expect(seenParent, 'team');
    expect(writes, <String>['ops/null/team@4']);
  });

  testWidgets('the parent picker hides the group and its descendants', (
    tester,
  ) async {
    const List<AddressBookGroup> nested = <AddressBookGroup>[
      AddressBookGroup(id: 'team', name: 'Team', revision: 2),
      AddressBookGroup(id: 'dev', name: 'Dev', parentId: 'team', revision: 3),
      AddressBookGroup(id: 'ops', name: 'Ops', revision: 4),
    ];
    final GroupsDialog dialog = GroupsDialog(
      server: settings,
      token: 'account-token',
      groups: nested,
      rename:
          (ServerSettings server, String token, String groupId, String? name,
              {String? parentId, int? position, int? ifRevision}) async {
        return AddressBookGroup(id: groupId, name: name ?? '');
      },
    );
    await open(tester, dialog);

    await tester.tap(find.byKey(const Key('group-rename-team')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('group-parent')));
    await tester.pumpAndSettle();

    expect(find.text('（根分组）'), findsWidgets);
    expect(find.text('Ops'), findsWidgets);
    // Only the row behind the dialog remains; the dropdown must not offer it.
    expect(find.text('Dev'), findsOneWidget);
  });
}
