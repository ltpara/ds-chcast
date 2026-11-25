import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/pages/login_devices_dialog.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  testWidgets(
    'enabling asks first and revoking a trusted device reloads the list',
    (WidgetTester tester) async {
      LoginDeviceState state = LoginDeviceState(
        enabled: false,
        devices: <TrustedLoginDevice>[
          const TrustedLoginDevice(
            id: 'current',
            deviceName: 'This PC',
            createdAtMs: 1,
            lastSeenAtMs: 2,
            current: true,
          ),
          const TrustedLoginDevice(
            id: 'other',
            deviceName: 'Laptop',
            createdAtMs: 1,
            lastSeenAtMs: 2,
          ),
        ],
      );
      bool? policyEnabled;
      final List<String> revoked = <String>[];

      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: Builder(
              builder: (BuildContext context) => TextButton(
                onPressed: () => showDialog<void>(
                  context: context,
                  builder: (_) => LoginDevicesDialog(
                    server: const ServerSettings(url: 'https://server.example'),
                    account: AccountSession(
                      server: 'https://server.example',
                      username: 'alice',
                      token: 'token',
                      expiresAt: DateTime.utc(2030),
                    ),
                    loadDevices: (_, _) async => state,
                    updatePolicy: (_, _, {required bool enabled}) async {
                      policyEnabled = enabled;
                      state = LoginDeviceState(
                        enabled: enabled,
                        devices: state.devices,
                      );
                      return state;
                    },
                    revokeDevice: (_, _, String id) async {
                      revoked.add(id);
                      state = LoginDeviceState(
                        enabled: state.enabled,
                        devices: state.devices
                            .where(
                              (TrustedLoginDevice device) => device.id != id,
                            )
                            .toList(),
                      );
                    },
                  ),
                ),
                child: const Text('open'),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('open'));
      await tester.pumpAndSettle();

      final SwitchListTile toggle = tester.widget<SwitchListTile>(
        find.byKey(const Key('login-device-allowlist')),
      );
      expect(toggle.value, isFalse);
      await tester.tap(find.byKey(const Key('login-device-allowlist')));
      await tester.pumpAndSettle();
      expect(policyEnabled, isNull);
      await tester.tap(find.text('启用').last);
      await tester.pumpAndSettle();
      expect(policyEnabled, isTrue);
      expect(
        tester
            .widget<SwitchListTile>(
              find.byKey(const Key('login-device-allowlist')),
            )
            .value,
        isTrue,
      );

      final ListTile current = tester.widget<ListTile>(
        find.ancestor(
          of: find.text('This PC'),
          matching: find.byType(ListTile),
        ),
      );
      final IconButton currentRevoke = current.trailing! as IconButton;
      expect(currentRevoke.onPressed, isNull);

      final Finder otherRow = find.ancestor(
        of: find.text('Laptop'),
        matching: find.byType(ListTile),
      );
      await tester.tap(
        find.descendant(of: otherRow, matching: find.byType(IconButton)),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.text('撤销').last);
      await tester.pumpAndSettle();
      expect(revoked, <String>['other']);
      expect(find.text('Laptop'), findsNothing);
    },
  );
}
