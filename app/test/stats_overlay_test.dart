import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/l10n.dart';
import 'package:chast_viewer/target.dart';
import 'package:chast_viewer/viewer_bridge.dart';
import 'package:chast_viewer/widgets/stats_overlay.dart';

void main() {
  tearDown(() {
    AppLanguage.current.value = kLanguageChinese;
  });

  const ViewerStatus status = ViewerStatus(
    state: ViewerState.streaming,
    audioAnnounced: true,
    audioEnabled: true,
    audioLatencyMs: 42.5,
    audioStarvedMs: 560,
    audioDropped: 24,
  );

  Widget overlay() => const MaterialApp(
        home: Scaffold(
          body: StatsOverlay(
            status: status,
            target: ConnectTarget('127.0.0.1', 8443),
          ),
        ),
      );

  testWidgets('the A/V row shows starved time and dropped blocks',
      (WidgetTester tester) async {
    await tester.pumpWidget(overlay());
    expect(find.textContaining('音频 43 ms'), findsOneWidget);
    expect(find.textContaining('空转 560 ms'), findsOneWidget);
    expect(find.textContaining('丢 24'), findsOneWidget);
    expect(find.textContaining('差 — ms'), findsOneWidget);
  });

  testWidgets('the English A/V row keeps the counters',
      (WidgetTester tester) async {
    AppLanguage.current.value = kLanguageEnglish;
    await tester.pumpWidget(overlay());
    expect(find.textContaining('audio 43 ms'), findsOneWidget);
    expect(find.textContaining('starved 560 ms'), findsOneWidget);
    expect(find.textContaining('dropped 24'), findsOneWidget);
  });
}
