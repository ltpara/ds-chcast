import 'viewer_bridge.dart';

/// Returns true once for each new voice-call failure. Native counters cover
/// refusals and decode/setup failures, while a host-initiated close can update
/// only [ViewerStatus.voiceError], so both have to participate in detection.
bool viewerVoiceFailureRaised(ViewerStatus previous, ViewerStatus status) {
  if (status.voiceErrors > previous.voiceErrors) {
    return true;
  }
  return status.voiceError.isNotEmpty &&
      status.voiceError != previous.voiceError;
}

/// Returns true once for each new system-audio failure. [audioErrors] also
/// catches stream/decode errors without a dedicated reason string; a changed
/// [audioError] catches decoder/device setup and output fallback notices; the
/// enabled transition catches a renderer that stopped without either.
bool viewerAudioFailureRaised(ViewerStatus previous, ViewerStatus status) {
  if (!status.audioAnnounced) {
    return false;
  }
  if (previous.audioEnabled && !status.audioEnabled) {
    return true;
  }
  if (status.audioError.isNotEmpty &&
      status.audioError != previous.audioError) {
    return true;
  }
  return status.audioErrors > previous.audioErrors;
}

/// Names every known permission bit in protocol order. Any reserved or future
/// bit outside the known mask is reported as a generic permission rather than
/// dropped, so a live permission-update notice can never become silent.
List<String> viewerPermissionNames(
  int mask, {
  String Function(String source)? translate,
}) {
  final String Function(String source) tr =
      translate ?? (String source) => source;
  final List<String> parts = <String>[];
  void add(int bit, String name) {
    if ((mask & bit) != 0) {
      parts.add(tr(name));
    }
  }

  add(ViewerPermission.input, '输入控制');
  add(ViewerPermission.clipboard, '剪贴板');
  add(ViewerPermission.audio, '音频');
  add(ViewerPermission.fileTransfer, '文件传输');
  add(ViewerPermission.microphone, '麦克风');
  add(ViewerPermission.privacyMode, '隐私模式');
  add(ViewerPermission.portForward, '端口转发');
  add(ViewerPermission.terminal, '终端');
  add(ViewerPermission.record, '录制');
  add(ViewerPermission.camera, '摄像头');
  add(ViewerPermission.restart, '重启');
  add(ViewerPermission.blockInput, '阻止输入');
  add(ViewerPermission.osLogin, '系统登录');
  add(ViewerPermission.sessionSelect, '会话切换');
  add(ViewerPermission.remoteConfig, '远程配置');

  const int known =
      ViewerPermission.input |
      ViewerPermission.clipboard |
      ViewerPermission.audio |
      ViewerPermission.fileTransfer |
      ViewerPermission.microphone |
      ViewerPermission.privacyMode |
      ViewerPermission.portForward |
      ViewerPermission.terminal |
      ViewerPermission.record |
      ViewerPermission.camera |
      ViewerPermission.restart |
      ViewerPermission.blockInput |
      ViewerPermission.osLogin |
      ViewerPermission.sessionSelect |
      ViewerPermission.remoteConfig;
  if ((mask & ~known) != 0) {
    parts.add(tr('其他远程权限'));
  }
  return parts;
}
