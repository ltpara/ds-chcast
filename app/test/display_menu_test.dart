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
    ValueChanged<int>? onOpenWindow,
    List<ViewerDisplayMode> modes = const <ViewerDisplayMode>[],
    bool modeChangeAvailable = true,
    ValueChanged<ViewerDisplayMode>? onSelectMode,
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
    bool blockInputAvailable = false,
    bool blockInputEnabled = false,
    VoidCallback? onToggleBlockInput,
    bool lockAfterSessionAvailable = false,
    bool lockAfterSessionEnabled = false,
    VoidCallback? onToggleLockAfterSession,
    bool displayControlSupported = false,
    bool displayControlManaged = false,
    bool privacyAvailable = true,
    void Function(int, int)? onControlDisplays,
    List<ViewerControlledDisplay> displayControlOutputs = const <ViewerControlledDisplay>[],
    void Function(int, bool)? onToggleVirtualDisplay,
    bool terminalOpen = false,
    VoidCallback? onToggleTerminal,
    bool streamOptionsAvailable = true,
    ViewerStreamCodec streamCodec = ViewerStreamCodec.auto,
    ViewerStreamQuality streamQuality = ViewerStreamQuality.high,
    int streamMaxFps = 0,
    ValueChanged<ViewerStreamCodec>? onSelectStreamCodec,
    ValueChanged<ViewerStreamQuality>? onSelectStreamQuality,
    ValueChanged<int>? onSelectStreamFps,
  }) {
    return MaterialApp(
      home: Scaffold(
        body: RemoteToolbar(
          collapsed: false,
          pinned: true,
          fullscreen: false,
          statsVisible: false,
          fit: ViewFit.adaptive,
          controlEnabled: false,
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
          displayControlAvailable: true,
          displayControlSupported: displayControlSupported,
          displayControlManaged: displayControlManaged,
          onControlDisplays: onControlDisplays,
          displayControlOutputs: displayControlOutputs,
          onToggleVirtualDisplay: onToggleVirtualDisplay,
          privacyAvailable: privacyAvailable,
          privacyEnabled: false,
          blockInputAvailable: blockInputAvailable,
          blockInputEnabled: blockInputEnabled,
          onToggleBlockInput: onToggleBlockInput,
          lockAfterSessionAvailable: lockAfterSessionAvailable,
          lockAfterSessionEnabled: lockAfterSessionEnabled,
          onToggleLockAfterSession: onToggleLockAfterSession,
          displays: announced,
          activeDisplay: active,
          onSelectDisplay: onSelect,
          onOpenDisplayWindow: onOpenWindow,
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

  testWidgets('targeted menu sends captured identity and desired state after reordering', (tester) async {
    tester.view.physicalSize = const Size(1800, 1400);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final List<Object> calls = <Object>[];
    await tester.pumpWidget(toolbar(announced: displays, active: 0, onSelect: (_) {},
      displayControlSupported: true,
      displayControlOutputs: const <ViewerControlledDisplay>[
        ViewerControlledDisplay(id: 31, active: true), ViewerControlledDisplay(id: 7, active: false),
      ], onToggleVirtualDisplay: (id, enabled) => calls.add(<Object>[id, enabled])));
    await tester.tap(find.byTooltip('虚拟显示器与隐私设置'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('虚拟屏 31'));
    await tester.pumpAndSettle();
    await tester.tap(find.byTooltip('虚拟显示器与隐私设置'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('虚拟屏 7'));
    await tester.pumpAndSettle();
    expect(calls, <Object>[<Object>[31, false], <Object>[7, true]]);
  });

  testWidgets('display menu gates mutation but preserves recovery after revocation', (tester) async {
    tester.view.physicalSize = const Size(1800, 1000);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final List<int> actions = <int>[];
    await tester.pumpWidget(toolbar(announced: displays, active: 0, onSelect: (_) {},
      displayControlSupported: true, displayControlManaged: true, privacyAvailable: false,
      onControlDisplays: (action, value) => actions.add(action)));
    await tester.tap(find.byTooltip('虚拟显示器与隐私设置'));
    await tester.pumpAndSettle();
    final CheckedPopupMenuItem<int> create = tester.widget(find.widgetWithText(CheckedPopupMenuItem<int>, '启用 1 个虚拟屏'));
    expect(create.enabled, isFalse);
    await tester.tap(find.text('恢复原显示布局'));
    await tester.pumpAndSettle();
    expect(actions, <int>[2]);
  });

  testWidgets('input blocking needs consent but restoring stays available', (tester) async {
    int toggles = 0;
    Widget build({bool available = false, bool enabled = false}) => toolbar(
      announced: displays,
      active: 0,
      onSelect: (_) {},
      blockInputAvailable: available,
      blockInputEnabled: enabled,
      onToggleBlockInput: () => ++toggles,
    );
    await tester.pumpWidget(build());
    await tester.tap(find.byTooltip('禁用主机本地键鼠'));
    expect(toggles, 0);
    await tester.pumpWidget(build(available: true));
    await tester.tap(find.byTooltip('禁用主机本地键鼠'));
    expect(toggles, 1);
    await tester.pumpWidget(build(enabled: true));
    await tester.tap(find.byTooltip('恢复主机本地键鼠'));
    expect(toggles, 2);
    expect(tester.takeException(), isNull);
  });

  testWidgets('lock after session requires permission and remains cancellable', (tester) async {
    int toggles = 0;
    Widget build({bool allowed = false, bool enabled = false}) => toolbar(
      announced: displays, active: 0, onSelect: (_) {},
      lockAfterSessionAvailable: allowed,
      lockAfterSessionEnabled: enabled,
      onToggleLockAfterSession: () => ++toggles,
    );
    await tester.pumpWidget(build());
    await tester.tap(find.byTooltip('会话结束后锁屏（最后一个桌面会话退出时）'));
    expect(toggles, 0);
    await tester.pumpWidget(build(allowed: true));
    await tester.tap(find.byTooltip('会话结束后锁屏（最后一个桌面会话退出时）'));
    expect(toggles, 1);
    await tester.pumpWidget(build(enabled: true));
    await tester.tap(find.byTooltip('取消会话结束后锁屏'));
    expect(toggles, 2);
    expect(tester.takeException(), isNull);
  });

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

  testWidgets('opening a monitor window leaves the current selection intact', (tester) async {
    int? selected;
    int? opened;
    await tester.pumpWidget(toolbar(
      announced: displays, active: 0,
      onSelect: (int index) => selected = index,
      onOpenWindow: (int index) => opened = index,
    ));
    await tester.tap(find.byIcon(Icons.monitor_rounded));
    await tester.pumpAndSettle();
    await tester.tap(find.text('在新窗口打开显示器 2'));
    await tester.pumpAndSettle();
    expect(opened, 1);
    expect(selected, isNull);
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

    expect(find.text('全部显示器（合并）'), findsOneWidget);
    expect(find.text('3640×1920 · 2 块屏'), findsOneWidget);
    expect(
      find.descendant(
        of: find.widgetWithText(PopupMenuItem<int>, '全部显示器（合并）'),
        matching: find.byIcon(Icons.check_rounded),
      ),
      findsOneWidget,
    );

    await tester.tap(find.text('全部显示器（合并）'));
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
}
