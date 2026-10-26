import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/pages/two_factor_dialog.dart';
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

  Future<void> open(WidgetTester tester, TwoFactorDialog dialog) async {
    tester.view.physicalSize = const Size(1400, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
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

  testWidgets('enrolling shows the secret and enables with a code',
      (WidgetTester tester) async {
    final List<String> calls = <String>[];
    await open(
      tester,
      TwoFactorDialog(
        server: settings,
        account: account,
        loadInfo: (ServerSettings server, String token) async {
          expect(token, 'account-token');
          return const AccountInfo(username: 'alice');
        },
        start: (ServerSettings server, String token) async {
          calls.add('start|$token');
          return const TwoFactorEnrollment(
            secret: 'JBSWY3DPEHPK3PXP',
            uri: 'otpauth://totp/chast:alice?secret=JBSWY3DPEHPK3PXP',
          );
        },
        enable: (ServerSettings server, String token, String code) async {
          calls.add('enable|$token|$code');
        },
      ),
    );

    expect(
      tester.widget<Text>(find.byKey(const Key('two-factor-status'))).data,
      '两步验证未启用',
    );
    expect(find.byKey(const Key('two-factor-secret')), findsNothing);

    await tester.tap(find.byKey(const Key('two-factor-start')));
    await tester.pumpAndSettle();
    expect(find.text('JBSWY3DPEHPK3PXP'), findsOneWidget);
    expect(find.textContaining('otpauth://totp/'), findsOneWidget);

    await tester.enterText(find.byKey(const Key('two-factor-code')), '123456');
    await tester.tap(find.byKey(const Key('two-factor-enable')));
    await tester.pumpAndSettle();

    expect(calls, <String>[
      'start|account-token',
      'enable|account-token|123456',
    ]);
    expect(
      tester.widget<Text>(find.byKey(const Key('two-factor-status'))).data,
      '两步验证已启用',
    );
    expect(
      tester.widget<Text>(find.byKey(const Key('two-factor-message'))).data,
      '两步验证已启用',
    );
    expect(find.byKey(const Key('two-factor-password')), findsOneWidget);
    expect(find.byKey(const Key('two-factor-secret')), findsNothing);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('disabling requires the password and a current code',
      (WidgetTester tester) async {
    final List<String> calls = <String>[];
    await open(
      tester,
      TwoFactorDialog(
        server: settings,
        account: account,
        loadInfo: (ServerSettings server, String token) async {
          return const AccountInfo(username: 'alice', twoFactor: true);
        },
        disable: (
          ServerSettings server,
          String token, {
          required String password,
          required String code,
        }) async {
          calls.add('$token|$password|$code');
        },
      ),
    );

    expect(find.byKey(const Key('two-factor-password')), findsOneWidget);
    await tester.tap(find.byKey(const Key('two-factor-disable')));
    await tester.pumpAndSettle();
    expect(find.text('请输入当前密码'), findsOneWidget);
    expect(calls, isEmpty);

    await tester.enterText(
      find.byKey(const Key('two-factor-password')),
      'secret123',
    );
    await tester.tap(find.byKey(const Key('two-factor-disable')));
    await tester.pumpAndSettle();
    expect(find.text('请输入身份验证器中的 6 位验证码'), findsOneWidget);
    expect(calls, isEmpty);

    await tester.enterText(find.byKey(const Key('two-factor-code')), '654321');
    await tester.tap(find.byKey(const Key('two-factor-disable')));
    await tester.pumpAndSettle();

    expect(calls, <String>['account-token|secret123|654321']);
    expect(
      tester.widget<Text>(find.byKey(const Key('two-factor-status'))).data,
      '两步验证未启用',
    );
    expect(
      tester.widget<Text>(find.byKey(const Key('two-factor-message'))).data,
      '两步验证已关闭',
    );

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a rejected code keeps the enrollment visible',
      (WidgetTester tester) async {
    await open(
      tester,
      TwoFactorDialog(
        server: settings,
        account: account,
        loadInfo: (ServerSettings server, String token) async {
          return const AccountInfo(username: 'alice');
        },
        start: (ServerSettings server, String token) async {
          return const TwoFactorEnrollment(
            secret: 'JBSWY3DPEHPK3PXP',
            uri: 'otpauth://totp/chast:alice?secret=JBSWY3DPEHPK3PXP',
          );
        },
        enable: (ServerSettings server, String token, String code) async {
          throw const ServerApiException('两步验证码不正确');
        },
      ),
    );

    await tester.tap(find.byKey(const Key('two-factor-start')));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('two-factor-code')), '000000');
    await tester.tap(find.byKey(const Key('two-factor-enable')));
    await tester.pumpAndSettle();

    expect(
      tester.widget<Text>(find.byKey(const Key('two-factor-error'))).data,
      '两步验证码不正确',
    );
    expect(find.text('JBSWY3DPEHPK3PXP'), findsOneWidget);
    expect(find.byKey(const Key('two-factor-enable')), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a failed status load can be retried', (WidgetTester tester) async {
    int loads = 0;
    await open(
      tester,
      TwoFactorDialog(
        server: settings,
        account: account,
        loadInfo: (ServerSettings server, String token) async {
          loads += 1;
          if (loads == 1) {
            throw const ServerApiException('账号登录已失效，请重新登录');
          }
          return const AccountInfo(username: 'alice', twoFactor: true);
        },
      ),
    );

    expect(
      tester.widget<Text>(find.byKey(const Key('two-factor-error'))).data,
      '账号登录已失效，请重新登录',
    );
    expect(find.byKey(const Key('two-factor-retry')), findsOneWidget);
    expect(find.byKey(const Key('two-factor-password')), findsNothing);

    await tester.tap(find.byKey(const Key('two-factor-retry')));
    await tester.pumpAndSettle();

    expect(loads, 2);
    expect(find.byKey(const Key('two-factor-error')), findsNothing);
    expect(find.byKey(const Key('two-factor-password')), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('recovery codes show the remaining count and are generated',
      (WidgetTester tester) async {
    final List<String> calls = <String>[];
    await open(
      tester,
      TwoFactorDialog(
        server: settings,
        account: account,
        loadInfo: (ServerSettings server, String token) async {
          return const AccountInfo(
            username: 'alice',
            twoFactor: true,
            recoveryCodes: 3,
          );
        },
        generateRecoveryCodes: (
          ServerSettings server,
          String token, {
          required String password,
          required String code,
        }) async {
          calls.add('$token|$password|$code');
          return <String>['ABCD-EFGH-IJKL-MNOP', 'QRST-UVWX-YZ23-4567'];
        },
      ),
    );

    expect(
      tester
          .widget<Text>(find.byKey(const Key('two-factor-recovery-count')))
          .data,
      contains('剩余恢复码：3'),
    );

    await tester.tap(find.byKey(const Key('two-factor-recovery-generate')));
    await tester.pumpAndSettle();
    expect(find.text('请输入当前密码'), findsOneWidget);
    expect(calls, isEmpty);

    await tester.enterText(
      find.byKey(const Key('two-factor-password')),
      'secret123',
    );
    await tester.tap(find.byKey(const Key('two-factor-recovery-generate')));
    await tester.pumpAndSettle();
    expect(find.text('请输入身份验证器中的 6 位验证码'), findsOneWidget);
    expect(calls, isEmpty);

    await tester.enterText(find.byKey(const Key('two-factor-code')), '654321');
    await tester.tap(find.byKey(const Key('two-factor-recovery-generate')));
    await tester.pumpAndSettle();

    expect(calls, <String>['account-token|secret123|654321']);
    expect(find.byKey(const Key('recovery-codes-dialog')), findsOneWidget);
    final SelectableText codes =
        tester.widget<SelectableText>(find.byKey(const Key('recovery-codes-list')));
    expect(codes.data, contains('ABCD-EFGH-IJKL-MNOP'));
    expect(codes.data, contains('QRST-UVWX-YZ23-4567'));

    await tester.tap(find.byKey(const Key('recovery-codes-close')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('recovery-codes-dialog')), findsNothing);
    expect(
      tester
          .widget<Text>(find.byKey(const Key('two-factor-recovery-count')))
          .data,
      contains('剩余恢复码：2'),
    );
    expect(
      tester.widget<Text>(find.byKey(const Key('two-factor-message'))).data,
      '已生成 2 个恢复码',
    );

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('a rejected recovery generation keeps the form usable',
      (WidgetTester tester) async {
    await open(
      tester,
      TwoFactorDialog(
        server: settings,
        account: account,
        loadInfo: (ServerSettings server, String token) async {
          return const AccountInfo(username: 'alice', twoFactor: true);
        },
        generateRecoveryCodes: (
          ServerSettings server,
          String token, {
          required String password,
          required String code,
        }) async {
          throw const ServerApiException('密码或两步验证码不正确');
        },
      ),
    );

    await tester.enterText(
      find.byKey(const Key('two-factor-password')),
      'wrong',
    );
    await tester.enterText(find.byKey(const Key('two-factor-code')), '000000');
    await tester.tap(find.byKey(const Key('two-factor-recovery-generate')));
    await tester.pumpAndSettle();

    expect(
      tester.widget<Text>(find.byKey(const Key('two-factor-error'))).data,
      '密码或两步验证码不正确',
    );
    expect(find.byKey(const Key('recovery-codes-dialog')), findsNothing);
    expect(find.byKey(const Key('two-factor-recovery-generate')), findsOneWidget);
    expect(find.byKey(const Key('two-factor-disable')), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
  });
}
