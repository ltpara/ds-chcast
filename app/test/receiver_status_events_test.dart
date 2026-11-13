import 'dart:convert';

import 'package:chast_viewer/receiver_status_events.dart';
import 'package:chast_viewer/viewer_bridge.dart';
import 'package:flutter_test/flutter_test.dart';

ViewerStatus _status({
  String host = '',
  int port = 0,
  String transport = 'quic',
  String transportError = '',
  String codec = '',
  String serverEncoder = '',
  String decoder = '',
  int width = 0,
  int height = 0,
  int sourceWidth = 0,
  int sourceHeight = 0,
  int framerate = 0,
  int serverBitrate = 0,
  double fps = 0,
  double kbps = 0,
  double rttMs = 0,
  double latencyMs = 0,
  int decodeErrors = 0,
  int gaps = 0,
  String sessionMode = 'desktop',
  int elapsedMs = 0,
  bool audioAnnounced = false,
  bool audioEnabled = false,
  bool audioMuted = false,
  int audioSampleRate = 0,
  int audioChannels = 0,
  int audioBitrate = 0,
  int audioReceived = 0,
  int audioDecoded = 0,
  int audioErrors = 0,
  int audioUnderruns = 0,
  int audioStarvedMs = 0,
  int audioDropped = 0,
  int audioRecoveries = 0,
  bool audioRecovering = false,
  int audioBufferMs = 0,
  double audioLatencyMs = -1,
  double avSkewMs = -1,
  String audioError = '',
  String audioDevice = '',
  String audioDeviceName = '',
  bool audioAec = true,
  bool audioNoiseSuppression = true,
  bool voiceAuthorized = false,
  bool voiceEnabled = false,
  bool voicePending = false,
  int voicePackets = 0,
  int voiceDropped = 0,
  int voiceErrors = 0,
  String voiceError = '',
}) {
  return ViewerStatus.fromMap(<Object?, Object?>{
    'host': host,
    'port': port,
    'transport': transport,
    'transportError': transportError,
    'codec': codec,
    'serverEncoder': serverEncoder,
    'decoder': decoder,
    'width': width,
    'height': height,
    'sourceWidth': sourceWidth,
    'sourceHeight': sourceHeight,
    'framerate': framerate,
    'serverBitrate': serverBitrate,
    'fps': fps,
    'kbps': kbps,
    'rttMs': rttMs,
    'latencyMs': latencyMs,
    'decodeErrors': decodeErrors,
    'gaps': gaps,
    'sessionMode': sessionMode,
    'elapsedMs': elapsedMs,
    'audioAnnounced': audioAnnounced,
    'audioEnabled': audioEnabled,
    'audioMuted': audioMuted,
    'audioSampleRate': audioSampleRate,
    'audioChannels': audioChannels,
    'audioBitrate': audioBitrate,
    'audioReceived': audioReceived,
    'audioDecoded': audioDecoded,
    'audioErrors': audioErrors,
    'audioUnderruns': audioUnderruns,
    'audioStarvedMs': audioStarvedMs,
    'audioDropped': audioDropped,
    'audioRecoveries': audioRecoveries,
    'audioRecovering': audioRecovering,
    'audioBufferMs': audioBufferMs,
    'audioLatencyMs': audioLatencyMs,
    'avSkewMs': avSkewMs,
    'audioError': audioError,
    'audioDevice': audioDevice,
    'audioDeviceName': audioDeviceName,
    'audioAec': audioAec,
    'audioNoiseSuppression': audioNoiseSuppression,
    'voiceAuthorized': voiceAuthorized,
    'voiceEnabled': voiceEnabled,
    'voicePending': voicePending,
    'voicePackets': voicePackets,
    'voiceDropped': voiceDropped,
    'voiceErrors': voiceErrors,
    'voiceError': voiceError,
  });
}

void main() {
  group('voice failures', () {
    test('reports counter and reason events exactly once', () {
      final ViewerStatus idle = _status();
      final ViewerStatus refused = _status(
        voiceErrors: 1,
        voiceError: 'the host refused the voice call',
      );
      expect(viewerVoiceFailureRaised(idle, refused), isTrue);
      expect(viewerVoiceFailureRaised(refused, refused), isFalse);

      final ViewerStatus ended = _status(
        voiceErrors: 1,
        voiceError: 'the host ended the voice call',
      );
      expect(viewerVoiceFailureRaised(refused, ended), isTrue);
      expect(viewerVoiceFailureRaised(ended, _status()), isFalse);
    });

    test('a bare decode counter increase is a failure', () {
      expect(
        viewerVoiceFailureRaised(
          _status(voiceEnabled: true),
          _status(voiceEnabled: true, voiceErrors: 1),
        ),
        isTrue,
      );
    });
  });

  group('system audio failures', () {
    test(
      'reports new counters, reasons and enabled-to-stopped transitions',
      () {
        final ViewerStatus playing = _status(
          audioAnnounced: true,
          audioEnabled: true,
        );
        expect(
          viewerAudioFailureRaised(
            playing,
            _status(audioAnnounced: true, audioEnabled: true, audioErrors: 1),
          ),
          isTrue,
        );
        expect(
          viewerAudioFailureRaised(
            playing,
            _status(
              audioAnnounced: true,
              audioEnabled: true,
              audioError: 'playback: output fallback',
            ),
          ),
          isTrue,
        );
        expect(
          viewerAudioFailureRaised(playing, _status(audioAnnounced: true)),
          isTrue,
        );
      },
    );

    test('does not report host-disabled or unchanged audio', () {
      final ViewerStatus disabled = _status();
      expect(
        viewerAudioFailureRaised(
          disabled,
          _status(audioErrors: 3, audioError: 'stale'),
        ),
        isFalse,
      );
      final ViewerStatus failed = _status(
        audioAnnounced: true,
        audioErrors: 1,
        audioError: 'decoder failed',
      );
      expect(viewerAudioFailureRaised(failed, failed), isFalse);
      expect(
        viewerAudioFailureRaised(
          failed,
          _status(audioAnnounced: true, audioErrors: 1),
        ),
        isFalse,
      );
    });
  });

  group('structured diagnostics', () {
    test('correlates failure context with host and receiver state', () {
      final ViewerStatus previous = _status(
        host: '10.20.30.40',
        port: 4433,
        transport: 'tcp',
        transportError: 'udp timed out',
        codec: 'h264',
        serverEncoder: 'onevpl',
        decoder: 'd3d11va',
        width: 1920,
        height: 1080,
        sourceWidth: 2560,
        sourceHeight: 1440,
        framerate: 60,
        serverBitrate: 8000,
        fps: 57.5,
        kbps: 6100,
        rttMs: 48,
        latencyMs: 121,
        decodeErrors: 2,
        gaps: 1,
        audioAnnounced: true,
        audioEnabled: true,
        audioSampleRate: 48000,
        audioChannels: 2,
        audioBitrate: 128000,
        audioReceived: 300,
        audioDecoded: 299,
        audioUnderruns: 1,
        audioStarvedMs: 20,
        audioDropped: 4,
        audioRecoveries: 1,
        audioBufferMs: 60,
        audioLatencyMs: 88,
        avSkewMs: 12,
        audioDevice: '{speaker-id}',
        audioDeviceName: 'Speakers',
        voiceAuthorized: true,
        voiceEnabled: true,
        voicePackets: 220,
      );
      final ViewerStatus failed = _status(
        host: '10.20.30.40',
        port: 4433,
        transport: 'tcp',
        transportError: 'udp timed out',
        codec: 'h264',
        serverEncoder: 'onevpl',
        decoder: 'd3d11va',
        width: 1920,
        height: 1080,
        sourceWidth: 2560,
        sourceHeight: 1440,
        framerate: 60,
        serverBitrate: 8000,
        fps: 0,
        kbps: 0,
        rttMs: 520,
        latencyMs: 1900,
        decodeErrors: 3,
        gaps: 2,
        audioAnnounced: true,
        audioErrors: 1,
        audioUnderruns: 2,
        audioStarvedMs: 145,
        audioDropped: 9,
        audioRecoveries: 2,
        audioBufferMs: 0,
        audioLatencyMs: -1,
        avSkewMs: -1,
        audioError: 'playback endpoint lost',
        audioDevice: '{speaker-id}',
        audioDeviceName: 'Speakers',
        voiceAuthorized: true,
        voiceErrors: 1,
        voicePackets: 220,
        voiceError: 'decoder stopped',
      );

      final List<ViewerFailureDiagnostic> events = viewerFailureDiagnostics(
        previous,
        failed,
        timestamp: DateTime.utc(2026, 11, 14, 12, 30),
      );
      expect(events, hasLength(2));
      expect(
        events.map((ViewerFailureDiagnostic event) => event.category),
        <ViewerFailureCategory>[
          ViewerFailureCategory.systemAudio,
          ViewerFailureCategory.voice,
        ],
      );

      final Map<String, Object?> audio = events.first.toJson();
      expect(audio['schema'], viewerDiagnosticSchema);
      expect(audio['version'], viewerDiagnosticVersion);
      expect(audio['event'], 'viewer.system_audio.failure');
      expect(audio['timestamp'], '2026-11-14T12:30:00.000Z');
      expect(
        (audio['failure']! as Map<String, Object?>)['reason'],
        'playback endpoint lost',
      );
      expect(
        (audio['failure']! as Map<String, Object?>)['triggers'],
        containsAll(<String>[
          'enabled_to_stopped',
          'reason_changed',
          'error_counter_increased',
        ]),
      );
      expect((audio['transition']! as Map<String, Object?>), <String, Object?>{
        'from': 'playing',
        'to': 'failed',
      });

      final Map<String, Object?> current =
          audio['current']! as Map<String, Object?>;
      final Map<String, Object?> video =
          current['video']! as Map<String, Object?>;
      expect(video['host'], '10.20.30.40');
      expect(video['transport'], 'tcp');
      expect(video['transport_error'], 'udp timed out');
      expect(video['server_encoder'], 'onevpl');
      expect(video['decoder'], 'd3d11va');
      expect(video['latency_ms'], 1900);
      final Map<String, Object?> audioState =
          current['audio']! as Map<String, Object?>;
      expect(audioState['starved_ms'], 145);
      expect(audioState['device_name'], 'Speakers');

      final Map<String, Object?> voice = events.last.toJson();
      expect(voice['event'], 'viewer.voice.failure');
      expect((voice['transition']! as Map<String, Object?>), <String, Object?>{
        'from': 'active',
        'to': 'failed',
      });
      expect(jsonEncode(voice), contains('"server_encoder":"onevpl"'));
    });

    test('emits each transition once through the injected reporter', () {
      final ViewerStatus playing = _status(
        audioAnnounced: true,
        audioEnabled: true,
        voiceAuthorized: true,
        voiceEnabled: true,
      );
      final ViewerStatus failed = _status(
        audioAnnounced: true,
        audioErrors: 1,
        audioError: 'decoder failed',
        voiceAuthorized: true,
        voiceErrors: 1,
        voiceError: 'host ended the call',
      );
      final List<Map<String, Object?>> emitted = <Map<String, Object?>>[];

      emitViewerFailureDiagnostics(
        playing,
        failed,
        report: emitted.add,
        timestamp: DateTime.utc(2026, 11, 14),
      );
      emitViewerFailureDiagnostics(
        failed,
        failed,
        report: emitted.add,
        timestamp: DateTime.utc(2026, 11, 14),
      );

      expect(emitted, hasLength(2));
      expect(
        emitted.map((Map<String, Object?> event) => event['event']),
        <String>['viewer.system_audio.failure', 'viewer.voice.failure'],
      );
    });

    test('logs a changed audio reason while playback is still enabled', () {
      final List<ViewerFailureDiagnostic> events = viewerFailureDiagnostics(
        _status(audioAnnounced: true, audioEnabled: true),
        _status(
          audioAnnounced: true,
          audioEnabled: true,
          audioError: 'output fallback',
        ),
        timestamp: DateTime.utc(2026, 11, 14),
      );

      expect(events, hasLength(1));
      expect(events.single.category, ViewerFailureCategory.systemAudio);
      expect(events.single.triggers, <String>['reason_changed']);
      expect(events.single.toJson()['transition'], <String, Object?>{
        'from': 'playing',
        'to': 'degraded',
      });
    });
  });

  group('permission names', () {
    const int all =
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

    test('names every protocol permission in order', () {
      expect(viewerPermissionNames(all), <String>[
        '输入控制',
        '剪贴板',
        '音频',
        '文件传输',
        '麦克风',
        '隐私模式',
        '端口转发',
        '终端',
        '录制',
        '摄像头',
        '重启',
        '阻止输入',
        '系统登录',
        '会话切换',
        '远程配置',
      ]);
    });

    test('never drops reserved or future permission bits', () {
      expect(viewerPermissionNames(0x0040), <String>['其他远程权限']);
      expect(
        viewerPermissionNames(ViewerPermission.terminal | 0x10000),
        <String>['终端', '其他远程权限'],
      );
    });

    test('applies the supplied translator', () {
      expect(
        viewerPermissionNames(
          ViewerPermission.camera,
          translate: (String source) => '[$source]',
        ),
        <String>['[摄像头]'],
      );
    });
  });
}
