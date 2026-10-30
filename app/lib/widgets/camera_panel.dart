import 'package:flutter/material.dart';

import '../theme.dart';
import '../viewer_bridge.dart';
import 'package:chast_viewer/l10n.dart';

/// Host camera preview. The host keeps one camera slot, so picking another
/// while a stream is open asks the page to stop first and then start; the
/// panel itself only renders state and forwards commands.
class CameraPanel extends StatefulWidget {
  const CameraPanel({
    super.key,
    required this.status,
    required this.textureId,
    required this.onRefresh,
    required this.onStart,
    required this.onStop,
    required this.onDismiss,
  });

  final ViewerStatus status;

  /// The plugin texture the native camera sink draws into. It is registered
  /// by the page when the panel opens and released when it closes.
  final int textureId;
  final VoidCallback onRefresh;
  final ValueChanged<int> onStart;
  final VoidCallback onStop;
  final VoidCallback onDismiss;

  @override
  State<CameraPanel> createState() => _CameraPanelState();
}

class _CameraPanelState extends State<CameraPanel> {
  int _selected = 0;

  ViewerStatus get _status => widget.status;
  ViewerCameraPhase get _phase => _status.cameraPhase;
  bool get _listing => _phase == ViewerCameraPhase.listing;
  bool get _starting => _phase == ViewerCameraPhase.starting;
  bool get _streaming => _phase == ViewerCameraPhase.streaming;

  @override
  void initState() {
    super.initState();
    _syncSelection();
  }

  @override
  void didUpdateWidget(CameraPanel oldWidget) {
    super.didUpdateWidget(oldWidget);
    _syncSelection();
  }

  /// Keeps the picker on a camera the newest list still offers: the first
  /// default, otherwise the first entry.
  void _syncSelection() {
    final List<ViewerCamera> cameras = _status.cameras;
    for (final ViewerCamera camera in cameras) {
      if (camera.id == _selected) {
        return;
      }
    }
    int next = cameras.isEmpty ? 0 : cameras.first.id;
    for (final ViewerCamera camera in cameras) {
      if (camera.isDefault) {
        next = camera.id;
        break;
      }
    }
    _selected = next;
  }

  String get _failureText {
    final String text =
        _status.cameraError.isNotEmpty ? _status.cameraError : _status.cameraReason;
    return text.isEmpty ? tr('摄像头打开失败') : text;
  }

  void _onSelected(int? cameraId) {
    if (cameraId == null || cameraId == _selected) {
      return;
    }
    setState(() => _selected = cameraId);
    if (_streaming) {
      // Replacing an open camera goes through the same stop-then-start path
      // as the start button.
      widget.onStart(cameraId);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Material(
      color: const Color(0xFF141519),
      elevation: 12,
      borderRadius: BorderRadius.circular(8),
      child: Container(
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: Colors.white24),
        ),
        clipBehavior: Clip.antiAlias,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            _buildHeader(),
            const Divider(height: 1, thickness: 1, color: Colors.white12),
            if (!_status.cameraSupported) _buildPermissionNote(),
            _buildPreview(),
            _buildControls(),
          ],
        ),
      ),
    );
  }

  Widget _buildHeader() {
    final String title = _streaming && _selected != 0
        ? _cameraLabel(_selected)
        : tr('主机摄像头');
    return Container(
      height: 36,
      color: const Color(0xFF1B1C20),
      padding: const EdgeInsets.only(left: 10, right: 4),
      child: Row(
        children: <Widget>[
          Icon(
            _streaming ? Icons.videocam_rounded : Icons.videocam_outlined,
            size: 16,
            color: _streaming ? kAccentBright : Colors.white54,
          ),
          const SizedBox(width: 8),
          Expanded(
            child: Text(
              title,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: const TextStyle(color: Colors.white70, fontSize: 12),
            ),
          ),
          IconButton(
            icon: const Icon(Icons.refresh_rounded, size: 16),
            color: Colors.white54,
            tooltip: _listing
                ? tr('正在读取摄像头列表')
                : _starting || _streaming
                    ? tr('请先停止摄像头')
                    : tr('重新读取摄像头列表'),
            disabledColor: Colors.white24,
            onPressed: _listing || _starting || _streaming
                ? null
                : widget.onRefresh,
            visualDensity: VisualDensity.compact,
          ),
          IconButton(
            icon: const Icon(Icons.close_rounded, size: 16),
            color: Colors.white54,
            tooltip: tr('关闭摄像头'),
            onPressed: widget.onDismiss,
            visualDensity: VisualDensity.compact,
          ),
        ],
      ),
    );
  }

  Widget _buildPermissionNote() {
    return Container(
      color: const Color(0xFF3A2C12),
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      child: Row(
        children: <Widget>[
          Icon(Icons.info_outline_rounded, size: 13, color: Colors.amberAccent),
          SizedBox(width: 6),
          Expanded(
            child: Text(
              tr('主机没有授予摄像头权限'),
              style: TextStyle(fontSize: 11, color: Colors.amberAccent),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildPreview() {
    final bool hasPicture = _streaming && _status.hasCameraFrame;
    return SizedBox(
      height: 230,
      child: ColoredBox(
        color: const Color(0xFF0C0C0C),
        child: hasPicture
            ? FittedBox(
                fit: BoxFit.contain,
                child: SizedBox(
                  width: _status.cameraWidth.toDouble(),
                  height: _status.cameraHeight.toDouble(),
                  child: Texture(
                    textureId: widget.textureId,
                    filterQuality: FilterQuality.medium,
                  ),
                ),
              )
            : _buildPlaceholder(),
      ),
    );
  }

  Widget _buildPlaceholder() {
    if (_listing) {
      return _CameraNote(
        icon: null,
        text: tr('正在读取主机摄像头…'),
      );
    }
    if (_starting) {
      return _CameraNote(icon: null, text: tr('正在打开摄像头…'));
    }
    if (_phase == ViewerCameraPhase.failed) {
      return _CameraNote(icon: Icons.error_outline_rounded, text: _failureText);
    }
    if (_status.cameras.isEmpty) {
      return _CameraNote(
        icon: Icons.videocam_off_outlined,
        text: _status.cameraListUpdates == 0 ? tr('尚未读取摄像头列表') : tr('主机没有可用的摄像头'),
      );
    }
    if (_phase == ViewerCameraPhase.stopped) {
      return _CameraNote(
        icon: Icons.videocam_off_outlined,
        text: tr('摄像头已停止'),
      );
    }
    return _CameraNote(
      icon: Icons.videocam_outlined,
      text: tr('选择摄像头后开始预览'),
    );
  }

  Widget _buildControls() {
    final List<ViewerCamera> cameras = _status.cameras;
    final bool canPick = cameras.isNotEmpty && !_listing && !_starting;
    final bool canStart =
        _selected != 0 && cameras.isNotEmpty && !_listing && !_starting;
    final String statusText;
    if (_streaming) {
      final String size = _status.hasCameraFrame
          ? '${_status.cameraWidth}×${_status.cameraHeight}'
          : tr('等待画面');
      statusText = tr('{size} · {p3} 帧', <String, Object?>{'size': size, 'p3': _status.cameraFrames});
    } else if (_starting) {
      statusText = tr('正在打开…');
    } else if (_phase == ViewerCameraPhase.failed) {
      statusText = _failureText;
    } else if (_phase == ViewerCameraPhase.listing) {
      statusText = tr('正在读取列表…');
    } else if (_phase == ViewerCameraPhase.stopped) {
      statusText = tr('已停止');
    } else {
      statusText = tr('未开始');
    }
    return Container(
      padding: const EdgeInsets.fromLTRB(10, 8, 10, 10),
      decoration: const BoxDecoration(
        border: Border(top: BorderSide(color: Colors.white12)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          if (cameras.isNotEmpty)
            Container(
              height: 34,
              padding: const EdgeInsets.symmetric(horizontal: 9),
              decoration: BoxDecoration(
                color: const Color(0xFF1B1C20),
                borderRadius: BorderRadius.circular(6),
                border: Border.all(color: Colors.white12),
              ),
              child: DropdownButtonHideUnderline(
                child: DropdownButton<int>(
                  value: _selected == 0 ? null : _selected,
                  isExpanded: true,
                  dropdownColor: const Color(0xFF24252B),
                  hint: Text(
                    tr('选择摄像头'),
                    style: TextStyle(color: Colors.white38, fontSize: 12),
                  ),
                  icon: const Icon(Icons.arrow_drop_down_rounded,
                      color: Colors.white54),
                  style: const TextStyle(color: Colors.white70, fontSize: 12),
                  onChanged: canPick ? _onSelected : null,
                  items: <DropdownMenuItem<int>>[
                    for (final ViewerCamera camera in cameras)
                      DropdownMenuItem<int>(
                        value: camera.id,
                        child: Row(
                          children: <Widget>[
                            Icon(
                              camera.isDefault
                                  ? Icons.star_rounded
                                  : Icons.videocam_outlined,
                              size: 14,
                              color: camera.isDefault
                                  ? kAccentBright
                                  : Colors.white38,
                            ),
                            const SizedBox(width: 7),
                            Expanded(
                              child: Text(
                                camera.label,
                                maxLines: 1,
                                overflow: TextOverflow.ellipsis,
                              ),
                            ),
                          ],
                        ),
                      ),
                  ],
                ),
              ),
            ),
          const SizedBox(height: 8),
          Row(
            children: <Widget>[
              Expanded(
                child: Text(
                  statusText,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    fontSize: 11,
                    color: _phase == ViewerCameraPhase.failed
                        ? Colors.orangeAccent
                        : Colors.white54,
                  ),
                ),
              ),
              if (_phase == ViewerCameraPhase.failed ||
                  _phase == ViewerCameraPhase.listing)
                TextButton(
                  onPressed: _listing ? null : widget.onRefresh,
                  style: TextButton.styleFrom(
                    foregroundColor: Colors.white70,
                    visualDensity: VisualDensity.compact,
                  ),
                  child: Text(tr('重试'), style: TextStyle(fontSize: 12)),
                ),
              const SizedBox(width: 4),
              if (_streaming || _starting)
                OutlinedButton(
                  onPressed: widget.onStop,
                  style: OutlinedButton.styleFrom(
                    foregroundColor: Colors.white70,
                    side: const BorderSide(color: Colors.white24),
                    visualDensity: VisualDensity.compact,
                  ),
                  child: Text(_starting ? tr('取消') : tr('停止'),
                      style: const TextStyle(fontSize: 12)),
                )
              else
                FilledButton(
                  onPressed: canStart ? () => widget.onStart(_selected) : null,
                  style: FilledButton.styleFrom(
                    backgroundColor: kAccentBright,
                    foregroundColor: Colors.black87,
                    disabledBackgroundColor: Colors.white12,
                    disabledForegroundColor: Colors.white38,
                    visualDensity: VisualDensity.compact,
                  ),
                  child: Text(tr('开始'), style: TextStyle(fontSize: 12)),
                ),
            ],
          ),
        ],
      ),
    );
  }

  String _cameraLabel(int id) {
    for (final ViewerCamera camera in _status.cameras) {
      if (camera.id == id) {
        return camera.label;
      }
    }
    return tr('主机摄像头');
  }
}

class _CameraNote extends StatelessWidget {
  const _CameraNote({required this.icon, required this.text});

  final IconData? icon;
  final String text;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 24),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            if (icon == null)
              const SizedBox(
                width: 20,
                height: 20,
                child: CircularProgressIndicator(
                  strokeWidth: 2,
                  color: Colors.white38,
                ),
              )
            else
              Icon(icon, size: 26, color: Colors.white24),
            const SizedBox(height: 10),
            Text(
              text,
              textAlign: TextAlign.center,
              style: const TextStyle(color: Colors.white38, fontSize: 11.5),
            ),
          ],
        ),
      ),
    );
  }
}
