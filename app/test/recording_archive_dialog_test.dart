import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/pages/recording_archive_dialog.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';

void main() {
  const ServerSettings settings = ServerSettings(url: 'http://127.0.0.1:18647');
  final AccountSession account = AccountSession(
    server: settings.url,
    username: 'alice',
    token: 'account-token',
    expiresAt: DateTime.utc(2030),
  );

  RemoteRecording recording({
    String id = 'a',
    String name = '会话录制.mp4',
    int size = 2048,
    String peerId = 'devbox',
    DateTime? created,
  }) {
    return RemoteRecording(
      id: id,
      name: name,
      peerId: peerId,
      size: size,
      durationMs: 65000,
      createdAtMs:
          (created ?? DateTime.utc(2026, 10, 28, 9)).millisecondsSinceEpoch,
    );
  }

  Future<void> open(WidgetTester tester, RecordingArchiveDialog dialog) async {
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

  testWidgets('lists recordings with usage and downloads one', (
    WidgetTester tester,
  ) async {
    final Directory dir = Directory.systemTemp.createTempSync('chast-dl-');
    addTearDown(() => dir.deleteSync(recursive: true));
    final List<String> downloaded = <String>[];
    final List<String> opened = <String>[];
    final List<String> revealed = <String>[];
    await open(
      tester,
      RecordingArchiveDialog(
        server: settings,
        account: account,
        downloadDirectory: dir,
        onOpenDownload: (String path) async => opened.add(path),
        onRevealDownload: (String path) async => revealed.add(path),
        loadArchive: (ServerSettings server, String token) async {
          expect(token, 'account-token');
          return RecordingArchive(
            recordings: <RemoteRecording>[
              recording(id: 'a' * 32, name: '会话录制.mp4'),
              recording(id: 'b' * 32, name: 'second.mp4', size: 1024),
            ],
            usageBytes: 3072,
            quotaBytes: 8192,
            maxUploadBytes: 4096,
          );
        },
        downloadRecording:
            (
              ServerSettings server,
              String token,
              RemoteRecording item,
              Directory directory,
            ) async {
              downloaded.add('$token|${item.id}');
              return File(
                '${directory.path}${Platform.pathSeparator}${item.name}',
              );
            },
      ),
    );

    expect(find.text('会话录制.mp4'), findsOneWidget);
    expect(find.text('second.mp4'), findsOneWidget);
    expect(find.text('已用 3.0 KB，配额 8.0 KB'), findsOneWidget);
    expect(find.text('单次上传上限 4.0 KB'), findsOneWidget);
    expect(find.textContaining('来自 devbox'), findsNWidgets(2));

    await tester.tap(find.byKey(Key('recording-download-${'a' * 32}')));
    await tester.pumpAndSettle();

    expect(downloaded, <String>['account-token|${'a' * 32}']);
    expect(find.byKey(const Key('recordings-saved')), findsOneWidget);

    final String saved = '${dir.path}${Platform.pathSeparator}会话录制.mp4';
    expect(
      tester.widget<Text>(find.byKey(const Key('recordings-saved'))).data,
      '已保存到 $saved',
    );
    await tester.tap(find.byKey(const Key('recordings-open')));
    await tester.pump();
    await tester.pump();
    expect(opened, <String>[saved]);
    await tester.tap(find.byKey(const Key('recordings-reveal')));
    await tester.pump();
    await tester.pump();
    expect(revealed, <String>[saved]);
  });

  testWidgets('copies a recording id to the clipboard', (
    WidgetTester tester,
  ) async {
    String? copied;
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      SystemChannels.platform,
      (MethodCall call) async {
        if (call.method == 'Clipboard.setData') {
          copied = (call.arguments as Map<Object?, Object?>)['text'] as String?;
        }
        return null;
      },
    );
    addTearDown(
      () => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        SystemChannels.platform,
        null,
      ),
    );
    const String id = 'abcdef0123456789';
    await open(
      tester,
      RecordingArchiveDialog(
        server: settings,
        account: account,
        loadArchive: (ServerSettings server, String token) async {
          return RecordingArchive(
            recordings: <RemoteRecording>[recording(id: id)],
            usageBytes: 2048,
            quotaBytes: 8192,
            maxUploadBytes: 4096,
          );
        },
      ),
    );

    await tester.tap(find.byKey(const Key('recording-copy-$id')));
    await tester.pumpAndSettle();

    expect(copied, id);
    expect(find.text('ID 已复制'), findsOneWidget);
  });

  testWidgets('deletes a recording after confirmation', (
    WidgetTester tester,
  ) async {
    final List<String> deleted = <String>[];
    await open(
      tester,
      RecordingArchiveDialog(
        server: settings,
        account: account,
        loadArchive: (ServerSettings server, String token) async {
          return RecordingArchive(
            recordings: <RemoteRecording>[recording(id: 'c' * 32)],
            usageBytes: 2048,
            quotaBytes: 8192,
          );
        },
        deleteRecording:
            (ServerSettings server, String token, String id) async {
              deleted.add('$token|$id');
            },
      ),
    );

    await tester.tap(find.byKey(Key('recording-delete-${'c' * 32}')));
    await tester.pumpAndSettle();
    expect(find.text('删除云端录制？'), findsOneWidget);
    expect(deleted, isEmpty);

    await tester.tap(find.byKey(const Key('recording-delete-confirm')));
    await tester.pumpAndSettle();

    expect(deleted, <String>['account-token|${'c' * 32}']);
    expect(find.byKey(Key('recording-${'c' * 32}')), findsNothing);
    expect(find.byKey(const Key('recordings-empty')), findsOneWidget);
    expect(find.text('已用 0 B，配额 8.0 KB'), findsOneWidget);
  });

  testWidgets('shows an empty archive and a load error', (
    WidgetTester tester,
  ) async {
    await open(
      tester,
      RecordingArchiveDialog(
        server: settings,
        account: account,
        loadArchive: (ServerSettings server, String token) async {
          return const RecordingArchive();
        },
      ),
    );
    expect(find.byKey(const Key('recordings-empty')), findsOneWidget);
    expect(find.text('没有云端录制。'), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
    await open(
      tester,
      RecordingArchiveDialog(
        server: settings,
        account: account,
        loadArchive: (ServerSettings server, String token) async {
          throw const ServerApiException('连接服务未启用录制归档');
        },
      ),
    );
    expect(find.byKey(const Key('recordings-error')), findsOneWidget);
    expect(find.text('连接服务未启用录制归档'), findsOneWidget);
  });

  testWidgets('filters recordings by name, peer or id', (
    WidgetTester tester,
  ) async {
    await open(
      tester,
      RecordingArchiveDialog(
        server: settings,
        account: account,
        loadArchive: (ServerSettings server, String token) async {
          return RecordingArchive(
            recordings: <RemoteRecording>[
              recording(id: 'a' * 32, name: '会话录制.mp4'),
              recording(
                id: 'b' * 32,
                name: 'second.mp4',
                size: 1024,
                peerId: 'labbox',
              ),
            ],
            usageBytes: 3072,
            quotaBytes: 8192,
          );
        },
      ),
    );

    final Finder filter = find.byKey(const Key('recordings-filter'));
    expect(filter, findsOneWidget);

    await tester.enterText(filter, 'SECOND');
    await tester.pump();
    expect(find.text('second.mp4'), findsOneWidget);
    expect(find.text('会话录制.mp4'), findsNothing);

    await tester.enterText(filter, 'labbox');
    await tester.pump();
    expect(find.text('second.mp4'), findsOneWidget);
    expect(find.text('会话录制.mp4'), findsNothing);

    await tester.enterText(filter, 'aaa');
    await tester.pump();
    expect(find.text('会话录制.mp4'), findsOneWidget);
    expect(find.text('second.mp4'), findsNothing);

    await tester.enterText(filter, 'zzz');
    await tester.pump();
    expect(find.byKey(const Key('recordings-no-match')), findsOneWidget);
    expect(find.text('没有匹配的录制'), findsOneWidget);
    expect(find.byKey(const Key('recordings-list')), findsNothing);
    expect(find.byKey(const Key('recordings-empty')), findsNothing);

    await tester.tap(find.byKey(const Key('recordings-filter-clear')));
    await tester.pump();
    expect(find.text('会话录制.mp4'), findsOneWidget);
    expect(find.text('second.mp4'), findsOneWidget);
    expect(find.byKey(const Key('recordings-no-match')), findsNothing);
  });

  testWidgets('sorts recordings by time, name and size', (
    WidgetTester tester,
  ) async {
    await open(
      tester,
      RecordingArchiveDialog(
        server: settings,
        account: account,
        loadArchive: (ServerSettings server, String token) async {
          return RecordingArchive(
            recordings: <RemoteRecording>[
              recording(
                id: 'a' * 32,
                name: 'alpha.mp4',
                size: 300,
                created: DateTime.utc(2026, 10, 28, 9),
              ),
              recording(
                id: 'b' * 32,
                name: 'beta.mp4',
                size: 100,
                created: DateTime.utc(2026, 10, 27, 9),
              ),
              recording(
                id: 'c' * 32,
                name: 'gamma.mp4',
                size: 200,
                created: DateTime.utc(2026, 10, 29, 9),
              ),
            ],
            usageBytes: 600,
            quotaBytes: 8192,
          );
        },
      ),
    );

    double top(String name) => tester.getTopLeft(find.text(name)).dy;
    void expectOrder(List<String> names) {
      for (int index = 0; index < names.length - 1; index++) {
        expect(
          top(names[index]),
          lessThan(top(names[index + 1])),
          reason: '${names[index]} should precede ${names[index + 1]}',
        );
      }
    }

    // Default: newest first.
    expectOrder(<String>['gamma.mp4', 'alpha.mp4', 'beta.mp4']);

    await tester.tap(find.byKey(const Key('recordings-sort')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('recordings-sort-name')));
    await tester.pumpAndSettle();
    expectOrder(<String>['alpha.mp4', 'beta.mp4', 'gamma.mp4']);
    expect(find.byTooltip('降序'), findsOneWidget);

    await tester.tap(find.byKey(const Key('recordings-sort-direction')));
    await tester.pump();
    expectOrder(<String>['gamma.mp4', 'beta.mp4', 'alpha.mp4']);

    await tester.tap(find.byKey(const Key('recordings-sort')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('recordings-sort-size')));
    await tester.pumpAndSettle();
    // Size starts largest-first.
    expectOrder(<String>['alpha.mp4', 'gamma.mp4', 'beta.mp4']);
    expect(find.byTooltip('升序'), findsOneWidget);
  });
}
