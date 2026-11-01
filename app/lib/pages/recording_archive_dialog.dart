import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../account_store.dart';
import '../server_api.dart';
import '../server_settings.dart';
import '../theme.dart';
import 'package:chast_viewer/l10n.dart';

typedef RecordingArchiveLoader =
    Future<RecordingArchive> Function(ServerSettings settings, String token);
typedef RecordingDownloader =
    Future<File> Function(
      ServerSettings settings,
      String token,
      RemoteRecording recording,
      Directory directory,
    );
typedef RecordingDeleter =
    Future<void> Function(
      ServerSettings settings,
      String token,
      String recordingId,
    );

enum _RecordingSort { time, name, size }

/// Lists the recordings the signed-in account uploaded to the server, with
/// download and delete actions. Downloads go to the user's Videos folder;
/// deleting removes both the server file and its metadata.
class RecordingArchiveDialog extends StatefulWidget {
  const RecordingArchiveDialog({
    super.key,
    required this.server,
    required this.account,
    this.loadArchive,
    this.downloadRecording,
    this.deleteRecording,
    this.downloadDirectory,
    this.onOpenDownload,
    this.onRevealDownload,
  });

  final ServerSettings server;
  final AccountSession account;
  final RecordingArchiveLoader? loadArchive;
  final RecordingDownloader? downloadRecording;
  final RecordingDeleter? deleteRecording;

  /// Overrides the Videos folder; tests pass a temporary directory.
  final Directory? downloadDirectory;

  /// Opens a finished download with its default application. Tests inject a
  /// recorder; the default starts explorer.exe with the file.
  final Future<void> Function(String path)? onOpenDownload;

  /// Opens the shell on a finished download with the file selected. Tests
  /// inject a recorder; the default starts explorer.exe.
  final Future<void> Function(String path)? onRevealDownload;

  @override
  State<RecordingArchiveDialog> createState() => _RecordingArchiveDialogState();
}

class _RecordingArchiveDialogState extends State<RecordingArchiveDialog> {
  RecordingArchive _archive = const RecordingArchive();
  bool _loading = true;
  String _error = '';
  String _downloaded = '';
  String _busyId = '';
  bool _deleting = false;
  String _query = '';
  final TextEditingController _filter = TextEditingController();
  _RecordingSort _sort = _RecordingSort.time;
  bool _sortAscending = false;

  bool get _busy => _busyId.isNotEmpty || _deleting;

  List<RemoteRecording> get _visibleRecordings {
    final String query = _query.trim().toLowerCase();
    final List<RemoteRecording> rows = query.isEmpty
        ? List<RemoteRecording>.of(_archive.recordings)
        : _archive.recordings.where((RemoteRecording recording) {
            return recording.name.toLowerCase().contains(query) ||
                recording.peerId.toLowerCase().contains(query) ||
                recording.id.toLowerCase().contains(query);
          }).toList();
    int compare(RemoteRecording a, RemoteRecording b) {
      int result;
      switch (_sort) {
        case _RecordingSort.time:
          result = a.createdAtMs.compareTo(b.createdAtMs);
        case _RecordingSort.name:
          result = a.name.toLowerCase().compareTo(b.name.toLowerCase());
        case _RecordingSort.size:
          result = a.size.compareTo(b.size);
      }
      if (result == 0) {
        result = a.name.toLowerCase().compareTo(b.name.toLowerCase());
      }
      if (result == 0) {
        result = a.id.compareTo(b.id);
      }
      return _sortAscending ? result : -result;
    }

    rows.sort(compare);
    return rows;
  }

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void dispose() {
    _filter.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    setState(() {
      _loading = true;
      _error = '';
    });
    try {
      final RecordingArchive archive =
          await (widget.loadArchive ?? ServerApi.listRecordings)(
            widget.server,
            widget.account.token,
          );
      if (!mounted) {
        return;
      }
      setState(() {
        _archive = archive;
        _loading = false;
      });
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _error = tr('读取云端录制失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _download(RemoteRecording recording) async {
    if (_busy) {
      return;
    }
    setState(() {
      _busyId = recording.id;
      _error = '';
      _downloaded = '';
    });
    try {
      final Directory directory =
          widget.downloadDirectory ?? _defaultDownloadDirectory();
      final File file =
          await (widget.downloadRecording ?? ServerApi.downloadRecording)(
            widget.server,
            widget.account.token,
            recording,
            directory,
          );
      if (!mounted) {
        return;
      }
      setState(() {
        _busyId = '';
        _downloaded = file.path;
      });
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            tr('已保存到 {path}', <String, Object?>{'path': file.path}),
          ),
        ),
      );
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _busyId = '';
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _busyId = '';
          _error = tr('下载录制失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _delete(RemoteRecording recording) async {
    if (_busy) {
      return;
    }
    final String name = recording.name.isEmpty ? tr('未命名录制') : recording.name;
    final bool confirmed =
        await showDialog<bool>(
          context: context,
          builder: (BuildContext context) => AlertDialog(
            backgroundColor: Theme.of(context).colorScheme.surface,
            title: Text(tr('删除云端录制？')),
            content: Text(
              tr('将从服务器永久删除“{name}”，此操作无法撤销。', <String, Object?>{'name': name}),
            ),
            actions: <Widget>[
              TextButton(
                onPressed: () => Navigator.of(context).pop(false),
                child: Text(tr('取消')),
              ),
              FilledButton(
                key: const Key('recording-delete-confirm'),
                onPressed: () => Navigator.of(context).pop(true),
                style: FilledButton.styleFrom(
                  backgroundColor: Theme.of(context).colorScheme.error,
                ),
                child: Text(tr('删除')),
              ),
            ],
          ),
        ) ??
        false;
    if (!confirmed || !mounted) {
      return;
    }
    setState(() {
      _deleting = true;
      _error = '';
    });
    try {
      await (widget.deleteRecording ?? ServerApi.deleteRecording)(
        widget.server,
        widget.account.token,
        recording.id,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _deleting = false;
        _archive = RecordingArchive(
          recordings: _archive.recordings
              .where((RemoteRecording other) => other.id != recording.id)
              .toList(),
          usageBytes: _archive.usageBytes - recording.size < 0
              ? 0
              : _archive.usageBytes - recording.size,
          quotaBytes: _archive.quotaBytes,
          maxUploadBytes: _archive.maxUploadBytes,
        );
      });
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(tr('已删除“{name}”', <String, Object?>{'name': name})),
        ),
      );
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _deleting = false;
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _deleting = false;
          _error = tr('删除云端录制失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  // Opening and revealing are conveniences, not transfer steps: a shell that
  // cannot start must not replace the saved-path footer with an error.
  Future<void> _openDownload(String path) async {
    final Future<void> Function(String path)? injected = widget.onOpenDownload;
    if (injected != null) {
      await injected(path);
      return;
    }
    try {
      await Process.start('explorer.exe', <String>[path]);
    } on ProcessException {
      // Explorer is unavailable; the footer still names the saved file.
    }
  }

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

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    return AlertDialog(
      backgroundColor: theme.colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: Text(tr('云端录制')),
      content: SizedBox(
        width: 520,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Text(
              tr('这些录制已上传到当前账号。下载保存到本机视频文件夹，删除会同时移除服务器上的文件和记录。'),
              style: mutedStyle(context, size: 12),
            ),
            const SizedBox(height: 10),
            Text(
              _archive.quotaBytes > 0
                  ? tr('已用 {used}，配额 {quota}', <String, Object?>{
                      'used': _formatBytes(_archive.usageBytes),
                      'quota': _formatBytes(_archive.quotaBytes),
                    })
                  : tr('已用 {used}', <String, Object?>{
                      'used': _formatBytes(_archive.usageBytes),
                    }),
              key: const Key('recording-usage'),
              style: mutedStyle(context, size: 11),
            ),
            if (_archive.maxUploadBytes > 0)
              Text(
                tr('单次上传上限 {size}', <String, Object?>{
                  'size': _formatBytes(_archive.maxUploadBytes),
                }),
                style: mutedStyle(context, size: 11),
              ),
            const SizedBox(height: 12),
            if (!_loading && _archive.recordings.isNotEmpty) ...<Widget>[
              Row(
                children: <Widget>[
                  Expanded(
                    child: TextField(
                      key: const Key('recordings-filter'),
                      controller: _filter,
                      style: const TextStyle(fontSize: 12),
                      decoration: InputDecoration(
                        isDense: true,
                        hintText: tr('筛选录制（名称、设备 ID 或录制 ID）'),
                        hintStyle: const TextStyle(fontSize: 12),
                        prefixIcon: const Icon(Icons.search_rounded, size: 18),
                        suffixIcon: _query.isEmpty
                            ? null
                            : IconButton(
                                key: const Key('recordings-filter-clear'),
                                tooltip: tr('清除筛选'),
                                icon: const Icon(Icons.close_rounded, size: 16),
                                visualDensity: VisualDensity.compact,
                                onPressed: () {
                                  _filter.clear();
                                  setState(() => _query = '');
                                },
                              ),
                        border: OutlineInputBorder(
                          borderRadius: BorderRadius.circular(10),
                        ),
                      ),
                      onChanged: (String value) =>
                          setState(() => _query = value),
                    ),
                  ),
                  const SizedBox(width: 4),
                  PopupMenuButton<_RecordingSort>(
                    key: const Key('recordings-sort'),
                    tooltip: tr('排序方式'),
                    icon: const Icon(Icons.sort_rounded, size: 18),
                    onSelected: (_RecordingSort value) {
                      setState(() {
                        _sort = value;
                        _sortAscending = value == _RecordingSort.name;
                      });
                    },
                    itemBuilder: (BuildContext context) =>
                        <PopupMenuEntry<_RecordingSort>>[
                          CheckedPopupMenuItem<_RecordingSort>(
                            key: const Key('recordings-sort-time'),
                            value: _RecordingSort.time,
                            checked: _sort == _RecordingSort.time,
                            child: Text(tr('按时间')),
                          ),
                          CheckedPopupMenuItem<_RecordingSort>(
                            key: const Key('recordings-sort-name'),
                            value: _RecordingSort.name,
                            checked: _sort == _RecordingSort.name,
                            child: Text(tr('按名称')),
                          ),
                          CheckedPopupMenuItem<_RecordingSort>(
                            key: const Key('recordings-sort-size'),
                            value: _RecordingSort.size,
                            checked: _sort == _RecordingSort.size,
                            child: Text(tr('按大小')),
                          ),
                        ],
                  ),
                  IconButton(
                    key: const Key('recordings-sort-direction'),
                    tooltip: _sortAscending ? tr('降序') : tr('升序'),
                    visualDensity: VisualDensity.compact,
                    icon: Icon(
                      _sortAscending
                          ? Icons.arrow_upward_rounded
                          : Icons.arrow_downward_rounded,
                      size: 18,
                    ),
                    onPressed: () =>
                        setState(() => _sortAscending = !_sortAscending),
                  ),
                ],
              ),
              const SizedBox(height: 10),
            ],
            if (_loading)
              const Padding(
                padding: EdgeInsets.symmetric(vertical: 28),
                child: Center(child: CircularProgressIndicator()),
              )
            else if (_archive.recordings.isEmpty)
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 18),
                child: Text(
                  tr('没有云端录制。'),
                  key: const Key('recordings-empty'),
                  style: mutedStyle(context, size: 12),
                ),
              )
            else if (_visibleRecordings.isEmpty)
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 18),
                child: Text(
                  tr('没有匹配的录制'),
                  key: const Key('recordings-no-match'),
                  style: mutedStyle(context, size: 12),
                ),
              )
            else
              ConstrainedBox(
                constraints: const BoxConstraints(maxHeight: 300),
                child: ListView.separated(
                  key: const Key('recordings-list'),
                  shrinkWrap: true,
                  itemCount: _visibleRecordings.length,
                  separatorBuilder: (BuildContext context, int index) =>
                      const SizedBox(height: 8),
                  itemBuilder: (BuildContext context, int index) {
                    return _recordingTile(context, _visibleRecordings[index]);
                  },
                ),
              ),
            if (_downloaded.isNotEmpty) ...<Widget>[
              const SizedBox(height: 8),
              Row(
                children: <Widget>[
                  Expanded(
                    child: Text(
                      tr('已保存到 {path}', <String, Object?>{'path': _downloaded}),
                      key: const Key('recordings-saved'),
                      style: mutedStyle(context, size: 10.5),
                    ),
                  ),
                  IconButton(
                    key: const Key('recordings-open'),
                    tooltip: tr('打开文件'),
                    visualDensity: VisualDensity.compact,
                    icon: const Icon(Icons.open_in_new_rounded, size: 16),
                    onPressed: () => _openDownload(_downloaded),
                  ),
                  IconButton(
                    key: const Key('recordings-reveal'),
                    tooltip: tr('在文件夹中显示'),
                    visualDensity: VisualDensity.compact,
                    icon: const Icon(Icons.folder_open_rounded, size: 16),
                    onPressed: () => _revealDownload(_downloaded),
                  ),
                ],
              ),
            ],
            if (_error.isNotEmpty) ...<Widget>[
              const SizedBox(height: 10),
              Text(
                _error,
                key: const Key('recordings-error'),
                style: TextStyle(color: theme.colorScheme.error, fontSize: 12),
              ),
            ],
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          key: const Key('recordings-refresh'),
          onPressed: _loading || _busy ? null : _load,
          child: Text(tr('刷新')),
        ),
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(tr('关闭')),
        ),
      ],
    );
  }

  Future<void> _copyId(RemoteRecording recording) async {
    bool copied = true;
    try {
      await Clipboard.setData(ClipboardData(text: recording.id));
    } catch (_) {
      copied = false;
    }
    if (!mounted) {
      return;
    }
    if (!copied) {
      setState(() => _error = tr('复制失败'));
      return;
    }
    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text(tr('ID 已复制'))));
  }

  Widget _recordingTile(BuildContext context, RemoteRecording recording) {
    final ThemeData theme = Theme.of(context);
    final String name = recording.name.isEmpty ? tr('未命名录制') : recording.name;
    final DateTime? created = recording.createdAt;
    final List<String> details = <String>[
      _formatBytes(recording.size),
      if (created != null) _formatTime(created),
      if (recording.durationMs > 0) _formatDuration(recording.durationMs),
      if (recording.peerId.isNotEmpty)
        tr('来自 {peer}', <String, Object?>{'peer': recording.peerId}),
    ];
    return Container(
      key: ValueKey<String>('recording-${recording.id}'),
      padding: const EdgeInsets.all(10),
      decoration: BoxDecoration(
        color: theme.colorScheme.surfaceContainerHighest.withValues(alpha: 0.4),
        borderRadius: BorderRadius.circular(10),
      ),
      child: Row(
        children: <Widget>[
          const Icon(Icons.movie_outlined, size: 20, color: kAccent),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(
                  name,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    fontSize: 13,
                    fontWeight: FontWeight.w600,
                    color: theme.colorScheme.onSurface,
                  ),
                ),
                Text(
                  details.join(' · '),
                  style: mutedStyle(context, size: 10.5),
                ),
              ],
            ),
          ),
          IconButton(
            key: ValueKey<String>('recording-copy-${recording.id}'),
            onPressed: () => _copyId(recording),
            tooltip: tr('复制 ID'),
            visualDensity: VisualDensity.compact,
            icon: const Icon(Icons.copy_rounded, size: 17),
          ),
          IconButton(
            key: ValueKey<String>('recording-download-${recording.id}'),
            onPressed: _busy ? null : () => _download(recording),
            tooltip: tr('下载'),
            visualDensity: VisualDensity.compact,
            icon: _busyId == recording.id
                ? const SizedBox(
                    width: 14,
                    height: 14,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : const Icon(Icons.download_rounded, size: 18),
          ),
          IconButton(
            key: ValueKey<String>('recording-delete-${recording.id}'),
            onPressed: _busy ? null : () => _delete(recording),
            tooltip: tr('删除'),
            visualDensity: VisualDensity.compact,
            icon: const Icon(Icons.delete_outline_rounded, size: 18),
          ),
        ],
      ),
    );
  }
}

Directory _defaultDownloadDirectory() {
  final String home =
      Platform.environment['USERPROFILE'] ?? Platform.environment['HOME'] ?? '';
  if (home.isEmpty) {
    return Directory.systemTemp;
  }
  return Directory(
    '$home${Platform.pathSeparator}Videos'
    '${Platform.pathSeparator}chast-recordings',
  );
}

String _formatBytes(int bytes) {
  if (bytes < 1024) {
    return '$bytes B';
  }
  if (bytes < 1024 * 1024) {
    return '${(bytes / 1024).toStringAsFixed(1)} KB';
  }
  if (bytes < 1024 * 1024 * 1024) {
    return '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MB';
  }
  return '${(bytes / (1024 * 1024 * 1024)).toStringAsFixed(2)} GB';
}

String _formatTime(DateTime time) {
  String two(int value) => value.toString().padLeft(2, '0');
  return '${time.year}-${two(time.month)}-${two(time.day)} '
      '${two(time.hour)}:${two(time.minute)}';
}

String _formatDuration(int milliseconds) {
  final int seconds = (milliseconds / 1000).round();
  final int hours = seconds ~/ 3600;
  final int minutes = (seconds % 3600) ~/ 60;
  final int rest = seconds % 60;
  String two(int value) => value.toString().padLeft(2, '0');
  return hours > 0
      ? '$hours:${two(minutes)}:${two(rest)}'
      : '${two(minutes)}:${two(rest)}';
}
