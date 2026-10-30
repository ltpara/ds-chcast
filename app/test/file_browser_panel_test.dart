import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';
import 'package:chast_viewer/widgets/file_browser_panel.dart';

ViewerFileEntry _file(String name, {int size = 0}) =>
    ViewerFileEntry(name: name, size: size, modifiedMs: 0, directory: false);

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

typedef _ListRequest = Future<bool> Function(String directory);
typedef _DownloadRequest = Future<bool> Function(String path);
typedef _TakeList = Future<ViewerFileListing?> Function();
typedef _UploadRequest = Future<int?> Function(String directory);
typedef _OperationRequest = Future<bool> Function(
    ViewerFileOperation op, String path, String newName);
typedef _FolderDownloadRequest = Future<bool> Function(String path);
typedef _DownloadEntriesRequest = Future<bool> Function(
    List<ViewerFileSelection> entries);

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
  ValueChanged<String>? onDirectoryChanged,
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
          onDirectoryChanged: onDirectoryChanged,
          onClose: onClose ?? () {},
        ),
      ),
    ),
  );
}

void main() {
  testWidgets('asks for the shared root on mount and adopts the listing',
      (WidgetTester tester) async {
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

    answer = _listing(
      '',
      <ViewerFileEntry>[
        _folder('photos'),
        _file('notes.txt', size: 1536),
      ],
      truncated: true,
    );
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

  testWidgets('a folder opens its own listing and the up row walks back',
      (WidgetTester tester) async {
    final List<String> requests = <String>[];
    ViewerFileListing? answer = _listing(
      '',
      <ViewerFileEntry>[_folder('photos'), _file('notes.txt')],
    );

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

    answer = _listing(
      'photos',
      <ViewerFileEntry>[_file('holiday.jpg', size: 2048)],
    );
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

  testWidgets('reports every accepted directory so drops follow the browser',
      (WidgetTester tester) async {
    final List<String> seen = <String>[];
    ViewerFileListing? answer = _listing(
      '',
      <ViewerFileEntry>[_folder('photos')],
    );

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

  testWidgets('a refused listing answer replaces the list with its reason',
      (WidgetTester tester) async {
    ViewerFileListing? answer = _listing(
      '',
      <ViewerFileEntry>[_file('notes.txt')],
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
    expect(find.text('notes.txt'), findsOneWidget);

    answer = _listing('private', <ViewerFileEntry>[],
        code: 1, reason: '主机未授权这个目录');
    await tester.pumpWidget(build(const ViewerStatus(fileListAnswers: 2)));
    await tester.pumpAndSettle();

    expect(find.text('主机未授权这个目录'), findsOneWidget);
  });

  testWidgets('a request that cannot be queued explains itself',
      (WidgetTester tester) async {
    await tester.pumpWidget(_host(
      status: const ViewerStatus(),
      onRequestList: (_) async => false,
    ));
    await tester.pumpAndSettle();

    expect(find.text('请求无法发送：会话未就绪或主机未授权文件传输'), findsOneWidget);
  });

  testWidgets('a refused download answer is shown under the list',
      (WidgetTester tester) async {
    ViewerFileListing? answer = _listing(
      '',
      <ViewerFileEntry>[_file('notes.txt')],
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

    await tester.pumpWidget(build(const ViewerStatus(
      fileListAnswers: 1,
      fileDownloadAnswers: 1,
      fileDownloadCode: 2,
      fileDownloadError: '主机正忙',
    )));
    await tester.pumpAndSettle();
    expect(find.text('主机正忙'), findsOneWidget);

    // An answer without a reason falls back to the translated default.
    await tester.pumpWidget(build(const ViewerStatus(
      fileListAnswers: 1,
      fileDownloadAnswers: 2,
      fileDownloadCode: 1,
    )));
    await tester.pumpAndSettle();
    expect(find.text('主机拒绝了下载请求'), findsOneWidget);
  });

  testWidgets('the footer follows the host-to-viewer download state',
      (WidgetTester tester) async {
    Widget build(ViewerStatus status) => _host(
          status: status,
          onRequestList: (_) async => true,
        );

    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      downloadPhase: ViewerFilePhase.sending,
      downloadName: 'x.bin',
      downloadSize: 2048,
      downloadReceived: 1024,
    )));
    await tester.pump();
    expect(find.text('正在下载 x.bin · 1.0 KiB / 2.0 KiB'), findsOneWidget);

    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      downloadPhase: ViewerFilePhase.done,
      downloadPath: r'C:\Users\me\Downloads\chast\x.bin',
    )));
    await tester.pump();
    expect(
      find.text(r'已保存到 C:\Users\me\Downloads\chast\x.bin'),
      findsOneWidget,
    );
  });

  testWidgets('the close button reports back', (WidgetTester tester) async {
    int closes = 0;
    await tester.pumpWidget(_host(
      status: const ViewerStatus(),
      onRequestList: (_) async => true,
      onClose: () => ++closes,
    ));
    await tester.pump();

    await tester.tap(find.byTooltip('关闭文件浏览'));
    expect(closes, 1);
  });

  testWidgets('upload targets the browsed folder and refreshes when it ends',
      (WidgetTester tester) async {
    final List<String> requests = <String>[];
    final List<String> uploads = <String>[];
    ViewerFileListing? answer = _listing(
      '',
      <ViewerFileEntry>[_folder('photos')],
    );

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

    await tester.pumpWidget(build(const ViewerStatus(state: ViewerState.streaming)));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 1,
    )));
    await tester.pumpAndSettle();

    await tester.tap(find.text('photos'));
    await tester.pump();
    answer = _listing('photos', <ViewerFileEntry>[]);
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 2,
    )));
    await tester.pumpAndSettle();
    expect(requests, <String>['', 'photos']);

    // The button hands the current directory to the bridge.
    await tester.tap(find.byTooltip('上传文件到此处'));
    await tester.pump();
    expect(uploads, <String>['photos']);

    // While the queue runs the footer names the target and the queue length.
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 2,
      filePhase: ViewerFilePhase.sending,
      fileQueue: 1,
      fileName: 'up.txt',
    )));
    await tester.pump();
    expect(find.text('正在上传到 photos（队列剩余 1 个）'), findsOneWidget);

    // The queue draining refreshes the listing, which then shows the file.
    answer = _listing('photos', <ViewerFileEntry>[_file('up.txt', size: 5)]);
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 2,
      filePhase: ViewerFilePhase.done,
      lastFileName: 'up.txt',
    )));
    await tester.pump();
    expect(requests, <String>['', 'photos', 'photos']);
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 3,
      filePhase: ViewerFilePhase.done,
      lastFileName: 'up.txt',
    )));
    await tester.pumpAndSettle();
    expect(find.text('up.txt'), findsOneWidget);
    expect(find.text('上传完成，列表已刷新'), findsOneWidget);
  });

  testWidgets('a fast upload is noticed through the finished entry',
      (WidgetTester tester) async {
    final List<String> requests = <String>[];
    Widget build(ViewerStatus status) => _host(
          status: status,
          onRequestList: (String directory) async {
            requests.add(directory);
            return true;
          },
          onUpload: (_) async => 1,
        );

    await tester.pumpWidget(build(const ViewerStatus(state: ViewerState.streaming)));
    await tester.pump();
    expect(requests, <String>['']);

    await tester.tap(find.byTooltip('上传文件到此处'));
    await tester.pump();

    // No poll ever saw the queue active; the finished entry still refreshes.
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      filePhase: ViewerFilePhase.done,
      lastFileName: 'tiny.txt',
    )));
    await tester.pump();
    expect(requests, <String>['', '']);
    expect(find.text('上传完成，列表已刷新'), findsOneWidget);
  });

  testWidgets('a failed upload shows the host reason', (WidgetTester tester) async {
    Widget build(ViewerStatus status) => _host(
          status: status,
          onRequestList: (_) async => true,
          onUpload: (_) async => 1,
        );

    await tester.pumpWidget(build(const ViewerStatus(state: ViewerState.streaming)));
    await tester.pump();
    await tester.tap(find.byTooltip('上传文件到此处'));
    await tester.pump();

    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      filePhase: ViewerFilePhase.failed,
      lastFileName: 'big.bin',
      lastFileError: 'the host is busy',
    )));
    await tester.pump();
    expect(find.text('上传失败：the host is busy'), findsOneWidget);
  });

  testWidgets('a cancelled picker changes nothing', (WidgetTester tester) async {
    final List<String> requests = <String>[];
    await tester.pumpWidget(_host(
      status: const ViewerStatus(),
      onRequestList: (String directory) async {
        requests.add(directory);
        return true;
      },
      onUpload: (_) async => null,
    ));
    await tester.pump();

    await tester.tap(find.byTooltip('上传文件到此处'));
    await tester.pump();
    expect(requests, <String>['']);
    expect(
      find.text('点击文件右侧的下载按钮保存到本机；多选可批量下载或删除；标题栏的上传按钮会把文件传到当前文件夹'),
      findsOneWidget,
    );
  });

  testWidgets('the new-folder button names a folder below the current one',
      (WidgetTester tester) async {
    final List<String> requests = <String>[];
    final List<String> ops = <String>[];
    ViewerFileListing? answer =
        _listing('', <ViewerFileEntry>[_file('notes.txt', size: 5)]);

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
        build(const ViewerStatus(state: ViewerState.streaming)));
    await tester.pump();
    expect(requests, <String>['']);
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 1,
    )));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('新建文件夹'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byType(TextField), 'docs');
    await tester.tap(find.widgetWithText(TextButton, '创建'));
    await tester.pumpAndSettle();
    expect(ops, <String>['makeDirectory:docs:']);

    // A successful answer reloads the current directory.
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileOpAnswers: 1,
      fileOpPath: 'docs',
    )));
    await tester.pump();
    expect(requests, <String>['', '']);
    expect(find.text('操作完成，列表已刷新'), findsOneWidget);

    // A refusal keeps the host's reason and does not reload.
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileOpAnswers: 2,
      fileOpCode: 1,
      fileOpPath: 'docs',
      fileOpError: 'already exists',
    )));
    await tester.pump();
    expect(find.text('already exists'), findsOneWidget);
    expect(requests, <String>['', '']);
  });

  testWidgets('a row menu renames the entry', (WidgetTester tester) async {
    final List<String> ops = <String>[];
    ViewerFileListing? answer =
        _listing('', <ViewerFileEntry>[_file('notes.txt', size: 5)]);

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
        build(const ViewerStatus(state: ViewerState.streaming)));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 1,
    )));
    await tester.pumpAndSettle();
    expect(find.text('notes.txt'), findsOneWidget);

    await tester.tap(find.byTooltip('更多操作'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('重命名'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byType(TextField), 'renamed.txt');
    await tester.tap(find.widgetWithText(TextButton, '重命名'));
    await tester.pumpAndSettle();
    expect(ops, <String>['rename:notes.txt:renamed.txt']);
  });

  testWidgets('deleting asks first and shows the host refusal',
      (WidgetTester tester) async {
    final List<String> ops = <String>[];
    ViewerFileListing? answer =
        _listing('', <ViewerFileEntry>[_folder('photos')]);

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
        build(const ViewerStatus(state: ViewerState.streaming)));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 1,
    )));
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

    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 1,
      fileOpAnswers: 1,
      fileOpCode: 2,
      fileOpPath: 'photos',
      fileOpError: '主机正忙',
    )));
    await tester.pump();
    expect(find.text('主机正忙'), findsOneWidget);
  });

  testWidgets('a folder row queues a recursive download and can be cancelled',
      (WidgetTester tester) async {
    final List<String> folders = <String>[];
    int cancels = 0;
    ViewerFileListing? answer =
        _listing('', <ViewerFileEntry>[_folder('photos'), _file('notes.txt')]);

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
        build(const ViewerStatus(state: ViewerState.streaming)));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 1,
    )));
    await tester.pumpAndSettle();

    // The folder row and the file row carry distinct download tooltips.
    await tester.tap(find.byTooltip('下载文件夹到本机'));
    await tester.pump();
    expect(folders, <String>['photos']);

    // While the walk runs the footer names the folder and its progress, and
    // the cancel button reaches the viewer.
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 1,
      folderDownloadActive: true,
      folderDownloadName: 'photos',
      folderDownloadFilesTotal: 3,
      folderDownloadFilesDone: 1,
      folderDownloadCurrent: 'photos/raw/a.jpg',
    )));
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

  testWidgets('a batch walk words its progress as selected entries',
      (WidgetTester tester) async {
    Widget build(ViewerStatus status) => _host(
          status: status,
          onRequestList: (_) async => true,
        );

    await tester.pumpWidget(
        build(const ViewerStatus(state: ViewerState.streaming)));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      folderDownloadActive: true,
      folderDownloadBatch: true,
      folderDownloadRoots: 3,
      folderDownloadFilesTotal: 2,
      folderDownloadFilesDone: 1,
    )));
    await tester.pump();
    expect(find.text('正在下载选中的 3 项 · 1/2 个文件'), findsOneWidget);
    expect(find.text('正在下载文件夹'), findsNothing);
  });

  testWidgets('folder download answers report done, cancel and failure',
      (WidgetTester tester) async {
    Widget build(ViewerStatus status) => _host(
          status: status,
          onRequestList: (_) async => true,
        );

    await tester.pumpWidget(
        build(const ViewerStatus(state: ViewerState.streaming)));
    await tester.pump();

    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      folderDownloadAnswers: 1,
      folderDownloadDestination: r'C:\Users\me\Downloads\chast\photos',
    )));
    await tester.pump();
    expect(
      find.text(r'文件夹已下载到 C:\Users\me\Downloads\chast\photos'),
      findsOneWidget,
    );

    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      folderDownloadAnswers: 2,
      folderDownloadCode: 2,
    )));
    await tester.pump();
    expect(find.text('已取消文件夹下载'), findsOneWidget);

    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      folderDownloadAnswers: 3,
      folderDownloadCode: 1,
      folderDownloadError: 'disk full',
    )));
    await tester.pump();
    expect(find.text('文件夹下载失败：disk full'), findsOneWidget);

    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      folderDownloadAnswers: 4,
      folderDownloadCode: 1,
    )));
    await tester.pump();
    expect(find.text('文件夹下载失败'), findsOneWidget);

    // A batch walk names the selection size instead of a single folder.
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      folderDownloadAnswers: 5,
      folderDownloadDestination: r'C:\Users\me\Downloads\chast',
      folderDownloadRoots: 3,
      folderDownloadBatch: true,
    )));
    await tester.pump();
    expect(
      find.text(r'选中的 3 项已下载到 C:\Users\me\Downloads\chast'),
      findsOneWidget,
    );

    // A batch can hold one file; the flag keeps the wording from turning into
    // a folder download.
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      folderDownloadAnswers: 6,
      folderDownloadDestination: r'C:\Users\me\Downloads\chast',
      folderDownloadRoots: 1,
      folderDownloadBatch: true,
    )));
    await tester.pump();
    expect(
      find.text(r'选中的 1 项已下载到 C:\Users\me\Downloads\chast'),
      findsOneWidget,
    );
  });

  testWidgets('a folder download the viewer will not take explains itself',
      (WidgetTester tester) async {
    ViewerFileListing? answer =
        _listing('', <ViewerFileEntry>[_folder('photos')]);

    Widget build(ViewerStatus status) => _host(
          status: status,
          onRequestList: (_) async => true,
          onTakeList: () async => answer,
          onDownloadFolder: (_) async => false,
        );

    await tester.pumpWidget(
        build(const ViewerStatus(state: ViewerState.streaming)));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 1,
    )));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('下载文件夹到本机'));
    await tester.pump();
    expect(find.text('请求无法发送：会话未就绪或主机未授权文件传输'), findsOneWidget);
  });

  testWidgets('multi-select batches downloads with directory flags',
      (WidgetTester tester) async {
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
        build(const ViewerStatus(state: ViewerState.streaming)));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 1,
    )));
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
    expect(find.text('已选 3 项'), findsOneWidget);
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
          .map((ViewerFileSelection entry) => '${entry.directory}:${entry.path}')
          .toList(),
      <String>['true:photos', 'false:notes.txt'],
    );
    // A queued batch leaves selection mode; progress arrives in the footer.
    expect(find.text('已选 0 项'), findsNothing);
    expect(find.byTooltip('多选'), findsOneWidget);
  });

  testWidgets('batch delete removes one entry at a time and sums failures',
      (WidgetTester tester) async {
    final List<String> ops = <String>[];
    final List<String> requests = <String>[];
    ViewerFileListing? answer = _listing(
      '',
      <ViewerFileEntry>[_folder('photos'), _file('notes.txt')],
    );

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
        build(const ViewerStatus(state: ViewerState.streaming)));
    await tester.pump();
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 1,
    )));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('多选'));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey<String>('file-select-all')));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey<String>('file-batch-delete')));
    await tester.pumpAndSettle();
    expect(find.text('删除选中的 2 项？'), findsOneWidget);
    await tester.tap(find.descendant(
      of: find.byType(AlertDialog),
      matching: find.widgetWithText(TextButton, '删除'),
    ));
    await tester.pumpAndSettle();

    // One request is outstanding at a time, so a poll cannot merge answers.
    expect(ops, <String>['remove:photos']);
    expect(find.text('正在删除选中的 2 项…'), findsOneWidget);

    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 1,
      fileOpAnswers: 1,
    )));
    await tester.pump();
    expect(ops, <String>['remove:photos', 'remove:notes.txt']);

    // The last answer fails: one success, one failure, and a single reload.
    await tester.pumpWidget(build(const ViewerStatus(
      state: ViewerState.streaming,
      fileListAnswers: 1,
      fileOpAnswers: 2,
      fileOpCode: 1,
      fileOpPath: 'notes.txt',
      fileOpError: 'in use',
    )));
    await tester.pump();
    expect(find.text('已删除 1 项，1 项失败：in use'), findsOneWidget);
    expect(requests, <String>['', '']);
    expect(find.text('已选 0 项'), findsNothing);
  });
}
