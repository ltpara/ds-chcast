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
          displays: announced,
          activeDisplay: active,
          onSelectDisplay: onSelect,
          onToggleControl: () {},
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
}
