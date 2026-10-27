import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/pages/recovery_dialog.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';

void main() {
  const ServerSettings settings = ServerSettings(url: 'http://127.0.0.1:18646');
  final AccountSession account = AccountSession(
    server: settings.url,
    username: 'alice',
    token: 'account-token',
    expiresAt: DateTime.utc(2030),
  );

  Future<List<T?>> open<T>(WidgetTester tester, Widget dialog) async {
    tester.view.physicalSize = const Size(1200, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final List<T?> results = <T?>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: Builder(
            builder: (BuildContext context) {
              return TextButton(
                onPressed: () async {
                  results.add(await showDialog<T>(
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

  testWidgets('recovery email shows the current address and saves a new one',
      (WidgetTester tester) async {
    final List<String> calls = <String>[];
    final List<String?> results = await open<String>(
      tester,
      RecoveryEmailDialog(
        server: settings,
        account: account,
        infoFetcher: (ServerSettings server, String token) async {
          calls.add('load|$token');
          return const AccountInfo(
            username: 'alice',
            email: 'old@example.com',
          );
        },
        changeEmail: (
          ServerSettings server,
          String token, {
          required String email,
          required String password,
        }) async {
          calls.add('save|$token|$email|$password');
          return email;
        },
      ),
    );

    expect(find.text('当前邮箱：old@example.com'), findsOneWidget);
    await tester.enterText(
      find.byKey(const Key('recovery-email-value')),
      ' new@example.com ',
    );
    await tester.enterText(
      find.byKey(const Key('recovery-email-password')),
      'secret123',
    );
    await tester.tap(find.byKey(const Key('recovery-email-save')));
    await tester.pumpAndSettle();

    expect(calls, <String>[
      'load|account-token',
      'save|account-token|new@example.com|secret123',
    ]);
    expect(results, <String?>['new@example.com']);
    expect(find.byKey(const Key('recovery-email-save')), findsNothing);
  });

  testWidgets('an empty address clears the recovery email',
      (WidgetTester tester) async {
    final List<String> saved = <String>[];
    final List<String?> results = await open<String>(
      tester,
      RecoveryEmailDialog(
        server: settings,
        account: account,
        infoFetcher: (ServerSettings server, String token) async {
          return const AccountInfo(username: 'alice', email: 'old@example.com');
        },
        changeEmail: (
          ServerSettings server,
          String token, {
          required String email,
          required String password,
        }) async {
          saved.add(email);
          return '';
        },
      ),
    );

    await tester.enterText(find.byKey(const Key('recovery-email-value')), '');
    await tester.enterText(
      find.byKey(const Key('recovery-email-password')),
      'secret123',
    );
    await tester.tap(find.byKey(const Key('recovery-email-save')));
    await tester.pumpAndSettle();

    expect(saved, <String>['']);
    expect(results, <String?>['']);
  });

  testWidgets('the email format is validated before calling the API',
      (WidgetTester tester) async {
    int calls = 0;
    await open<String>(
      tester,
      RecoveryEmailDialog(
        server: settings,
        account: account,
        infoFetcher: (ServerSettings server, String token) async {
          return const AccountInfo(username: 'alice');
        },
        changeEmail: (
          ServerSettings server,
          String token, {
          required String email,
          required String password,
        }) async {
          calls += 1;
          return email;
        },
      ),
    );

    await tester.enterText(
      find.byKey(const Key('recovery-email-value')),
      'not-an-email',
    );
    await tester.enterText(
      find.byKey(const Key('recovery-email-password')),
      'secret123',
    );
    await tester.tap(find.byKey(const Key('recovery-email-save')));
    await tester.pumpAndSettle();

    expect(calls, 0);
    expect(find.text('请输入有效的邮箱地址'), findsOneWidget);
  });

  testWidgets('a failed email load still allows saving a new address',
      (WidgetTester tester) async {
    final List<String?> results = await open<String>(
      tester,
      RecoveryEmailDialog(
        server: settings,
        account: account,
        infoFetcher: (ServerSettings server, String token) async {
          throw const ServerApiException('账号登录已失效，请重新登录');
        },
        changeEmail: (
          ServerSettings server,
          String token, {
          required String email,
          required String password,
        }) async {
          return email;
        },
      ),
    );

    expect(
      find.byKey(const Key('recovery-email-load-error')),
      findsOneWidget,
    );
    expect(find.textContaining('账号登录已失效'), findsOneWidget);

    await tester.enterText(
      find.byKey(const Key('recovery-email-value')),
      'new@example.com',
    );
    await tester.enterText(
      find.byKey(const Key('recovery-email-password')),
      'secret123',
    );
    await tester.tap(find.byKey(const Key('recovery-email-save')));
    await tester.pumpAndSettle();

    expect(results, <String?>['new@example.com']);
  });

  testWidgets('recovery phone shows the current number and saves a new one',
      (WidgetTester tester) async {
    final List<String> calls = <String>[];
    final List<String?> results = await open<String>(
      tester,
      RecoveryPhoneDialog(
        server: settings,
        account: account,
        infoFetcher: (ServerSettings server, String token) async {
          calls.add('load|$token');
          return const AccountInfo(
            username: 'alice',
            phone: '+1 555 010 0001',
          );
        },
        changePhone: (
          ServerSettings server,
          String token, {
          required String phone,
          required String password,
        }) async {
          calls.add('save|$token|$phone|$password');
          return phone;
        },
      ),
    );

    expect(find.text('当前手机号：+1 555 010 0001'), findsOneWidget);
    await tester.enterText(
      find.byKey(const Key('recovery-phone-value')),
      ' +1 555 010 9999 ',
    );
    await tester.enterText(
      find.byKey(const Key('recovery-phone-password')),
      'secret123',
    );
    await tester.tap(find.byKey(const Key('recovery-phone-save')));
    await tester.pumpAndSettle();

    expect(calls, <String>[
      'load|account-token',
      'save|account-token|+1 555 010 9999|secret123',
    ]);
    expect(results, <String?>['+1 555 010 9999']);
    expect(find.byKey(const Key('recovery-phone-save')), findsNothing);
  });

  testWidgets('an empty number clears the recovery phone',
      (WidgetTester tester) async {
    final List<String> saved = <String>[];
    final List<String?> results = await open<String>(
      tester,
      RecoveryPhoneDialog(
        server: settings,
        account: account,
        infoFetcher: (ServerSettings server, String token) async {
          return const AccountInfo(
            username: 'alice',
            phone: '+1 555 010 0001',
          );
        },
        changePhone: (
          ServerSettings server,
          String token, {
          required String phone,
          required String password,
        }) async {
          saved.add(phone);
          return '';
        },
      ),
    );

    await tester.enterText(find.byKey(const Key('recovery-phone-value')), '');
    await tester.enterText(
      find.byKey(const Key('recovery-phone-password')),
      'secret123',
    );
    await tester.tap(find.byKey(const Key('recovery-phone-save')));
    await tester.pumpAndSettle();

    expect(saved, <String>['']);
    expect(results, <String?>['']);
  });

  testWidgets('the phone format is validated before calling the API',
      (WidgetTester tester) async {
    int calls = 0;
    await open<String>(
      tester,
      RecoveryPhoneDialog(
        server: settings,
        account: account,
        infoFetcher: (ServerSettings server, String token) async {
          return const AccountInfo(username: 'alice');
        },
        changePhone: (
          ServerSettings server,
          String token, {
          required String phone,
          required String password,
        }) async {
          calls += 1;
          return phone;
        },
      ),
    );

    await tester.enterText(
      find.byKey(const Key('recovery-phone-value')),
      '12345',
    );
    await tester.enterText(
      find.byKey(const Key('recovery-phone-password')),
      'secret123',
    );
    await tester.tap(find.byKey(const Key('recovery-phone-save')));
    await tester.pumpAndSettle();

    expect(calls, 0);
    expect(find.text('手机号需为 6–15 位数字'), findsOneWidget);
  });

  testWidgets('forgot password requests a token then completes the reset',
      (WidgetTester tester) async {
    final List<String> calls = <String>[];
    final List<String?> results = await open<String>(
      tester,
      ForgotPasswordDialog(
        server: settings,
        initialUsername: 'alice',
        requestRecovery: (
          ServerSettings server, {
          required String username,
        }) async {
          calls.add('send|$username');
        },
        completeRecovery: (
          ServerSettings server, {
          required String token,
          required String password,
        }) async {
          calls.add('complete|$token|$password');
          return 'alice';
        },
      ),
    );

    expect(find.byKey(const Key('forgot-username')), findsOneWidget);
    await tester.tap(find.byKey(const Key('forgot-send')));
    await tester.pumpAndSettle();

    expect(calls, <String>['send|alice']);
    expect(find.byKey(const Key('forgot-notice')), findsOneWidget);
    expect(find.byKey(const Key('forgot-token')), findsOneWidget);

    await tester.enterText(find.byKey(const Key('forgot-token')), ' token ');
    await tester.enterText(
      find.byKey(const Key('forgot-password')),
      'new secret 1',
    );
    await tester.enterText(find.byKey(const Key('forgot-confirm')), 'mismatch');
    await tester.tap(find.byKey(const Key('forgot-complete')));
    await tester.pumpAndSettle();

    expect(find.text('两次输入的新密码不一致'), findsOneWidget);
    expect(calls, <String>['send|alice']);

    await tester.enterText(
      find.byKey(const Key('forgot-confirm')),
      'new secret 1',
    );
    await tester.tap(find.byKey(const Key('forgot-complete')));
    await tester.pumpAndSettle();

    expect(calls, <String>['send|alice', 'complete|token|new secret 1']);
    expect(results, <String?>['alice']);
    expect(find.byKey(const Key('forgot-complete')), findsNothing);
  });

  testWidgets('forgot password keeps the form on a request failure',
      (WidgetTester tester) async {
    final List<String?> results = await open<String>(
      tester,
      ForgotPasswordDialog(
        server: settings,
        initialUsername: 'bob',
        requestRecovery: (
          ServerSettings server, {
          required String username,
        }) async {
          throw const ServerApiException('连接服务未配置账号或邮件找回');
        },
      ),
    );

    await tester.tap(find.byKey(const Key('forgot-send')));
    await tester.pumpAndSettle();

    expect(find.text('连接服务未配置账号或邮件找回'), findsOneWidget);
    expect(find.byKey(const Key('forgot-token')), findsNothing);
    expect(results, isEmpty);
  });

  testWidgets('forgot password can switch to an SMS code', (WidgetTester tester) async {
    final List<String> calls = <String>[];
    final List<String?> results = await open<String>(
      tester,
      ForgotPasswordDialog(
        server: settings,
        initialUsername: 'alice',
        requestRecovery: (
          ServerSettings server, {
          required String username,
        }) async {
          calls.add('email-send|$username');
        },
        requestSMSRecovery: (
          ServerSettings server, {
          required String username,
        }) async {
          calls.add('sms-send|$username');
        },
        completeSMSRecovery: (
          ServerSettings server, {
          required String username,
          required String code,
          required String password,
        }) async {
          calls.add('sms-complete|$username|$code|$password');
          return username;
        },
      ),
    );

    await tester.tap(find.byKey(const Key('forgot-channel-sms')));
    await tester.pumpAndSettle();
    expect(find.text('发送验证码'), findsOneWidget);

    await tester.tap(find.byKey(const Key('forgot-send')));
    await tester.pumpAndSettle();

    expect(calls, <String>['sms-send|alice']);
    final Text notice = tester.widget<Text>(
      find.byKey(const Key('forgot-notice')),
    );
    expect(notice.data, contains('8 位验证码'));
    expect(find.text('短信验证码'), findsOneWidget);

    await tester.enterText(find.byKey(const Key('forgot-token')), '1234');
    await tester.enterText(
      find.byKey(const Key('forgot-password')),
      'new secret 1',
    );
    await tester.enterText(
      find.byKey(const Key('forgot-confirm')),
      'new secret 1',
    );
    await tester.tap(find.byKey(const Key('forgot-complete')));
    await tester.pumpAndSettle();

    expect(find.text('短信验证码需为 8 位数字'), findsOneWidget);
    expect(calls, <String>['sms-send|alice']);

    await tester.enterText(
      find.byKey(const Key('forgot-token')),
      ' 1234-5678 ',
    );
    await tester.tap(find.byKey(const Key('forgot-complete')));
    await tester.pumpAndSettle();

    expect(calls, <String>[
      'sms-send|alice',
      'sms-complete|alice|1234-5678|new secret 1',
    ]);
    expect(results, <String?>['alice']);
  });
}
