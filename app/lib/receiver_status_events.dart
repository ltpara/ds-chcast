import 'dart:convert';

import 'package:flutter/foundation.dart';

import 'viewer_bridge.dart';

const String viewerDiagnosticSchema = 'chast.viewer.failure';
const int viewerDiagnosticVersion = 1;

typedef ViewerDiagnosticReporter = void Function(Map<String, Object?> event);

enum ViewerFailureCategory {
  systemAudio,
  voice;

  String get eventName => switch (this) {
    ViewerFailureCategory.systemAudio => 'viewer.system_audio.failure',
    ViewerFailureCategory.voice => 'viewer.voice.failure',
  };

  String get wireName => switch (this) {
    ViewerFailureCategory.systemAudio => 'system_audio',
    ViewerFailureCategory.voice => 'voice',
  };
}

/// One asynchronous receiver failure with enough immutable state to correlate
/// it with the host's encoder, the selected transport and the local playback
/// pipeline. This is diagnostic data only: it is never sent to the host.
class ViewerFailureDiagnostic {
  ViewerFailureDiagnostic({
    required this.category,
    required this.previous,
    required this.status,
    required this.reason,
    required this.triggers,
    required this.timestamp,
  });

  final ViewerFailureCategory category;
  final ViewerStatus previous;
  final ViewerStatus status;
  final String reason;
  final List<String> triggers;
  final DateTime timestamp;

  Map<String, Object?> toJson() {
    return <String, Object?>{
      'schema': viewerDiagnosticSchema,
      'version': viewerDiagnosticVersion,
      'event': category.eventName,
      'severity': 'error',
      'timestamp': timestamp.toUtc().toIso8601String(),
      'failure': <String, Object?>{
        'category': category.wireName,
        'reason': reason,
        'triggers': List<String>.unmodifiable(triggers),
      },
      'transition': <String, Object?>{
        'from': _phase(previous, category),
        'to': _phase(status, category),
      },
      'previous': _statusSnapshot(previous),
      'current': _statusSnapshot(status),
    };
  }
}

/// Selects at most one event per failed subsystem for this status transition.
/// Existing counter/reason predicates remain the source of truth, so the log
/// and the user-visible SnackBar cannot disagree about whether a new failure
/// happened.
List<ViewerFailureDiagnostic> viewerFailureDiagnostics(
  ViewerStatus previous,
  ViewerStatus status, {
  DateTime? timestamp,
}) {
  final DateTime at = timestamp ?? DateTime.now();
  final List<ViewerFailureDiagnostic> events = <ViewerFailureDiagnostic>[];

  if (viewerAudioFailureRaised(previous, status)) {
    events.add(
      ViewerFailureDiagnostic(
        category: ViewerFailureCategory.systemAudio,
        previous: previous,
        status: status,
        reason: status.audioError,
        triggers: _audioTriggers(previous, status),
        timestamp: at,
      ),
    );
  }
  if (viewerVoiceFailureRaised(previous, status)) {
    events.add(
      ViewerFailureDiagnostic(
        category: ViewerFailureCategory.voice,
        previous: previous,
        status: status,
        reason: status.voiceError,
        triggers: _voiceTriggers(previous, status),
        timestamp: at,
      ),
    );
  }
  return events;
}

/// Emits newline-delimited JSON through [report], or Flutter's debug log when
/// no custom sink is supplied. Call this once per status transition; repeated
/// polls with unchanged status deliberately produce no output.
void emitViewerFailureDiagnostics(
  ViewerStatus previous,
  ViewerStatus status, {
  ViewerDiagnosticReporter? report,
  DateTime? timestamp,
}) {
  final ViewerDiagnosticReporter sink =
      report ??
      (Map<String, Object?> event) {
        debugPrint('viewer-diagnostic ${jsonEncode(event)}');
      };
  for (final ViewerFailureDiagnostic event in viewerFailureDiagnostics(
    previous,
    status,
    timestamp: timestamp,
  )) {
    sink(event.toJson());
  }
}

List<String> _audioTriggers(ViewerStatus previous, ViewerStatus status) {
  final List<String> triggers = <String>[];
  if (previous.audioEnabled && !status.audioEnabled) {
    triggers.add('enabled_to_stopped');
  }
  if (status.audioError.isNotEmpty &&
      status.audioError != previous.audioError) {
    triggers.add('reason_changed');
  }
  if (status.audioErrors > previous.audioErrors) {
    triggers.add('error_counter_increased');
  }
  return triggers;
}

List<String> _voiceTriggers(ViewerStatus previous, ViewerStatus status) {
  final List<String> triggers = <String>[];
  if (status.voiceErrors > previous.voiceErrors) {
    triggers.add('error_counter_increased');
  }
  if (status.voiceError.isNotEmpty &&
      status.voiceError != previous.voiceError) {
    triggers.add('reason_changed');
  }
  return triggers;
}

String _phase(ViewerStatus status, ViewerFailureCategory category) {
  if (category == ViewerFailureCategory.systemAudio) {
    if (!status.audioAnnounced) {
      return 'unavailable';
    }
    if (!status.audioEnabled) {
      return status.audioError.isEmpty ? 'stopped' : 'failed';
    }
    if (status.audioRecovering) {
      return 'recovering';
    }
    if (status.audioError.isNotEmpty) {
      return 'degraded';
    }
    return status.audioMuted ? 'muted' : 'playing';
  }
  if (!status.voiceAuthorized) {
    return 'unavailable';
  }
  if (status.voicePending) {
    return 'pending';
  }
  if (status.voiceEnabled) {
    return 'active';
  }
  return status.voiceError.isEmpty ? 'idle' : 'failed';
}

Map<String, Object?> _statusSnapshot(ViewerStatus status) {
  return <String, Object?>{
    'viewer': <String, Object?>{
      'state': status.state.name,
      'session_mode': status.sessionMode,
      'elapsed_ms': status.elapsedMs,
    },
    'video': <String, Object?>{
      'host': status.host,
      'port': status.port,
      'transport': status.transport,
      'transport_error': status.transportError,
      'codec': status.codec,
      'server_encoder': status.serverEncoder,
      'decoder': status.decoder,
      'width': status.width,
      'height': status.height,
      'source_width': status.sourceWidth,
      'source_height': status.sourceHeight,
      'framerate': status.framerate,
      'server_bitrate': status.serverBitrate,
      'fps': status.fps,
      'kbps': status.kbps,
      'rtt_ms': status.rttMs,
      'latency_ms': status.latencyMs,
      'decode_errors': status.decodeErrors,
      'gaps': status.gaps,
    },
    'audio': <String, Object?>{
      'announced': status.audioAnnounced,
      'enabled': status.audioEnabled,
      'muted': status.audioMuted,
      'sample_rate': status.audioSampleRate,
      'channels': status.audioChannels,
      'bitrate': status.audioBitrate,
      'received': status.audioReceived,
      'decoded': status.audioDecoded,
      'errors': status.audioErrors,
      'underruns': status.audioUnderruns,
      'starved_ms': status.audioStarvedMs,
      'dropped': status.audioDropped,
      'recoveries': status.audioRecoveries,
      'recovering': status.audioRecovering,
      'buffer_ms': status.audioBufferMs,
      'latency_ms': status.audioLatencyMs,
      'av_skew_ms': status.avSkewMs,
      'error': status.audioError,
      'device': status.audioDevice,
      'device_name': status.audioDeviceName,
      'aec': status.audioAec,
      'noise_suppression': status.audioNoiseSuppression,
    },
    'voice': <String, Object?>{
      'authorized': status.voiceAuthorized,
      'enabled': status.voiceEnabled,
      'pending': status.voicePending,
      'packets': status.voicePackets,
      'dropped': status.voiceDropped,
      'errors': status.voiceErrors,
      'error': status.voiceError,
    },
  };
}

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
