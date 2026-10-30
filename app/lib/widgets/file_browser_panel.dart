import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../theme.dart';
import '../viewer_bridge.dart';
import 'package:chast_viewer/l10n.dart';

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
    required this.onClose,
    this.onDirectoryChanged,
    this.width = 460,
    this.height = 480,
  });

  final ViewerStatus status;
  // The panel's outer size. The session overlay uses the compact default; a
  // file-transfer-only session gives the browser most of the window.
  final double width;
  final double height;
  final Future<bool> Function(String directory) onRequestList;
  final Future<bool> Function(String path) onRequestDownload;
  final Future<ViewerFileListing?> Function() onTakeList;
  // Queues the recursive download of one folder; false means the viewer could
  // not accept it (no session, permission revoked or another walk running).
  final Future<bool> Function(String path) onDownloadFolder;
  // Queues one walk over the selected entries: folders mirror below their own
  // names, files land at the top of the receive directory. False means the
  // viewer could not accept it (no session, permission revoked or another
  // walk running).
  final Future<bool> Function(List<ViewerFileSelection> entries)
      onDownloadEntries;
  final Future<void> Function() onCancelFolderDownload;
  // Reports the directory whose listing was accepted, so dropped files can
  // follow the browser instead of always landing in the shared root.
  final ValueChanged<String>? onDirectoryChanged;
  // Opens the local file picker and queues the chosen files into the shared
  // directory argument. Returns how many roots were queued; null means the
  // picker was cancelled.
  final Future<int?> Function(String directory) onUpload;
  // Queues one new-folder, rename or remove request. `path` is relative to
  // the shared root; `newName` is only used by rename. False means the
  // request could not be queued.
  final Future<bool> Function(
      ViewerFileOperation op, String path, String newName) onFileOperation;
  final VoidCallback onClose;

  @override
  State<FileBrowserPanel> createState() => _FileBrowserPanelState();
}

class _FileBrowserPanelState extends State<FileBrowserPanel> {
  String _directory = '';
  List<ViewerFileEntry> _entries = const <ViewerFileEntry>[];
  bool _loading = true;
  bool _truncated = false;
  String? _error;
  int _takenListAnswers = 0;
  int _takenDownloadAnswers = 0;
  // Set while the uploads queued from the panel are still running; the first
  // status poll that returns to idle then refreshes the listing once.
  bool _uploadAwaitingEnd = false;
  bool _uploadWasActive = false;
  String _uploadBaselineLast = '';
  String? _uploadResult;
  bool _uploadResultFailed = false;
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
  final List<String> _deleteQueue = <String>[];

  @override
  void initState() {
    super.initState();
    _takenListAnswers = widget.status.fileListAnswers;
    _takenDownloadAnswers = widget.status.fileDownloadAnswers;
    _takenOpAnswers = widget.status.fileOpAnswers;
    _takenFolderAnswers = widget.status.folderDownloadAnswers;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) {
        unawaited(_request(''));
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
    if (_uploadAwaitingEnd) {
      // A tiny file can finish before the first poll shows it as active; the
      // finished entry then only shows up in lastFileName.
      final bool active = widget.status.fileActive || widget.status.fileQueue > 0;
      final bool finished = widget.status.lastFileName.isNotEmpty &&
          widget.status.lastFileName != _uploadBaselineLast;
      if (active) {
        _uploadWasActive = true;
      } else if (_uploadWasActive || finished) {
        _finishUpload();
      }
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
      _uploadAwaitingEnd = true;
      _uploadWasActive = false;
      _uploadBaselineLast = widget.status.lastFileName;
    });
  }

  Future<void> _request(String directory) async {
    setState(() {
      _loading = true;
      _error = null;
    });
    bool queued = false;
    try {
      queued = await widget.onRequestList(directory);
    } on PlatformException {
      queued = false;
    } on MissingPluginException {
      queued = false;
    }
    if (!mounted) {
      return;
    }
    if (!queued) {
      setState(() {
        _loading = false;
        _error = tr('请求无法发送：会话未就绪或主机未授权文件传输');
      });
    }
  }

  Future<void> _takeAnswer() async {
    _takenListAnswers = widget.status.fileListAnswers;
    ViewerFileListing? listing;
    try {
      listing = await widget.onTakeList();
    } on PlatformException {
      listing = null;
    } on MissingPluginException {
      listing = null;
    }
    if (!mounted) {
      return;
    }
    if (listing == null) {
      setState(() => _loading = false);
      return;
    }
    final ViewerFileListing answer = listing;
    setState(() {
      _loading = false;
      if (answer.ok) {
        if (answer.directory != _directory) {
          _directory = answer.directory;
          widget.onDirectoryChanged?.call(_directory);
        }
        _entries = answer.entries;
        _truncated = answer.truncated;
        _error = null;
        _selected.clear();
      } else {
        _error = answer.reason.isEmpty ? tr('主机拒绝列出这个目录') : answer.reason;
      }
    });
  }

  Future<void> _download(ViewerFileEntry entry) async {
    final String path =
        _directory.isEmpty ? entry.name : '$_directory/${entry.name}';
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
      queued = await widget.onDownloadFolder(_join(entry.name));
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

  void _toggleSelection(String name) {
    setState(() {
      if (!_selected.add(name)) {
        _selected.remove(name);
      }
    });
  }

  void _toggleSelectAll() {
    setState(() {
      if (_selected.length == _entries.length) {
        _selected.clear();
      } else {
        _selected
          ..clear()
          ..addAll(_entries.map((ViewerFileEntry entry) => entry.name));
      }
    });
  }

  List<ViewerFileSelection> _selection() => <ViewerFileSelection>[
        for (final ViewerFileEntry entry in _entries)
          if (_selected.contains(entry.name))
            ViewerFileSelection(
                path: _join(entry.name), directory: entry.directory),
      ];

  Future<void> _downloadSelected() async {
    final List<ViewerFileSelection> entries = _selection();
    if (entries.isEmpty) {
      return;
    }
    bool queued = false;
    try {
      queued = await widget.onDownloadEntries(entries);
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
    final List<String> paths = <String>[
      for (final ViewerFileEntry entry in _entries)
        if (_selected.contains(entry.name)) _join(entry.name),
    ];
    if (paths.isEmpty || _batchDeleting) {
      return;
    }
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) => AlertDialog(
        backgroundColor: kPanelDark,
        title: Text(
          tr('删除选中的 {n} 项？', <String, Object?>{'n': paths.length}),
          style: const TextStyle(fontSize: 13.5),
        ),
        content: Text(
          tr('文件夹必须为空才能删除，此操作不可撤销。'),
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
      _batchTotal = paths.length;
      _batchFailures = 0;
      _batchFirstError = '';
      _opResult = null;
      _opResultFailed = false;
      _deleteQueue
        ..clear()
        ..addAll(paths.skip(1));
    });
    final bool queued = await _sendRemove(paths.first);
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

  Future<bool> _sendRemove(String path) async {
    try {
      return await widget.onFileOperation(ViewerFileOperation.remove, path, '');
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
    final String path = _deleteQueue.removeAt(0);
    unawaited(_sendRemove(path).then((bool queued) {
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
    }));
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

  Future<void> _operate(ViewerFileOperation op, String path,
      {String newName = ''}) async {
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

  Future<void> _createFolder() async {
    final String? name = await _promptName(
      title: tr('新建文件夹'),
      initial: '',
      confirm: tr('创建'),
    );
    if (name == null || !mounted) {
      return;
    }
    final String? complaint = _checkName(name);
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
    final String? complaint = _checkName(name);
    if (complaint != null) {
      setState(() => _error = complaint);
      return;
    }
    await _operate(ViewerFileOperation.rename, _join(entry.name),
        newName: name);
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
          entry.directory
              ? tr('文件夹必须为空才能删除，此操作不可撤销。')
              : tr('此操作不可撤销。'),
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
    await _operate(ViewerFileOperation.remove, _join(entry.name));
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
                  const Icon(Icons.folder_open_rounded,
                      size: 16, color: kAccentBright),
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
                      icon: const Icon(Icons.create_new_folder_outlined,
                          size: 16),
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
                      onPressed:
                          _batchDeleting ? null : () => unawaited(_upload()),
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
            if (_selecting) _buildSelectionBar(),
            const Divider(height: 1, color: Colors.white12),
            Expanded(child: _buildBody()),
            if (_error != null)
              Padding(
                padding: const EdgeInsets.fromLTRB(12, 4, 12, 4),
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: Text(
                    _error!,
                    style: const TextStyle(
                        color: Colors.orangeAccent, fontSize: 10.5),
                  ),
                ),
              ),
            _buildFooter(),
          ],
        ),
      ),
    );
  }

  Widget _buildBreadcrumb() {
    final List<String> parts =
        _directory.isEmpty ? const <String>[] : _directory.split('/');
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
      items.add(const Padding(
        padding: EdgeInsets.symmetric(horizontal: 3),
        child: Text('/',
            style: TextStyle(color: Colors.white24, fontSize: 11)),
      ));
      final bool last = i == parts.length - 1;
      final String target = path.toString();
      items.add(InkWell(
        onTap: last ? null : () => unawaited(_request(target)),
        child: Text(
          parts[i],
          style: TextStyle(
            color: last ? kAccentBright : Colors.white70,
            fontSize: 11.5,
          ),
        ),
      ));
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

  Widget _buildSelectionBar() {
    final bool all =
        _entries.isNotEmpty && _selected.length == _entries.length;
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
            onPressed: _entries.isEmpty ? null : _toggleSelectAll,
            child: Text(
              all ? tr('清空') : tr('全选'),
              style: const TextStyle(fontSize: 11),
            ),
          ),
          const SizedBox(width: 4),
          Text(
            tr('已选 {n} 项', <String, Object?>{'n': _selected.length}),
            style: const TextStyle(color: Colors.white54, fontSize: 10.5),
          ),
          const Spacer(),
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
    if (_directory.isNotEmpty) {
      rows.add(_entryRow(
        icon: Icons.arrow_upward_rounded,
        name: tr('返回上一级'),
        detail: '',
        onTap: () {
          final int slash = _directory.lastIndexOf('/');
          unawaited(_request(slash <= 0 ? '' : _directory.substring(0, slash)));
        },
      ));
    }
    for (final ViewerFileEntry entry in _entries) {
      final bool transfersBusy =
          widget.status.downloadActive || widget.status.folderDownloadActive;
      rows.add(_entryRow(
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
        downloadTooltip:
            entry.directory ? tr('下载文件夹到本机') : tr('下载到本机'),
        onRename: _batchDeleting ? null : () => unawaited(_rename(entry)),
        onDelete: _batchDeleting ? null : () => unawaited(_delete(entry)),
      ));
    }
    if (rows.isEmpty) {
      return Center(
        child: Text(
          _loading ? tr('正在读取目录…') : tr('这个文件夹是空的'),
          style: const TextStyle(color: Colors.white30, fontSize: 11),
        ),
      );
    }
    return ListView(
      padding: const EdgeInsets.symmetric(vertical: 4),
      children: <Widget>[
        ...rows,
        if (_truncated)
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 8, 12, 8),
            child: Text(
              tr('条目太多，主机只返回了前一部分'),
              style: const TextStyle(color: Colors.white38, fontSize: 10.5),
            ),
          ),
      ],
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
    VoidCallback? onToggle,
    VoidCallback? onRename,
    VoidCallback? onDelete,
  }) {
    return InkWell(
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
            Icon(icon,
                size: 17, color: accent ? kAccentBright : Colors.white54),
            const SizedBox(width: 8),
            Expanded(
              child: Text(
                name,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(color: Colors.white, fontSize: 12),
              ),
            ),
            if (detail.isNotEmpty)
              Text(
                detail,
                style: const TextStyle(color: Colors.white30, fontSize: 10),
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
                  onSelected: (String value) {
                    if (value == 'rename') {
                      onRename();
                    } else if (value == 'delete') {
                      onDelete?.call();
                    }
                  },
                  itemBuilder: (BuildContext context) =>
                      <PopupMenuEntry<String>>[
                    PopupMenuItem<String>(
                      value: 'rename',
                      height: 34,
                      child: Text(tr('重命名'),
                          style: const TextStyle(
                              color: Colors.white, fontSize: 11.5)),
                    ),
                    PopupMenuItem<String>(
                      value: 'delete',
                      height: 34,
                      child: Text(tr('删除'),
                          style: const TextStyle(
                              color: Colors.white, fontSize: 11.5)),
                    ),
                  ],
                ),
              )
            else
              const SizedBox(width: 30),
            const SizedBox(width: 8),
          ],
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
          ? (status.folderDownloadFilesDone /
                  status.folderDownloadFilesTotal)
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
                      ? tr('正在下载选中的 {n} 项 · {done}/{total} 个文件',
                          <String, Object?>{
                            'n': status.folderDownloadRoots,
                            'done': status.folderDownloadFilesDone,
                            'total': status.folderDownloadFilesTotal,
                          })
                      : tr('正在下载文件夹 {name} · {done}/{total} 个文件',
                          <String, Object?>{
                            'name': status.folderDownloadName,
                            'done': status.folderDownloadFilesDone,
                            'total': status.folderDownloadFilesTotal,
                          }),
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
    } else if (status.downloadActive) {
      final double progress = status.downloadSize > 0
          ? (status.downloadReceived / status.downloadSize).clamp(0.0, 1.0)
          : 0.0;
      child = Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Text(
            tr('正在下载 {name} · {p1} / {p2}', <String, Object?>{
              'name': status.downloadName,
              'p1': _formatSize(status.downloadReceived),
              'p2': _formatSize(status.downloadSize),
            }),
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
      );
    } else if (_batchDeleting) {
      child = Text(
        tr('正在删除选中的 {n} 项…', <String, Object?>{'n': _batchTotal}),
        style: const TextStyle(color: Colors.white54, fontSize: 10.5),
      );
    } else if (_folderResult != null) {
      child = Text(
        _folderResult!,
        style: TextStyle(
          color: _folderResultFailed ? Colors.orangeAccent : Colors.greenAccent,
          fontSize: 10.5,
        ),
      );
    } else if (status.downloadPhase == ViewerFilePhase.done &&
        status.downloadPath.isNotEmpty) {
      child = Text(
        tr('已保存到 {path}', <String, Object?>{'path': status.downloadPath}),
        style: const TextStyle(color: Colors.greenAccent, fontSize: 10.5),
      );
    } else if (status.downloadPhase == ViewerFilePhase.failed &&
        status.downloadError.isNotEmpty) {
      child = Text(
        status.downloadError,
        style: const TextStyle(color: Colors.orangeAccent, fontSize: 10.5),
      );
    } else if (_uploadAwaitingEnd &&
        (status.fileActive || status.fileQueue > 0)) {
      child = Text(
        tr('正在上传到 {dir}（队列剩余 {n} 个）', <String, Object?>{
          'dir': _directory.isEmpty ? tr('共享文件夹') : _directory,
          'n': status.fileQueue,
        }),
        style: const TextStyle(color: Colors.white54, fontSize: 10.5),
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
  late final TextEditingController _name =
      TextEditingController(text: widget.initial);

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
        decoration: InputDecoration(
          hintText: tr('名称'),
          isDense: true,
        ),
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
