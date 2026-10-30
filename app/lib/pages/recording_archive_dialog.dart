import 'dart:io';

import 'package:flutter/material.dart';

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
  });

  final ServerSettings server;
  final AccountSession account;
  final RecordingArchiveLoader? loadArchive;
  final RecordingDownloader? downloadRecording;
  final RecordingDeleter? deleteRecording;

  /// Overrides the Videos folder; tests pass a temporary directory.
  final Directory? downloadDirectory;

  @override
  State<RecordingArchiveDialog> createState() =>
      _RecordingArchiveDialogState();
}

class _RecordingArchiveDialogState extends State<RecordingArchiveDialog> {
  RecordingArchive _archive = const RecordingArchive();
  bool _loading = true;
  String _error = '';
  String _downloaded = '';
  String _busyId = '';
  bool _deleting = false;

  bool get _busy => _busyId.isNotEmpty || _deleting;

  @override
  void initState() {
    super.initState();
    _load();
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
        SnackBar(content: Text(tr('已保存到 {path}', <String, Object?>{'path': file.path}))),
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
    final String name = recording.name.isEmpty
        ? tr('未命名录制')
        : recording.name;
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
        SnackBar(content: Text(tr('已删除“{name}”', <String, Object?>{'name': name}))),
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
                  ? tr('已用 {used}，配额 {quota}', <String, Object?>{'used': _formatBytes(_archive.usageBytes), 'quota': _formatBytes(_archive.quotaBytes)})
                  : tr('已用 {used}', <String, Object?>{'used': _formatBytes(_archive.usageBytes)}),
              key: const Key('recording-usage'),
              style: mutedStyle(context, size: 11),
            ),
            if (_archive.maxUploadBytes > 0)
              Text(
                tr('单次上传上限 {size}', <String, Object?>{'size': _formatBytes(_archive.maxUploadBytes)}),
                style: mutedStyle(context, size: 11),
              ),
            const SizedBox(height: 12),
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
            else
              ConstrainedBox(
                constraints: const BoxConstraints(maxHeight: 300),
                child: ListView.separated(
                  key: const Key('recordings-list'),
                  shrinkWrap: true,
                  itemCount: _archive.recordings.length,
                  separatorBuilder: (BuildContext context, int index) =>
                      const SizedBox(height: 8),
                  itemBuilder: (BuildContext context, int index) {
                    return _recordingTile(context, _archive.recordings[index]);
                  },
                ),
              ),
            if (_downloaded.isNotEmpty) ...<Widget>[
              const SizedBox(height: 8),
              Text(
                tr('已保存到 {path}', <String, Object?>{'path': _downloaded}),
                key: const Key('recordings-saved'),
                style: mutedStyle(context, size: 10.5),
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

  Widget _recordingTile(BuildContext context, RemoteRecording recording) {
    final ThemeData theme = Theme.of(context);
    final String name = recording.name.isEmpty
        ? tr('未命名录制')
        : recording.name;
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
                Text(details.join(' · '), style: mutedStyle(context, size: 10.5)),
              ],
            ),
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
