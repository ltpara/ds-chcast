import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';

void main() {
  test('status carries the audio pipeline and counters', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
      'audioAnnounced': true,
      'audioEnabled': true,
      'audioMuted': true,
      'audioSampleRate': 48000,
      'audioChannels': 2,
      'audioBitrate': 160000,
      'audioReceived': 1200,
      'audioDecoded': 1198,
      'audioErrors': 1,
      'audioUnderruns': 2,
      'audioRecoveries': 3,
      'audioRecovering': true,
      'audioBufferMs': 84,
      'audioError': 'playback: device removed',
    });
    expect(status.audioAnnounced, isTrue);
    expect(status.audioEnabled, isTrue);
    expect(status.audioMuted, isTrue);
    expect(status.audioSampleRate, 48000);
    expect(status.audioChannels, 2);
    expect(status.audioBitrate, 160000);
    expect(status.audioReceived, 1200);
    expect(status.audioDecoded, 1198);
    expect(status.audioErrors, 1);
    expect(status.audioUnderruns, 2);
    expect(status.audioRecoveries, 3);
    expect(status.audioRecovering, isTrue);
    expect(status.audioBufferMs, 84);
    expect(status.audioError, 'playback: device removed');
    expect(status.acceptsAudio, isTrue);
    expect(status.audioSampleRateLabel, '48 kHz');
    expect(status.audioChannelLabel, '立体声');
  });

  test('audio defaults to disabled when the host did not announce it', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'streaming',
    });
    expect(status.audioAnnounced, isFalse);
    expect(status.audioEnabled, isFalse);
    expect(status.audioMuted, isFalse);
    expect(status.audioSampleRate, 0);
    expect(status.audioChannels, 0);
    expect(status.audioBitrate, 0);
    expect(status.audioReceived, 0);
    expect(status.audioDecoded, 0);
    expect(status.audioErrors, 0);
    expect(status.audioUnderruns, 0);
    expect(status.audioRecoveries, 0);
    expect(status.audioRecovering, isFalse);
    expect(status.audioBufferMs, 0);
    expect(status.audioError, isEmpty);
    expect(status.acceptsAudio, isFalse);
    expect(status.audioSampleRateLabel, isEmpty);
    expect(status.audioChannelLabel, '单声道');
  });

  test('an enabled audio pipeline still waits for the live stream', () {
    final ViewerStatus connecting = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'handshaking',
      'audioAnnounced': true,
      'audioEnabled': true,
    });
    expect(connecting.acceptsAudio, isFalse);

    final ViewerStatus over = ViewerStatus.fromMap(<Object?, Object?>{
      'state': 'ended',
      'audioEnabled': true,
    });
    expect(over.acceptsAudio, isFalse);
  });

  test('the sample rate label keeps one decimal only when needed', () {
    final ViewerStatus status = ViewerStatus.fromMap(<Object?, Object?>{
      'audioSampleRate': 44100,
    });
    expect(status.audioSampleRateLabel, '44.1 kHz');
  });
}
