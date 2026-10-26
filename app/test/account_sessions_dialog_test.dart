import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/pages/account_sessions_dialog.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';

void main() {
  const ServerSettings settings = ServerSettings(url: 'http://127.0.0.1:18644');
  final AccountSession account = AccountSession(
    server: settings.url,
    username: 'alice',
    token: 'account-token',
    expiresAt: DateTime.utc(2030),
  );

  Future<void> open(WidgetTester tester, AccountSessionsDialog dialog) async {
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: Builder(
            builder: (BuildContext context) {
              return TextButton(
                onPressed: () => showDialog<void>(
                  context: context,
                  builder: (BuildContext context) => dialog,
                ),
                child: const Text('打开'),
              );
            },
          ),
        ),
      ),
    );
    await tester.tap(find.text('打开'));
    await tester.pumpAndSettle();
  }

  AccountDeviceSession session({
    required String id,
    String deviceName = '',
    bool current = false,
  }) {
    return AccountDeviceSession(
      id: id,
      deviceName: deviceName,
      createdAt: DateTime.utc(2026, 10, 20, 8, 30),
      expiresAt: DateTime.utc(2026, 11, 19, 8, 30),
      current: current,
    );
  }

  testWidgets('lists devices and revokes a non-current session', (
    WidgetTester tester,
  ) async {
    final List<String> revoked = <String>[];
    await open(
      tester,
      AccountSessionsDialog(
        server: settings,
        account: account,
        loadSessions: (ServerSettings server, String token) async {
          expect(token, 'account-token');
          return <AccountDeviceSession>[
            session(id: 's1', deviceName: '书房-PC', current: true),
            session(id: 's2', deviceName: '笔记本'),
          ];
        },
        revokeSession:
            (ServerSettings server, String token, String sessionId) async {
              revoked.add('$token|$sessionId');
            },
      ),
    );

    expect(find.text('书房-PC'), findsOneWidget);
    expect(find.text('当前设备'), findsOneWidget);
    expect(find.text('笔记本'), findsOneWidget);
    expect(find.byKey(const Key('account-session-revoke-s1')), findsNothing);
    expect(find.byKey(const Key('account-session-revoke-s2')), findsOneWidget);

    await tester.tap(find.byKey(const Key('account-session-revoke-s2')));
    await tester.pumpAndSettle();

    expect(revoked, <String>['account-token|s2']);
    expect(find.text('笔记本'), findsNothing);
    expect(find.text('书房-PC'), findsOneWidget);
    expect(find.text('已退出该设备'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a failed revoke keeps the device listed and reports the error', (
    WidgetTester tester,
  ) async {
    await open(
      tester,
      AccountSessionsDialog(
        server: settings,
        account: account,
        loadSessions: (ServerSettings server, String token) async =>
            <AccountDeviceSession>[
              session(id: 's1', deviceName: '书房-PC', current: true),
              session(id: 's2', deviceName: '笔记本'),
            ],
        revokeSession:
            (ServerSettings server, String token, String sessionId) async {
              throw const ServerApiException('会话不存在或已退出');
            },
      ),
    );

    await tester.tap(find.byKey(const Key('account-session-revoke-s2')));
    await tester.pumpAndSettle();

    expect(find.text('笔记本'), findsOneWidget);
    expect(find.text('会话不存在或已退出'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('revokes every other session at once', (
    WidgetTester tester,
  ) async {
    int calls = 0;
    await open(
      tester,
      AccountSessionsDialog(
        server: settings,
        account: account,
        loadSessions: (ServerSettings server, String token) async =>
            <AccountDeviceSession>[
              session(id: 's1', deviceName: '书房-PC', current: true),
              session(id: 's2', deviceName: '笔记本'),
              session(id: 's3', deviceName: '办公室'),
            ],
        revokeOtherSessions: (ServerSettings server, String token) async {
          calls += 1;
          expect(token, 'account-token');
          return 2;
        },
      ),
    );

    await tester.tap(find.byKey(const Key('account-sessions-revoke-others')));
    await tester.pumpAndSettle();

    expect(calls, 1);
    expect(find.text('书房-PC'), findsOneWidget);
    expect(find.text('笔记本'), findsNothing);
    expect(find.text('办公室'), findsNothing);
    expect(find.text('已退出 2 台其他设备'), findsOneWidget);
    final TextButton button = tester.widget<TextButton>(
      find.byKey(const Key('account-sessions-revoke-others')),
    );
    expect(button.onPressed, isNull);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a failed bulk revoke keeps the devices listed', (
    WidgetTester tester,
  ) async {
    await open(
      tester,
      AccountSessionsDialog(
        server: settings,
        account: account,
        loadSessions: (ServerSettings server, String token) async =>
            <AccountDeviceSession>[
              session(id: 's1', deviceName: '书房-PC', current: true),
              session(id: 's2', deviceName: '笔记本'),
            ],
        revokeOtherSessions: (ServerSettings server, String token) async {
          throw const ServerApiException('会话不存在或已退出');
        },
      ),
    );

    await tester.tap(find.byKey(const Key('account-sessions-revoke-others')));
    await tester.pumpAndSettle();

    expect(find.text('笔记本'), findsOneWidget);
    expect(find.text('会话不存在或已退出'), findsOneWidget);
    final TextButton button = tester.widget<TextButton>(
      find.byKey(const Key('account-sessions-revoke-others')),
    );
    expect(button.onPressed, isNotNull);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('bulk revoke is disabled without other sessions', (
    WidgetTester tester,
  ) async {
    await open(
      tester,
      AccountSessionsDialog(
        server: settings,
        account: account,
        loadSessions: (ServerSettings server, String token) async =>
            <AccountDeviceSession>[
              session(id: 's1', deviceName: '书房-PC', current: true),
            ],
      ),
    );

    final TextButton button = tester.widget<TextButton>(
      find.byKey(const Key('account-sessions-revoke-others')),
    );
    expect(button.onPressed, isNull);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('renames a session and updates the listed name', (
    WidgetTester tester,
  ) async {
    final List<String> renamed = <String>[];
    await open(
      tester,
      AccountSessionsDialog(
        server: settings,
        account: account,
        loadSessions: (ServerSettings server, String token) async =>
            <AccountDeviceSession>[
              session(id: 's1', deviceName: '书房-PC', current: true),
              session(id: 's2', deviceName: '笔记本'),
            ],
        renameSession:
            (
              ServerSettings server,
              String token,
              String sessionId,
              String deviceName,
            ) async {
              renamed.add('$token|$sessionId|$deviceName');
            },
      ),
    );

    expect(find.byKey(const Key('account-session-rename-s1')), findsOneWidget);
    await tester.tap(find.byKey(const Key('account-session-rename-s2')));
    await tester.pumpAndSettle();
    expect(find.text('重命名设备'), findsOneWidget);
    await tester.enterText(
      find.byKey(const Key('account-session-rename-field')),
      '  书房 PC  ',
    );
    await tester.tap(find.byKey(const Key('account-session-rename-save')));
    await tester.pumpAndSettle();

    expect(renamed, <String>['account-token|s2|书房 PC']);
    expect(find.text('书房 PC'), findsOneWidget);
    expect(find.text('笔记本'), findsNothing);
    expect(find.text('已更新设备名称'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a failed rename keeps the old name and reports the error', (
    WidgetTester tester,
  ) async {
    await open(
      tester,
      AccountSessionsDialog(
        server: settings,
        account: account,
        loadSessions: (ServerSettings server, String token) async =>
            <AccountDeviceSession>[
              session(id: 's1', deviceName: '书房-PC', current: true),
              session(id: 's2', deviceName: '笔记本'),
            ],
        renameSession:
            (
              ServerSettings server,
              String token,
              String sessionId,
              String deviceName,
            ) async {
              throw const ServerApiException('会话不存在或已退出');
            },
      ),
    );

    await tester.tap(find.byKey(const Key('account-session-rename-s2')));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const Key('account-session-rename-field')),
      '新名字',
    );
    await tester.tap(find.byKey(const Key('account-session-rename-save')));
    await tester.pumpAndSettle();

    expect(find.text('笔记本'), findsOneWidget);
    expect(find.text('新名字'), findsNothing);
    expect(find.text('会话不存在或已退出'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('an empty device name is refused before calling the server', (
    WidgetTester tester,
  ) async {
    int calls = 0;
    await open(
      tester,
      AccountSessionsDialog(
        server: settings,
        account: account,
        loadSessions: (ServerSettings server, String token) async =>
            <AccountDeviceSession>[
              session(id: 's1', deviceName: '书房-PC', current: true),
              session(id: 's2', deviceName: '笔记本'),
            ],
        renameSession:
            (
              ServerSettings server,
              String token,
              String sessionId,
              String deviceName,
            ) async {
              calls += 1;
            },
      ),
    );

    await tester.tap(find.byKey(const Key('account-session-rename-s2')));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const Key('account-session-rename-field')),
      '   ',
    );
    await tester.tap(find.byKey(const Key('account-session-rename-save')));
    await tester.pumpAndSettle();

    expect(find.text('请输入设备名称'), findsOneWidget);
    expect(find.text('重命名设备'), findsOneWidget);
    expect(calls, 0);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a failed load can be retried', (WidgetTester tester) async {
    int attempts = 0;
    await open(
      tester,
      AccountSessionsDialog(
        server: settings,
        account: account,
        loadSessions: (ServerSettings server, String token) async {
          attempts += 1;
          if (attempts == 1) {
            throw const ServerApiException('账号登录已失效，请重新登录');
          }
          return <AccountDeviceSession>[
            session(id: 's1', deviceName: '书房-PC', current: true),
          ];
        },
      ),
    );

    expect(find.text('账号登录已失效，请重新登录'), findsOneWidget);
    await tester.tap(find.byKey(const Key('account-sessions-refresh')));
    await tester.pumpAndSettle();

    expect(attempts, 2);
    expect(find.text('书房-PC'), findsOneWidget);
    expect(find.text('账号登录已失效，请重新登录'), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a lone current session shows the empty hint', (
    WidgetTester tester,
  ) async {
    await open(
      tester,
      AccountSessionsDialog(
        server: settings,
        account: account,
        loadSessions: (ServerSettings server, String token) async =>
            <AccountDeviceSession>[],
      ),
    );

    expect(find.byKey(const Key('account-sessions-empty')), findsOneWidget);
    expect(find.text('没有其他已登录设备。'), findsOneWidget);
    expect(find.byKey(const Key('account-sessions')), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });
}
