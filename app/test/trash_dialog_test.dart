import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/pages/trash_dialog.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';

const ServerSettings _settings = ServerSettings(url: 'http://127.0.0.1:8443');

AddressBookPeer _peer(String id, {String name = '', String note = ''}) {
  return AddressBookPeer(
    id: id,
    name: name,
    tags: const <String>['office'],
    note: note,
    deleted: true,
    revision: 4,
  );
}

Future<void> _open(WidgetTester tester, TrashDialog dialog) async {
  await tester.pumpWidget(
    MaterialApp(
      home: Builder(
        builder: (BuildContext context) => Scaffold(
          body: Center(
            child: TextButton(
              onPressed: () =>
                  showDialog<bool>(context: context, builder: (_) => dialog),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    ),
  );
  await tester.tap(find.text('open'));
  await tester.pumpAndSettle();
}

void main() {
  testWidgets('the trash lists deleted entries and restores one', (
    WidgetTester tester,
  ) async {
    final List<String> restored = <String>[];
    await _open(
      tester,
      TrashDialog(
        server: _settings,
        token: 'tok',
        fetch: (ServerSettings server, String token) async => <AddressBookPeer>[
          _peer('studio', name: '工作室', note: '机柜 A3'),
          _peer('lab', name: 'Lab'),
        ],
        restore: (ServerSettings server, String token, String peerId) async {
          restored.add(peerId);
          return AddressBookPeer(id: peerId, revision: 5);
        },
      ),
    );

    expect(find.byKey(const Key('trash-row-studio')), findsOneWidget);
    expect(find.byKey(const Key('trash-row-lab')), findsOneWidget);
    expect(find.text('机柜 A3'), findsOneWidget);

    await tester.tap(find.byKey(const Key('trash-restore-studio')));
    await tester.pumpAndSettle();

    expect(restored, <String>['studio']);
    expect(find.byKey(const Key('trash-row-studio')), findsNothing);
    expect(find.byKey(const Key('trash-row-lab')), findsOneWidget);
    expect(find.byKey(const Key('trash-notice')), findsOneWidget);
    expect(find.textContaining('工作室'), findsWidgets);
  });

  testWidgets('purging a trash entry asks first and then deletes it', (
    WidgetTester tester,
  ) async {
    final List<String> purged = <String>[];
    await _open(
      tester,
      TrashDialog(
        server: _settings,
        token: 'tok',
        fetch: (ServerSettings server, String token) async => <AddressBookPeer>[
          _peer('studio', name: '工作室'),
        ],
        purge: (ServerSettings server, String token, String peerId) async {
          purged.add(peerId);
        },
      ),
    );

    await tester.tap(find.byKey(const Key('trash-purge-studio')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('trash-purge-confirm')), findsOneWidget);
    expect(purged, isEmpty);

    await tester.tap(find.byKey(const Key('trash-purge-confirm')));
    await tester.pumpAndSettle();

    expect(purged, <String>['studio']);
    expect(find.byKey(const Key('trash-row-studio')), findsNothing);
    expect(find.textContaining('回收站是空的'), findsOneWidget);
  });

  testWidgets('emptying the trash removes every row', (
    WidgetTester tester,
  ) async {
    int emptied = 0;
    await _open(
      tester,
      TrashDialog(
        server: _settings,
        token: 'tok',
        fetch: (ServerSettings server, String token) async => <AddressBookPeer>[
          _peer('studio', name: '工作室'),
          _peer('lab', name: 'Lab'),
        ],
        empty: (ServerSettings server, String token) async {
          emptied++;
          return 2;
        },
      ),
    );

    expect(find.byKey(const Key('trash-empty-all')), findsOneWidget);
    await tester.tap(find.byKey(const Key('trash-empty-all')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('trash-empty-confirm')), findsOneWidget);

    await tester.tap(find.byKey(const Key('trash-empty-confirm')));
    await tester.pumpAndSettle();

    expect(emptied, 1);
    expect(find.byKey(const Key('trash-row-studio')), findsNothing);
    expect(find.byKey(const Key('trash-row-lab')), findsNothing);
    expect(find.byKey(const Key('trash-empty-all')), findsNothing);
    expect(find.textContaining('回收站是空的'), findsOneWidget);
  });

  testWidgets('a stale trash entry refreshes the list and explains why', (
    WidgetTester tester,
  ) async {
    int fetches = 0;
    await _open(
      tester,
      TrashDialog(
        server: _settings,
        token: 'tok',
        fetch: (ServerSettings server, String token) async {
          fetches++;
          return fetches == 1
              ? <AddressBookPeer>[_peer('studio', name: '工作室')]
              : <AddressBookPeer>[];
        },
        restore: (ServerSettings server, String token, String peerId) async {
          throw const ServerApiException('回收站中没有这个条目', statusCode: 404);
        },
      ),
    );

    await tester.tap(find.byKey(const Key('trash-restore-studio')));
    await tester.pumpAndSettle();

    expect(fetches, 2);
    expect(find.byKey(const Key('trash-row-studio')), findsNothing);
    expect(find.byKey(const Key('trash-error')), findsOneWidget);
    expect(find.textContaining('已在别处修改'), findsOneWidget);
  });

  testWidgets('a failed trash read shows the server message', (
    WidgetTester tester,
  ) async {
    await _open(
      tester,
      TrashDialog(
        server: _settings,
        token: 'tok',
        fetch: (ServerSettings server, String token) async {
          throw const ServerApiException('连接服务超时', transient: true);
        },
      ),
    );

    expect(find.byKey(const Key('trash-error')), findsOneWidget);
    expect(find.text('连接服务超时'), findsOneWidget);
  });
}
