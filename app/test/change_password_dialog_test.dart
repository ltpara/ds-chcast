import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/pages/change_password_dialog.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';

void main() {
  const ServerSettings settings = ServerSettings(url: 'http://127.0.0.1:18645');
  final AccountSession account = AccountSession(
    server: settings.url,
    username: 'alice',
    token: 'account-token',
    expiresAt: DateTime.utc(2030),
  );

  Future<List<bool?>> open(
    WidgetTester tester,
    ChangePasswordDialog dialog,
  ) async {
    final List<bool?> results = <bool?>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: Builder(
            builder: (BuildContext context) {
              return TextButton(
                onPressed: () async {
                  results.add(await showDialog<bool>(
                    context: context,
                    builder: (BuildContext context) => dialog,
                  ));
                },
                child: const Text('打开'),
              );
            },
          ),
        ),
      ),
    );
    await tester.tap(find.text('打开'));
    await tester.pumpAndSettle();
    return results;
  }

  testWidgets('a successful change sends both passwords and closes',
      (WidgetTester tester) async {
    final List<String> calls = <String>[];
    final List<bool?> results = await open(
      tester,
      ChangePasswordDialog(
        server: settings,
        account: account,
        changePassword: (
          ServerSettings server,
          String token, {
          required String currentPassword,
          required String newPassword,
        }) async {
          calls.add('$token|$currentPassword|$newPassword');
        },
      ),
    );

    await tester.enterText(
      find.byKey(const Key('change-password-current')),
      'old secret',
    );
    await tester.enterText(
      find.byKey(const Key('change-password-new')),
      'new secret 1',
    );
    await tester.enterText(
      find.byKey(const Key('change-password-confirm')),
      'new secret 1',
    );
    await tester.tap(find.byKey(const Key('change-password-submit')));
    await tester.pumpAndSettle();

    expect(calls, <String>['account-token|old secret|new secret 1']);
    expect(results, <bool?>[true]);
    expect(find.byKey(const Key('change-password-submit')), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('length and confirmation are validated before calling the API',
      (WidgetTester tester) async {
    int calls = 0;
    await open(
      tester,
      ChangePasswordDialog(
        server: settings,
        account: account,
        changePassword: (
          ServerSettings server,
          String token, {
          required String currentPassword,
          required String newPassword,
        }) async {
          calls += 1;
        },
      ),
    );

    await tester.enterText(
      find.byKey(const Key('change-password-current')),
      'old secret',
    );
    await tester.enterText(
      find.byKey(const Key('change-password-new')),
      'short',
    );
    await tester.enterText(
      find.byKey(const Key('change-password-confirm')),
      'short',
    );
    await tester.tap(find.byKey(const Key('change-password-submit')));
    await tester.pumpAndSettle();
    expect(find.text('新密码长度需为 8–128 个字节'), findsOneWidget);

    await tester.enterText(
      find.byKey(const Key('change-password-new')),
      'new secret 1',
    );
    await tester.enterText(
      find.byKey(const Key('change-password-confirm')),
      'new secret 2',
    );
    await tester.tap(find.byKey(const Key('change-password-submit')));
    await tester.pumpAndSettle();
    expect(find.text('两次输入的新密码不一致'), findsOneWidget);
    expect(calls, 0);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a server rejection keeps the dialog open', (tester) async {
    await open(
      tester,
      ChangePasswordDialog(
        server: settings,
        account: account,
        changePassword: (
          ServerSettings server,
          String token, {
          required String currentPassword,
          required String newPassword,
        }) async {
          throw const ServerApiException('当前密码不正确');
        },
      ),
    );

    await tester.enterText(
      find.byKey(const Key('change-password-current')),
      'wrong secret',
    );
    await tester.enterText(
      find.byKey(const Key('change-password-new')),
      'new secret 1',
    );
    await tester.enterText(
      find.byKey(const Key('change-password-confirm')),
      'new secret 1',
    );
    await tester.tap(find.byKey(const Key('change-password-submit')));
    await tester.pumpAndSettle();

    expect(find.text('当前密码不正确'), findsOneWidget);
    expect(find.byKey(const Key('change-password-submit')), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });
}
