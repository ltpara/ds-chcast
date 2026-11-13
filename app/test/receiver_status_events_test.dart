import 'package:chast_viewer/receiver_status_events.dart';
import 'package:chast_viewer/viewer_bridge.dart';
import 'package:flutter_test/flutter_test.dart';

ViewerStatus _status({
  bool audioAnnounced = false,
  bool audioEnabled = false,
  int audioErrors = 0,
  String audioError = '',
  bool voiceEnabled = false,
  int voiceErrors = 0,
  String voiceError = '',
}) {
  return ViewerStatus.fromMap(<Object?, Object?>{
    'audioAnnounced': audioAnnounced,
    'audioEnabled': audioEnabled,
    'audioErrors': audioErrors,
    'audioError': audioError,
    'voiceEnabled': voiceEnabled,
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
