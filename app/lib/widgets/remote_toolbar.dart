import 'dart:async';

import 'package:flutter/material.dart';

import '../theme.dart';
import '../viewer_bridge.dart';
import 'remote_view.dart';

/// A system shortcut the toolbar can ask the host to inject. The key chords
/// travel as ordinary HID events; [sas] is the exception because Windows only
/// accepts Ctrl+Alt+Del through SendSAS, which the host's service can call.
enum RemoteSpecialAction {
  win,
  altTab,
  altF4,
  taskManager,
  sas,
}

/// The two microphone processing switches the long-press menu exposes above
/// the device list.
enum _MicSetting { aec, noiseSuppression }

class RemoteToolbar extends StatelessWidget {
  const RemoteToolbar({
    super.key,
    required this.collapsed,
    required this.pinned,
    required this.fullscreen,
    required this.statsVisible,
    required this.fit,
    required this.controlEnabled,
    required this.inputAvailable,
    required this.clipboardEnabled,
    required this.clipboardAvailable,
    required this.fileAvailable,
    required this.fileActive,
    required this.audioAvailable,
    required this.audioMuted,
    required this.audioDevice,
    required this.onLoadAudioOutputs,
    required this.onSelectAudioOutput,
    required this.microphoneAvailable,
    required this.microphoneEnabled,
    required this.microphoneDevice,
    required this.audioAec,
    required this.audioNoiseSuppression,
    required this.onLoadMicrophones,
    required this.onSelectMicrophone,
    required this.onSetAudioProcessing,
    required this.privacyAvailable,
    required this.privacyEnabled,
    required this.displays,
    required this.activeDisplay,
    required this.onSelectDisplay,
    required this.modes,
    required this.modeChangeAvailable,
    required this.onSelectMode,
    required this.streamOptionsAvailable,
    required this.streamCodec,
    required this.streamQuality,
    required this.streamMaxFps,
    required this.onSelectStreamCodec,
    required this.onSelectStreamQuality,
    required this.onSelectStreamFps,
    required this.onToggleControl,
    required this.specialKeysAvailable,
    required this.onSpecialAction,
    required this.onToggleClipboard,
    required this.onSendFile,
    required this.chatOpen,
    required this.onToggleChat,
    required this.onToggleAudio,
    required this.onToggleMicrophone,
    required this.onTogglePrivacy,
    required this.onTogglePin,
    required this.onToggleStats,
    required this.onToggleFullscreen,
    required this.onCollapse,
    required this.onExpand,
    required this.onClose,
    required this.onFitChanged,
  });

  final bool collapsed;
  final bool pinned;
  final bool fullscreen;
  final bool statsVisible;
  final ViewFit fit;
  final bool controlEnabled;
  final bool inputAvailable;
  final bool clipboardEnabled;
  final bool clipboardAvailable;
  final bool fileAvailable;
  final bool fileActive;
  final bool audioAvailable;
  final bool audioMuted;

  /// The pinned playback endpoint id; empty follows the system default (the
  /// menu marks that entry).
  final String audioDevice;
  final Future<List<ViewerAudioOutput>> Function() onLoadAudioOutputs;
  final Future<void> Function(String deviceId) onSelectAudioOutput;
  final bool microphoneAvailable;
  final bool microphoneEnabled;

  /// The pinned capture endpoint id; empty follows the default communications
  /// device (the menu marks that entry).
  final String microphoneDevice;
  final bool audioAec;
  final bool audioNoiseSuppression;
  final Future<List<ViewerMicrophoneDevice>> Function() onLoadMicrophones;
  final Future<void> Function(String deviceId) onSelectMicrophone;
  final Future<void> Function(bool aec, bool noiseSuppression) onSetAudioProcessing;
  final bool privacyAvailable;
  final bool privacyEnabled;
  final List<ViewerDisplay> displays;
  final int activeDisplay;
  final ValueChanged<int> onSelectDisplay;
  final List<ViewerDisplayMode> modes;
  final bool modeChangeAvailable;
  final ValueChanged<ViewerDisplayMode> onSelectMode;
  final bool streamOptionsAvailable;
  final ViewerStreamCodec streamCodec;
  final ViewerStreamQuality streamQuality;
  final int streamMaxFps;
  final ValueChanged<ViewerStreamCodec> onSelectStreamCodec;
  final ValueChanged<ViewerStreamQuality> onSelectStreamQuality;
  final ValueChanged<int> onSelectStreamFps;
  final VoidCallback onToggleControl;
  /// True while remote control is on, so a shortcut would actually reach the
  /// host. The button stays visible but disabled otherwise.
  final bool specialKeysAvailable;
  final ValueChanged<RemoteSpecialAction> onSpecialAction;
  final VoidCallback onToggleClipboard;
  final VoidCallback onSendFile;
  final bool chatOpen;
  final VoidCallback onToggleChat;
  final VoidCallback onToggleAudio;
  final VoidCallback onToggleMicrophone;
  final VoidCallback onTogglePrivacy;
  final VoidCallback onTogglePin;
  final VoidCallback onToggleStats;
  final VoidCallback onToggleFullscreen;
  final VoidCallback onCollapse;
  final VoidCallback onExpand;
  final VoidCallback onClose;
  final ValueChanged<ViewFit> onFitChanged;

  /// Lists the playback endpoints at the moment of the long press, so a
  /// device plugged in after the session started is offered too.
  Future<void> _openAudioOutputMenu(BuildContext context) async {
    final List<ViewerAudioOutput> devices = await onLoadAudioOutputs();
    if (!context.mounted) {
      return;
    }
    final RenderBox? box = context.findRenderObject() as RenderBox?;
    if (box == null) {
      return;
    }
    final Offset origin = box.localToGlobal(Offset.zero);
    final String? selected = await showMenu<String>(
      context: context,
      position: RelativeRect.fromLTRB(
        origin.dx,
        origin.dy + box.size.height,
        origin.dx + 300,
        origin.dy + box.size.height,
      ),
      color: const Color(0xFF24252B),
      elevation: 8,
      items: <PopupMenuEntry<String>>[
        _audioOutputItem('', '系统默认输出', '跟随 Windows 默认设备'),
        for (final ViewerAudioOutput device in devices)
          _audioOutputItem(device.id, device.label,
              device.isDefault ? '系统默认设备' : '输出设备'),
      ],
    );
    if (selected != null && selected != audioDevice) {
      await onSelectAudioOutput(selected);
    }
  }

  PopupMenuItem<String> _audioOutputItem(String id, String title, String detail) {
    final bool selected = id == audioDevice;
    return PopupMenuItem<String>(
      value: id,
      height: 46,
      child: Row(
        children: <Widget>[
          Icon(
            id.isEmpty ? Icons.speaker_rounded : Icons.volume_up_rounded,
            size: 16,
            color: selected ? kAccentBright : Colors.white54,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                Text(
                  title,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    fontSize: 12,
                    color: selected ? kAccentBright : Colors.white70,
                  ),
                ),
                Text(
                  detail,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(fontSize: 10, color: Colors.white38),
                ),
              ],
            ),
          ),
          if (selected) const Icon(Icons.check_rounded, size: 15, color: kAccentBright),
        ],
      ),
    );
  }

  /// Lists the processing switches and capture endpoints at the moment of the
  /// long press, so a device plugged in after the session started is offered
  /// too.
  Future<void> _openMicrophoneMenu(BuildContext context) async {
    final List<ViewerMicrophoneDevice> devices = await onLoadMicrophones();
    if (!context.mounted) {
      return;
    }
    final RenderBox? box = context.findRenderObject() as RenderBox?;
    if (box == null) {
      return;
    }
    final Offset origin = box.localToGlobal(Offset.zero);
    final Object? selected = await showMenu<Object>(
      context: context,
      position: RelativeRect.fromLTRB(
        origin.dx,
        origin.dy + box.size.height,
        origin.dx + 300,
        origin.dy + box.size.height,
      ),
      color: const Color(0xFF24252B),
      elevation: 8,
      items: <PopupMenuEntry<Object>>[
        _processingItem(_MicSetting.aec, '回声消除', '用本机播放的系统声音作参考', audioAec),
        _processingItem(_MicSetting.noiseSuppression, '噪声抑制', '抑制平稳底噪，语音优先',
            audioNoiseSuppression),
        const PopupMenuDivider(),
        _microphoneItem('', '系统默认麦克风', '跟随默认通信设备'),
        for (final ViewerMicrophoneDevice device in devices)
          _microphoneItem(device.id, device.label,
              device.isDefault ? '系统默认设备' : '输入设备'),
      ],
    );
    if (selected is _MicSetting) {
      await onSetAudioProcessing(
        selected == _MicSetting.aec ? !audioAec : audioAec,
        selected == _MicSetting.noiseSuppression ? !audioNoiseSuppression : audioNoiseSuppression,
      );
    } else if (selected is String && selected != microphoneDevice) {
      await onSelectMicrophone(selected);
    }
  }

  PopupMenuItem<Object> _processingItem(
      _MicSetting setting, String title, String detail, bool enabled) {
    return PopupMenuItem<Object>(
      value: setting,
      height: 44,
      child: Row(
        children: <Widget>[
          Icon(
            enabled ? Icons.check_box_rounded : Icons.check_box_outline_blank_rounded,
            size: 16,
            color: enabled ? kAccentBright : Colors.white54,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                Text(
                  title,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    fontSize: 12,
                    color: enabled ? kAccentBright : Colors.white70,
                  ),
                ),
                Text(
                  detail,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(fontSize: 10, color: Colors.white38),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  PopupMenuItem<Object> _microphoneItem(String id, String title, String detail) {
    final bool selected = id == microphoneDevice;
    return PopupMenuItem<Object>(
      value: id,
      height: 46,
      child: Row(
        children: <Widget>[
          Icon(
            id.isEmpty ? Icons.settings_voice_rounded : Icons.mic_none_rounded,
            size: 16,
            color: selected ? kAccentBright : Colors.white54,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                Text(
                  title,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    fontSize: 12,
                    color: selected ? kAccentBright : Colors.white70,
                  ),
                ),
                Text(
                  detail,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(fontSize: 10, color: Colors.white38),
                ),
              ],
            ),
          ),
          if (selected) const Icon(Icons.check_rounded, size: 15, color: kAccentBright),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    if (collapsed) {
      return _CollapsedBar(
        onExpand: onExpand,
        onToggleFullscreen: onToggleFullscreen,
        fullscreen: fullscreen,
      );
    }
    return Material(
      color: kToolbarDark,
      elevation: 3,
      borderRadius: BorderRadius.circular(4),
      child: Container(
        height: 40,
        padding: const EdgeInsets.symmetric(horizontal: 4),
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(4),
          border: Border.all(color: Colors.white24),
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            _BarButton(
              icon: pinned ? Icons.push_pin_rounded : Icons.push_pin_outlined,
              tooltip: pinned ? '取消固定' : '固定工具栏',
              active: pinned,
              onTap: onTogglePin,
            ),
            _FitButton(fit: fit, onChanged: onFitChanged),
            const _BarSeparator(),
            _DisplayButton(
              displays: displays,
              active: activeDisplay,
              onSelected: onSelectDisplay,
            ),
            if (activeDisplay != kViewerDisplayAll)
              _ResolutionButton(
                modes: modes,
                available: modeChangeAvailable,
                onSelected: onSelectMode,
              ),
            _StreamOptionsButton(
              available: streamOptionsAvailable,
              codec: streamCodec,
              quality: streamQuality,
              maxFps: streamMaxFps,
              onSelectCodec: onSelectStreamCodec,
              onSelectQuality: onSelectStreamQuality,
              onSelectFps: onSelectStreamFps,
            ),
            _BarButton(
              icon: Icons.keyboard_alt_outlined,
              tooltip: !inputAvailable
                  ? '服务端未开放输入'
                  : controlEnabled
                      ? '关闭远程键鼠'
                      : '开启远程键鼠',
              disabled: !inputAvailable,
              active: controlEnabled,
              onTap: onToggleControl,
            ),
            _SpecialKeysButton(
              available: specialKeysAvailable,
              onSelected: onSpecialAction,
            ),
            _BarButton(
              icon: clipboardEnabled
                  ? Icons.content_paste_go_rounded
                  : Icons.content_paste_off_rounded,
              tooltip: !clipboardAvailable
                  ? '服务端未开放剪贴板'
                  : clipboardEnabled
                      ? '暂停剪贴板同步'
                      : '恢复剪贴板同步',
              disabled: !clipboardAvailable,
              active: clipboardAvailable && clipboardEnabled,
              onTap: onToggleClipboard,
            ),
            _BarButton(
              icon: Icons.upload_file_rounded,
              tooltip: !fileAvailable
                  ? '服务端未开放文件传输'
                  : fileActive
                      ? '文件正在发送'
                      : '发送文件到主机',
              disabled: !fileAvailable || fileActive,
              active: fileActive,
              onTap: onSendFile,
            ),
            _BarButton(
              icon: chatOpen ? Icons.forum_rounded : Icons.forum_outlined,
              tooltip: chatOpen ? '关闭会话聊天' : '会话聊天',
              active: chatOpen,
              onTap: onToggleChat,
            ),
            _BarButton(
              icon: audioMuted ? Icons.volume_off_rounded : Icons.volume_up_rounded,
              tooltip: !audioAvailable
                  ? '服务端未启用声音'
                  : audioMuted
                      ? '取消静音（长按选择输出设备）'
                      : '静音（长按选择输出设备）',
              disabled: !audioAvailable,
              active: audioAvailable && !audioMuted,
              onTap: onToggleAudio,
              onLongPress: (BuildContext context) =>
                  unawaited(_openAudioOutputMenu(context)),
            ),
            _BarButton(
              icon: microphoneEnabled ? Icons.mic_rounded : Icons.mic_off_rounded,
              tooltip: !microphoneAvailable
                  ? '服务端未开放麦克风'
                  : microphoneEnabled
                      ? '关闭麦克风（长按选择设备）'
                      : '开启麦克风（长按选择设备）',
              disabled: !microphoneAvailable,
              active: microphoneEnabled,
              onTap: onToggleMicrophone,
              onLongPress: (BuildContext context) =>
                  unawaited(_openMicrophoneMenu(context)),
            ),
            _BarButton(
              icon: privacyEnabled
                  ? Icons.shield_rounded
                  : Icons.shield_outlined,
              tooltip: !privacyAvailable
                  ? '服务端未开放隐私模式'
                  : privacyEnabled
                      ? '关闭隐私模式（恢复主机本地画面）'
                      : '开启隐私模式（遮挡主机本地画面）',
              disabled: !privacyAvailable,
              active: privacyEnabled,
              onTap: onTogglePrivacy,
            ),
            _BarButton(
              icon: Icons.speed_rounded,
              tooltip: '显示或隐藏统计',
              active: statsVisible,
              onTap: onToggleStats,
            ),
            const _BarSeparator(),
            _BarButton(
              icon: fullscreen ? Icons.fullscreen_exit_rounded : Icons.fullscreen_rounded,
              tooltip: fullscreen ? '退出全屏' : '全屏',
              active: fullscreen,
              onTap: onToggleFullscreen,
            ),
            _BarButton(
              icon: Icons.expand_less_rounded,
              tooltip: '收起工具栏',
              onTap: onCollapse,
            ),
            const SizedBox(width: 2),
            _BarButton(
              icon: Icons.close_rounded,
              tooltip: '断开连接',
              danger: true,
              onTap: onClose,
            ),
          ],
        ),
      ),
    );
  }
}

class _CollapsedBar extends StatelessWidget {
  const _CollapsedBar({
    required this.onExpand,
    required this.onToggleFullscreen,
    required this.fullscreen,
  });

  final VoidCallback onExpand;
  final VoidCallback onToggleFullscreen;
  final bool fullscreen;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: kToolbarDark,
      elevation: 3,
      borderRadius: BorderRadius.circular(5),
      child: Container(
        width: 136,
        height: 20,
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(5),
          border: Border.all(color: Colors.white24),
        ),
        child: Row(
          children: <Widget>[
            const SizedBox(width: 6),
            const Icon(Icons.drag_indicator_rounded, size: 12, color: Colors.white30),
            Expanded(
              child: InkWell(
                borderRadius: BorderRadius.circular(4),
                onTap: onExpand,
                child: const Center(
                  child: Icon(Icons.expand_more_rounded, size: 15, color: Colors.grey),
                ),
              ),
            ),
            InkWell(
              borderRadius: BorderRadius.circular(4),
              onTap: onToggleFullscreen,
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 3),
                child: Icon(
                  fullscreen ? Icons.fullscreen_exit_rounded : Icons.fullscreen_rounded,
                  size: 13,
                  color: Colors.grey,
                ),
              ),
            ),
            const SizedBox(width: 4),
          ],
        ),
      ),
    );
  }
}

/// Lists the system shortcuts. The menu is built on demand so the host's
/// service capability does not have to be known up front; a refusal from the
/// host explains itself in the session page.
class _SpecialKeysButton extends StatelessWidget {
  const _SpecialKeysButton({required this.available, required this.onSelected});

  final bool available;
  final ValueChanged<RemoteSpecialAction> onSelected;

  Future<void> _open(BuildContext context) async {
    final RenderBox? box = context.findRenderObject() as RenderBox?;
    if (box == null) {
      return;
    }
    final Offset origin = box.localToGlobal(Offset.zero);
    final RemoteSpecialAction? selected = await showMenu<RemoteSpecialAction>(
      context: context,
      position: RelativeRect.fromLTRB(
        origin.dx,
        origin.dy + box.size.height,
        origin.dx + 280,
        origin.dy + box.size.height,
      ),
      color: const Color(0xFF24252B),
      elevation: 8,
      items: <PopupMenuEntry<RemoteSpecialAction>>[
        _item(RemoteSpecialAction.win, Icons.window_rounded, 'Win', '开始菜单'),
        _item(RemoteSpecialAction.altTab, Icons.swap_horiz_rounded, 'Alt+Tab',
            '切换窗口'),
        _item(RemoteSpecialAction.altF4, Icons.close_rounded, 'Alt+F4', '关闭当前窗口'),
        _item(RemoteSpecialAction.taskManager, Icons.task_alt_rounded,
            'Ctrl+Shift+Esc', '任务管理器'),
        const PopupMenuDivider(),
        _item(RemoteSpecialAction.sas, Icons.lock_reset_rounded, 'Ctrl+Alt+Del',
            '安全注意序列（需主机以服务方式运行）'),
      ],
    );
    if (selected != null) {
      onSelected(selected);
    }
  }

  PopupMenuItem<RemoteSpecialAction> _item(
      RemoteSpecialAction action, IconData icon, String title, String detail) {
    return PopupMenuItem<RemoteSpecialAction>(
      value: action,
      height: 44,
      child: Row(
        children: <Widget>[
          Icon(icon, size: 16, color: Colors.white54),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                Text(
                  title,
                  style: const TextStyle(fontSize: 12, color: Colors.white70),
                ),
                Text(
                  detail,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(fontSize: 10, color: Colors.white38),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Builder(
      builder: (BuildContext buttonContext) => _BarButton(
        icon: Icons.keyboard_command_key_rounded,
        tooltip: available ? '特殊按键' : '先开启远程键鼠',
        disabled: !available,
        onTap: () => unawaited(_open(buttonContext)),
      ),
    );
  }
}

class _BarButton extends StatelessWidget {
  const _BarButton({
    required this.icon,
    required this.tooltip,
    this.onTap,
    this.onLongPress,
    this.active = false,
    this.danger = false,
    this.disabled = false,
  });

  final IconData icon;
  final String tooltip;
  final VoidCallback? onTap;
  final void Function(BuildContext)? onLongPress;
  final bool active;
  final bool danger;
  final bool disabled;

  @override
  Widget build(BuildContext context) {
    final Color color = disabled
        ? Colors.white24
        : danger
            ? Colors.redAccent
            : active
                ? kAccentBright
                : Colors.grey.shade300;
    return Tooltip(
      message: tooltip,
      waitDuration: const Duration(milliseconds: 450),
      child: SizedBox(
        width: 32,
        height: 32,
        child: Material(
          color: Colors.transparent,
          borderRadius: BorderRadius.circular(4),
          child: InkWell(
            borderRadius: BorderRadius.circular(4),
            hoverColor: Colors.white.withValues(alpha: 0.08),
            onTap: disabled ? null : onTap,
            onLongPress:
                disabled || onLongPress == null ? null : () => onLongPress!(context),
            child: Icon(icon, size: 18, color: color),
          ),
        ),
      ),
    );
  }
}

class _FitButton extends StatelessWidget {
  const _FitButton({required this.fit, required this.onChanged});

  final ViewFit fit;
  final ValueChanged<ViewFit> onChanged;

  @override
  Widget build(BuildContext context) {
    return Tooltip(
      message: '画面适应方式',
      waitDuration: const Duration(milliseconds: 450),
      child: PopupMenuButton<ViewFit>(
        tooltip: '',
        color: const Color(0xFF24252B),
        elevation: 8,
        padding: EdgeInsets.zero,
        onSelected: onChanged,
        itemBuilder: (BuildContext context) {
          return <PopupMenuEntry<ViewFit>>[
            _fitItem(ViewFit.adaptive, '自适应窗口', Icons.fit_screen_rounded),
            _fitItem(ViewFit.original, '原始大小', Icons.crop_original_rounded),
          ];
        },
        child: SizedBox(
          width: 32,
          height: 32,
          child: Icon(
            fit == ViewFit.original ? Icons.crop_original_rounded : Icons.fit_screen_rounded,
            size: 18,
            color: Colors.grey.shade300,
          ),
        ),
      ),
    );
  }

  PopupMenuItem<ViewFit> _fitItem(ViewFit value, String label, IconData icon) {
    final bool selected = fit == value;
    return PopupMenuItem<ViewFit>(
      value: value,
      height: 38,
      child: Row(
        children: <Widget>[
          Icon(icon, size: 16, color: selected ? kAccentBright : Colors.white54),
          const SizedBox(width: 10),
          Text(
            label,
            style: TextStyle(
              fontSize: 12,
              color: selected ? kAccentBright : Colors.white70,
            ),
          ),
          const Spacer(),
          if (selected) const Icon(Icons.check_rounded, size: 15, color: kAccentBright),
        ],
      ),
    );
  }
}

class _DisplayButton extends StatelessWidget {
  const _DisplayButton({
    required this.displays,
    required this.active,
    required this.onSelected,
  });

  final List<ViewerDisplay> displays;
  final int active;
  final ValueChanged<int> onSelected;

  @override
  Widget build(BuildContext context) {
    final bool available = displays.isNotEmpty;
    return Tooltip(
      message: available
          ? '切换显示器'
          : '服务端尚未公布显示器列表',
      waitDuration: const Duration(milliseconds: 450),
      child: PopupMenuButton<int>(
        enabled: available,
        tooltip: '',
        color: const Color(0xFF24252B),
        elevation: 8,
        padding: EdgeInsets.zero,
        constraints: const BoxConstraints(minWidth: 210, maxWidth: 300),
        onSelected: onSelected,
        itemBuilder: (BuildContext context) {
          return <PopupMenuEntry<int>>[
            _displayItem(kViewerDisplayAuto, '服务端默认', '由服务端选择输出',
                Icons.auto_mode_rounded),
            _displayItem(kViewerDisplayAll, '全部显示器（合并）',
                _mergedDetail(displays), Icons.grid_view_rounded),
            for (final ViewerDisplay display in displays)
              _displayItem(
                display.index,
                display.label.isEmpty ? '显示器 ${display.index}' : display.label,
                _detail(display),
                Icons.monitor_rounded,
              ),
          ];
        },
        child: SizedBox(
          width: 32,
          height: 32,
          child: Icon(
            Icons.monitor_rounded,
            size: 18,
            color: available ? Colors.grey.shade300 : Colors.white24,
          ),
        ),
      ),
    );
  }

  static String _detail(ViewerDisplay display) {
    final StringBuffer text = StringBuffer('${display.width}×${display.height}');
    if (display.primary) {
      text.write(' · 主屏');
    }
    if (display.rotation != 0) {
      text.write(' · ${display.rotation}°');
    }
    return text.toString();
  }

  /// The merged stream's canvas covers the union of every listed output, so
  /// the subtitle reports that union instead of one display's size.
  static String _mergedDetail(List<ViewerDisplay> displays) {
    final ViewerDisplay first = displays.first;
    int left = first.left;
    int top = first.top;
    int right = first.right;
    int bottom = first.bottom;
    for (final ViewerDisplay display in displays.skip(1)) {
      if (display.left < left) {
        left = display.left;
      }
      if (display.top < top) {
        top = display.top;
      }
      if (display.right > right) {
        right = display.right;
      }
      if (display.bottom > bottom) {
        bottom = display.bottom;
      }
    }
    return '${right - left}×${bottom - top} · ${displays.length} 块屏';
  }

  PopupMenuItem<int> _displayItem(int value, String title, String detail, IconData icon) {
    final bool selected = value == active;
    return PopupMenuItem<int>(
      value: value,
      height: 46,
      child: Row(
        children: <Widget>[
          Icon(icon, size: 16, color: selected ? kAccentBright : Colors.white54),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                Text(
                  title,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    fontSize: 12,
                    color: selected ? kAccentBright : Colors.white70,
                  ),
                ),
                Text(
                  detail,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(fontSize: 10, color: Colors.white38),
                ),
              ],
            ),
          ),
          if (selected) const Icon(Icons.check_rounded, size: 15, color: kAccentBright),
        ],
      ),
    );
  }
}

class _ResolutionButton extends StatelessWidget {
  const _ResolutionButton({
    required this.modes,
    required this.available,
    required this.onSelected,
  });

  final List<ViewerDisplayMode> modes;
  final bool available;
  final ValueChanged<ViewerDisplayMode> onSelected;

  @override
  Widget build(BuildContext context) {
    final bool enabled = available && modes.isNotEmpty;
    final String tooltip = !available
        ? '服务端未开放输入，无法修改分辨率'
        : modes.isEmpty
            ? '正在读取主机的显示模式'
            : '修改分辨率（会话内生效，断开后恢复）';
    return Tooltip(
      message: tooltip,
      waitDuration: const Duration(milliseconds: 450),
      child: PopupMenuButton<ViewerDisplayMode>(
        enabled: enabled,
        tooltip: '',
        color: const Color(0xFF24252B),
        elevation: 8,
        padding: EdgeInsets.zero,
        constraints: const BoxConstraints(minWidth: 190, maxWidth: 280),
        onSelected: onSelected,
        itemBuilder: (BuildContext context) {
          return <PopupMenuEntry<ViewerDisplayMode>>[
            for (final ViewerDisplayMode mode in modes)
              PopupMenuItem<ViewerDisplayMode>(
                value: mode,
                height: 42,
                child: Row(
                  children: <Widget>[
                    Icon(
                      Icons.aspect_ratio_rounded,
                      size: 16,
                      color: mode.current ? kAccentBright : Colors.white54,
                    ),
                    const SizedBox(width: 10),
                    Expanded(
                      child: Text(
                        mode.label,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: TextStyle(
                          fontSize: 12,
                          color: mode.current ? kAccentBright : Colors.white70,
                        ),
                      ),
                    ),
                    if (mode.current)
                      const Icon(Icons.check_rounded, size: 15, color: kAccentBright),
                  ],
                ),
              ),
          ];
        },
        child: SizedBox(
          width: 32,
          height: 32,
          child: Icon(
            Icons.aspect_ratio_rounded,
            size: 18,
            color: enabled ? Colors.grey.shade300 : Colors.white24,
          ),
        ),
      ),
    );
  }
}

sealed class _StreamChoice {
  const _StreamChoice();
}

class _QualityChoice extends _StreamChoice {
  const _QualityChoice(this.value);
  final ViewerStreamQuality value;
}

class _CodecChoice extends _StreamChoice {
  const _CodecChoice(this.value);
  final ViewerStreamCodec value;
}

class _FpsChoice extends _StreamChoice {
  const _FpsChoice(this.value);
  final int value;
}

/// Encoder, quality preset and frame-rate cap for the running session. The
/// host answers every change with the values it actually runs, so the page
/// can put a refused choice back on the previous one.
class _StreamOptionsButton extends StatelessWidget {
  const _StreamOptionsButton({
    required this.available,
    required this.codec,
    required this.quality,
    required this.maxFps,
    required this.onSelectCodec,
    required this.onSelectQuality,
    required this.onSelectFps,
  });

  final bool available;
  final ViewerStreamCodec codec;
  final ViewerStreamQuality quality;
  final int maxFps;
  final ValueChanged<ViewerStreamCodec> onSelectCodec;
  final ValueChanged<ViewerStreamQuality> onSelectQuality;
  final ValueChanged<int> onSelectFps;

  static const List<int> _fpsCaps = <int>[15, 30, 60];

  @override
  Widget build(BuildContext context) {
    return Tooltip(
      message: available ? '画面质量、帧率上限与编码器' : '连接后可以调整画面',
      waitDuration: const Duration(milliseconds: 450),
      child: PopupMenuButton<_StreamChoice>(
        enabled: available,
        tooltip: '',
        color: const Color(0xFF24252B),
        elevation: 8,
        padding: EdgeInsets.zero,
        constraints: const BoxConstraints(minWidth: 230, maxWidth: 300),
        onSelected: (choice) {
          if (choice is _QualityChoice) {
            onSelectQuality(choice.value);
          } else if (choice is _CodecChoice) {
            onSelectCodec(choice.value);
          } else if (choice is _FpsChoice) {
            onSelectFps(choice.value);
          }
        },
        itemBuilder: (BuildContext context) {
          return <PopupMenuEntry<_StreamChoice>>[
            _section('画质'),
            _item(_QualityChoice(ViewerStreamQuality.high), '高清', '细节优先，占用最高带宽',
                Icons.high_quality_rounded, quality == ViewerStreamQuality.high),
            _item(_QualityChoice(ViewerStreamQuality.balanced), '均衡', '默认，细节与带宽兼顾',
                Icons.balance_rounded, quality == ViewerStreamQuality.balanced),
            _item(_QualityChoice(ViewerStreamQuality.smooth), '流畅', '带宽最低，优先帧率与延迟',
                Icons.speed_rounded, quality == ViewerStreamQuality.smooth),
            _section('帧率上限'),
            _fpsItem(0, '自动', '跟随主机设置'),
            for (final int cap in _fpsCaps) _fpsItem(cap, '$cap fps', '最高 $cap 帧/秒'),
            _section('编码器'),
            _item(_CodecChoice(ViewerStreamCodec.auto), '自动', '由主机选择，优先 HEVC',
                Icons.auto_awesome_rounded, codec == ViewerStreamCodec.auto),
            _item(_CodecChoice(ViewerStreamCodec.h264), 'H.264', '兼容性最好',
                Icons.movie_outlined, codec == ViewerStreamCodec.h264),
            _item(_CodecChoice(ViewerStreamCodec.hevc), 'HEVC', '同画质更省带宽',
                Icons.hd_outlined, codec == ViewerStreamCodec.hevc),
          ];
        },
        child: SizedBox(
          width: 32,
          height: 32,
          child: Icon(
            Icons.tune_rounded,
            size: 18,
            color: available ? Colors.grey.shade300 : Colors.white24,
          ),
        ),
      ),
    );
  }

  PopupMenuItem<_StreamChoice> _section(String title) {
    return PopupMenuItem<_StreamChoice>(
      enabled: false,
      height: 28,
      child: Text(
        title,
        style: const TextStyle(fontSize: 10, color: Colors.white38, letterSpacing: 0.6),
      ),
    );
  }

  PopupMenuItem<_StreamChoice> _fpsItem(int cap, String title, String detail) {
    return _item(_FpsChoice(cap), title, detail, Icons.timer_outlined, maxFps == cap);
  }

  PopupMenuItem<_StreamChoice> _item(_StreamChoice choice, String title, String detail,
      IconData icon, bool selected) {
    return PopupMenuItem<_StreamChoice>(
      value: choice,
      height: 42,
      child: Row(
        children: <Widget>[
          Icon(icon, size: 16, color: selected ? kAccentBright : Colors.white54),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                Text(
                  title,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    fontSize: 12,
                    color: selected ? kAccentBright : Colors.white70,
                  ),
                ),
                Text(
                  detail,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(fontSize: 10, color: Colors.white38),
                ),
              ],
            ),
          ),
          if (selected) const Icon(Icons.check_rounded, size: 15, color: kAccentBright),
        ],
      ),
    );
  }
}

class _BarSeparator extends StatelessWidget {
  const _BarSeparator();

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 1,
      height: 18,
      margin: const EdgeInsets.symmetric(horizontal: 4),
      color: Colors.white12,
    );
  }
}
