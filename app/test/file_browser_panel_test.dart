import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';
import 'package:chast_viewer/widgets/file_browser_panel.dart';

ViewerFileEntry _file(String name, {int size = 0, int modifiedMs = 0}) =>
    ViewerFileEntry(
      name: name,
      size: size,
      modifiedMs: modifiedMs,
      directory: false,
    );

ViewerFileEntry _folder(String name) =>
    ViewerFileEntry(name: name, size: 0, modifiedMs: 0, directory: true);

ViewerFileListing _listing(
  String directory,
  List<ViewerFileEntry> entries, {
  int code = 0,
  String reason = '',
  bool truncated = false,
}) {
  return ViewerFileListing(
    directory: directory,
    code: code,
    reason: reason,
    truncated: truncated,
    entries: entries,
  );
}

// Vertical position of a row's name inside the listing, for order assertions.
double _rowTop(WidgetTester tester, String name) => tester
    .getTopLeft(
      find.descendant(of: find.byType(ListView), matching: find.text(name)),
    )
    .dy;

// CheckedPopupMenuItem keeps its label in a ListTile-like row, so the text
// itself does not receive the tap; hit the enclosing menu item.
Future<void> _chooseSort(WidgetTester tester, String label) async {
  await tester.tap(
    find.ancestor(
      of: find.text(label),
      matching: find.byWidgetPredicate(
        (Widget widget) => widget is CheckedPopupMenuItem,
      ),
    ),
  );
  await tester.pumpAndSettle();
}

typedef _ListRequest = Future<bool> Function(String directory);
typedef _DownloadRequest = Future<bool> Function(String path);
typedef _TakeList = Future<ViewerFileListing?> Function();
typedef _UploadRequest = Future<int?> Function(String directory);
typedef _OperationRequest =
    Future<bool> Function(ViewerFileOperation op, String path, String newName);
typedef _FolderDownloadRequest = Future<bool> Function(String path);
typedef _DownloadEntriesRequest =
    Future<bool> Function(List<ViewerFileSelection> entries);
typedef _RevealDownload = Future<void> Function(String path);
typedef _OpenDownload = Future<void> Function(String path);

Widget _host({
  required ViewerStatus status,
  required _ListRequest onRequestList,
  _DownloadRequest? onRequestDownload,
  _TakeList? onTakeList,
  _UploadRequest? onUpload,
  _OperationRequest? onFileOperation,
  _FolderDownloadRequest? onDownloadFolder,
  _DownloadEntriesRequest? onDownloadEntries,
  Future<void> Function()? onCancelFolderDownload,
  Future<void> Function()? onCancelUpload,
  Future<void> Function()? onCancelDownload,
  _RevealDownload? onRevealDownload,
  _OpenDownload? onOpenDownload,
  String initialDirectory = '',
  ValueChanged<String>? onDirectoryChanged,
  FileBrowserSort initialSort = FileBrowserSort.name,
  bool initialSortAscending = true,
  FileBrowserSortChanged? onSortChanged,
  VoidCallback? onClose,
}) {
  return MaterialApp(
    home: Scaffold(
      backgroundColor: Colors.black,
      body: Center(
        child: FileBrowserPanel(
          status: status,
          onRequestList: onRequestList,
          onRequestDownload: onRequestDownload ?? (_) async => true,
          onTakeList: onTakeList ?? () async => null,
          onUpload: onUpload ?? (_) async => null,
          onFileOperation: onFileOperation ?? (_, _, _) async => true,
          onDownloadFolder: onDownloadFolder ?? (_) async => true,
          onDownloadEntries: onDownloadEntries ?? (_) async => true,
          onCancelFolderDownload: onCancelFolderDownload ?? () async {},
          onCancelUpload: onCancelUpload ?? () async {},
          onCancelDownload: onCancelDownload ?? () async {},
          onRevealDownload: onRevealDownload,
          onOpenDownload: onOpenDownload,
          initialDirectory: initialDirectory,
          onDirectoryChanged: onDirectoryChanged,
          initialSort: initialSort,
          initialSortAscending: initialSortAscending,
          onSortChanged: onSortChanged,
          onClose: onClose ?? () {},
        ),
      ),
    ),
  );
}

void main() {
  testWidgets('asks for the shared root on mount and adopts the listing', (
    WidgetTester tester,
  ) async {
    final List<String> requests = <String>[];
    final List<String> downloads = <String>[];
    ViewerFileListing? answer;

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (String directory) async {
        requests.add(directory);
        return true;
      },
      onRequestDownload: (String path) async {
        downloads.add(path);
        return true;
      },
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    expect(requests, <String>['']);

    answer = _listing('', <ViewerFileEntry>[
      _folder('photos'),
      _file('notes.txt', size: 1536),
    ], truncated: true);
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    expect(find.text('photos'), findsOneWidget);
    expect(find.text('notes.txt'), findsOneWidget);
    expect(find.text('1.5 KiB'), findsOneWidget);
    expect(find.text('条目太多，主机只返回了前一部分'), findsOneWidget);

    await tester.tap(find.byTooltip('下载到本机'));
    await tester.pump();
    expect(downloads, <String>['notes.txt']);
  });

  testWidgets('a double click on a file downloads it', (
    WidgetTester tester,
  ) async {
    final List<String> downloads = <String>[];
    final ViewerFileListing listing = _listing('', <ViewerFileEntry>[
      _folder('photos'),
      _file('notes.txt', size: 1536),
    ]);
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onRequestDownload: (String path) async {
        downloads.add(path);
        return true;
      },
      onTakeList: () async => listing,
    );
    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.tap(find.text('notes.txt'));
    await tester.pump(const Duration(milliseconds: 50));
    await tester.tap(find.text('notes.txt'));
    await tester.pump(const Duration(milliseconds: 350));
    expect(downloads, <String>['notes.txt']);

    await tester.tap(find.text('photos'));
    await tester.pump(const Duration(milliseconds: 50));
    await tester.tap(find.text('photos'));
    await tester.pump(const Duration(milliseconds: 350));
    expect(downloads, <String>['notes.txt']);
  });

  testWidgets('a double click while selecting only toggles the row', (
    WidgetTester tester,
  ) async {
    final List<String> downloads = <String>[];
    final ViewerFileListing listing = _listing('', <ViewerFileEntry>[
      _file('notes.txt'),
    ]);
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onRequestDownload: (String path) async {
        downloads.add(path);
        return true;
      },
      onTakeList: () async => listing,
    );
    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.tap(find.byKey(const ValueKey<String>('file-select-mode')));
    await tester.pump();
    await tester.tap(find.text('notes.txt'));
    await tester.pump(const Duration(milliseconds: 50));
    await tester.tap(find.text('notes.txt'));
    await tester.pump(const Duration(milliseconds: 350));
    expect(downloads, isEmpty);
  });

  testWidgets('a folder opens its own listing and the up row walks back', (
    WidgetTester tester,
  ) async {
    final List<String> requests = <String>[];
    ViewerFileListing? answer = _listing('', <ViewerFileEntry>[
      _folder('photos'),
      _file('notes.txt'),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (String directory) async {
        requests.add(directory);
        return true;
      },
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.tap(find.text('photos'));
    await tester.pump();
    expect(requests, <String>['', 'photos']);

    answer = _listing('photos', <ViewerFileEntry>[
      _file('holiday.jpg', size: 2048),
    ]);
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 2)));
    await tester.pumpAndSettle();

    expect(find.text('返回上一级'), findsOneWidget);
    expect(find.text('holiday.jpg'), findsOneWidget);
    expect(find.text('2.0 KiB'), findsOneWidget);
    // The breadcrumb carries the current directory.
    expect(find.text('photos'), findsOneWidget);

    await tester.tap(find.text('返回上一级'));
    await tester.pump();
    expect(requests, <String>['', 'photos', '']);
  });

  testWidgets('reports every accepted directory so drops follow the browser', (
    WidgetTester tester,
  ) async {
    final List<String> seen = <String>[];
    ViewerFileListing? answer = _listing('', <ViewerFileEntry>[
      _folder('photos'),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (String directory) async => true,
      onTakeList: () async => answer,
      onDirectoryChanged: seen.add,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();
    // The initial root listing is the pane's starting point, not a change.
    expect(seen, isEmpty);

    await tester.tap(find.text('photos'));
    await tester.pump();
    answer = _listing('photos', <ViewerFileEntry>[_file('holiday.jpg')]);
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 2)));
    await tester.pumpAndSettle();
    expect(seen, <String>['photos']);

    await tester.tap(find.text('返回上一级'));
    await tester.pump();
    answer = _listing('', <ViewerFileEntry>[_folder('photos')]);
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 3)));
    await tester.pumpAndSettle();
    expect(seen, <String>['photos', '']);
  });

  testWidgets('a refused listing answer replaces the list with its reason', (
    WidgetTester tester,
  ) async {
    ViewerFileListing? answer = _listing('', <ViewerFileEntry>[
      _file('notes.txt'),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();
    expect(find.text('notes.txt'), findsOneWidget);

    answer = _listing(
      'private',
      <ViewerFileEntry>[],
      code: 1,
      reason: '主机未授权这个目录',
    );
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 2)));
    await tester.pumpAndSettle();

    expect(find.text('主机未授权这个目录'), findsOneWidget);
  });

  testWidgets('a request that cannot be queued explains itself', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      _host(status: const ViewerStatus(), onRequestList: (_) async => false),
    );
    await tester.pumpAndSettle();

    expect(find.text('请求无法发送：会话未就绪或主机未授权文件传输'), findsOneWidget);
  });

  testWidgets('a refused download answer is shown under the list', (
    WidgetTester tester,
  ) async {
    ViewerFileListing? answer = _listing('', <ViewerFileEntry>[
      _file('notes.txt'),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.pumpWidget(
      build(
        const ViewerStatus(
          fileListAnswers: 1,
          fileDownloadAnswers: 1,
          fileDownloadCode: 2,
          fileDownloadError: '主机正忙',
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('主机正忙'), findsOneWidget);

    // An answer without a reason falls back to the translated default.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          fileListAnswers: 1,
          fileDownloadAnswers: 2,
          fileDownloadCode: 1,
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('主机拒绝了下载请求'), findsOneWidget);
  });

  testWidgets('the footer follows the host-to-viewer download state', (
    WidgetTester tester,
  ) async {
    Widget build(ViewerStatus status) =>
        _host(status: status, onRequestList: (_) async => true);

    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          downloadPhase: ViewerFilePhase.sending,
          downloadName: 'x.bin',
          downloadSize: 2048,
          downloadReceived: 1024,
        ),
      ),
    );
    await tester.pump();
    expect(find.text('正在下载 x.bin · 1.0 KiB / 2.0 KiB · 50%'), findsOneWidget);

    // A host that sends no total size cannot yield a percentage.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          downloadPhase: ViewerFilePhase.sending,
          downloadName: 'y.bin',
          downloadReceived: 300,
        ),
      ),
    );
    await tester.pump();
    expect(find.text('正在下载 y.bin · 300 B / 0 B'), findsOneWidget);

    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          downloadPhase: ViewerFilePhase.done,
          downloadPath: r'C:\Users\me\Downloads\chast\x.bin',
        ),
      ),
    );
    await tester.pump();
    expect(
      find.text(r'已保存到 C:\Users\me\Downloads\chast\x.bin'),
      findsOneWidget,
    );
  });

  testWidgets('a finished download can be revealed in the folder', (
    WidgetTester tester,
  ) async {
    final List<String> revealed = <String>[];
    final List<String> opened = <String>[];
    final List<String> copied = <String>[];
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      SystemChannels.platform,
      (MethodCall call) async {
        if (call.method == 'Clipboard.setData') {
          copied.add(
            (call.arguments as Map<Object?, Object?>)['text'] as String,
          );
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
    await tester.pumpWidget(
      _host(
        status: const ViewerStatus(
          state: ViewerState.streaming,
          downloadPhase: ViewerFilePhase.done,
          downloadPath: r'C:\Users\me\Downloads\chast\x.bin',
        ),
        onRequestList: (_) async => true,
        onRevealDownload: (String path) async => revealed.add(path),
        onOpenDownload: (String path) async => opened.add(path),
      ),
    );
    await tester.pump();

    expect(find.byTooltip('打开文件'), findsOneWidget);
    await tester.tap(find.byTooltip('打开文件'));
    await tester.pump();
    expect(opened, <String>[r'C:\Users\me\Downloads\chast\x.bin']);

    expect(find.byTooltip('在文件夹中显示'), findsOneWidget);
    await tester.tap(find.byTooltip('在文件夹中显示'));
    await tester.pump();
    expect(revealed, <String>[r'C:\Users\me\Downloads\chast\x.bin']);

    expect(find.byTooltip('复制路径'), findsOneWidget);
    await tester.tap(find.byTooltip('复制路径'));
    await tester.pump();
    expect(copied, <String>[r'C:\Users\me\Downloads\chast\x.bin']);
    expect(find.text('路径已复制'), findsOneWidget);
  });

  testWidgets('a clipboard failure keeps the footer and says so', (
    WidgetTester tester,
  ) async {
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      SystemChannels.platform,
      (MethodCall call) async {
        if (call.method == 'Clipboard.setData') {
          throw PlatformException(code: 'clipboard');
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

    await tester.pumpWidget(
      _host(
        status: const ViewerStatus(
          state: ViewerState.streaming,
          downloadPhase: ViewerFilePhase.done,
          downloadPath: r'C:\Users\me\Downloads\chast\x.bin',
        ),
        onRequestList: (_) async => true,
      ),
    );
    await tester.pump();

    await tester.tap(find.byTooltip('复制路径'));
    await tester.pump();

    expect(find.text('路径已复制'), findsNothing);
    expect(find.text('复制失败'), findsOneWidget);
    expect(
      find.text(r'已保存到 C:\Users\me\Downloads\chast\x.bin'),
      findsOneWidget,
    );
  });

  testWidgets('no reveal button when the finished download has no path', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      _host(
        status: const ViewerStatus(
          state: ViewerState.streaming,
          downloadPhase: ViewerFilePhase.done,
        ),
        onRequestList: (_) async => true,
      ),
    );
    await tester.pump();

    expect(find.byTooltip('在文件夹中显示'), findsNothing);
    expect(find.byTooltip('打开文件'), findsNothing);
    expect(find.byTooltip('复制路径'), findsNothing);
  });

  testWidgets('the listing can be filtered by name', (
    WidgetTester tester,
  ) async {
    final ViewerFileListing answer = _listing('', <ViewerFileEntry>[
      _file('alpha.txt'),
      _file('beta.txt'),
      _folder('photos'),
    ]);
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();
    expect(find.text('beta.txt'), findsOneWidget);

    await tester.enterText(
      find.byKey(const ValueKey<String>('file-filter')),
      'ALPHA',
    );
    await tester.pump();
    expect(find.text('alpha.txt'), findsOneWidget);
    expect(find.text('beta.txt'), findsNothing);
    expect(find.text('photos'), findsNothing);

    await tester.enterText(
      find.byKey(const ValueKey<String>('file-filter')),
      'zzz',
    );
    await tester.pump();
    expect(find.text('没有匹配的条目'), findsOneWidget);

    await tester.tap(find.byTooltip('清除筛选'));
    await tester.pump();
    expect(find.text('beta.txt'), findsOneWidget);
    expect(find.text('photos'), findsOneWidget);
  });

  testWidgets('a refused listing offers a retry', (WidgetTester tester) async {
    final List<String> requests = <String>[];
    bool accept = false;
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (String directory) async {
        requests.add(directory);
        return accept;
      },
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();

    expect(requests, <String>['']);
    expect(find.text('请求无法发送：会话未就绪或主机未授权文件传输'), findsOneWidget);

    accept = true;
    await tester.tap(find.byKey(const ValueKey<String>('file-retry')));
    await tester.pump();

    expect(requests, <String>['', '']);
    expect(find.text('请求无法发送：会话未就绪或主机未授权文件传输'), findsNothing);
  });

  testWidgets('a row menu copies its host path', (WidgetTester tester) async {
    final ViewerFileListing answer = _listing('', <ViewerFileEntry>[
      _file('a.bin'),
    ]);
    final List<String> copied = <String>[];
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      SystemChannels.platform,
      (MethodCall call) async {
        if (call.method == 'Clipboard.setData') {
          copied.add(
            (call.arguments as Map<Object?, Object?>)['text'] as String,
          );
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
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('更多操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('复制路径'));
    await tester.pump();

    expect(copied, <String>['a.bin']);
    expect(find.text('路径已复制'), findsOneWidget);
  });

  testWidgets('a row right click opens the same host menu', (
    WidgetTester tester,
  ) async {
    final ViewerFileListing answer = _listing('', <ViewerFileEntry>[
      _file('a.bin'),
    ]);
    final List<String> copied = <String>[];
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      SystemChannels.platform,
      (MethodCall call) async {
        if (call.method == 'Clipboard.setData') {
          copied.add(
            (call.arguments as Map<Object?, Object?>)['text'] as String,
          );
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
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.tap(find.text('a.bin'), buttons: kSecondaryButton);
    await tester.pumpAndSettle();

    expect(find.text('复制路径'), findsOneWidget);
    expect(find.text('重命名'), findsOneWidget);
    expect(find.text('删除'), findsOneWidget);

    await tester.tap(find.text('复制路径'));
    await tester.pump();

    expect(copied, <String>['a.bin']);
    expect(find.text('路径已复制'), findsOneWidget);
  });

  testWidgets('a long press opens the same row menu', (
    WidgetTester tester,
  ) async {
    final ViewerFileListing answer = _listing('', <ViewerFileEntry>[
      _folder('photos'),
    ]);
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.longPress(find.text('photos'));
    await tester.pumpAndSettle();

    expect(find.text('复制路径'), findsOneWidget);
    expect(find.text('重命名'), findsOneWidget);
    expect(find.text('删除'), findsOneWidget);
  });

  testWidgets('a right click on the parent row opens no menu', (
    WidgetTester tester,
  ) async {
    final ViewerFileListing answer = _listing('docs', <ViewerFileEntry>[]);
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
      initialDirectory: 'docs',
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.tap(find.text('返回上一级'), buttons: kSecondaryButton);
    await tester.pumpAndSettle();

    expect(find.text('复制路径'), findsNothing);
    expect(find.text('重命名'), findsNothing);
  });

  testWidgets('the filter row walks back to the parent folder', (
    WidgetTester tester,
  ) async {
    final List<String> requests = <String>[];
    ViewerFileListing answer = _listing('', <ViewerFileEntry>[
      _folder('photos'),
    ]);
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (String directory) async {
        requests.add(directory);
        return true;
      },
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();
    expect(find.byTooltip('上一级'), findsNothing);

    await tester.tap(find.text('photos'));
    await tester.pump();
    answer = _listing('photos', <ViewerFileEntry>[_file('a.bin')]);
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 2)));
    await tester.pumpAndSettle();

    expect(find.byTooltip('上一级'), findsOneWidget);
    await tester.tap(find.byTooltip('上一级'));
    await tester.pump();
    expect(requests, <String>['', 'photos', '']);

    answer = _listing('', <ViewerFileEntry>[_folder('photos')]);
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 3)));
    await tester.pumpAndSettle();
    expect(find.byTooltip('上一级'), findsNothing);
  });

  testWidgets('the filter row shows the folder item count', (
    WidgetTester tester,
  ) async {
    final ViewerFileListing answer = _listing('', <ViewerFileEntry>[
      _file('alpha.txt'),
      _file('beta.txt'),
      _folder('photos'),
    ]);
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();
    expect(find.text('共 3 项'), findsOneWidget);

    await tester.enterText(
      find.byKey(const ValueKey<String>('file-filter')),
      'ALPHA',
    );
    await tester.pump();
    expect(find.text('匹配 1 / 3 项'), findsOneWidget);

    await tester.enterText(
      find.byKey(const ValueKey<String>('file-filter')),
      'zzz',
    );
    await tester.pump();
    expect(find.text('匹配 0 / 3 项'), findsOneWidget);
  });

  testWidgets('entering a folder clears the filter', (
    WidgetTester tester,
  ) async {
    final List<String> requests = <String>[];
    ViewerFileListing answer = _listing('', <ViewerFileEntry>[
      _file('alpha.txt'),
      _folder('photos'),
    ]);
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (String directory) async {
        requests.add(directory);
        return true;
      },
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.enterText(
      find.byKey(const ValueKey<String>('file-filter')),
      'photos',
    );
    await tester.pump();
    await tester.tap(
      find.descendant(of: find.byType(ListView), matching: find.text('photos')),
    );
    await tester.pump();
    expect(requests, <String>['', 'photos']);

    answer = _listing('photos', <ViewerFileEntry>[_file('gamma.txt')]);
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 2)));
    await tester.pumpAndSettle();
    expect(find.text('gamma.txt'), findsOneWidget);
    expect(find.text('alpha.txt'), findsNothing);
    final TextField field = tester.widget<TextField>(
      find.byKey(const ValueKey<String>('file-filter')),
    );
    expect(field.controller?.text, isEmpty);
  });

  testWidgets('rows sort by size and the direction button flips them', (
    WidgetTester tester,
  ) async {
    final ViewerFileListing answer = _listing('', <ViewerFileEntry>[
      _file('alpha.bin', size: 300),
      _file('beta.bin', size: 5),
      _file('gamma.bin', size: 90),
    ]);
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();
    // The default is the host's own order: by name.
    expect(_rowTop(tester, 'alpha.bin'), lessThan(_rowTop(tester, 'beta.bin')));

    await tester.tap(find.byTooltip('排序'));
    await tester.pumpAndSettle();
    await _chooseSort(tester, '按大小');
    expect(_rowTop(tester, 'beta.bin'), lessThan(_rowTop(tester, 'gamma.bin')));
    expect(
      _rowTop(tester, 'gamma.bin'),
      lessThan(_rowTop(tester, 'alpha.bin')),
    );
    expect(find.byTooltip('降序'), findsOneWidget);

    await tester.tap(find.byTooltip('降序'));
    await tester.pump();
    expect(
      _rowTop(tester, 'alpha.bin'),
      lessThan(_rowTop(tester, 'gamma.bin')),
    );
    expect(_rowTop(tester, 'gamma.bin'), lessThan(_rowTop(tester, 'beta.bin')));
    expect(find.byTooltip('升序'), findsOneWidget);
  });

  testWidgets('folders stay above files whatever the sort', (
    WidgetTester tester,
  ) async {
    final ViewerFileListing answer = _listing('', <ViewerFileEntry>[
      _file('old.txt', modifiedMs: 1000),
      _folder('photos'),
      _file('new.txt', modifiedMs: 9000),
    ]);
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('排序'));
    await tester.pumpAndSettle();
    await _chooseSort(tester, '按时间');
    expect(_rowTop(tester, 'photos'), lessThan(_rowTop(tester, 'old.txt')));
    expect(_rowTop(tester, 'old.txt'), lessThan(_rowTop(tester, 'new.txt')));

    await tester.tap(find.byTooltip('降序'));
    await tester.pump();
    expect(_rowTop(tester, 'photos'), lessThan(_rowTop(tester, 'new.txt')));
    expect(_rowTop(tester, 'new.txt'), lessThan(_rowTop(tester, 'old.txt')));
  });

  testWidgets('the sort choice is reported and seeds the next open', (
    WidgetTester tester,
  ) async {
    final ViewerFileListing answer = _listing('', <ViewerFileEntry>[
      _file('alpha.bin', size: 300),
      _file('beta.bin', size: 5),
      _file('gamma.bin', size: 90),
    ]);
    final List<String> changes = <String>[];
    FileBrowserSort sort = FileBrowserSort.name;
    bool ascending = true;

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
      initialSort: sort,
      initialSortAscending: ascending,
      onSortChanged: (FileBrowserSort next, bool nextAscending) {
        sort = next;
        ascending = nextAscending;
        changes.add('${next.name}:$nextAscending');
      },
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('排序'));
    await tester.pumpAndSettle();
    await _chooseSort(tester, '按大小');
    await tester.tap(find.byTooltip('降序'));
    await tester.pump();
    expect(changes, <String>['size:true', 'size:false']);

    // Reopening with the remembered choice starts in that order.
    await tester.pumpWidget(const SizedBox());
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 2)));
    await tester.pumpAndSettle();
    expect(
      _rowTop(tester, 'alpha.bin'),
      lessThan(_rowTop(tester, 'gamma.bin')),
    );
    expect(_rowTop(tester, 'gamma.bin'), lessThan(_rowTop(tester, 'beta.bin')));
    expect(find.byTooltip('升序'), findsOneWidget);
  });

  testWidgets('reopening the browser starts in the last folder', (
    WidgetTester tester,
  ) async {
    final List<String> requests = <String>[];
    final ViewerFileListing answer = _listing('photos', <ViewerFileEntry>[
      _file('holiday.jpg', size: 1536),
    ]);
    Widget build(ViewerStatus status) => _host(
      status: status,
      initialDirectory: 'photos',
      onRequestList: (String directory) async {
        requests.add(directory);
        return true;
      },
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    expect(requests, <String>['photos']);

    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();
    expect(find.text('holiday.jpg'), findsOneWidget);
    expect(find.text('photos'), findsOneWidget);
  });

  testWidgets('a saved folder the host refuses falls back to the root', (
    WidgetTester tester,
  ) async {
    final List<String> requests = <String>[];
    final List<String> seen = <String>[];
    ViewerFileListing answer = _listing(
      'gone',
      <ViewerFileEntry>[],
      code: 1,
      reason: '文件夹不存在',
    );
    Widget build(ViewerStatus status) => _host(
      status: status,
      initialDirectory: 'gone',
      onRequestList: (String directory) async {
        requests.add(directory);
        return true;
      },
      onTakeList: () async => answer,
      onDirectoryChanged: seen.add,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    expect(requests, <String>['gone']);

    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pump();
    expect(requests, <String>['gone', '']);

    answer = _listing('', <ViewerFileEntry>[_file('notes.txt')]);
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 2)));
    await tester.pumpAndSettle();
    expect(requests, <String>['gone', '']);
    expect(seen, <String>['']);
    expect(find.text('文件夹不存在'), findsNothing);
    expect(find.text('notes.txt'), findsOneWidget);
  });

  testWidgets('the close button reports back', (WidgetTester tester) async {
    int closes = 0;
    await tester.pumpWidget(
      _host(
        status: const ViewerStatus(),
        onRequestList: (_) async => true,
        onClose: () => ++closes,
      ),
    );
    await tester.pump();

    await tester.tap(find.byTooltip('关闭文件浏览'));
    expect(closes, 1);
  });

  testWidgets('upload targets the browsed folder and refreshes when it ends', (
    WidgetTester tester,
  ) async {
    final List<String> requests = <String>[];
    final List<String> uploads = <String>[];
    ViewerFileListing? answer = _listing('', <ViewerFileEntry>[
      _folder('photos'),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (String directory) async {
        requests.add(directory);
        return true;
      },
      onTakeList: () async => answer,
      onUpload: (String directory) async {
        uploads.add(directory);
        return 1;
      },
    );

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();
    await tester.pumpWidget(
      build(
        const ViewerStatus(state: ViewerState.streaming, fileListAnswers: 1),
      ),
    );
    await tester.pumpAndSettle();

    await tester.tap(find.text('photos'));
    await tester.pump();
    answer = _listing('photos', <ViewerFileEntry>[]);
    await tester.pumpWidget(
      build(
        const ViewerStatus(state: ViewerState.streaming, fileListAnswers: 2),
      ),
    );
    await tester.pumpAndSettle();
    expect(requests, <String>['', 'photos']);

    // The button hands the current directory to the bridge.
    await tester.tap(find.byTooltip('上传文件到此处'));
    await tester.pump();
    expect(uploads, <String>['photos']);

    // While the queue runs the footer names the target and the queue length.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          fileListAnswers: 2,
          filePhase: ViewerFilePhase.sending,
          fileQueue: 1,
          fileName: 'up.txt',
        ),
      ),
    );
    await tester.pump();
    expect(find.text('正在上传到 photos（队列剩余 1 个）'), findsOneWidget);

    // The queue draining refreshes the listing, which then shows the file.
    answer = _listing('photos', <ViewerFileEntry>[_file('up.txt', size: 5)]);
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          fileListAnswers: 2,
          filePhase: ViewerFilePhase.done,
          lastFileName: 'up.txt',
        ),
      ),
    );
    await tester.pump();
    expect(requests, <String>['', 'photos', 'photos']);
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          fileListAnswers: 3,
          filePhase: ViewerFilePhase.done,
          lastFileName: 'up.txt',
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('up.txt'), findsOneWidget);
    expect(find.text('上传完成，列表已刷新'), findsOneWidget);
  });

  testWidgets('a dropped upload also offers the cancel action', (
    WidgetTester tester,
  ) async {
    int cancels = 0;
    await tester.pumpWidget(
      _host(
        status: const ViewerStatus(
          state: ViewerState.streaming,
          filePhase: ViewerFilePhase.sending,
          fileQueue: 2,
          fileName: 'drop.bin',
        ),
        onRequestList: (_) async => true,
        onCancelUpload: () async => ++cancels,
      ),
    );
    await tester.pump();

    expect(find.text('正在上传到 共享文件夹（队列剩余 2 个）'), findsOneWidget);
    expect(find.byType(LinearProgressIndicator), findsNothing);
    await tester.tap(find.byTooltip('取消上传'));
    await tester.pump();
    expect(cancels, 1);
    expect(find.text('已取消上传'), findsOneWidget);
  });

  testWidgets('the upload footer shows bytes and a progress bar', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      _host(
        status: const ViewerStatus(
          state: ViewerState.streaming,
          filePhase: ViewerFilePhase.sending,
          fileQueue: 1,
          fileName: 'big.bin',
          fileSize: 2048,
          fileSent: 1024,
        ),
        onRequestList: (_) async => true,
      ),
    );
    await tester.pump();

    expect(
      find.text('正在上传到 共享文件夹 · 1.0 KiB / 2.0 KiB · 50%（队列剩余 1 个）'),
      findsOneWidget,
    );
    LinearProgressIndicator bar = tester.widget<LinearProgressIndicator>(
      find.byType(LinearProgressIndicator),
    );
    expect(bar.value, 0.5);

    // A later status with more bytes moves both the text and the bar.
    await tester.pumpWidget(
      _host(
        status: const ViewerStatus(
          state: ViewerState.streaming,
          filePhase: ViewerFilePhase.sending,
          fileQueue: 1,
          fileName: 'big.bin',
          fileSize: 2048,
          fileSent: 2048,
        ),
        onRequestList: (_) async => true,
      ),
    );
    await tester.pump();

    expect(
      find.text('正在上传到 共享文件夹 · 2.0 KiB / 2.0 KiB · 100%（队列剩余 1 个）'),
      findsOneWidget,
    );
    bar = tester.widget<LinearProgressIndicator>(
      find.byType(LinearProgressIndicator),
    );
    expect(bar.value, 1.0);
  });

  testWidgets('a fast upload is noticed through the finished entry', (
    WidgetTester tester,
  ) async {
    final List<String> requests = <String>[];
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (String directory) async {
        requests.add(directory);
        return true;
      },
      onUpload: (_) async => 1,
    );

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();
    expect(requests, <String>['']);

    await tester.tap(find.byTooltip('上传文件到此处'));
    await tester.pump();

    // No poll ever saw the queue active; the finished entry still refreshes.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          filePhase: ViewerFilePhase.done,
          lastFileName: 'tiny.txt',
        ),
      ),
    );
    await tester.pump();
    expect(requests, <String>['', '']);
    expect(find.text('上传完成，列表已刷新'), findsOneWidget);
  });

  testWidgets('a failed upload shows the host reason', (
    WidgetTester tester,
  ) async {
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onUpload: (_) async => 1,
    );

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();
    await tester.tap(find.byTooltip('上传文件到此处'));
    await tester.pump();

    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          filePhase: ViewerFilePhase.failed,
          lastFileName: 'big.bin',
          lastFileError: 'the host is busy',
        ),
      ),
    );
    await tester.pump();
    expect(find.text('上传失败：the host is busy'), findsOneWidget);
  });

  testWidgets('an upload in progress can be cancelled from the footer', (
    WidgetTester tester,
  ) async {
    int cancels = 0;
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onUpload: (_) async => 2,
      onCancelUpload: () async => ++cancels,
    );

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();
    await tester.tap(find.byTooltip('上传文件到此处'));
    await tester.pump();

    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          filePhase: ViewerFilePhase.sending,
          fileQueue: 1,
          fileName: 'big.bin',
        ),
      ),
    );
    await tester.pump();
    expect(find.text('正在上传到 共享文件夹（队列剩余 1 个）'), findsOneWidget);

    await tester.tap(find.byTooltip('取消上传'));
    await tester.pump();
    expect(cancels, 1);
    expect(find.text('已取消上传'), findsOneWidget);
    expect(find.byTooltip('取消上传'), findsNothing);

    // The host reports the aborted transfer as an error; the panel keeps the
    // cancellation wording instead of surfacing that host message.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          filePhase: ViewerFilePhase.failed,
          lastFileName: 'big.bin',
          lastFileError: 'the viewer cancelled the upload',
        ),
      ),
    );
    await tester.pump();
    expect(find.text('已取消上传'), findsOneWidget);
    expect(
      find.textContaining('the viewer cancelled the upload'),
      findsNothing,
    );
  });

  testWidgets('a download in progress can be cancelled from the footer', (
    WidgetTester tester,
  ) async {
    int cancels = 0;
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onCancelDownload: () async => ++cancels,
    );

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();

    // A clipboard pull also reports a download, but it keeps running: the
    // engine has no cancel for it, so the footer must not offer one.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          downloadPhase: ViewerFilePhase.sending,
          downloadName: 'copy.bin',
          downloadSize: 400,
          downloadReceived: 100,
        ),
      ),
    );
    await tester.pump();
    expect(find.textContaining('正在下载 copy.bin'), findsOneWidget);
    expect(find.byTooltip('取消下载'), findsNothing);

    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          downloadPhase: ViewerFilePhase.sending,
          downloadName: 'Big.bin',
          downloadSize: 16000000,
          downloadReceived: 1000000,
          downloadCancellable: true,
        ),
      ),
    );
    await tester.pump();
    expect(find.textContaining('正在下载 Big.bin'), findsOneWidget);

    await tester.tap(find.byTooltip('取消下载'));
    await tester.pump();
    expect(cancels, 1);
    expect(find.text('已取消下载'), findsOneWidget);
    expect(find.byTooltip('取消下载'), findsNothing);

    // The engine reports the aborted transfer as a failure; the panel keeps
    // the cancellation wording instead of surfacing that reason.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          downloadPhase: ViewerFilePhase.failed,
          downloadName: 'Big.bin',
          downloadError: 'the viewer cancelled the download',
        ),
      ),
    );
    await tester.pump();
    expect(find.text('已取消下载'), findsOneWidget);
    expect(
      find.textContaining('the viewer cancelled the download'),
      findsNothing,
    );

    // A new download replaces the wording.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          downloadPhase: ViewerFilePhase.sending,
          downloadName: 'Next.bin',
          downloadSize: 800,
          downloadReceived: 100,
          downloadCancellable: true,
        ),
      ),
    );
    await tester.pump();
    expect(find.textContaining('正在下载 Next.bin'), findsOneWidget);
    expect(find.text('已取消下载'), findsNothing);
  });

  testWidgets('a cancelled picker changes nothing', (
    WidgetTester tester,
  ) async {
    final List<String> requests = <String>[];
    await tester.pumpWidget(
      _host(
        status: const ViewerStatus(),
        onRequestList: (String directory) async {
          requests.add(directory);
          return true;
        },
        onUpload: (_) async => null,
      ),
    );
    await tester.pump();

    await tester.tap(find.byTooltip('上传文件到此处'));
    await tester.pump();
    expect(requests, <String>['']);
    expect(
      find.text('点击文件右侧的下载按钮保存到本机；多选可批量下载或删除；标题栏的上传按钮会把文件传到当前文件夹'),
      findsOneWidget,
    );
  });

  testWidgets('the new-folder button names a folder below the current one', (
    WidgetTester tester,
  ) async {
    final List<String> requests = <String>[];
    final List<String> ops = <String>[];
    ViewerFileListing? answer = _listing('', <ViewerFileEntry>[
      _file('notes.txt', size: 5),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (String directory) async {
        requests.add(directory);
        return true;
      },
      onTakeList: () async => answer,
      onFileOperation:
          (ViewerFileOperation op, String path, String newName) async {
            ops.add('${op.name}:$path:$newName');
            return true;
          },
    );

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();
    expect(requests, <String>['']);
    await tester.pumpWidget(
      build(
        const ViewerStatus(state: ViewerState.streaming, fileListAnswers: 1),
      ),
    );
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('新建文件夹'));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.descendant(
        of: find.byType(AlertDialog),
        matching: find.byType(TextField),
      ),
      'docs',
    );
    await tester.tap(find.widgetWithText(TextButton, '创建'));
    await tester.pumpAndSettle();
    expect(ops, <String>['makeDirectory:docs:']);

    // A successful answer reloads the current directory.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          fileOpAnswers: 1,
          fileOpPath: 'docs',
        ),
      ),
    );
    await tester.pump();
    expect(requests, <String>['', '']);
    expect(find.text('操作完成，列表已刷新'), findsOneWidget);

    // A refusal keeps the host's reason and does not reload.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          fileOpAnswers: 2,
          fileOpCode: 1,
          fileOpPath: 'docs',
          fileOpError: 'already exists',
        ),
      ),
    );
    await tester.pump();
    expect(find.text('already exists'), findsOneWidget);
    expect(requests, <String>['', '']);
  });

  testWidgets('a row menu renames the entry', (WidgetTester tester) async {
    final List<String> ops = <String>[];
    ViewerFileListing? answer = _listing('', <ViewerFileEntry>[
      _file('notes.txt', size: 5),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
      onFileOperation:
          (ViewerFileOperation op, String path, String newName) async {
            ops.add('${op.name}:$path:$newName');
            return true;
          },
    );

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();
    await tester.pumpWidget(
      build(
        const ViewerStatus(state: ViewerState.streaming, fileListAnswers: 1),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('notes.txt'), findsOneWidget);

    await tester.tap(find.byTooltip('更多操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('重命名'));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.descendant(
        of: find.byType(AlertDialog),
        matching: find.byType(TextField),
      ),
      'renamed.txt',
    );
    await tester.tap(find.widgetWithText(TextButton, '重命名'));
    await tester.pumpAndSettle();
    expect(ops, <String>['rename:notes.txt:renamed.txt']);
  });

  testWidgets('renaming onto an existing entry is refused locally', (
    WidgetTester tester,
  ) async {
    final List<String> ops = <String>[];
    final ViewerFileListing listing = _listing('', <ViewerFileEntry>[
      _folder('photos'),
      _file('notes.txt', size: 5),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => listing,
      onFileOperation:
          (ViewerFileOperation op, String path, String newName) async {
            ops.add('${op.name}:$path:$newName');
            return true;
          },
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('更多操作').last);
    await tester.pumpAndSettle();
    await tester.tap(find.text('重命名'));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.descendant(
        of: find.byType(AlertDialog),
        matching: find.byType(TextField),
      ),
      'PHOTOS',
    );
    await tester.tap(find.widgetWithText(TextButton, '重命名'));
    await tester.pumpAndSettle();
    expect(ops, isEmpty);
    expect(find.text('同名文件已存在'), findsOneWidget);
  });

  testWidgets('creating a folder that collides with an entry is refused', (
    WidgetTester tester,
  ) async {
    final List<String> ops = <String>[];
    final ViewerFileListing listing = _listing('', <ViewerFileEntry>[
      _folder('photos'),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => listing,
      onFileOperation:
          (ViewerFileOperation op, String path, String newName) async {
            ops.add('${op.name}:$path:$newName');
            return true;
          },
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('新建文件夹'));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.descendant(
        of: find.byType(AlertDialog),
        matching: find.byType(TextField),
      ),
      'Photos',
    );
    await tester.tap(find.widgetWithText(TextButton, '创建'));
    await tester.pumpAndSettle();
    expect(ops, isEmpty);
    expect(find.text('同名文件已存在'), findsOneWidget);
  });

  testWidgets('deleting asks first and shows the host refusal', (
    WidgetTester tester,
  ) async {
    final List<String> ops = <String>[];
    ViewerFileListing? answer = _listing('', <ViewerFileEntry>[
      _folder('photos'),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
      onFileOperation:
          (ViewerFileOperation op, String path, String newName) async {
            ops.add('${op.name}:$path');
            return true;
          },
    );

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();
    await tester.pumpWidget(
      build(
        const ViewerStatus(state: ViewerState.streaming, fileListAnswers: 1),
      ),
    );
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('更多操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('删除'));
    await tester.pumpAndSettle();
    expect(find.text('删除“photos”？'), findsOneWidget);
    expect(find.text('文件夹必须为空才能删除，此操作不可撤销。'), findsOneWidget);
    await tester.tap(find.widgetWithText(TextButton, '取消'));
    await tester.pumpAndSettle();
    expect(ops, isEmpty);

    await tester.tap(find.byTooltip('更多操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('删除'));
    await tester.pumpAndSettle();
    await tester.tap(find.widgetWithText(TextButton, '删除'));
    await tester.pumpAndSettle();
    expect(ops, <String>['remove:photos']);

    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          fileListAnswers: 1,
          fileOpAnswers: 1,
          fileOpCode: 2,
          fileOpPath: 'photos',
          fileOpError: '主机正忙',
        ),
      ),
    );
    await tester.pump();
    expect(find.text('主机正忙'), findsOneWidget);
  });

  testWidgets('a folder row queues a recursive download and can be cancelled', (
    WidgetTester tester,
  ) async {
    final List<String> folders = <String>[];
    int cancels = 0;
    ViewerFileListing? answer = _listing('', <ViewerFileEntry>[
      _folder('photos'),
      _file('notes.txt'),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
      onDownloadFolder: (String path) async {
        folders.add(path);
        return true;
      },
      onCancelFolderDownload: () async => ++cancels,
    );

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();
    await tester.pumpWidget(
      build(
        const ViewerStatus(state: ViewerState.streaming, fileListAnswers: 1),
      ),
    );
    await tester.pumpAndSettle();

    // The folder row and the file row carry distinct download tooltips.
    await tester.tap(find.byTooltip('下载文件夹到本机'));
    await tester.pump();
    expect(folders, <String>['photos']);

    // While the walk runs the footer names the folder and its progress, and
    // the cancel button reaches the viewer.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          fileListAnswers: 1,
          folderDownloadActive: true,
          folderDownloadName: 'photos',
          folderDownloadFilesTotal: 3,
          folderDownloadFilesDone: 1,
          folderDownloadCurrent: 'photos/raw/a.jpg',
        ),
      ),
    );
    await tester.pump();
    expect(find.text('正在下载文件夹 photos · 1/3 个文件'), findsOneWidget);
    expect(find.text('当前：photos/raw/a.jpg'), findsOneWidget);
    // The row buttons are parked while the walk owns the download slot.
    expect(find.byTooltip('下载到本机'), findsNothing);
    expect(find.byTooltip('下载文件夹到本机'), findsNothing);

    await tester.tap(find.byTooltip('取消文件夹下载'));
    await tester.pump();
    expect(cancels, 1);
  });

  testWidgets('a batch walk words its progress as selected entries', (
    WidgetTester tester,
  ) async {
    Widget build(ViewerStatus status) =>
        _host(status: status, onRequestList: (_) async => true);

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          folderDownloadActive: true,
          folderDownloadBatch: true,
          folderDownloadRoots: 3,
          folderDownloadFilesTotal: 2,
          folderDownloadFilesDone: 1,
        ),
      ),
    );
    await tester.pump();
    expect(find.text('正在下载选中的 3 项 · 1/2 个文件'), findsOneWidget);
    expect(find.text('正在下载文件夹'), findsNothing);
  });

  testWidgets('folder download answers report done, cancel and failure', (
    WidgetTester tester,
  ) async {
    final List<String> revealed = <String>[];
    final List<String> opened = <String>[];
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onRevealDownload: (String path) async => revealed.add(path),
      onOpenDownload: (String path) async => opened.add(path),
    );

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();

    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          folderDownloadAnswers: 1,
          folderDownloadDestination: r'C:\Users\me\Downloads\chast\photos',
        ),
      ),
    );
    await tester.pump();
    expect(
      find.text(r'文件夹已下载到 C:\Users\me\Downloads\chast\photos'),
      findsOneWidget,
    );
    await tester.tap(find.byTooltip('打开文件夹'));
    await tester.pump();
    expect(opened, <String>[r'C:\Users\me\Downloads\chast\photos']);
    await tester.tap(find.byTooltip('在文件夹中显示'));
    await tester.pump();
    expect(revealed, <String>[r'C:\Users\me\Downloads\chast\photos']);

    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          folderDownloadAnswers: 2,
          folderDownloadCode: 2,
        ),
      ),
    );
    await tester.pump();
    expect(find.text('已取消文件夹下载'), findsOneWidget);
    expect(find.byTooltip('打开文件夹'), findsNothing);
    expect(find.byTooltip('在文件夹中显示'), findsNothing);

    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          folderDownloadAnswers: 3,
          folderDownloadCode: 1,
          folderDownloadError: 'disk full',
        ),
      ),
    );
    await tester.pump();
    expect(find.text('文件夹下载失败：disk full'), findsOneWidget);

    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          folderDownloadAnswers: 4,
          folderDownloadCode: 1,
        ),
      ),
    );
    await tester.pump();
    expect(find.text('文件夹下载失败'), findsOneWidget);

    // A batch walk names the selection size instead of a single folder.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          folderDownloadAnswers: 5,
          folderDownloadDestination: r'C:\Users\me\Downloads\chast',
          folderDownloadRoots: 3,
          folderDownloadBatch: true,
        ),
      ),
    );
    await tester.pump();
    expect(
      find.text(r'选中的 3 项已下载到 C:\Users\me\Downloads\chast'),
      findsOneWidget,
    );

    // A batch can hold one file; the flag keeps the wording from turning into
    // a folder download.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          folderDownloadAnswers: 6,
          folderDownloadDestination: r'C:\Users\me\Downloads\chast',
          folderDownloadRoots: 1,
          folderDownloadBatch: true,
        ),
      ),
    );
    await tester.pump();
    expect(
      find.text(r'选中的 1 项已下载到 C:\Users\me\Downloads\chast'),
      findsOneWidget,
    );
  });

  testWidgets('a folder download the viewer will not take explains itself', (
    WidgetTester tester,
  ) async {
    ViewerFileListing? answer = _listing('', <ViewerFileEntry>[
      _folder('photos'),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
      onDownloadFolder: (_) async => false,
    );

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();
    await tester.pumpWidget(
      build(
        const ViewerStatus(state: ViewerState.streaming, fileListAnswers: 1),
      ),
    );
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('下载文件夹到本机'));
    await tester.pump();
    expect(find.text('请求无法发送：会话未就绪或主机未授权文件传输'), findsOneWidget);
  });

  testWidgets('multi-select batches downloads with directory flags', (
    WidgetTester tester,
  ) async {
    final List<List<ViewerFileSelection>> batches =
        <List<ViewerFileSelection>>[];
    final List<String> requests = <String>[];
    ViewerFileListing? answer = _listing('', <ViewerFileEntry>[
      _folder('photos'),
      _file('notes.txt', size: 5),
      _file('a.bin'),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (String directory) async {
        requests.add(directory);
        return true;
      },
      onTakeList: () async => answer,
      onDownloadEntries: (List<ViewerFileSelection> entries) async {
        batches.add(entries);
        return true;
      },
    );

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();
    await tester.pumpWidget(
      build(
        const ViewerStatus(state: ViewerState.streaming, fileListAnswers: 1),
      ),
    );
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('多选'));
    await tester.pump();
    expect(find.text('已选 0 项'), findsOneWidget);

    // A row taps toggle the selection, including folders that would otherwise
    // open.
    await tester.tap(find.text('photos'));
    await tester.pump();
    expect(find.text('已选 1 项'), findsOneWidget);
    expect(requests, <String>['']);

    await tester.tap(find.byKey(const ValueKey<String>('file-select-all')));
    await tester.pump();
    expect(find.text('已选 3 项 · 5 B'), findsOneWidget);
    expect(find.text('清空'), findsOneWidget);

    await tester.tap(find.byKey(const ValueKey<String>('file-select-all')));
    await tester.pump();
    expect(find.text('已选 0 项'), findsOneWidget);
    expect(find.text('全选'), findsOneWidget);

    await tester.tap(find.text('photos'));
    await tester.pump();
    await tester.tap(find.text('notes.txt'));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey<String>('file-batch-download')));
    await tester.pump();

    expect(batches, hasLength(1));
    expect(
      batches.single
          .map(
            (ViewerFileSelection entry) => '${entry.directory}:${entry.path}',
          )
          .toList(),
      <String>['true:photos', 'false:notes.txt'],
    );
    // A queued batch leaves selection mode; progress arrives in the footer.
    expect(find.text('已选 0 项'), findsNothing);
    expect(find.byTooltip('多选'), findsOneWidget);
  });

  testWidgets('the selection bar sums the selected file sizes', (
    WidgetTester tester,
  ) async {
    final ViewerFileListing answer = _listing('', <ViewerFileEntry>[
      _folder('photos'),
      _file('a.bin', size: 1024),
      _file('b.bin', size: 2048),
    ]);
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('多选'));
    await tester.pump();
    await tester.tap(find.text('photos'));
    await tester.pump();
    // A folder carries no size of its own.
    expect(find.text('已选 1 项'), findsOneWidget);

    await tester.tap(find.text('a.bin'));
    await tester.pump();
    await tester.tap(find.text('b.bin'));
    await tester.pump();
    expect(find.text('已选 3 项 · 3.0 KiB'), findsOneWidget);
  });

  testWidgets('the selection bar copies the selected host paths', (
    WidgetTester tester,
  ) async {
    final ViewerFileListing answer = _listing('', <ViewerFileEntry>[
      _folder('photos'),
      _file('a.bin'),
    ]);
    final List<String> copied = <String>[];
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      SystemChannels.platform,
      (MethodCall call) async {
        if (call.method == 'Clipboard.setData') {
          copied.add(
            (call.arguments as Map<Object?, Object?>)['text'] as String,
          );
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
    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (_) async => true,
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('多选'));
    await tester.pump();
    await tester.tap(find.text('全选'));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey<String>('file-batch-copy')));
    await tester.pump();

    expect(copied, <String>['photos\na.bin']);
    expect(find.text('路径已复制'), findsOneWidget);
  });

  testWidgets('a same-folder refresh keeps the multi-select choice', (
    WidgetTester tester,
  ) async {
    final List<String> requests = <String>[];
    ViewerFileListing? answer = _listing('', <ViewerFileEntry>[
      _file('a.bin'),
      _file('b.bin'),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (String directory) async {
        requests.add(directory);
        return true;
      },
      onTakeList: () async => answer,
    );

    await tester.pumpWidget(build(const ViewerStatus()));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 1)));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('多选'));
    await tester.pump();
    await tester.tap(find.text('a.bin'));
    await tester.pump();
    expect(find.text('已选 1 项'), findsOneWidget);

    // Uploads and renames reload the same folder; the pending batch choice
    // must survive that reload.
    await tester.tap(find.byTooltip('刷新'));
    await tester.pump();
    expect(requests, <String>['', '']);
    answer = _listing('', <ViewerFileEntry>[
      _file('a.bin'),
      _file('c.bin', size: 3),
    ]);
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 2)));
    await tester.pumpAndSettle();
    expect(find.text('已选 1 项'), findsOneWidget);
    expect(find.text('c.bin'), findsOneWidget);

    // A selected name that the host no longer lists is dropped.
    await tester.tap(find.byTooltip('刷新'));
    await tester.pump();
    answer = _listing('', <ViewerFileEntry>[_file('b.bin', size: 2)]);
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 3)));
    await tester.pumpAndSettle();
    expect(find.text('已选 0 项'), findsOneWidget);
    expect(find.text('a.bin'), findsNothing);
  });

  testWidgets('batch delete removes one entry at a time and sums failures', (
    WidgetTester tester,
  ) async {
    final List<String> ops = <String>[];
    final List<String> requests = <String>[];
    ViewerFileListing? answer = _listing('', <ViewerFileEntry>[
      _folder('photos'),
      _file('notes.txt'),
    ]);

    Widget build(ViewerStatus status) => _host(
      status: status,
      onRequestList: (String directory) async {
        requests.add(directory);
        return true;
      },
      onTakeList: () async => answer,
      onFileOperation:
          (ViewerFileOperation op, String path, String newName) async {
            ops.add('${op.name}:$path');
            return true;
          },
    );

    await tester.pumpWidget(
      build(const ViewerStatus(state: ViewerState.streaming)),
    );
    await tester.pump();
    await tester.pumpWidget(
      build(
        const ViewerStatus(state: ViewerState.streaming, fileListAnswers: 1),
      ),
    );
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('多选'));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey<String>('file-select-all')));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey<String>('file-batch-delete')));
    await tester.pumpAndSettle();
    expect(find.text('删除选中的 2 项？'), findsOneWidget);
    await tester.tap(
      find.descendant(
        of: find.byType(AlertDialog),
        matching: find.widgetWithText(TextButton, '删除'),
      ),
    );
    await tester.pumpAndSettle();

    // One request is outstanding at a time, so a poll cannot merge answers.
    expect(ops, <String>['remove:photos']);
    expect(find.text('正在删除选中的 2 项…'), findsOneWidget);

    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          fileListAnswers: 1,
          fileOpAnswers: 1,
        ),
      ),
    );
    await tester.pump();
    expect(ops, <String>['remove:photos', 'remove:notes.txt']);

    // The last answer fails: one success, one failure, and a single reload.
    await tester.pumpWidget(
      build(
        const ViewerStatus(
          state: ViewerState.streaming,
          fileListAnswers: 1,
          fileOpAnswers: 2,
          fileOpCode: 1,
          fileOpPath: 'notes.txt',
          fileOpError: 'in use',
        ),
      ),
    );
    await tester.pump();
    expect(find.text('已删除 1 项，1 项失败：in use'), findsOneWidget);
    expect(requests, <String>['', '']);
    expect(find.text('已选 0 项'), findsNothing);
  });
}
