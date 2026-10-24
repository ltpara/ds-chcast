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
    List<ViewerDisplayMode> modes = const <ViewerDisplayMode>[],
    bool modeChangeAvailable = true,
    ValueChanged<ViewerDisplayMode>? onSelectMode,
    bool fileAvailable = true,
    bool fileActive = false,
    VoidCallback? onSendFile,
    bool specialKeysAvailable = false,
    ValueChanged<RemoteSpecialAction>? onSpecialAction,
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
          audioAvailable: true,
          audioMuted: false,
          microphoneAvailable: true,
          microphoneEnabled: false,
          microphoneDevice: '',
          onLoadMicrophones: () async => const <ViewerMicrophoneDevice>[],
          onSelectMicrophone: (_) async {},
          privacyAvailable: true,
          privacyEnabled: false,
          displays: announced,
          activeDisplay: active,
          onSelectDisplay: onSelect,
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
          onToggleClipboard: () {},
          onSendFile: onSendFile ?? () {},
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
}
