import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../theme.dart';
import '../viewer_bridge.dart';
import 'package:chast_viewer/l10n.dart';

// How the current listing is ordered locally. Directories always stay ahead
// of files, so this only ranks within each group. Public so the session page
// can remember the choice for the next open.
enum FileBrowserSort { name, size, modified }

// Reports a sort change to the page; `ascending` is the new direction.
typedef FileBrowserSortChanged =
    void Function(FileBrowserSort sort, bool ascending);

/// Modeless browser for the host's shared folder. The page owns the transport
/// (request, download, listing pop); the panel keeps the current directory,
/// the entries it last received and the download state it was shown.
class FileBrowserPanel extends StatefulWidget {
  const FileBrowserPanel({
    super.key,
    required this.status,
    required this.onRequestList,
    required this.onRequestDownload,
    required this.onTakeList,
    required this.onUpload,
    required this.onFileOperation,
    required this.onDownloadFolder,
    required this.onDownloadEntries,
    required this.onCancelFolderDownload,
    required this.onCancelUpload,
    required this.onCancelDownload,
    required this.onClose,
    this.onDirectoryChanged,
    this.onRevealDownload,
    this.onOpenDownload,
    this.initialDirectory = '',
    this.initialSort = FileBrowserSort.name,
    this.initialSortAscending = true,
    this.initialShowHidden = false,
    this.onSortChanged,
    this.onShowHiddenChanged,
    this.width = 460,
    this.height = 480,
  });

  final ViewerStatus status;
  // The panel's outer size. The session overlay uses the compact default; a
  // file-transfer-only session gives the browser most of the window.
  final double width;
  final double height;
  final Future<bool> Function(
    String directory, [
    String cursor,
    bool showHidden,
  ])
  onRequestList;
  final Future<bool> Function(String path) onRequestDownload;
  final Future<ViewerFileListing?> Function() onTakeList;
  // Queues the recursive download of one folder; false means the viewer could
  // not accept it (no session, permission revoked or another walk running).
  // The walk lists the same hidden-entry view the browser currently shows.
  final Future<bool> Function(String path, [bool showHidden])
  onDownloadFolder;
  // Queues one walk over the selected entries: folders mirror below their own
  // names, files land at the top of the receive directory. False means the
  // viewer could not accept it (no session, permission revoked or another
  // walk running). The walk lists the same hidden-entry view as the browser.
  final Future<bool> Function(
    List<ViewerFileSelection> entries, [
    bool showHidden,
  ])
  onDownloadEntries;
  final Future<void> Function() onCancelFolderDownload;
  // Drops the ordinary uploads still queued or in flight; clipboard uploads
  // keep going.
  final Future<void> Function() onCancelUpload;
  // Stops the single-file download in progress and removes its partial file;
  // folder-walk and clipboard transfers keep going.
  final Future<void> Function() onCancelDownload;
  // Reports the directory whose listing was accepted, so dropped files can
  // follow the browser instead of always landing in the shared root.
  final ValueChanged<String>? onDirectoryChanged;
  // Opens the shell on a finished download. Tests inject a recorder; the
  // default starts explorer.exe with the file selected.
  final Future<void> Function(String path)? onRevealDownload;
  // Opens a finished download with its default application; a folder opens in
  // Explorer. Tests inject a recorder.
  final Future<void> Function(String path)? onOpenDownload;
  // The first directory to list. The session page passes the folder the last
  // browser showed so reopening does not jump back to the shared root.
  final String initialDirectory;
  // The ordering to start with. The session page passes the last choice so
  // reopening keeps it; `onSortChanged` updates that memory.
  final FileBrowserSort initialSort;
  final bool initialSortAscending;
  // Whether hidden entries are listed. The session page passes the last
  // choice so reopening keeps it; `onShowHiddenChanged` updates that memory.
  final bool initialShowHidden;
  final FileBrowserSortChanged? onSortChanged;
  final ValueChanged<bool>? onShowHiddenChanged;
  // Opens the local file picker and queues the chosen files into the shared
  // directory argument. Returns how many roots were queued; null means the
  // picker was cancelled.
  final Future<int?> Function(String directory) onUpload;
  // Queues one new-folder, rename or remove request. `path` is relative to
  // the shared root; `newName` is only used by rename. False means the
  // request could not be queued.
  final Future<bool> Function(
    ViewerFileOperation op,
    String path,
    String newName,
  )
  onFileOperation;
  final VoidCallback onClose;

  @override
  State<FileBrowserPanel> createState() => _FileBrowserPanelState();
}

class _FileBrowserPanelState extends State<FileBrowserPanel> {
  String _directory = '';
  List<ViewerFileEntry> _entries = const <ViewerFileEntry>[];
  bool _loading = true;
  bool _truncated = false;
  String _nextCursor = '';
  String _requestedDirectory = '';
  String _requestedCursor = '';
  bool _requestedShowHidden = false;
  String _retryCursor = '';
  int _requestEpoch = 0;
  String? _error;
  int _takenListAnswers = 0;
  int _takenDownloadAnswers = 0;
  // Set while the uploads queued from the panel are still running; the first
  // status poll that returns to idle then refreshes the listing once.
  bool _uploadAwaitingEnd = false;
  bool _uploadWasActive = false;
  // Set between a cancel request and the status that stops reporting the
  // upload, so the progress row does not reappear under the cancel wording.
  bool _uploadCancelPending = false;
  String _uploadBaselineLast = '';
  String? _uploadResult;
  bool _uploadResultFailed = false;
  // Set between a download cancel request and the status that stops reporting
  // the download; the cancel wording replaces the progress row meanwhile.
  bool _downloadCancelPending = false;
  String? _downloadResult;
  // The newest host answer to a new-folder/rename/remove request. A success
  // reloads the listing; a refusal keeps the host's reason in the footer.
  int _takenOpAnswers = 0;
  String? _opResult;
  bool _opResultFailed = false;
  // The newest answer to a folder-download walk; remains in the footer until
  // the next walk starts.
  int _takenFolderAnswers = 0;
  String? _folderResult;
  bool _folderResultFailed = false;
  // Multi-select: rows are checked by name and the batch buttons act on them.
  // A new listing, a navigation or a finished batch clears the selection.
  bool _selecting = false;
  final Set<String> _selected = <String>{};
  // Batches remove entries one request at a time: only the outstanding answer
  // can arrive before the next request is sent, so a status poll never merges
  // two results into one.
  bool _batchDeleting = false;
  int _batchTotal = 0;
  int _batchFailures = 0;
  String _batchFirstError = '';
  // Each entry keeps whether it is a folder, because a folder is removed
  // together with its contents on the host.
  final List<MapEntry<String, bool>> _deleteQueue = <MapEntry<String, bool>>[];

  // Client-side name filter over the current listing. Cleared when the
  // accepted listing names a different directory, so entering a folder never
  // lands on a stale filter from its parent.
  final TextEditingController _filterController = TextEditingController();
  String _filter = '';
  // A saved initial folder may have been removed on the host. The first
  // refusal retries the shared root once instead of leaving an empty pane.
  bool _initialFallbackTried = false;
  // Local ordering of the accepted listing; the host always sends directories
  // first by name, and this only reshuffles what the page shows.
  FileBrowserSort _sort = FileBrowserSort.name;
  bool _sortAscending = true;
  // Hidden entries are omitted by default, like RustDesk's remote browser.
  // The toggle reloads the current folder from its first page.
  bool _showHidden = false;

  List<ViewerFileEntry> get _visibleEntries {
    final String needle = _filter.toLowerCase();
    final List<ViewerFileEntry> entries = needle.isEmpty
        ? List<ViewerFileEntry>.of(_entries)
        : _entries
              .where(
                (ViewerFileEntry entry) =>
                    entry.name.toLowerCase().contains(needle),
              )
              .toList();
    // Directories stay first whatever the ranking; the default (name,
    // ascending) reproduces the host's order, so an untouched browser does
    // not move rows around.
    entries.sort((ViewerFileEntry a, ViewerFileEntry b) {
      if (a.directory != b.directory) {
        return a.directory ? -1 : 1;
      }
      int result;
      if (_sort == FileBrowserSort.size) {
        result = a.size.compareTo(b.size);
      } else if (_sort == FileBrowserSort.modified) {
        result = a.modifiedMs.compareTo(b.modifiedMs);
      } else {
        result = a.name.toLowerCase().compareTo(b.name.toLowerCase());
      }
      if (!_sortAscending) {
        result = -result;
      }
      // The name breaks ties so equal sizes or times keep a stable order even
      // though List.sort is not stable.
      return result != 0
          ? result
          : a.name.toLowerCase().compareTo(b.name.toLowerCase());
    });
    return entries;
  }

  bool get _allVisibleSelected =>
      _visibleEntries.isNotEmpty &&
      _visibleEntries.every(
        (ViewerFileEntry entry) => _selected.contains(entry.name),
      );

  @override
  void dispose() {
    _filterController.dispose();
    super.dispose();
  }

  @override
  void initState() {
    super.initState();
    _takenListAnswers = widget.status.fileListAnswers;
    _takenDownloadAnswers = widget.status.fileDownloadAnswers;
    _takenOpAnswers = widget.status.fileOpAnswers;
    _takenFolderAnswers = widget.status.folderDownloadAnswers;
    _sort = widget.initialSort;
    _sortAscending = widget.initialSortAscending;
    _showHidden = widget.initialShowHidden;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) {
        unawaited(_request(widget.initialDirectory));
      }
    });
  }

  @override
  void didUpdateWidget(FileBrowserPanel oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.status.fileListAnswers != _takenListAnswers) {
      unawaited(_takeAnswer());
    }
    if (widget.status.fileDownloadAnswers != _takenDownloadAnswers) {
      _takenDownloadAnswers = widget.status.fileDownloadAnswers;
      if (widget.status.fileDownloadCode != 0) {
        final String reason = widget.status.fileDownloadError;
        _error = reason.isEmpty ? tr('主机拒绝了下载请求') : reason;
      } else {
        _error = null;
        // A freshly accepted request hides the previous cancel wording.
        _downloadResult = null;
      }
    }
    if (widget.status.fileOpAnswers != _takenOpAnswers) {
      _takenOpAnswers = widget.status.fileOpAnswers;
      final bool ok = widget.status.fileOpCode == 0;
      final String reason = widget.status.fileOpError;
      if (_batchDeleting) {
        if (!ok) {
          ++_batchFailures;
          if (_batchFirstError.isEmpty) {
            _batchFirstError = reason.isEmpty ? tr('主机拒绝了操作') : reason;
          }
        }
        _sendNextDelete();
      } else {
        _opResultFailed = !ok;
        _opResult = ok
            ? tr('操作完成，列表已刷新')
            : (reason.isEmpty ? tr('主机拒绝了操作') : reason);
        if (ok) {
          unawaited(_request(_directory));
        }
      }
    }
    if (widget.status.folderDownloadAnswers != _takenFolderAnswers) {
      _takenFolderAnswers = widget.status.folderDownloadAnswers;
      final int code = widget.status.folderDownloadCode;
      if (code == 0) {
        _folderResultFailed = false;
        _folderResult = widget.status.folderDownloadBatch
            ? tr('选中的 {n} 项已下载到 {path}', <String, Object?>{
                'n': widget.status.folderDownloadRoots,
                'path': widget.status.folderDownloadDestination,
              })
            : tr('文件夹已下载到 {path}', <String, Object?>{
                'path': widget.status.folderDownloadDestination,
              });
      } else if (code == 2) {
        _folderResultFailed = false;
        _folderResult = tr('已取消文件夹下载');
      } else {
        final String reason = widget.status.folderDownloadError;
        _folderResultFailed = true;
        _folderResult = reason.isEmpty
            ? tr('文件夹下载失败')
            : tr('文件夹下载失败：{reason}', <String, Object?>{'reason': reason});
      }
    }
    final bool uploadActive =
        widget.status.fileActive || widget.status.fileQueue > 0;
    if (uploadActive) {
      // A new upload hides the previous outcome. Drops start uploads without
      // passing through _upload(), so the clear belongs here.
      if (!_uploadCancelPending) {
        _uploadResult = null;
      }
      if (_uploadAwaitingEnd) {
        _uploadWasActive = true;
      }
    } else {
      _uploadCancelPending = false;
      if (_uploadAwaitingEnd) {
        // A tiny file can finish before the first poll shows it as active; the
        // finished entry then only shows up in lastFileName.
        final bool finished =
            widget.status.lastFileName.isNotEmpty &&
            widget.status.lastFileName != _uploadBaselineLast;
        if (_uploadWasActive || finished) {
          _finishUpload();
        }
      }
    }
    if (widget.status.downloadActive) {
      // A new download hides the previous cancel wording, unless the cancel
      // has not landed on the status yet.
      if (!_downloadCancelPending) {
        _downloadResult = null;
      }
    } else {
      _downloadCancelPending = false;
    }
  }

  void _finishUpload() {
    _uploadAwaitingEnd = false;
    _uploadWasActive = false;
    final String error = widget.status.lastFileError;
    _uploadResultFailed = error.isNotEmpty;
    _uploadResult = _uploadResultFailed
        ? tr('上传失败：{reason}', <String, Object?>{'reason': error})
        : tr('上传完成，列表已刷新');
    unawaited(_request(_directory));
  }

  Future<void> _upload() async {
    int? count;
    bool failed = false;
    try {
      count = await widget.onUpload(_directory);
    } on PlatformException {
      failed = true;
    } on MissingPluginException {
      failed = true;
    }
    if (!mounted) {
      return;
    }
    if (failed) {
      setState(() => _error = tr('请求无法发送：会话未就绪或主机未授权文件传输'));
      return;
    }
    if (count == null) {
      return; // The picker was cancelled; nothing was queued.
    }
    setState(() {
      _error = null;
      _uploadResult = null;
      _uploadResultFailed = false;
      _uploadCancelPending = false;
      _uploadAwaitingEnd = true;
      _uploadWasActive = false;
      _uploadBaselineLast = widget.status.lastFileName;
    });
  }

  Future<void> _request(String directory, [String cursor = '']) async {
    if (cursor.isNotEmpty &&
        (_loading || directory != _directory || cursor != _nextCursor)) {
      return;
    }
    final int epoch = ++_requestEpoch;
    _requestedDirectory = directory;
    _requestedCursor = cursor;
    _requestedShowHidden = _showHidden;
    _retryCursor = '';
    setState(() {
      _loading = true;
      _error = null;
    });
    bool queued = false;
    try {
      queued = await widget.onRequestList(directory, cursor, _showHidden);
    } on PlatformException {
      queued = false;
    } on MissingPluginException {
      queued = false;
    }
    if (!mounted || epoch != _requestEpoch) {
      return;
    }
    if (!queued) {
      setState(() {
        _retryCursor = cursor;
        _loading = false;
        _error = tr('请求无法发送：会话未就绪或主机未授权文件传输');
      });
    }
  }

  Future<void> _takeAnswer() async {
    _takenListAnswers = widget.status.fileListAnswers;
    final int epoch = _requestEpoch;
    ViewerFileListing? listing;
    try {
      listing = await widget.onTakeList();
    } on PlatformException {
      listing = null;
    } on MissingPluginException {
      listing = null;
    }
    if (!mounted || epoch != _requestEpoch) {
      return;
    }
    if (listing == null) {
      return;
    }
    final ViewerFileListing answer = listing;
    if (answer.directory != _requestedDirectory ||
        answer.cursor != _requestedCursor ||
        answer.showHidden != _requestedShowHidden) {
      return;
    }
    final bool append =
        answer.cursor.isNotEmpty && answer.directory == _directory;
    final bool directoryChanged = answer.directory != _directory;
    bool retryRoot = false;
    setState(() {
      _loading = false;
      if (answer.ok) {
        if (directoryChanged) {
          _directory = answer.directory;
          _filter = '';
          _filterController.clear();
          widget.onDirectoryChanged?.call(_directory);
          _selected.clear();
        } else if (!append) {
          // Uploads, renames and the refresh button re-request the same
          // directory; a pending batch selection should survive that, but a
          // name that vanished from the new listing must not linger.
          _selected.retainAll(
            answer.entries.map((ViewerFileEntry entry) => entry.name),
          );
        }
        if (append) {
          final Map<String, ViewerFileEntry> combined =
              <String, ViewerFileEntry>{
                for (final ViewerFileEntry entry in _entries) entry.name: entry,
                for (final ViewerFileEntry entry in answer.entries)
                  entry.name: entry,
              };
          _entries = combined.values.toList();
        } else {
          _entries = answer.entries;
        }
        _truncated = answer.truncated;
        _nextCursor = answer.nextCursor;
        _error = null;
      } else if (_directory.isEmpty &&
          widget.initialDirectory.isNotEmpty &&
          !_initialFallbackTried) {
        // The folder that was open when the browser last closed is gone or
        // no longer readable; start at the shared root instead.
        _initialFallbackTried = true;
        retryRoot = true;
        _error = null;
        _loading = true;
      } else {
        _retryCursor = append ? answer.cursor : '';
        _error = answer.reason.isEmpty ? tr('主机拒绝列出这个目录') : answer.reason;
      }
    });
    if (retryRoot) {
      widget.onDirectoryChanged?.call('');
      unawaited(_request(''));
    }
  }

  Future<void> _download(ViewerFileEntry entry) async {
    final String path = _directory.isEmpty
        ? entry.name
        : '$_directory/${entry.name}';
    bool queued = false;
    try {
      queued = await widget.onRequestDownload(path);
    } on PlatformException {
      queued = false;
    } on MissingPluginException {
      queued = false;
    }
    if (!mounted) {
      return;
    }
    setState(() {
      _error = queued ? null : tr('请求无法发送：会话未就绪或主机未授权文件传输');
      if (queued) {
        _folderResult = null;
        _folderResultFailed = false;
      }
    });
  }

  String _join(String name) => _directory.isEmpty ? name : '$_directory/$name';

  Future<void> _downloadFolder(ViewerFileEntry entry) async {
    bool queued = false;
    try {
      queued = await widget.onDownloadFolder(_join(entry.name), _showHidden);
    } on PlatformException {
      queued = false;
    } on MissingPluginException {
      queued = false;
    }
    if (!mounted) {
      return;
    }
    setState(() {
      _error = queued ? null : tr('请求无法发送：会话未就绪或主机未授权文件传输');
      if (queued) {
        _folderResult = null;
        _folderResultFailed = false;
      }
    });
  }

  Future<void> _cancelFolderDownload() async {
    bool failed = false;
    try {
      await widget.onCancelFolderDownload();
    } on PlatformException {
      failed = true;
    } on MissingPluginException {
      failed = true;
    }
    if (!mounted || !failed) {
      return;
    }
    setState(() => _error = tr('请求无法发送：会话未就绪或主机未授权文件传输'));
  }

  Future<void> _cancelUpload() async {
    bool failed = false;
    try {
      await widget.onCancelUpload();
    } on PlatformException {
      failed = true;
    } on MissingPluginException {
      failed = true;
    }
    if (!mounted) {
      return;
    }
    if (failed) {
      setState(() => _error = tr('请求无法发送：会话未就绪或主机未授权文件传输'));
      return;
    }
    setState(() {
      _uploadCancelPending = true;
      _uploadAwaitingEnd = false;
      _uploadWasActive = false;
      _uploadResultFailed = false;
      _uploadResult = tr('已取消上传');
    });
    unawaited(_request(_directory));
  }

  Future<void> _cancelDownload() async {
    bool failed = false;
    try {
      await widget.onCancelDownload();
    } on PlatformException {
      failed = true;
    } on MissingPluginException {
      failed = true;
    }
    if (!mounted) {
      return;
    }
    if (failed) {
      setState(() => _error = tr('请求无法发送：会话未就绪或主机未授权文件传输'));
      return;
    }
    setState(() {
      _downloadCancelPending = true;
      _downloadResult = tr('已取消下载');
    });
  }

  // A reveal is a convenience, not a transfer step: a shell that cannot start
  // must not replace the "saved" footer with an error.
  Future<void> _revealDownload(String path) async {
    final Future<void> Function(String path)? injected =
        widget.onRevealDownload;
    if (injected != null) {
      await injected(path);
      return;
    }
    try {
      await Process.start('explorer.exe', <String>['/select,', path]);
    } on ProcessException {
      // Explorer is unavailable; the footer still names the saved file.
    }
  }

  // Opening is a convenience like revealing: a shell that cannot start must
  // not replace the "saved" footer with an error.
  Future<void> _openDownload(String path) async {
    final Future<void> Function(String path)? injected = widget.onOpenDownload;
    if (injected != null) {
      await injected(path);
      return;
    }
    try {
      // Explorer opens a file with its default application and a directory in
      // a window; passing the path as data avoids a command shell.
      await Process.start('explorer.exe', <String>[path]);
    } on ProcessException {
      // Explorer is unavailable; the footer still names the saved path.
    }
  }

  Future<void> _copyPath(String path) async {
    bool copied = true;
    try {
      await Clipboard.setData(ClipboardData(text: path));
    } catch (_) {
      copied = false;
    }
    if (!mounted) {
      return;
    }
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(copied ? tr('路径已复制') : tr('复制失败')),
        duration: const Duration(seconds: 2),
      ),
    );
  }

  Future<void> _copySelectedPaths() async {
    // Host order, one per line; the batch buttons act on the same set.
    final List<String> paths = <String>[];
    for (final ViewerFileEntry entry in _entries) {
      if (_selected.contains(entry.name)) {
        paths.add(_join(entry.name));
      }
    }
    if (paths.isEmpty) {
      return;
    }
    await _copyPath(paths.join('\n'));
  }

  void _toggleSelection(String name) {
    setState(() {
      if (!_selected.add(name)) {
        _selected.remove(name);
      }
    });
  }

  void _toggleSelectAll() {
    setState(() {
      final List<ViewerFileEntry> visible = _visibleEntries;
      if (_allVisibleSelected) {
        for (final ViewerFileEntry entry in visible) {
          _selected.remove(entry.name);
        }
      } else {
        _selected.addAll(visible.map((ViewerFileEntry entry) => entry.name));
      }
    });
  }

  List<ViewerFileSelection> _selection() => <ViewerFileSelection>[
    for (final ViewerFileEntry entry in _entries)
      if (_selected.contains(entry.name))
        ViewerFileSelection(
          path: _join(entry.name),
          directory: entry.directory,
        ),
  ];

  Future<void> _downloadSelected() async {
    final List<ViewerFileSelection> entries = _selection();
    if (entries.isEmpty) {
      return;
    }
    bool queued = false;
    try {
      queued = await widget.onDownloadEntries(entries, _showHidden);
    } on PlatformException {
      queued = false;
    } on MissingPluginException {
      queued = false;
    }
    if (!mounted) {
      return;
    }
    setState(() {
      _error = queued ? null : tr('请求无法发送：会话未就绪或主机未授权文件传输');
      if (queued) {
        _folderResult = null;
        _folderResultFailed = false;
        _selected.clear();
        _selecting = false;
      }
    });
  }

  Future<void> _deleteSelected() async {
    final List<MapEntry<String, bool>> targets = <MapEntry<String, bool>>[
      for (final ViewerFileEntry entry in _entries)
        if (_selected.contains(entry.name))
          MapEntry<String, bool>(_join(entry.name), entry.directory),
    ];
    if (targets.isEmpty || _batchDeleting) {
      return;
    }
    final bool hasFolder = targets.any(
      (MapEntry<String, bool> target) => target.value,
    );
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) => AlertDialog(
        backgroundColor: kPanelDark,
        title: Text(
          tr('删除选中的 {n} 项？', <String, Object?>{'n': targets.length}),
          style: const TextStyle(fontSize: 13.5),
        ),
        content: Text(
          hasFolder ? tr('选中的文件夹及其内容将被删除，此操作不可撤销。') : tr('此操作不可撤销。'),
          style: const TextStyle(fontSize: 11.5),
        ),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: Text(tr('取消')),
          ),
          TextButton(
            onPressed: () => Navigator.of(context).pop(true),
            child: Text(tr('删除')),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) {
      return;
    }
    setState(() {
      _batchDeleting = true;
      _batchTotal = targets.length;
      _batchFailures = 0;
      _batchFirstError = '';
      _opResult = null;
      _opResultFailed = false;
      _deleteQueue
        ..clear()
        ..addAll(targets.skip(1));
    });
    final bool queued = await _sendRemove(targets.first);
    if (!mounted || !_batchDeleting) {
      return;
    }
    if (!queued) {
      ++_batchFailures;
      _batchFirstError = tr('请求无法发送：会话未就绪或主机未授权文件传输');
      _deleteQueue.clear();
      _finishBatchDelete();
    }
  }

  Future<bool> _sendRemove(MapEntry<String, bool> target) async {
    try {
      return await widget.onFileOperation(
        target.value
            ? ViewerFileOperation.removeTree
            : ViewerFileOperation.remove,
        target.key,
        '',
      );
    } on PlatformException {
      return false;
    } on MissingPluginException {
      return false;
    }
  }

  void _sendNextDelete() {
    if (_deleteQueue.isEmpty) {
      _finishBatchDelete();
      return;
    }
    final MapEntry<String, bool> target = _deleteQueue.removeAt(0);
    unawaited(
      _sendRemove(target).then((bool queued) {
        if (!mounted || !_batchDeleting) {
          return;
        }
        if (!queued) {
          ++_batchFailures;
          if (_batchFirstError.isEmpty) {
            _batchFirstError = tr('请求无法发送：会话未就绪或主机未授权文件传输');
          }
          _deleteQueue.clear();
          _finishBatchDelete();
        }
      }),
    );
  }

  void _finishBatchDelete() {
    _batchDeleting = false;
    _opResultFailed = _batchFailures > 0;
    if (_batchFailures == 0) {
      _opResult = tr('已删除 {n} 项', <String, Object?>{'n': _batchTotal});
    } else if (_batchFailures < _batchTotal) {
      _opResult = tr('已删除 {done} 项，{failed} 项失败：{reason}', <String, Object?>{
        'done': _batchTotal - _batchFailures,
        'failed': _batchFailures,
        'reason': _batchFirstError,
      });
    } else {
      _opResult = tr('删除失败：{reason}', <String, Object?>{
        'reason': _batchFirstError,
      });
    }
    _selected.clear();
    _selecting = false;
    unawaited(_request(_directory));
  }

  static String? _checkName(String name) {
    if (name.isEmpty) {
      return tr('名称不能为空');
    }
    if (name == '.' ||
        name == '..' ||
        name.contains('/') ||
        name.contains('\\') ||
        name.contains(':')) {
      return tr('名称不能包含 / \\ : 或使用 . 与 ..');
    }
    return null;
  }

  Future<void> _operate(
    ViewerFileOperation op,
    String path, {
    String newName = '',
  }) async {
    if (_batchDeleting) {
      return;
    }
    bool queued = false;
    try {
      queued = await widget.onFileOperation(op, path, newName);
    } on PlatformException {
      queued = false;
    } on MissingPluginException {
      queued = false;
    }
    if (!mounted) {
      return;
    }
    setState(() {
      _error = queued ? null : tr('请求无法发送：会话未就绪或主机未授权文件传输');
      _opResult = null;
      _opResultFailed = false;
    });
  }

  // Windows hosts compare names case-insensitively, so the guard does too;
  // the host stays authoritative when a truncated listing hid the real clash.
  bool _nameTaken(String name, {String? except}) {
    final String folded = name.toLowerCase();
    final String? ignored = except?.toLowerCase();
    return _entries.any(
      (ViewerFileEntry entry) =>
          entry.name.toLowerCase() == folded &&
          entry.name.toLowerCase() != ignored,
    );
  }

  Future<void> _createFolder() async {
    final String? name = await _promptName(
      title: tr('新建文件夹'),
      initial: '',
      confirm: tr('创建'),
    );
    if (name == null || !mounted) {
      return;
    }
    final String? complaint =
        _checkName(name) ?? (_nameTaken(name) ? tr('同名文件已存在') : null);
    if (complaint != null) {
      setState(() => _error = complaint);
      return;
    }
    await _operate(ViewerFileOperation.makeDirectory, _join(name));
  }

  Future<void> _rename(ViewerFileEntry entry) async {
    final String? name = await _promptName(
      title: tr('重命名'),
      initial: entry.name,
      confirm: tr('重命名'),
    );
    if (name == null || name == entry.name || !mounted) {
      return;
    }
    final String? complaint =
        _checkName(name) ??
        (_nameTaken(name, except: entry.name) ? tr('同名文件已存在') : null);
    if (complaint != null) {
      setState(() => _error = complaint);
      return;
    }
    await _operate(
      ViewerFileOperation.rename,
      _join(entry.name),
      newName: name,
    );
  }

  Future<void> _delete(ViewerFileEntry entry) async {
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) => AlertDialog(
        backgroundColor: kPanelDark,
        title: Text(
          tr('删除“{name}”？', <String, Object?>{'name': entry.name}),
          style: const TextStyle(fontSize: 13.5),
        ),
        content: Text(
          entry.directory ? tr('文件夹及其内容将被删除，此操作不可撤销。') : tr('此操作不可撤销。'),
          style: const TextStyle(fontSize: 11.5),
        ),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: Text(tr('取消')),
          ),
          TextButton(
            onPressed: () => Navigator.of(context).pop(true),
            child: Text(tr('删除')),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) {
      return;
    }
    await _operate(
      entry.directory
          ? ViewerFileOperation.removeTree
          : ViewerFileOperation.remove,
      _join(entry.name),
    );
  }

  Future<String?> _promptName({
    required String title,
    required String initial,
    required String confirm,
  }) async {
    final String? name = await showDialog<String>(
      context: context,
      builder: (BuildContext context) =>
          _NameDialog(title: title, initial: initial, confirm: confirm),
    );
    if (name == null || name.isEmpty) {
      return null;
    }
    return name;
  }

  static String _formatSize(int bytes) {
    if (bytes < 1024) {
      return '$bytes B';
    }
    if (bytes < 1024 * 1024) {
      return '${(bytes / 1024).toStringAsFixed(1)} KiB';
    }
    if (bytes < 1024 * 1024 * 1024) {
      return '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MiB';
    }
    return '${(bytes / (1024 * 1024 * 1024)).toStringAsFixed(2)} GiB';
  }

  static String _formatTime(int ms) {
    if (ms <= 0) {
      return '';
    }
    final DateTime at = DateTime.fromMillisecondsSinceEpoch(ms);
    String two(int value) => value.toString().padLeft(2, '0');
    return '${at.year}-${two(at.month)}-${two(at.day)} '
        '${two(at.hour)}:${two(at.minute)}';
  }

  @override
  Widget build(BuildContext context) {
    return Material(
      color: kPanelDark,
      elevation: 10,
      borderRadius: BorderRadius.circular(9),
      child: Container(
        width: widget.width,
        height: widget.height,
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(9),
          border: Border.all(color: Colors.white24),
        ),
        child: Column(
          children: <Widget>[
            SizedBox(
              height: 38,
              child: Row(
                children: <Widget>[
                  const SizedBox(width: 10),
                  const Icon(
                    Icons.folder_open_rounded,
                    size: 16,
                    color: kAccentBright,
                  ),
                  const SizedBox(width: 7),
                  Text(
                    tr('主机共享文件夹'),
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 12.5,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                  const Spacer(),
                  SizedBox(
                    width: 30,
                    height: 30,
                    child: IconButton(
                      key: const ValueKey<String>('file-select-mode'),
                      padding: EdgeInsets.zero,
                      tooltip: tr('多选'),
                      icon: const Icon(Icons.checklist_rounded, size: 16),
                      color: _selecting ? kAccentBright : Colors.white54,
                      onPressed: () => setState(() {
                        _selecting = !_selecting;
                        if (!_selecting) {
                          _selected.clear();
                        }
                      }),
                    ),
                  ),
                  SizedBox(
                    width: 30,
                    height: 30,
                    child: IconButton(
                      padding: EdgeInsets.zero,
                      tooltip: tr('新建文件夹'),
                      icon: const Icon(
                        Icons.create_new_folder_outlined,
                        size: 16,
                      ),
                      color: Colors.white54,
                      onPressed: _batchDeleting
                          ? null
                          : () => unawaited(_createFolder()),
                    ),
                  ),
                  SizedBox(
                    width: 30,
                    height: 30,
                    child: IconButton(
                      padding: EdgeInsets.zero,
                      tooltip: tr('上传文件到此处'),
                      icon: const Icon(Icons.upload_file_rounded, size: 16),
                      color: Colors.white54,
                      onPressed: _batchDeleting
                          ? null
                          : () => unawaited(_upload()),
                    ),
                  ),
                  SizedBox(
                    width: 30,
                    height: 30,
                    child: IconButton(
                      key: const ValueKey<String>('file-show-hidden'),
                      padding: EdgeInsets.zero,
                      tooltip: _showHidden
                          ? tr('不显示隐藏文件')
                          : tr('显示隐藏文件'),
                      icon: Icon(
                        _showHidden
                            ? Icons.visibility_rounded
                            : Icons.visibility_off_outlined,
                        size: 16,
                      ),
                      color: _showHidden ? kAccentBright : Colors.white54,
                      onPressed: () {
                        setState(() {
                          _showHidden = !_showHidden;
                          _selected.clear();
                        });
                        widget.onShowHiddenChanged?.call(_showHidden);
                        unawaited(_request(_directory));
                      },
                    ),
                  ),
                  SizedBox(
                    width: 30,
                    height: 30,
                    child: IconButton(
                      padding: EdgeInsets.zero,
                      tooltip: tr('刷新'),
                      icon: const Icon(Icons.refresh_rounded, size: 16),
                      color: Colors.white54,
                      onPressed: _loading
                          ? null
                          : () => unawaited(_request(_directory)),
                    ),
                  ),
                  SizedBox(
                    width: 30,
                    height: 30,
                    child: IconButton(
                      padding: EdgeInsets.zero,
                      tooltip: tr('关闭文件浏览'),
                      icon: const Icon(Icons.close_rounded, size: 16),
                      color: Colors.white54,
                      onPressed: widget.onClose,
                    ),
                  ),
                  const SizedBox(width: 4),
                ],
              ),
            ),
            const Divider(height: 1, color: Colors.white12),
            _buildBreadcrumb(),
            if (_entries.isNotEmpty || _filter.isNotEmpty) _buildFilter(),
            if (_selecting) _buildSelectionBar(),
            const Divider(height: 1, color: Colors.white12),
            Expanded(child: _buildBody()),
            if (_error != null)
              Padding(
                padding: const EdgeInsets.fromLTRB(12, 4, 12, 4),
                child: Row(
                  children: <Widget>[
                    Expanded(
                      child: Text(
                        _error!,
                        style: const TextStyle(
                          color: Colors.orangeAccent,
                          fontSize: 10.5,
                        ),
                      ),
                    ),
                    const SizedBox(width: 6),
                    TextButton(
                      key: const ValueKey<String>('file-retry'),
                      style: TextButton.styleFrom(
                        foregroundColor: kAccentBright,
                        padding: const EdgeInsets.symmetric(horizontal: 8),
                        minimumSize: const Size(0, 26),
                        tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                      ),
                      onPressed: () =>
                          unawaited(_request(_directory, _retryCursor)),
                      child: Text(
                        tr('重试'),
                        style: const TextStyle(fontSize: 11),
                      ),
                    ),
                  ],
                ),
              ),
            _buildFooter(),
          ],
        ),
      ),
    );
  }

  Widget _buildBreadcrumb() {
    final List<String> parts = _directory.isEmpty
        ? const <String>[]
        : _directory.split('/');
    final List<Widget> items = <Widget>[
      InkWell(
        onTap: _directory.isEmpty ? null : () => unawaited(_request('')),
        child: Text(
          tr('共享文件夹'),
          style: TextStyle(
            color: _directory.isEmpty ? kAccentBright : Colors.white70,
            fontSize: 11.5,
          ),
        ),
      ),
    ];
    final StringBuffer path = StringBuffer();
    for (int i = 0; i < parts.length; i++) {
      if (path.isNotEmpty) {
        path.write('/');
      }
      path.write(parts[i]);
      items.add(
        const Padding(
          padding: EdgeInsets.symmetric(horizontal: 3),
          child: Text(
            '/',
            style: TextStyle(color: Colors.white24, fontSize: 11),
          ),
        ),
      );
      final bool last = i == parts.length - 1;
      final String target = path.toString();
      items.add(
        InkWell(
          onTap: last ? null : () => unawaited(_request(target)),
          child: Text(
            parts[i],
            style: TextStyle(
              color: last ? kAccentBright : Colors.white70,
              fontSize: 11.5,
            ),
          ),
        ),
      );
    }
    return SizedBox(
      height: 30,
      child: SingleChildScrollView(
        scrollDirection: Axis.horizontal,
        padding: const EdgeInsets.symmetric(horizontal: 10),
        child: Row(children: items),
      ),
    );
  }

  Widget _buildFilter() {
    return SizedBox(
      height: 30,
      child: Row(
        children: <Widget>[
          if (_directory.isNotEmpty)
            SizedBox(
              width: 26,
              height: 26,
              child: IconButton(
                padding: EdgeInsets.zero,
                tooltip: tr('上一级'),
                icon: const Icon(Icons.arrow_back_rounded, size: 13),
                color: Colors.white54,
                onPressed: () {
                  final List<String> parts = _directory.split('/')
                    ..removeLast();
                  unawaited(_request(parts.join('/')));
                },
              ),
            ),
          const SizedBox(width: 10),
          const Icon(Icons.search_rounded, size: 14, color: Colors.white30),
          const SizedBox(width: 4),
          Expanded(
            child: TextField(
              key: const ValueKey<String>('file-filter'),
              controller: _filterController,
              style: const TextStyle(color: Colors.white70, fontSize: 11),
              cursorColor: kAccentBright,
              decoration: InputDecoration(
                isDense: true,
                border: InputBorder.none,
                hintText: tr(_truncated ? '筛选已加载的条目' : '筛选当前文件夹'),
                hintStyle: const TextStyle(color: Colors.white24, fontSize: 11),
              ),
              onChanged: (String value) => setState(() => _filter = value),
            ),
          ),
          if (_filter.isNotEmpty)
            SizedBox(
              width: 26,
              height: 26,
              child: IconButton(
                padding: EdgeInsets.zero,
                tooltip: tr('清除筛选'),
                icon: const Icon(Icons.close_rounded, size: 12),
                color: Colors.white54,
                onPressed: () {
                  _filterController.clear();
                  setState(() => _filter = '');
                },
              ),
            ),
          SizedBox(
            width: 26,
            height: 26,
            child: PopupMenuButton<FileBrowserSort>(
              tooltip: tr('排序'),
              padding: EdgeInsets.zero,
              iconSize: 14,
              icon: const Icon(
                Icons.swap_vert_rounded,
                size: 14,
                color: Colors.white54,
              ),
              onSelected: (FileBrowserSort mode) {
                setState(() => _sort = mode);
                widget.onSortChanged?.call(_sort, _sortAscending);
              },
              itemBuilder: (BuildContext context) =>
                  <PopupMenuEntry<FileBrowserSort>>[
                    CheckedPopupMenuItem<FileBrowserSort>(
                      value: FileBrowserSort.name,
                      checked: _sort == FileBrowserSort.name,
                      child: Text(
                        tr('按名称'),
                        style: const TextStyle(fontSize: 12),
                      ),
                    ),
                    CheckedPopupMenuItem<FileBrowserSort>(
                      value: FileBrowserSort.size,
                      checked: _sort == FileBrowserSort.size,
                      child: Text(
                        tr('按大小'),
                        style: const TextStyle(fontSize: 12),
                      ),
                    ),
                    CheckedPopupMenuItem<FileBrowserSort>(
                      value: FileBrowserSort.modified,
                      checked: _sort == FileBrowserSort.modified,
                      child: Text(
                        tr('按时间'),
                        style: const TextStyle(fontSize: 12),
                      ),
                    ),
                  ],
            ),
          ),
          SizedBox(
            width: 26,
            height: 26,
            child: IconButton(
              padding: EdgeInsets.zero,
              // The tooltip names what pressing the button will do.
              tooltip: _sortAscending ? tr('降序') : tr('升序'),
              icon: Icon(
                _sortAscending
                    ? Icons.arrow_upward_rounded
                    : Icons.arrow_downward_rounded,
                size: 13,
              ),
              color: Colors.white54,
              onPressed: () {
                setState(() => _sortAscending = !_sortAscending);
                widget.onSortChanged?.call(_sort, _sortAscending);
              },
            ),
          ),
          const SizedBox(width: 4),
          Text(
            _filter.isEmpty
                ? tr('共 {n} 项', <String, Object?>{'n': _visibleEntries.length})
                : tr('匹配 {n} / {total} 项', <String, Object?>{
                    'n': _visibleEntries.length,
                    'total': _entries.length,
                  }),
            style: const TextStyle(color: Colors.white38, fontSize: 10.5),
          ),
          const SizedBox(width: 8),
        ],
      ),
    );
  }

  Widget _buildSelectionBar() {
    final bool all = _allVisibleSelected;
    // Folders carry no size of their own, so only files add to the total.
    // The sum covers every selected name, including rows hidden by the
    // filter, because the batch buttons act on the whole selection.
    int selectedBytes = 0;
    for (final ViewerFileEntry entry in _entries) {
      if (!entry.directory && _selected.contains(entry.name)) {
        selectedBytes += entry.size;
      }
    }
    final String selectedText = tr('已选 {n} 项', <String, Object?>{
      'n': _selected.length,
    });
    final bool transfersBusy =
        widget.status.downloadActive || widget.status.folderDownloadActive;
    final bool canAct = _selected.isNotEmpty && !_batchDeleting;
    final ButtonStyle compact = TextButton.styleFrom(
      padding: const EdgeInsets.symmetric(horizontal: 6),
      minimumSize: const Size(0, 28),
      tapTargetSize: MaterialTapTargetSize.shrinkWrap,
    );
    return Container(
      height: 32,
      padding: const EdgeInsets.symmetric(horizontal: 8),
      color: Colors.white10,
      child: Row(
        children: <Widget>[
          TextButton(
            key: const ValueKey<String>('file-select-all'),
            style: compact,
            onPressed: _visibleEntries.isEmpty ? null : _toggleSelectAll,
            child: Text(
              all ? tr('清空') : tr('全选'),
              style: const TextStyle(fontSize: 11),
            ),
          ),
          const SizedBox(width: 4),
          Text(
            selectedBytes > 0
                ? '$selectedText · ${_formatSize(selectedBytes)}'
                : selectedText,
            style: const TextStyle(color: Colors.white54, fontSize: 10.5),
          ),
          const Spacer(),
          TextButton(
            key: const ValueKey<String>('file-batch-copy'),
            style: compact,
            onPressed: canAct ? () => unawaited(_copySelectedPaths()) : null,
            child: Text(tr('复制路径'), style: const TextStyle(fontSize: 11)),
          ),
          TextButton(
            key: const ValueKey<String>('file-batch-download'),
            style: compact,
            onPressed: canAct && !transfersBusy
                ? () => unawaited(_downloadSelected())
                : null,
            child: Text(tr('下载'), style: const TextStyle(fontSize: 11)),
          ),
          TextButton(
            key: const ValueKey<String>('file-batch-delete'),
            style: compact,
            onPressed: canAct ? () => unawaited(_deleteSelected()) : null,
            child: Text(
              tr('删除'),
              style: TextStyle(
                fontSize: 11,
                color: canAct ? Colors.orangeAccent : Colors.white24,
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildBody() {
    if (_loading && _entries.isEmpty) {
      return const Center(
        child: SizedBox(
          width: 18,
          height: 18,
          child: CircularProgressIndicator(
            strokeWidth: 2,
            color: kAccentBright,
          ),
        ),
      );
    }
    final List<Widget> rows = <Widget>[];
    final List<ViewerFileEntry> visible = _visibleEntries;
    if (_directory.isNotEmpty) {
      rows.add(
        _entryRow(
          icon: Icons.arrow_upward_rounded,
          name: tr('返回上一级'),
          detail: '',
          onTap: () {
            final int slash = _directory.lastIndexOf('/');
            unawaited(
              _request(slash <= 0 ? '' : _directory.substring(0, slash)),
            );
          },
        ),
      );
    }
    for (final ViewerFileEntry entry in visible) {
      final bool transfersBusy =
          widget.status.downloadActive || widget.status.folderDownloadActive;
      rows.add(
        _entryRow(
          icon: entry.directory
              ? Icons.folder_rounded
              : Icons.insert_drive_file_outlined,
          name: entry.name,
          detail: entry.directory
              ? _formatTime(entry.modifiedMs)
              : <String>[
                  _formatSize(entry.size),
                  _formatTime(entry.modifiedMs),
                ].where((String part) => part.isNotEmpty).join(' · '),
          accent: entry.directory,
          selected: _selected.contains(entry.name),
          onToggle: _selecting ? () => _toggleSelection(entry.name) : null,
          onTap: _selecting
              ? () => _toggleSelection(entry.name)
              : (entry.directory
                    ? () => unawaited(_request(_join(entry.name)))
                    : null),
          onDownload: transfersBusy
              ? null
              : (entry.directory
                    ? () => unawaited(_downloadFolder(entry))
                    : () => unawaited(_download(entry))),
          // A double click is the file-transfer equivalent of the trailing
          // arrow; folders already open on the first click.
          onDoubleTap: entry.directory || transfersBusy || _selecting
              ? null
              : () => unawaited(_download(entry)),
          downloadTooltip: entry.directory ? tr('下载文件夹到本机') : tr('下载到本机'),
          onRename: _batchDeleting ? null : () => unawaited(_rename(entry)),
          onDelete: _batchDeleting ? null : () => unawaited(_delete(entry)),
          onCopyPath: _batchDeleting
              ? null
              : () => unawaited(_copyPath(_join(entry.name))),
        ),
      );
    }
    if (rows.isEmpty && !_truncated) {
      final bool filtering = _filter.isNotEmpty && _entries.isNotEmpty;
      return Center(
        child: Text(
          _loading
              ? tr('正在读取目录…')
              : (filtering ? tr('没有匹配的条目') : tr('这个文件夹是空的')),
          style: const TextStyle(color: Colors.white30, fontSize: 11),
        ),
      );
    }
    return ListView(
      padding: const EdgeInsets.symmetric(vertical: 4),
      children: <Widget>[
        ...rows,
        if (_filter.isNotEmpty && visible.isEmpty)
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 8, 12, 8),
            child: Text(
              tr('没有匹配的条目'),
              style: const TextStyle(color: Colors.white30, fontSize: 11),
            ),
          ),
        if (_truncated)
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 8, 12, 8),
            child: TextButton(
              onPressed: _loading || _nextCursor.isEmpty
                  ? null
                  : () => unawaited(_request(_directory, _nextCursor)),
              child: Text(
                _loading
                    ? tr('正在读取目录…')
                    : tr('加载更多（已显示 {n} 项）', <String, Object?>{
                        'n': _entries.length,
                      }),
              ),
            ),
          ),
      ],
    );
  }

  List<PopupMenuEntry<String>> _entryMenuItems() => <PopupMenuEntry<String>>[
    PopupMenuItem<String>(
      value: 'copy-path',
      height: 34,
      child: Text(
        tr('复制路径'),
        style: const TextStyle(color: Colors.white, fontSize: 11.5),
      ),
    ),
    PopupMenuItem<String>(
      value: 'rename',
      height: 34,
      child: Text(
        tr('重命名'),
        style: const TextStyle(color: Colors.white, fontSize: 11.5),
      ),
    ),
    PopupMenuItem<String>(
      value: 'delete',
      height: 34,
      child: Text(
        tr('删除'),
        style: const TextStyle(color: Colors.white, fontSize: 11.5),
      ),
    ),
  ];

  void _runEntryMenu(
    String value, {
    VoidCallback? onRename,
    VoidCallback? onDelete,
    VoidCallback? onCopyPath,
  }) {
    if (value == 'rename') {
      onRename?.call();
    } else if (value == 'delete') {
      onDelete?.call();
    } else if (value == 'copy-path') {
      onCopyPath?.call();
    }
  }

  /// A right click or long press on the row opens the same menu as the
  /// trailing button; the long press is the touch-screen equivalent.
  Future<void> _showEntryContextMenu(
    BuildContext context,
    Offset position, {
    VoidCallback? onRename,
    VoidCallback? onDelete,
    VoidCallback? onCopyPath,
  }) async {
    if (onRename == null) {
      return;
    }
    final String? value = await showMenu<String>(
      context: context,
      position: RelativeRect.fromLTRB(
        position.dx,
        position.dy,
        position.dx,
        position.dy,
      ),
      color: kPanelDark,
      items: _entryMenuItems(),
    );
    if (value == null) {
      return;
    }
    _runEntryMenu(
      value,
      onRename: onRename,
      onDelete: onDelete,
      onCopyPath: onCopyPath,
    );
  }

  Widget _entryRow({
    required IconData icon,
    required String name,
    required String detail,
    bool accent = false,
    bool selected = false,
    String? downloadTooltip,
    VoidCallback? onTap,
    VoidCallback? onDownload,
    VoidCallback? onDoubleTap,
    VoidCallback? onToggle,
    VoidCallback? onRename,
    VoidCallback? onDelete,
    VoidCallback? onCopyPath,
  }) {
    return GestureDetector(
      onSecondaryTapDown: (TapDownDetails details) {
        unawaited(
          _showEntryContextMenu(
            context,
            details.globalPosition,
            onRename: onRename,
            onDelete: onDelete,
            onCopyPath: onCopyPath,
          ),
        );
      },
      onLongPressStart: (LongPressStartDetails details) {
        unawaited(
          _showEntryContextMenu(
            context,
            details.globalPosition,
            onRename: onRename,
            onDelete: onDelete,
            onCopyPath: onCopyPath,
          ),
        );
      },
      child: InkWell(
        onTap: onTap,
        child: SizedBox(
          height: 34,
          child: Row(
            children: <Widget>[
              const SizedBox(width: 12),
              if (_selecting)
                SizedBox(
                  width: 26,
                  height: 26,
                  child: onToggle == null
                      ? null
                      : Checkbox(
                          value: selected,
                          onChanged: (_) => onToggle(),
                          materialTapTargetSize:
                              MaterialTapTargetSize.shrinkWrap,
                          visualDensity: VisualDensity.compact,
                          side: const BorderSide(color: Colors.white38),
                          activeColor: kAccentBright,
                          checkColor: Colors.black,
                        ),
                ),
              // The double click only covers the informational part of the
              // row: a recognizer spanning the whole row would hold the tap
              // arena open and delay the trailing download/menu buttons.
              Expanded(
                child: GestureDetector(
                  behavior: HitTestBehavior.opaque,
                  onDoubleTap: onDoubleTap,
                  child: Row(
                    children: <Widget>[
                      Icon(
                        icon,
                        size: 17,
                        color: accent ? kAccentBright : Colors.white54,
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Text(
                          name,
                          overflow: TextOverflow.ellipsis,
                          style: const TextStyle(
                            color: Colors.white,
                            fontSize: 12,
                          ),
                        ),
                      ),
                      if (detail.isNotEmpty)
                        Text(
                          detail,
                          style: const TextStyle(
                            color: Colors.white30,
                            fontSize: 10,
                          ),
                        ),
                    ],
                  ),
                ),
              ),
              if (onDownload != null)
                SizedBox(
                  width: 30,
                  height: 30,
                  child: IconButton(
                    padding: EdgeInsets.zero,
                    tooltip: downloadTooltip ?? tr('下载到本机'),
                    icon: const Icon(Icons.download_rounded, size: 16),
                    color: kAccentBright,
                    onPressed: onDownload,
                  ),
                )
              else
                const SizedBox(width: 30),
              if (onRename != null)
                SizedBox(
                  width: 30,
                  height: 30,
                  child: PopupMenuButton<String>(
                    padding: EdgeInsets.zero,
                    tooltip: tr('更多操作'),
                    icon: const Icon(Icons.more_vert_rounded, size: 16),
                    iconColor: Colors.white54,
                    color: kPanelDark,
                    onSelected: (String value) => _runEntryMenu(
                      value,
                      onRename: onRename,
                      onDelete: onDelete,
                      onCopyPath: onCopyPath,
                    ),
                    itemBuilder: (BuildContext context) => _entryMenuItems(),
                  ),
                )
              else
                const SizedBox(width: 30),
              const SizedBox(width: 8),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildFooter() {
    final ViewerStatus status = widget.status;
    Widget child;
    if (status.folderDownloadActive) {
      final bool batch = status.folderDownloadBatch;
      final double progress = status.folderDownloadFilesTotal > 0
          ? (status.folderDownloadFilesDone / status.folderDownloadFilesTotal)
                .clamp(0.0, 1.0)
          : 0.0;
      child = Row(
        children: <Widget>[
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(
                  batch
                      ? tr(
                          '正在下载选中的 {n} 项 · {done}/{total} 个文件',
                          <String, Object?>{
                            'n': status.folderDownloadRoots,
                            'done': status.folderDownloadFilesDone,
                            'total': status.folderDownloadFilesTotal,
                          },
                        )
                      : tr(
                          '正在下载文件夹 {name} · {done}/{total} 个文件',
                          <String, Object?>{
                            'name': status.folderDownloadName,
                            'done': status.folderDownloadFilesDone,
                            'total': status.folderDownloadFilesTotal,
                          },
                        ),
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(color: Colors.white54, fontSize: 10.5),
                ),
                if (status.folderDownloadCurrent.isNotEmpty)
                  Text(
                    tr('当前：{path}', <String, Object?>{
                      'path': status.folderDownloadCurrent,
                    }),
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(color: Colors.white30, fontSize: 10),
                  ),
                const SizedBox(height: 4),
                LinearProgressIndicator(
                  value: progress,
                  minHeight: 3,
                  backgroundColor: Colors.white12,
                  color: kAccentBright,
                ),
              ],
            ),
          ),
          const SizedBox(width: 6),
          SizedBox(
            width: 28,
            height: 28,
            child: IconButton(
              padding: EdgeInsets.zero,
              tooltip: tr('取消文件夹下载'),
              icon: const Icon(Icons.close_rounded, size: 14),
              color: Colors.white54,
              onPressed: () => unawaited(_cancelFolderDownload()),
            ),
          ),
        ],
      );
    } else if (status.downloadActive && !_downloadCancelPending) {
      final double progress = status.downloadSize > 0
          ? (status.downloadReceived / status.downloadSize).clamp(0.0, 1.0)
          : 0.0;
      final String label = tr('正在下载 {name} · {p1} / {p2}', <String, Object?>{
        'name': status.downloadName,
        'p1': _formatSize(status.downloadReceived),
        'p2': _formatSize(status.downloadSize),
      });
      child = Row(
        children: <Widget>[
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(
                  // An unknown total (size 0) cannot yield a percentage.
                  status.downloadSize > 0
                      ? '$label · ${(progress * 100).round()}%'
                      : label,
                  style: const TextStyle(color: Colors.white54, fontSize: 10.5),
                ),
                const SizedBox(height: 4),
                LinearProgressIndicator(
                  value: progress,
                  minHeight: 3,
                  backgroundColor: Colors.white12,
                  color: kAccentBright,
                ),
              ],
            ),
          ),
          if (status.downloadCancellable) ...<Widget>[
            const SizedBox(width: 6),
            SizedBox(
              width: 28,
              height: 28,
              child: IconButton(
                padding: EdgeInsets.zero,
                tooltip: tr('取消下载'),
                icon: const Icon(Icons.close_rounded, size: 14),
                color: Colors.white54,
                onPressed: () => unawaited(_cancelDownload()),
              ),
            ),
          ],
        ],
      );
    } else if (_batchDeleting) {
      child = Text(
        tr('正在删除选中的 {n} 项…', <String, Object?>{'n': _batchTotal}),
        style: const TextStyle(color: Colors.white54, fontSize: 10.5),
      );
    } else if (_folderResult != null) {
      final String destination = status.folderDownloadDestination;
      final Widget label = Text(
        _folderResult!,
        style: TextStyle(
          color: _folderResultFailed ? Colors.orangeAccent : Colors.greenAccent,
          fontSize: 10.5,
        ),
      );
      child = !_folderResultFailed && destination.isNotEmpty
          ? Row(
              children: <Widget>[
                Expanded(child: label),
                const SizedBox(width: 6),
                SizedBox(
                  width: 28,
                  height: 28,
                  child: IconButton(
                    padding: EdgeInsets.zero,
                    tooltip: tr('打开文件夹'),
                    icon: const Icon(Icons.open_in_new_rounded, size: 14),
                    color: Colors.white54,
                    onPressed: () => unawaited(_openDownload(destination)),
                  ),
                ),
                SizedBox(
                  width: 28,
                  height: 28,
                  child: IconButton(
                    padding: EdgeInsets.zero,
                    tooltip: tr('在文件夹中显示'),
                    icon: const Icon(Icons.folder_open_rounded, size: 14),
                    color: Colors.white54,
                    onPressed: () => unawaited(_revealDownload(destination)),
                  ),
                ),
                SizedBox(
                  width: 28,
                  height: 28,
                  child: IconButton(
                    padding: EdgeInsets.zero,
                    tooltip: tr('复制路径'),
                    icon: const Icon(Icons.copy_rounded, size: 13),
                    color: Colors.white54,
                    onPressed: () => unawaited(_copyPath(destination)),
                  ),
                ),
              ],
            )
          : label;
    } else if (status.downloadPhase == ViewerFilePhase.done &&
        status.downloadPath.isNotEmpty) {
      child = Row(
        children: <Widget>[
          Expanded(
            child: Text(
              tr('已保存到 {path}', <String, Object?>{'path': status.downloadPath}),
              style: const TextStyle(color: Colors.greenAccent, fontSize: 10.5),
              overflow: TextOverflow.ellipsis,
            ),
          ),
          const SizedBox(width: 6),
          SizedBox(
            width: 28,
            height: 28,
            child: IconButton(
              padding: EdgeInsets.zero,
              tooltip: tr('打开文件'),
              icon: const Icon(Icons.open_in_new_rounded, size: 14),
              color: Colors.white54,
              onPressed: () => unawaited(_openDownload(status.downloadPath)),
            ),
          ),
          SizedBox(
            width: 28,
            height: 28,
            child: IconButton(
              padding: EdgeInsets.zero,
              tooltip: tr('在文件夹中显示'),
              icon: const Icon(Icons.folder_open_rounded, size: 14),
              color: Colors.white54,
              onPressed: () => unawaited(_revealDownload(status.downloadPath)),
            ),
          ),
          SizedBox(
            width: 28,
            height: 28,
            child: IconButton(
              padding: EdgeInsets.zero,
              tooltip: tr('复制路径'),
              icon: const Icon(Icons.copy_rounded, size: 13),
              color: Colors.white54,
              onPressed: () => unawaited(_copyPath(status.downloadPath)),
            ),
          ),
        ],
      );
    } else if (_downloadResult != null) {
      // A cancel that landed on the engine; the failed phase below carries the
      // engine's wording, which the page replaces while this is set.
      child = Text(
        _downloadResult!,
        style: const TextStyle(color: Colors.greenAccent, fontSize: 10.5),
      );
    } else if (status.downloadPhase == ViewerFilePhase.failed &&
        status.downloadError.isNotEmpty) {
      child = Text(
        status.downloadError,
        style: const TextStyle(color: Colors.orangeAccent, fontSize: 10.5),
      );
    } else if ((status.fileActive || status.fileQueue > 0) &&
        !_uploadCancelPending) {
      final String target = _directory.isEmpty ? tr('共享文件夹') : _directory;
      // The engine reports the size once the offer arrives; before that only
      // the queue is known, so the footer keeps the wording without a bar.
      final bool sized = status.fileSize > 0;
      final String label = sized
          ? tr(
              '正在上传到 {dir} · {p1} / {p2} · {pct}%（队列剩余 {n} 个）',
              <String, Object?>{
                'dir': target,
                'p1': _formatSize(status.fileSent),
                'p2': _formatSize(status.fileSize),
                'pct': (status.fileProgress * 100).round(),
                'n': status.fileQueue,
              },
            )
          : tr('正在上传到 {dir}（队列剩余 {n} 个）', <String, Object?>{
              'dir': target,
              'n': status.fileQueue,
            });
      child = Row(
        children: <Widget>[
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(
                  label,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(color: Colors.white54, fontSize: 10.5),
                ),
                if (sized) ...<Widget>[
                  const SizedBox(height: 4),
                  LinearProgressIndicator(
                    value: status.fileProgress,
                    minHeight: 3,
                    backgroundColor: Colors.white12,
                    color: kAccentBright,
                  ),
                ],
              ],
            ),
          ),
          const SizedBox(width: 6),
          SizedBox(
            width: 28,
            height: 28,
            child: IconButton(
              padding: EdgeInsets.zero,
              tooltip: tr('取消上传'),
              icon: const Icon(Icons.close_rounded, size: 14),
              color: Colors.white54,
              onPressed: () => unawaited(_cancelUpload()),
            ),
          ),
        ],
      );
    } else if (_uploadResult != null) {
      child = Text(
        _uploadResult!,
        style: TextStyle(
          color: _uploadResultFailed ? Colors.orangeAccent : Colors.greenAccent,
          fontSize: 10.5,
        ),
      );
    } else if (_opResult != null) {
      child = Text(
        _opResult!,
        style: TextStyle(
          color: _opResultFailed ? Colors.orangeAccent : Colors.greenAccent,
          fontSize: 10.5,
        ),
      );
    } else {
      child = Text(
        tr('点击文件右侧的下载按钮保存到本机；多选可批量下载或删除；标题栏的上传按钮会把文件传到当前文件夹'),
        style: const TextStyle(color: Colors.white30, fontSize: 10),
      );
    }
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.fromLTRB(12, 6, 12, 8),
      decoration: const BoxDecoration(
        border: Border(top: BorderSide(color: Colors.white12)),
      ),
      child: child,
    );
  }
}

/// Asks for one entry name. The controller lives in the dialog's state so it
/// outlives the route's closing transition.
class _NameDialog extends StatefulWidget {
  const _NameDialog({
    required this.title,
    required this.initial,
    required this.confirm,
  });

  final String title;
  final String initial;
  final String confirm;

  @override
  State<_NameDialog> createState() => _NameDialogState();
}

class _NameDialogState extends State<_NameDialog> {
  late final TextEditingController _name = TextEditingController(
    text: widget.initial,
  );

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      backgroundColor: kPanelDark,
      title: Text(widget.title, style: const TextStyle(fontSize: 13.5)),
      content: TextField(
        controller: _name,
        autofocus: true,
        style: const TextStyle(fontSize: 12.5),
        decoration: InputDecoration(hintText: tr('名称'), isDense: true),
        onSubmitted: (String value) => Navigator.of(context).pop(value.trim()),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
        TextButton(
          onPressed: () => Navigator.of(context).pop(_name.text.trim()),
          child: Text(widget.confirm),
        ),
      ],
    );
  }
}
