import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/viewer_bridge.dart';
import 'package:chast_viewer/widgets/remote_toolbar.dart';
import 'package:chast_viewer/widgets/remote_view.dart';

void main() {
  const List<ViewerDisplay> displays = <ViewerDisplay>[
    ViewerDisplay(
      index: 0,
      name: r'\\.\DISPLAY1',
      left: 0,
      top: 0,
      right: 2560,
      bottom: 1440,
      rotation: 0,
      primary: true,
    ),
    ViewerDisplay(
      index: 1,
      name: r'\\.\DISPLAY2',
      left: -1080,
      top: 0,
      right: 0,
      bottom: 1920,
      rotation: 90,
      primary: false,
    ),
  ];

  Widget toolbar({
    required List<ViewerDisplay> announced,
    required int active,
    required ValueChanged<int> onSelect,
    ValueChanged<int>? onOpenDisplayWindow,
    VoidCallback? onIndependentDisplays,
    bool independentDisplays = false,
    bool followCursor = false,
    bool followWindow = false,
    VoidCallback? onToggleFollowCursor,
    VoidCallback? onToggleFollowWindow,
    List<ViewerDisplayMode> modes = const <ViewerDisplayMode>[],
    bool modeChangeAvailable = true,
    ValueChanged<ViewerDisplayMode>? onSelectMode,
    bool reverseWheel = false,
    bool swapButtons = false,
    VoidCallback? onToggleReverseWheel,
    VoidCallback? onToggleSwapButtons,
    bool relativeMouse = false,
    VoidCallback? onToggleRelativeMouse,
    bool controlEnabled = false,
    bool fileAvailable = true,
    bool fileActive = false,
    VoidCallback? onSendFile,
    bool chatOpen = false,
    VoidCallback? onToggleChat,
    bool audioAvailable = true,
    bool audioMuted = false,
    String audioDevice = '',
    Future<List<ViewerAudioOutput>> Function()? onLoadAudioOutputs,
    Future<void> Function(String)? onSelectAudioOutput,
    String microphoneDevice = '',
    bool audioAec = true,
    bool audioNoiseSuppression = true,
    Future<List<ViewerMicrophoneDevice>> Function()? onLoadMicrophones,
    Future<void> Function(String)? onSelectMicrophone,
    Future<void> Function(bool, bool)? onSetAudioProcessing,
    bool specialKeysAvailable = false,
    ValueChanged<RemoteSpecialAction>? onSpecialAction,
    VoidCallback? onSendText,
    bool terminalAvailable = false,
    bool terminalOpen = false,
    VoidCallback? onToggleTerminal,
    bool streamOptionsAvailable = true,
    ViewerStreamCodec streamCodec = ViewerStreamCodec.auto,
    ViewerStreamQuality streamQuality = ViewerStreamQuality.high,
    int streamMaxFps = 0,
    ValueChanged<ViewerStreamCodec>? onSelectStreamCodec,
    ValueChanged<ViewerStreamQuality>? onSelectStreamQuality,
    ValueChanged<int>? onSelectStreamFps,
    bool restartAvailable = false,
    bool lockAvailable = false,
    bool blockInputAvailable = false,
    bool blockInputActive = false,
    ValueChanged<ViewerHostAction>? onHostAction,
    bool remoteConfigAvailable = false,
    VoidCallback? onRemoteConfig,
    bool displayControlAvailable = true,
    bool displayControlSupported = false,
    bool displayControlManaged = false,
    bool displayControlBusy = false,
    int displayControlCount = 0,
    int displayControlPrivacy = 0,
    List<ViewerControlledDisplay> displayControlOutputs =
        const <ViewerControlledDisplay>[],
    void Function(int action, int value)? onControlDisplays,
    void Function(int target, bool enabled)? onToggleVirtualDisplay,
    bool privacyAvailable = true,
    bool recordingAvailable = true,
    bool recording = false,
    VoidCallback? onToggleRecording,
    bool autoRecord = false,
    int recordKeep = 0,
    ValueChanged<bool>? onAutoRecordChanged,
    ValueChanged<int>? onRecordKeepChanged,
  }) {
    return MaterialApp(
      home: Scaffold(
        body: RemoteToolbar(
          collapsed: false,
          pinned: true,
          fullscreen: false,
          statsVisible: false,
          fit: ViewFit.adaptive,
          controlEnabled: controlEnabled,
          inputAvailable: true,
          clipboardEnabled: true,
          clipboardAvailable: true,
          fileAvailable: fileAvailable,
          fileActive: fileActive,
          audioAvailable: audioAvailable,
          audioMuted: audioMuted,
          audioDevice: audioDevice,
          onLoadAudioOutputs: onLoadAudioOutputs ??
              () async => const <ViewerAudioOutput>[],
          onSelectAudioOutput: onSelectAudioOutput ?? (_) async {},
          microphoneAvailable: true,
          microphoneEnabled: false,
          microphoneDevice: microphoneDevice,
          audioAec: audioAec,
          audioNoiseSuppression: audioNoiseSuppression,
          onLoadMicrophones: onLoadMicrophones ??
              () async => const <ViewerMicrophoneDevice>[],
          onSelectMicrophone: onSelectMicrophone ?? (_) async {},
          onSetAudioProcessing: onSetAudioProcessing ?? (_, _) async {},
          voiceAvailable: false,
          voiceEnabled: false,
          voicePending: false,
          onToggleVoiceCall: () {},
          displayControlAvailable: displayControlAvailable,
          displayControlSupported: displayControlSupported,
          displayControlManaged: displayControlManaged,
          displayControlBusy: displayControlBusy,
          displayControlCount: displayControlCount,
          displayControlPrivacy: displayControlPrivacy,
          displayControlOutputs: displayControlOutputs,
          onControlDisplays: onControlDisplays,
          onToggleVirtualDisplay: onToggleVirtualDisplay,
          privacyAvailable: privacyAvailable,
          privacyEnabled: false,
          displays: announced,
          activeDisplay: active,
          followCursor: followCursor,
          followWindow: followWindow,
          onToggleFollowCursor: onToggleFollowCursor ?? () {},
          onToggleFollowWindow: onToggleFollowWindow ?? () {},
          onSelectDisplay: onSelect,
          onOpenDisplayWindow: onOpenDisplayWindow,
          onIndependentDisplays: onIndependentDisplays,
          independentDisplays: independentDisplays,
          modes: modes,
          modeChangeAvailable: modeChangeAvailable,
          onSelectMode: onSelectMode ?? (_) {},
          streamOptionsAvailable: streamOptionsAvailable,
          streamCodec: streamCodec,
          streamQuality: streamQuality,
          streamMaxFps: streamMaxFps,
          onSelectStreamCodec: onSelectStreamCodec ?? (_) {},
          onSelectStreamQuality: onSelectStreamQuality ?? (_) {},
          onSelectStreamFps: onSelectStreamFps ?? (_) {},
          onToggleControl: () {},
          reverseWheel: reverseWheel,
          swapButtons: swapButtons,
          onToggleReverseWheel: onToggleReverseWheel,
          onToggleSwapButtons: onToggleSwapButtons,
          relativeMouse: relativeMouse,
          onToggleRelativeMouse: onToggleRelativeMouse,
          specialKeysAvailable: specialKeysAvailable,
          onSpecialAction: onSpecialAction ?? (_) {},
          onSendText: onSendText ?? () {},
          onToggleClipboard: () {},
          onSendFile: onSendFile ?? () {},
          fileBrowseOpen: false,
          onToggleFileBrowse: () {},
          chatOpen: chatOpen,
          onToggleChat: onToggleChat ?? () {},
          terminalAvailable: terminalAvailable,
          terminalOpen: terminalOpen,
          onToggleTerminal: onToggleTerminal ?? () {},
          cameraAvailable: false,
          cameraOpen: false,
          onToggleCamera: () {},
          portForwardAvailable: true,
          portForwardActive: false,
          onOpenPortForward: () {},
          restartAvailable: restartAvailable,
          lockAvailable: lockAvailable,
          blockInputAvailable: blockInputAvailable,
          blockInputActive: blockInputActive,
          onHostAction: onHostAction ?? (_) {},
          remoteConfigAvailable: remoteConfigAvailable,
          onRemoteConfig: onRemoteConfig,
          recordingAvailable: recordingAvailable,
          recording: recording,
          onToggleRecording: onToggleRecording,
          autoRecord: autoRecord,
          recordKeep: recordKeep,
          onAutoRecordChanged: onAutoRecordChanged,
          onRecordKeepChanged: onRecordKeepChanged,
          onToggleAudio: () {},
          onToggleMicrophone: () {},
          onTogglePrivacy: () {},
          onTogglePin: () {},
          onToggleStats: () {},
          onToggleFullscreen: () {},
          onCollapse: () {},
          onExpand: () {},
          onClose: () {},
          onFitChanged: (_) {},
        ),
      ),
    );
  }

  testWidgets('lists every announced display and reports the pick', (WidgetTester tester) async {
    int? picked;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) => picked = index,
    ));

    await tester.tap(find.byIcon(Icons.monitor_rounded));
    await tester.pumpAndSettle();

    expect(find.text('服务端默认'), findsOneWidget);
    expect(find.text('DISPLAY1'), findsOneWidget);
    expect(find.text('DISPLAY2'), findsOneWidget);
    expect(find.text('1080×1920 · 90°'), findsOneWidget);

    await tester.tap(find.text('DISPLAY2'));
    await tester.pumpAndSettle();
    expect(picked, 1);
  });

  testWidgets('the default entry asks the server to choose', (WidgetTester tester) async {
    int? picked;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 1,
      onSelect: (int index) => picked = index,
    ));

    await tester.tap(find.byIcon(Icons.monitor_rounded));
    await tester.pumpAndSettle();
    await tester.tap(find.text('服务端默认'));
    await tester.pumpAndSettle();
    expect(picked, kViewerDisplayAuto);
  });

  testWidgets('offers the merged view with the union geometry', (WidgetTester tester) async {
    int? picked;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: kViewerDisplayAll,
      onSelect: (int index) => picked = index,
    ));

    await tester.tap(find.byIcon(Icons.monitor_rounded));
    await tester.pumpAndSettle();

    expect(find.text('全部显示器（完整桌面）'), findsOneWidget);
    expect(find.text('3640×1920 · 2 块屏'), findsOneWidget);
    expect(
      find.descendant(
        of: find.widgetWithText(PopupMenuItem<int>, '全部显示器（完整桌面）'),
        matching: find.byIcon(Icons.check_rounded),
      ),
      findsOneWidget,
    );

    await tester.tap(find.text('全部显示器（完整桌面）'));
    await tester.pumpAndSettle();
    expect(picked, kViewerDisplayAll);
  });

  testWidgets('the merged stream hides the resolution picker',
      (WidgetTester tester) async {
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: kViewerDisplayAll,
      onSelect: (int index) {},
      modes: const <ViewerDisplayMode>[
        ViewerDisplayMode(width: 1920, height: 1080, refresh: 60, current: true),
      ],
    ));

    expect(find.byIcon(Icons.aspect_ratio_rounded), findsNothing);
  });

  testWidgets('the picker is inert before a list arrives', (WidgetTester tester) async {
    int? picked;
    await tester.pumpWidget(toolbar(
      announced: const <ViewerDisplay>[],
      active: kViewerDisplayAuto,
      onSelect: (int index) => picked = index,
    ));

    await tester.tap(find.byIcon(Icons.monitor_rounded));
    await tester.pumpAndSettle();
    expect(find.text('服务端默认'), findsNothing);
    expect(picked, isNull);
  });

  testWidgets('the follow toggles report clicks and mark the active mode',
      (WidgetTester tester) async {
    int cursorTaps = 0;
    int windowTaps = 0;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      followCursor: true,
      onToggleFollowCursor: () => ++cursorTaps,
      onToggleFollowWindow: () => ++windowTaps,
    ));

    await tester.tap(find.byIcon(Icons.monitor_rounded));
    await tester.pumpAndSettle();
    expect(find.text('跟随远程光标'), findsOneWidget);
    expect(find.text('跟随远程窗口'), findsOneWidget);
    expect(
      find.descendant(
        of: find.widgetWithText(PopupMenuItem<int>, '跟随远程光标'),
        matching: find.byIcon(Icons.check_rounded),
      ),
      findsOneWidget,
    );
    expect(
      find.descendant(
        of: find.widgetWithText(PopupMenuItem<int>, '跟随远程窗口'),
        matching: find.byIcon(Icons.check_rounded),
      ),
      findsNothing,
    );

    await tester.tap(find.text('跟随远程窗口'));
    await tester.pumpAndSettle();
    expect(windowTaps, 1);
    expect(cursorTaps, 0);
  });

  testWidgets('follow stays inert on a merged stream', (WidgetTester tester) async {
    int taps = 0;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: kViewerDisplayAll,
      onSelect: (int index) {},
      onToggleFollowCursor: () => ++taps,
      onToggleFollowWindow: () => ++taps,
    ));

    await tester.tap(find.byIcon(Icons.monitor_rounded));
    await tester.pumpAndSettle();
    await tester.tap(find.text('跟随远程光标'));
    await tester.pumpAndSettle();
    expect(taps, 0);
  });

  testWidgets('lists the host modes and reports the pick', (WidgetTester tester) async {
    ViewerDisplayMode? picked;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      modes: const <ViewerDisplayMode>[
        ViewerDisplayMode(width: 1920, height: 1080, refresh: 60, current: true),
        ViewerDisplayMode(width: 2560, height: 1440, refresh: 144),
      ],
      onSelectMode: (ViewerDisplayMode mode) => picked = mode,
    ));

    await tester.tap(find.byIcon(Icons.aspect_ratio_rounded));
    await tester.pumpAndSettle();

    expect(find.text('1920 × 1080 · 60 Hz'), findsOneWidget);
    expect(find.text('2560 × 1440 · 144 Hz'), findsOneWidget);

    await tester.tap(find.text('2560 × 1440 · 144 Hz'));
    await tester.pumpAndSettle();
    expect(picked?.width, 2560);
    expect(picked?.height, 1440);
    expect(picked?.refresh, 144);
  });

  testWidgets('the resolution picker is inert without input access', (WidgetTester tester) async {
    ViewerDisplayMode? picked;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      modes: const <ViewerDisplayMode>[
        ViewerDisplayMode(width: 1920, height: 1080, refresh: 60, current: true),
      ],
      modeChangeAvailable: false,
      onSelectMode: (ViewerDisplayMode mode) => picked = mode,
    ));

    await tester.tap(find.byIcon(Icons.aspect_ratio_rounded));
    await tester.pumpAndSettle();
    expect(find.text('1920 × 1080 · 60 Hz'), findsNothing);
    expect(picked, isNull);
  });

  testWidgets('the upload button follows the host gate and the active transfer',
      (WidgetTester tester) async {
    int taps = 0;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      onSendFile: () => ++taps,
    ));
    await tester.tap(find.byIcon(Icons.upload_file_rounded));
    expect(taps, 1);

    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      fileAvailable: false,
      onSendFile: () => ++taps,
    ));
    await tester.tap(find.byIcon(Icons.upload_file_rounded));
    expect(taps, 1);

    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      fileActive: true,
      onSendFile: () => ++taps,
    ));
    await tester.tap(find.byIcon(Icons.upload_file_rounded));
    expect(taps, 1);
  });

  testWidgets('the chat button mirrors the panel state and reports taps',
      (WidgetTester tester) async {
    int taps = 0;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      onToggleChat: () => ++taps,
    ));
    await tester.tap(find.byIcon(Icons.forum_outlined));
    expect(taps, 1);

    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      chatOpen: true,
      onToggleChat: () => ++taps,
    ));
    expect(find.byIcon(Icons.forum_outlined), findsNothing);
    await tester.tap(find.byIcon(Icons.forum_rounded));
    expect(taps, 2);
  });

  testWidgets('the special-key menu reports the chosen shortcut',
      (WidgetTester tester) async {
    RemoteSpecialAction? picked;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      specialKeysAvailable: true,
      onSpecialAction: (RemoteSpecialAction action) => picked = action,
    ));

    await tester.tap(find.byIcon(Icons.keyboard_command_key_rounded));
    await tester.pumpAndSettle();
    expect(find.text('Alt+Tab'), findsOneWidget);
    expect(find.text('Ctrl+Alt+Del'), findsOneWidget);

    await tester.tap(find.text('Ctrl+Alt+Del'));
    await tester.pumpAndSettle();
    expect(picked, RemoteSpecialAction.sas);
  });

  testWidgets('special keys stay inert while control is off',
      (WidgetTester tester) async {
    RemoteSpecialAction? picked;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      onSpecialAction: (RemoteSpecialAction action) => picked = action,
    ));

    await tester.tap(find.byIcon(Icons.keyboard_command_key_rounded));
    await tester.pumpAndSettle();
    expect(find.text('Alt+Tab'), findsNothing);
    expect(picked, isNull);
  });

  testWidgets('the send-text button reports taps only while control is on',
      (WidgetTester tester) async {
    int taps = 0;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      onSendText: () => ++taps,
    ));

    await tester.tap(find.byIcon(Icons.text_fields_rounded));
    expect(taps, 0);

    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      specialKeysAvailable: true,
      onSendText: () => ++taps,
    ));
    await tester.tap(find.byIcon(Icons.text_fields_rounded));
    expect(taps, 1);
  });

  testWidgets('the picture menu reports quality, frame cap and encoder picks',
      (WidgetTester tester) async {
    ViewerStreamQuality? quality;
    int? fps;
    ViewerStreamCodec? codec;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      streamQuality: ViewerStreamQuality.balanced,
      streamMaxFps: 30,
      onSelectStreamQuality: (ViewerStreamQuality value) => quality = value,
      onSelectStreamFps: (int value) => fps = value,
      onSelectStreamCodec: (ViewerStreamCodec value) => codec = value,
    ));

    await tester.tap(find.byIcon(Icons.tune_rounded));
    await tester.pumpAndSettle();
    expect(find.text('画质'), findsOneWidget);
    expect(find.text('帧率上限'), findsOneWidget);
    expect(find.text('编码器'), findsOneWidget);
    expect(find.text('高清'), findsOneWidget);
    expect(find.text('均衡'), findsOneWidget);
    expect(find.text('流畅'), findsOneWidget);
    expect(find.text('自动'), findsNWidgets(2));
    expect(find.text('15 fps'), findsOneWidget);
    expect(find.text('30 fps'), findsOneWidget);
    expect(find.text('60 fps'), findsOneWidget);
    expect(find.text('H.264'), findsOneWidget);
    expect(find.text('HEVC'), findsOneWidget);
    expect(find.text('AV1'), findsOneWidget);

    await tester.tap(find.text('流畅'));
    await tester.pumpAndSettle();
    expect(quality, ViewerStreamQuality.smooth);

    await tester.tap(find.byIcon(Icons.tune_rounded));
    await tester.pumpAndSettle();
    await tester.tap(find.text('60 fps'));
    await tester.pumpAndSettle();
    expect(fps, 60);

    await tester.tap(find.byIcon(Icons.tune_rounded));
    await tester.pumpAndSettle();
    await tester.tap(find.text('HEVC'));
    await tester.pumpAndSettle();
    expect(codec, ViewerStreamCodec.hevc);
  });

  testWidgets('the picture menu is inert before the stream is live',
      (WidgetTester tester) async {
    ViewerStreamQuality? quality;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      streamOptionsAvailable: false,
      onSelectStreamQuality: (ViewerStreamQuality value) => quality = value,
    ));

    await tester.tap(find.byIcon(Icons.tune_rounded));
    await tester.pumpAndSettle();
    expect(find.text('高清'), findsNothing);
    expect(quality, isNull);
  });

  testWidgets('the host settings button follows the host grant',
      (WidgetTester tester) async {
    int taps = 0;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      remoteConfigAvailable: true,
      onRemoteConfig: () => ++taps,
    ));
    await tester.tap(find.byIcon(Icons.settings_suggest_rounded));
    expect(taps, 1);

    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      remoteConfigAvailable: false,
      onRemoteConfig: () => ++taps,
    ));
    await tester.tap(find.byIcon(Icons.settings_suggest_rounded));
    expect(taps, 1);
  });

  testWidgets('the volume long press lists playback endpoints and reports the pick',
      (WidgetTester tester) async {
    String? picked;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      audioDevice: '',
      onLoadAudioOutputs: () async => const <ViewerAudioOutput>[
        ViewerAudioOutput(id: 'out-1', name: '扬声器', isDefault: true),
        ViewerAudioOutput(id: 'out-2', name: '耳机'),
      ],
      onSelectAudioOutput: (String id) async {
        picked = id;
      },
    ));

    await tester.longPress(find.byIcon(Icons.volume_up_rounded));
    await tester.pumpAndSettle();

    expect(find.text('系统默认输出'), findsOneWidget);
    expect(find.text('扬声器'), findsOneWidget);
    expect(find.text('耳机'), findsOneWidget);
    expect(
      find.descendant(
        of: find.widgetWithText(PopupMenuItem<String>, '系统默认输出'),
        matching: find.byIcon(Icons.check_rounded),
      ),
      findsOneWidget,
    );

    await tester.tap(find.text('耳机'));
    await tester.pumpAndSettle();
    expect(picked, 'out-2');
  });

  testWidgets('the output menu marks the endpoint in use', (WidgetTester tester) async {
    String? picked;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      audioDevice: 'out-2',
      onLoadAudioOutputs: () async => const <ViewerAudioOutput>[
        ViewerAudioOutput(id: 'out-1', name: '扬声器', isDefault: true),
        ViewerAudioOutput(id: 'out-2', name: '耳机'),
      ],
      onSelectAudioOutput: (String id) async {
        picked = id;
      },
    ));

    await tester.longPress(find.byIcon(Icons.volume_up_rounded));
    await tester.pumpAndSettle();
    expect(
      find.descendant(
        of: find.widgetWithText(PopupMenuItem<String>, '耳机'),
        matching: find.byIcon(Icons.check_rounded),
      ),
      findsOneWidget,
    );

    await tester.tap(find.text('扬声器'));
    await tester.pumpAndSettle();
    expect(picked, 'out-1');
  });

  testWidgets('the microphone long press toggles echo cancellation and noise suppression',
      (WidgetTester tester) async {
    bool? pickedAec;
    bool? pickedNoise;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      audioAec: true,
      audioNoiseSuppression: true,
      onSetAudioProcessing: (bool aec, bool noise) async {
        pickedAec = aec;
        pickedNoise = noise;
      },
    ));

    await tester.longPress(find.byIcon(Icons.mic_off_rounded));
    await tester.pumpAndSettle();
    expect(find.text('回声消除'), findsOneWidget);
    expect(find.text('噪声抑制'), findsOneWidget);
    expect(find.text('系统默认麦克风'), findsOneWidget);

    await tester.tap(find.text('噪声抑制'));
    await tester.pumpAndSettle();
    expect(pickedAec, isTrue);
    expect(pickedNoise, isFalse);
  });

  testWidgets('the processing items follow the current switches',
      (WidgetTester tester) async {
    bool? pickedAec;
    bool? pickedNoise;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      audioAec: false,
      audioNoiseSuppression: true,
      onSetAudioProcessing: (bool aec, bool noise) async {
        pickedAec = aec;
        pickedNoise = noise;
      },
    ));

    await tester.longPress(find.byIcon(Icons.mic_off_rounded));
    await tester.pumpAndSettle();
    expect(
      find.descendant(
        of: find.widgetWithText(PopupMenuItem<Object>, '回声消除'),
        matching: find.byIcon(Icons.check_box_outline_blank_rounded),
      ),
      findsOneWidget,
    );

    await tester.tap(find.text('回声消除'));
    await tester.pumpAndSettle();
    expect(pickedAec, isTrue);
    expect(pickedNoise, isTrue);
  });

  testWidgets('the volume menu stays inert when the host has no audio',
      (WidgetTester tester) async {
    String? picked;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      audioAvailable: false,
      onLoadAudioOutputs: () async => const <ViewerAudioOutput>[
        ViewerAudioOutput(id: 'out-1', name: '扬声器', isDefault: true),
      ],
      onSelectAudioOutput: (String id) async {
        picked = id;
      },
    ));

    await tester.longPress(find.byIcon(Icons.volume_up_rounded));
    await tester.pumpAndSettle();
    expect(find.text('扬声器'), findsNothing);
    expect(picked, isNull);
  });

  testWidgets('the power menu reports the picked host action',
      (WidgetTester tester) async {
    ViewerHostAction? picked;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      restartAvailable: true,
      lockAvailable: true,
      blockInputAvailable: true,
      onHostAction: (ViewerHostAction action) => picked = action,
    ));

    await tester.tap(find.byIcon(Icons.power_settings_new_rounded));
    await tester.pumpAndSettle();
    expect(find.text('锁定主机屏幕'), findsOneWidget);
    expect(find.text('阻止主机本地输入'), findsOneWidget);
    expect(find.text('重启主机…'), findsOneWidget);

    await tester.tap(find.text('锁定主机屏幕'));
    await tester.pumpAndSettle();
    expect(picked, ViewerHostAction.lockScreen);
  });

  testWidgets('the power menu mirrors the blocked local input state',
      (WidgetTester tester) async {
    ViewerHostAction? picked;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      blockInputAvailable: true,
      blockInputActive: true,
      onHostAction: (ViewerHostAction action) => picked = action,
    ));

    expect(find.text('阻止主机本地输入'), findsNothing);
    await tester.tap(find.byIcon(Icons.power_settings_new_rounded));
    await tester.pumpAndSettle();
    expect(find.text('恢复主机本地输入'), findsOneWidget);
    await tester.tap(find.text('恢复主机本地输入'));
    await tester.pumpAndSettle();
    expect(picked, ViewerHostAction.blockInput);
  });

  testWidgets('the power menu is inert without a host grant',
      (WidgetTester tester) async {
    ViewerHostAction? picked;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      onHostAction: (ViewerHostAction action) => picked = action,
    ));

    await tester.tap(find.byIcon(Icons.power_settings_new_rounded));
    await tester.pumpAndSettle();
    expect(find.text('锁定主机屏幕'), findsNothing);
    expect(find.text('重启主机…'), findsNothing);
    expect(picked, isNull);
  });

  testWidgets('the mouse menu reports the remapping toggles and the pointer lock',
      (WidgetTester tester) async {
    int reverse = 0;
    int swap = 0;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      reverseWheel: true,
      onToggleReverseWheel: () => ++reverse,
      onToggleSwapButtons: () => ++swap,
    ));

    await tester.tap(find.byIcon(Icons.mouse_rounded));
    await tester.pumpAndSettle();
    expect(find.text('反向滚轮'), findsOneWidget);
    expect(find.text('交换左右键'), findsOneWidget);
    expect(find.text('相对鼠标'), findsOneWidget);
    expect(
      tester
          .widget<PopupMenuItem<int>>(
              find.widgetWithText(PopupMenuItem<int>, '相对鼠标'))
          .enabled,
      isFalse,
    );
    expect(
      find.descendant(
        of: find.widgetWithText(PopupMenuItem<int>, '反向滚轮'),
        matching: find.byIcon(Icons.check_rounded),
      ),
      findsOneWidget,
    );
    expect(
      find.descendant(
        of: find.widgetWithText(PopupMenuItem<int>, '交换左右键'),
        matching: find.byIcon(Icons.check_rounded),
      ),
      findsNothing,
    );

    await tester.tap(find.text('交换左右键'));
    await tester.pumpAndSettle();
    expect(swap, 1);
    expect(reverse, 0);

    await tester.tap(find.byIcon(Icons.mouse_rounded));
    await tester.pumpAndSettle();
    await tester.tap(find.text('反向滚轮'));
    await tester.pumpAndSettle();
    expect(reverse, 1);
  });

  testWidgets('the pointer lock reports taps while control is on',
      (WidgetTester tester) async {
    int lock = 0;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      controlEnabled: true,
      relativeMouse: true,
      onToggleRelativeMouse: () => ++lock,
    ));

    await tester.tap(find.byIcon(Icons.mouse_rounded));
    await tester.pumpAndSettle();
    expect(
      find.descendant(
        of: find.widgetWithText(PopupMenuItem<int>, '相对鼠标'),
        matching: find.byIcon(Icons.check_rounded),
      ),
      findsOneWidget,
    );

    await tester.tap(find.text('相对鼠标'));
    await tester.pumpAndSettle();
    expect(lock, 1);
  });

  testWidgets('the virtual display menu toggles outputs by stable id',
      (WidgetTester tester) async {
    tester.view.physicalSize = const Size(1800, 1400);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final List<Object> calls = <Object>[];
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      displayControlSupported: true,
      displayControlOutputs: const <ViewerControlledDisplay>[
        ViewerControlledDisplay(id: 31, active: true),
        ViewerControlledDisplay(id: 7, active: false),
      ],
      onToggleVirtualDisplay: (int id, bool enabled) =>
          calls.add(<Object>[id, enabled]),
    ));

    await tester.tap(find.byTooltip('虚拟显示器与隐私设置'));
    await tester.pumpAndSettle();
    await tester.tap(
        find.widgetWithText(CheckedPopupMenuItem<int>, '虚拟屏 31'));
    await tester.pumpAndSettle();
    await tester.tap(find.byTooltip('虚拟显示器与隐私设置'));
    await tester.pumpAndSettle();
    await tester.tap(
        find.widgetWithText(CheckedPopupMenuItem<int>, '虚拟屏 7'));
    await tester.pumpAndSettle();

    expect(calls, <Object>[
      <Object>[31, false],
      <Object>[7, true],
    ]);
  });

  testWidgets('display mutation follows consent while restore stays available',
      (WidgetTester tester) async {
    tester.view.physicalSize = const Size(1800, 1000);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final List<int> actions = <int>[];
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      displayControlSupported: true,
      displayControlManaged: true,
      privacyAvailable: false,
      onControlDisplays: (int action, int value) => actions.add(action),
    ));

    await tester.tap(find.byTooltip('虚拟显示器与隐私设置'));
    await tester.pumpAndSettle();
    final CheckedPopupMenuItem<int> create = tester.widget(
      find.widgetWithText(CheckedPopupMenuItem<int>, '启用 1 个虚拟屏'),
    );
    expect(create.enabled, isFalse);

    await tester.tap(find.text('恢复原显示布局'));
    await tester.pumpAndSettle();
    expect(actions, <int>[2]);
  });

  testWidgets('per-display window entries report the display index',
      (WidgetTester tester) async {
    int? opened;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      onOpenDisplayWindow: (int index) => opened = index,
    ));

    await tester.tap(find.byIcon(Icons.monitor_rounded));
    await tester.pumpAndSettle();
    expect(find.text('在新窗口打开显示器 1'), findsOneWidget);
    expect(find.text('在新窗口打开显示器 2'), findsOneWidget);

    await tester.tap(find.text('在新窗口打开显示器 2'));
    await tester.pumpAndSettle();
    expect(opened, 1);
  });

  testWidgets('independent display action is separate from merge and disables automatic following',
      (WidgetTester tester) async {
    int opens = 0;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: -3,
      onSelect: (_) => fail('independent action must not send a legacy display index'),
      onIndependentDisplays: () => opens++,
      independentDisplays: true,
    ));
    await tester.tap(find.byIcon(Icons.monitor_rounded));
    await tester.pumpAndSettle();
    expect(find.text('全部显示器（完整桌面）'), findsOneWidget);
    final follow = tester.widget<PopupMenuItem<int>>(
      find.widgetWithText(PopupMenuItem<int>, '跟随远程光标'));
    expect(follow.enabled, isFalse);
    await tester.tap(find.text('全部显示器（独立画面）'));
    await tester.pumpAndSettle();
    expect(opens, 1);
  });

  testWidgets('per-display entries stay hidden without a window opener',
      (WidgetTester tester) async {
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
    ));

    await tester.tap(find.byIcon(Icons.monitor_rounded));
    await tester.pumpAndSettle();
    expect(find.text('在新窗口打开显示器 1'), findsNothing);
    expect(find.text('在新窗口打开显示器 2'), findsNothing);
  });

  testWidgets('long press on record edits auto-record and retention',
      (WidgetTester tester) async {
    bool? auto;
    int? keep;
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
      autoRecord: true,
      recordKeep: 10,
      onAutoRecordChanged: (bool value) => auto = value,
      onRecordKeepChanged: (int value) => keep = value,
    ));

    await tester.longPress(find.byIcon(Icons.fiber_manual_record_rounded));
    await tester.pumpAndSettle();
    expect(find.text('保留最近 10 个录制'), findsOneWidget);
    expect(find.text('保留最近 30 个录制'), findsOneWidget);

    await tester.tap(find.text('每次会话自动录制'), warnIfMissed: false);
    await tester.pumpAndSettle();
    expect(auto, isFalse);

    await tester.longPress(find.byIcon(Icons.fiber_manual_record_rounded));
    await tester.pumpAndSettle();
    await tester.tap(find.text('保留最近 30 个录制'), warnIfMissed: false);
    await tester.pumpAndSettle();
    expect(keep, 30);
  });

  testWidgets('the recording menu stays closed without callbacks',
      (WidgetTester tester) async {
    await tester.pumpWidget(toolbar(
      announced: displays,
      active: 0,
      onSelect: (int index) {},
    ));

    await tester.longPress(find.byIcon(Icons.fiber_manual_record_rounded));
    await tester.pumpAndSettle();
    expect(find.text('每次会话自动录制'), findsNothing);
  });
}
