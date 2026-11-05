import 'dart:async';

import 'package:flutter/material.dart';

import '../theme.dart';
import '../viewer_bridge.dart';
import 'remote_view.dart';
import 'package:chast_viewer/l10n.dart';

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
    required this.voiceAvailable,
    required this.voiceEnabled,
    required this.voicePending,
    required this.onToggleVoiceCall,
    required this.audioAec,
    required this.audioNoiseSuppression,
    required this.onLoadMicrophones,
    required this.onSelectMicrophone,
    required this.onSetAudioProcessing,
    required this.privacyAvailable,
    required this.privacyEnabled,
    this.osLoginAvailable = false,
    this.onOsLogin,
    this.sessionSelectAvailable = false,
    this.onSessionSelect,
    required this.portForwardAvailable,
    required this.portForwardActive,
    required this.onOpenPortForward,
    required this.displays,
    required this.activeDisplay,
    required this.onSelectDisplay,
    required this.followCursor,
    required this.followWindow,
    required this.onToggleFollowCursor,
    required this.onToggleFollowWindow,
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
    required this.onSendText,
    this.screenshotAvailable = false,
    this.onScreenshot,
    required this.onToggleClipboard,
    required this.onSendFile,
    required this.fileBrowseOpen,
    required this.onToggleFileBrowse,
    required this.chatOpen,
    required this.onToggleChat,
    required this.terminalAvailable,
    required this.terminalOpen,
    required this.onToggleTerminal,
    required this.cameraAvailable,
    required this.cameraOpen,
    required this.onToggleCamera,
    required this.onToggleAudio,
    required this.onToggleMicrophone,
    required this.onTogglePrivacy,
    required this.restartAvailable,
    required this.lockAvailable,
    required this.blockInputAvailable,
    required this.blockInputActive,
    required this.onHostAction,
    this.recordingAvailable = false,
    this.recordingDenied = false,
    this.recording = false,
    this.onToggleRecording,
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
  final bool voiceAvailable;
  final bool voiceEnabled;
  final bool voicePending;
  final VoidCallback onToggleVoiceCall;
  final bool audioAec;
  final bool audioNoiseSuppression;
  final Future<List<ViewerMicrophoneDevice>> Function() onLoadMicrophones;
  final Future<void> Function(String deviceId) onSelectMicrophone;
  final Future<void> Function(bool aec, bool noiseSuppression) onSetAudioProcessing;
  final bool privacyAvailable;
  final bool privacyEnabled;

  /// The host granted input and OS login, so the viewer may offer a password
  /// dialog. The host still refuses the password unless it is at a logon or
  /// lock screen, which is why the button stays available at other desktops.
  final bool osLoginAvailable;
  final VoidCallback? onOsLogin;

  /// The host's service backend lets the viewer list and switch the Windows
  /// session it streams from. The page owns the picker and the requests.
  final bool sessionSelectAvailable;
  final VoidCallback? onSessionSelect;
  final bool restartAvailable;
  final bool lockAvailable;
  final bool blockInputAvailable;
  final bool blockInputActive;
  final ValueChanged<ViewerHostAction> onHostAction;

  /// A viewer-side port-forward listener is running and the host accepted
  /// the target; the button stays enabled either way so the dialog can stop
  /// or replace it.
  final bool portForwardAvailable;
  final bool portForwardActive;
  final VoidCallback onOpenPortForward;
  final List<ViewerDisplay> displays;
  final int activeDisplay;
  final ValueChanged<int> onSelectDisplay;

  /// The host switched output to the display under the remote pointer (or
  /// the window with foreground focus) on its own.
  final bool followCursor;
  final bool followWindow;
  final VoidCallback onToggleFollowCursor;
  final VoidCallback onToggleFollowWindow;
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
  final VoidCallback onSendText;

  /// Viewer-side screenshot: the engine reads the newest decoded picture back
  /// from the GPU and writes a PNG on this machine. Optional so tests and
  /// sessions without a picture keep the toolbar unchanged.
  final bool screenshotAvailable;
  final VoidCallback? onScreenshot;
  final VoidCallback onToggleClipboard;
  final VoidCallback onSendFile;

  /// The shared-folder browser is open. It needs the same permission as
  /// uploads, so it shares [fileAvailable].
  final bool fileBrowseOpen;
  final VoidCallback onToggleFileBrowse;
  final bool chatOpen;
  final VoidCallback onToggleChat;
  final bool terminalAvailable;
  final bool terminalOpen;
  final VoidCallback onToggleTerminal;
  final bool cameraAvailable;
  final bool cameraOpen;
  final VoidCallback onToggleCamera;
  final VoidCallback onToggleAudio;
  final VoidCallback onToggleMicrophone;
  final VoidCallback onTogglePrivacy;

  /// Viewer-side recording. Recording is local to this viewer: nothing is sent
  /// to the host and nothing is uploaded. The switch is optional so tests and
  /// hosts without a session can keep the toolbar unchanged.
  final bool recordingAvailable;
  /// The session is live but the host did not grant recording consent, which
  /// calls for a different tooltip than "connect first".
  final bool recordingDenied;
  final bool recording;
  final VoidCallback? onToggleRecording;
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
        _audioOutputItem('', tr('系统默认输出'), tr('跟随 Windows 默认设备')),
        for (final ViewerAudioOutput device in devices)
          _audioOutputItem(device.id, device.label,
              device.isDefault ? tr('系统默认设备') : tr('输出设备')),
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
        _processingItem(_MicSetting.aec, tr('回声消除'), tr('用本机播放的系统声音作参考'), audioAec),
        _processingItem(_MicSetting.noiseSuppression, tr('噪声抑制'), tr('抑制平稳底噪，语音优先'),
            audioNoiseSuppression),
        const PopupMenuDivider(),
        _microphoneItem('', tr('系统默认麦克风'), tr('跟随默认通信设备')),
        for (final ViewerMicrophoneDevice device in devices)
          _microphoneItem(device.id, device.label,
              device.isDefault ? tr('系统默认设备') : tr('输入设备')),
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
        child: FittedBox(
          fit: BoxFit.scaleDown,
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              _BarButton(
                icon: pinned ? Icons.push_pin_rounded : Icons.push_pin_outlined,
                tooltip: pinned ? tr('取消固定') : tr('固定工具栏'),
                active: pinned,
                onTap: onTogglePin,
              ),
              _FitButton(fit: fit, onChanged: onFitChanged),
              const _BarSeparator(),
              _DisplayButton(
                displays: displays,
                active: activeDisplay,
                followCursor: followCursor,
                followWindow: followWindow,
                inputAvailable: inputAvailable,
                onSelected: onSelectDisplay,
                onToggleFollowCursor: onToggleFollowCursor,
                onToggleFollowWindow: onToggleFollowWindow,
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
                    ? tr('服务端未开放输入')
                    : controlEnabled
                        ? tr('关闭远程键鼠')
                        : tr('开启远程键鼠'),
                disabled: !inputAvailable,
                active: controlEnabled,
                onTap: onToggleControl,
              ),
              _SpecialKeysButton(
                available: specialKeysAvailable,
                onSelected: onSpecialAction,
              ),
              _BarButton(
                icon: Icons.text_fields_rounded,
                tooltip: specialKeysAvailable
                    ? tr('发送文本')
                    : tr('需要远程键鼠控制'),
                disabled: !specialKeysAvailable,
                onTap: onSendText,
              ),
              _BarButton(
                icon: Icons.photo_camera_rounded,
                tooltip: !screenshotAvailable
                    ? tr('会话建立后可截图')
                    : tr('截图（保存为本地 PNG）'),
                disabled: !screenshotAvailable,
                onTap: () => onScreenshot?.call(),
              ),
              _BarButton(
                icon: clipboardEnabled
                    ? Icons.content_paste_go_rounded
                    : Icons.content_paste_off_rounded,
                tooltip: !clipboardAvailable
                    ? tr('服务端未开放剪贴板')
                    : clipboardEnabled
                        ? tr('暂停剪贴板同步')
                        : tr('恢复剪贴板同步'),
                disabled: !clipboardAvailable,
                active: clipboardAvailable && clipboardEnabled,
                onTap: onToggleClipboard,
              ),
              _BarButton(
                icon: Icons.upload_file_rounded,
                tooltip: !fileAvailable
                    ? tr('服务端未开放文件传输')
                    : fileActive
                        ? tr('文件正在发送')
                        : tr('发送文件到主机'),
                disabled: !fileAvailable || fileActive,
                active: fileActive,
                onTap: onSendFile,
              ),
              _BarButton(
                icon: fileBrowseOpen
                    ? Icons.folder_open_rounded
                    : Icons.folder_outlined,
                tooltip: !fileAvailable
                    ? tr('服务端未开放文件传输')
                    : fileBrowseOpen
                        ? tr('关闭主机文件浏览')
                        : tr('浏览主机共享文件夹'),
                disabled: !fileAvailable,
                active: fileBrowseOpen,
                onTap: onToggleFileBrowse,
              ),
              _BarButton(
                icon: chatOpen ? Icons.forum_rounded : Icons.forum_outlined,
                tooltip: chatOpen ? tr('关闭会话聊天') : tr('会话聊天'),
                active: chatOpen,
                onTap: onToggleChat,
              ),
              _BarButton(
                icon: Icons.terminal_rounded,
                tooltip: !terminalAvailable
                    ? tr('服务端未开放远程终端')
                    : terminalOpen
                        ? tr('关闭远程终端')
                        : tr('远程终端'),
                disabled: !terminalAvailable,
                active: terminalOpen,
                onTap: onToggleTerminal,
              ),
              _BarButton(
                icon: cameraOpen
                    ? Icons.videocam_rounded
                    : Icons.videocam_outlined,
                tooltip: !cameraAvailable
                    ? tr('服务端未开放摄像头')
                    : cameraOpen
                        ? tr('关闭远程摄像头')
                        : tr('查看主机摄像头'),
                disabled: !cameraAvailable,
                active: cameraOpen,
                onTap: onToggleCamera,
              ),
              _BarButton(
                icon: audioMuted ? Icons.volume_off_rounded : Icons.volume_up_rounded,
                tooltip: !audioAvailable
                    ? tr('服务端未启用声音')
                    : audioMuted
                        ? tr('取消静音（长按选择输出设备）')
                        : tr('静音（长按选择输出设备）'),
                disabled: !audioAvailable,
                active: audioAvailable && !audioMuted,
                onTap: onToggleAudio,
                onLongPress: (BuildContext context) =>
                    unawaited(_openAudioOutputMenu(context)),
              ),
              _BarButton(
                icon: microphoneEnabled ? Icons.mic_rounded : Icons.mic_off_rounded,
                tooltip: !microphoneAvailable
                    ? tr('服务端未开放麦克风')
                    : microphoneEnabled
                        ? tr('关闭麦克风（长按选择设备）')
                        : tr('开启麦克风（长按选择设备）'),
                disabled: !microphoneAvailable,
                active: microphoneEnabled,
                onTap: onToggleMicrophone,
                onLongPress: (BuildContext context) =>
                    unawaited(_openMicrophoneMenu(context)),
              ),
              _BarButton(
                icon: voiceEnabled
                    ? Icons.phone_in_talk_rounded
                    : Icons.phone_outlined,
                tooltip: !voiceAvailable
                    ? tr('服务端未开放语音通话')
                    : voicePending
                        ? tr('正在接通主机麦克风…')
                        : voiceEnabled
                            ? tr('结束语音通话')
                            : tr('收听主机麦克风'),
                disabled: !voiceAvailable || voicePending,
                active: voiceEnabled,
                onTap: onToggleVoiceCall,
              ),
              _BarButton(
                icon: privacyEnabled
                    ? Icons.shield_rounded
                    : Icons.shield_outlined,
                tooltip: !privacyAvailable
                    ? tr('服务端未开放隐私模式')
                    : privacyEnabled
                        ? tr('关闭隐私模式（恢复主机本地画面）')
                        : tr('开启隐私模式（遮挡主机本地画面）'),
                disabled: !privacyAvailable,
                active: privacyEnabled,
                onTap: onTogglePrivacy,
              ),
              _BarButton(
                icon: Icons.password_rounded,
                tooltip: osLoginAvailable
                    ? tr('在主机登录或锁屏界面输入密码')
                    : tr('服务端未开放系统登录'),
                disabled: !osLoginAvailable,
                onTap: onOsLogin,
              ),
              _BarButton(
                icon: Icons.swap_horiz_rounded,
                tooltip: sessionSelectAvailable
                    ? tr('切换主机的 Windows 会话')
                    : tr('服务端未开放会话切换'),
                disabled: !sessionSelectAvailable,
                onTap: onSessionSelect,
              ),
              _HostPowerButton(
                restartAvailable: restartAvailable,
                lockAvailable: lockAvailable,
                blockInputAvailable: blockInputAvailable,
                blockInputActive: blockInputActive,
                onSelected: onHostAction,
              ),
              _BarButton(
                icon: recording
                    ? Icons.stop_circle_rounded
                    : Icons.fiber_manual_record_rounded,
                tooltip: !recordingAvailable
                    ? (recordingDenied ? tr('主机未开放录制') : tr('会话建立后可录制到本地'))
                    : recording
                        ? tr('停止录制（保存为本地 MP4）')
                        : tr('录制到本地（保存为 MP4）'),
                disabled: !recordingAvailable,
                active: recording,
                onTap: () => onToggleRecording?.call(),
              ),
              _BarButton(
                icon: portForwardActive
                    ? Icons.alt_route_rounded
                    : Icons.alt_route,
                tooltip: !portForwardAvailable
                    ? tr('服务端未开放端口转发')
                    : portForwardActive
                        ? tr('端口转发进行中（点击查看或停止）')
                        : tr('端口转发'),
                disabled: !portForwardAvailable,
                active: portForwardActive,
                onTap: onOpenPortForward,
              ),
              _BarButton(
                icon: Icons.speed_rounded,
                tooltip: tr('显示或隐藏统计'),
                active: statsVisible,
                onTap: onToggleStats,
              ),
              const _BarSeparator(),
              _BarButton(
                icon: fullscreen ? Icons.fullscreen_exit_rounded : Icons.fullscreen_rounded,
                tooltip: fullscreen ? tr('退出全屏') : tr('全屏'),
                active: fullscreen,
                onTap: onToggleFullscreen,
              ),
              _BarButton(
                icon: Icons.expand_less_rounded,
                tooltip: tr('收起工具栏'),
                onTap: onCollapse,
              ),
              const SizedBox(width: 2),
              _BarButton(
                icon: Icons.close_rounded,
                tooltip: tr('断开连接'),
                danger: true,
                onTap: onClose,
              ),
            ],
          ),
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
        _item(RemoteSpecialAction.win, Icons.window_rounded, 'Win', tr('开始菜单')),
        _item(RemoteSpecialAction.altTab, Icons.swap_horiz_rounded, 'Alt+Tab',
            tr('切换窗口')),
        _item(RemoteSpecialAction.altF4, Icons.close_rounded, 'Alt+F4', tr('关闭当前窗口')),
        _item(RemoteSpecialAction.taskManager, Icons.task_alt_rounded,
            'Ctrl+Shift+Esc', tr('任务管理器')),
        const PopupMenuDivider(),
        _item(RemoteSpecialAction.sas, Icons.lock_reset_rounded, 'Ctrl+Alt+Del',
            tr('安全注意序列（需主机以服务方式运行）')),
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
        tooltip: available ? tr('特殊按键') : tr('先开启远程键鼠'),
        disabled: !available,
        onTap: () => unawaited(_open(buttonContext)),
      ),
    );
  }
}

/// Host power and local-input controls. Every entry is gated by the host's
/// permission; the restart confirmation lives in the session page so this
/// widget stays stateless.
class _HostPowerButton extends StatelessWidget {
  const _HostPowerButton({
    required this.restartAvailable,
    required this.lockAvailable,
    required this.blockInputAvailable,
    required this.blockInputActive,
    required this.onSelected,
  });

  final bool restartAvailable;
  final bool lockAvailable;
  final bool blockInputAvailable;
  final bool blockInputActive;
  final ValueChanged<ViewerHostAction> onSelected;

  Future<void> _open(BuildContext context) async {
    final RenderBox? box = context.findRenderObject() as RenderBox?;
    if (box == null) {
      return;
    }
    final Offset origin = box.localToGlobal(Offset.zero);
    final ViewerHostAction? selected = await showMenu<ViewerHostAction>(
      context: context,
      position: RelativeRect.fromLTRB(
        origin.dx,
        origin.dy + box.size.height,
        origin.dx + 300,
        origin.dy + box.size.height,
      ),
      color: const Color(0xFF24252B),
      elevation: 8,
      items: <PopupMenuEntry<ViewerHostAction>>[
        _item(
          ViewerHostAction.lockScreen,
          Icons.lock_outline_rounded,
          tr('锁定主机屏幕'),
          tr('保留远程画面，重新登录需要凭据'),
          enabled: lockAvailable,
        ),
        _item(
          ViewerHostAction.blockInput,
          blockInputActive
              ? Icons.pause_circle_rounded
              : Icons.pause_circle_outline_rounded,
          blockInputActive ? tr('恢复主机本地输入') : tr('阻止主机本地输入'),
          tr('需要主机同时开放键鼠与阻止输入'),
          enabled: blockInputAvailable,
        ),
        const PopupMenuDivider(),
        _item(
          ViewerHostAction.restart,
          Icons.power_settings_new_rounded,
          tr('重启主机…'),
          tr('会中断本次会话，确认后立即重启'),
          enabled: restartAvailable,
          danger: true,
        ),
      ],
    );
    if (selected != null) {
      onSelected(selected);
    }
  }

  PopupMenuItem<ViewerHostAction> _item(
    ViewerHostAction action,
    IconData icon,
    String title,
    String detail, {
    required bool enabled,
    bool danger = false,
  }) {
    return PopupMenuItem<ViewerHostAction>(
      value: action,
      height: 44,
      enabled: enabled,
      child: Row(
        children: <Widget>[
          Icon(
            icon,
            size: 16,
            color: !enabled
                ? Colors.white24
                : danger
                    ? Colors.redAccent
                    : Colors.white54,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                Text(
                  title,
                  style: TextStyle(
                    fontSize: 12,
                    color: enabled ? Colors.white70 : Colors.white24,
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

  @override
  Widget build(BuildContext context) {
    final bool any = restartAvailable || lockAvailable || blockInputAvailable;
    return Builder(
      builder: (BuildContext buttonContext) => _BarButton(
        icon: Icons.power_settings_new_rounded,
        tooltip: any ? tr('主机控制（锁屏、阻止输入、重启）') : tr('服务端未开放主机控制'),
        disabled: !any,
        active: blockInputActive,
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
      message: tr('画面适应方式'),
      waitDuration: const Duration(milliseconds: 450),
      child: PopupMenuButton<ViewFit>(
        tooltip: '',
        color: const Color(0xFF24252B),
        elevation: 8,
        padding: EdgeInsets.zero,
        onSelected: onChanged,
        itemBuilder: (BuildContext context) {
          return <PopupMenuEntry<ViewFit>>[
            _fitItem(ViewFit.adaptive, tr('自适应窗口'), Icons.fit_screen_rounded),
            _fitItem(ViewFit.original, tr('原始大小'), Icons.crop_original_rounded),
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
    required this.followCursor,
    required this.followWindow,
    required this.inputAvailable,
    required this.onSelected,
    required this.onToggleFollowCursor,
    required this.onToggleFollowWindow,
  });

  final List<ViewerDisplay> displays;
  final int active;
  final bool followCursor;
  final bool followWindow;
  final bool inputAvailable;
  final ValueChanged<int> onSelected;
  final VoidCallback onToggleFollowCursor;
  final VoidCallback onToggleFollowWindow;

  // Negative ids keep the follow toggles apart from display indexes and the
  // kViewerDisplayAuto/kViewerDisplayAll sentinels.
  static const int _followCursorItem = -1;
  static const int _followWindowItem = -2;

  @override
  Widget build(BuildContext context) {
    final bool available = displays.isNotEmpty;
    final bool followAvailable = available && inputAvailable && active != kViewerDisplayAll;
    return Tooltip(
      message: available
          ? tr('切换显示器')
          : tr('服务端尚未公布显示器列表'),
      waitDuration: const Duration(milliseconds: 450),
      child: PopupMenuButton<int>(
        enabled: available,
        tooltip: '',
        color: const Color(0xFF24252B),
        elevation: 8,
        padding: EdgeInsets.zero,
        constraints: const BoxConstraints(minWidth: 210, maxWidth: 300),
        onSelected: (int value) {
          if (value == _followCursorItem) {
            onToggleFollowCursor();
          } else if (value == _followWindowItem) {
            onToggleFollowWindow();
          } else {
            onSelected(value);
          }
        },
        itemBuilder: (BuildContext context) {
          return <PopupMenuEntry<int>>[
            _displayItem(kViewerDisplayAuto, tr('服务端默认'), tr('由服务端选择输出'),
                Icons.auto_mode_rounded),
            _displayItem(kViewerDisplayAll, tr('全部显示器（合并）'),
                _mergedDetail(displays), Icons.grid_view_rounded),
            for (final ViewerDisplay display in displays)
              _displayItem(
                display.index,
                display.label.isEmpty ? tr('显示器 {p1}', <String, Object?>{'p1': display.index}) : display.label,
                _detail(display),
                Icons.monitor_rounded,
              ),
            const PopupMenuDivider(height: 8),
            _followItem(
              _followCursorItem,
              tr('跟随远程光标'),
              followCursor,
              followAvailable,
              Icons.ads_click_rounded,
            ),
            _followItem(
              _followWindowItem,
              tr('跟随远程窗口'),
              followWindow,
              followAvailable,
              Icons.web_asset_rounded,
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
      text.write(tr(' · 主屏'));
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
    return tr('{p5}×{p6} · {p7} 块屏', <String, Object?>{'p5': right - left, 'p6': bottom - top, 'p7': displays.length});
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

  /// Follow rows stay visible but disabled when the host cannot act on them:
  /// one display needs no switching, the merged stream already shows every
  /// output, and changing output requires input permission.
  PopupMenuItem<int> _followItem(
    int value,
    String title,
    bool enabledFlag,
    bool availableFlag,
    IconData icon,
  ) {
    final Color color = !availableFlag
        ? Colors.white24
        : enabledFlag
            ? kAccentBright
            : Colors.white70;
    return PopupMenuItem<int>(
      value: value,
      height: 40,
      enabled: availableFlag,
      child: Row(
        children: <Widget>[
          Icon(icon, size: 16, color: color),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              title,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: TextStyle(fontSize: 12, color: color),
            ),
          ),
          if (enabledFlag && availableFlag)
            const Icon(Icons.check_rounded, size: 15, color: kAccentBright),
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
        ? tr('服务端未开放输入，无法修改分辨率')
        : modes.isEmpty
            ? tr('正在读取主机的显示模式')
            : tr('修改分辨率（会话内生效，断开后恢复）');
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
      message: available ? tr('画面质量、帧率上限与编码器') : tr('连接后可以调整画面'),
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
            _section(tr('画质')),
            _item(_QualityChoice(ViewerStreamQuality.high), tr('高清'), tr('细节优先，占用最高带宽'),
                Icons.high_quality_rounded, quality == ViewerStreamQuality.high),
            _item(_QualityChoice(ViewerStreamQuality.balanced), tr('均衡'), tr('默认，细节与带宽兼顾'),
                Icons.balance_rounded, quality == ViewerStreamQuality.balanced),
            _item(_QualityChoice(ViewerStreamQuality.smooth), tr('流畅'), tr('带宽最低，优先帧率与延迟'),
                Icons.speed_rounded, quality == ViewerStreamQuality.smooth),
            _section(tr('帧率上限')),
            _fpsItem(0, tr('自动'), tr('跟随主机设置')),
            for (final int cap in _fpsCaps) _fpsItem(cap, '$cap fps', tr('最高 {cap} 帧/秒', <String, Object?>{'cap': cap})),
            _section(tr('编码器')),
            _item(_CodecChoice(ViewerStreamCodec.auto), tr('自动'), tr('由主机选择，优先 HEVC'),
                Icons.auto_awesome_rounded, codec == ViewerStreamCodec.auto),
            _item(_CodecChoice(ViewerStreamCodec.h264), 'H.264', tr('兼容性最好'),
                Icons.movie_outlined, codec == ViewerStreamCodec.h264),
            _item(_CodecChoice(ViewerStreamCodec.hevc), 'HEVC', tr('同画质更省带宽'),
                Icons.hd_outlined, codec == ViewerStreamCodec.hevc),
            _item(_CodecChoice(ViewerStreamCodec.av1), 'AV1', tr('压缩率更高，需要 AV1 硬件'),
                Icons.auto_awesome_motion_outlined, codec == ViewerStreamCodec.av1),
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
