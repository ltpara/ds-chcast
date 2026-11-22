import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/widgets/trackpad_speed_dialog.dart';

void main() {
  testWidgets('applies a trackpad speed change', (WidgetTester tester) async {
    int? result;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: Builder(
            builder: (BuildContext context) => TextButton(
              onPressed: () async {
                result = await showDialog<int>(
                  context: context,
                  builder: (_) => const TrackpadSpeedDialog(initialValue: 100),
                );
              },
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );

    await tester.tap(find.text('open'));
    await tester.pumpAndSettle();
    expect(find.text('100%'), findsOneWidget);

    await tester.drag(find.byType(Slider), const Offset(-1000, 0));
    await tester.pumpAndSettle();
    expect(find.text('10%'), findsOneWidget);
    await tester.tap(find.text('应用'));
    await tester.pumpAndSettle();

    expect(result, 10);
  });

  testWidgets('cancel does not apply a trackpad speed change', (
    WidgetTester tester,
  ) async {
    int? result;
    var completed = false;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: Builder(
            builder: (BuildContext context) => TextButton(
              onPressed: () async {
                result = await showDialog<int>(
                  context: context,
                  builder: (_) => const TrackpadSpeedDialog(initialValue: 350),
                );
                completed = true;
              },
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );

    await tester.tap(find.text('open'));
    await tester.pumpAndSettle();
    expect(find.text('350%'), findsOneWidget);
    await tester.tap(find.text('取消'));
    await tester.pumpAndSettle();

    expect(completed, isTrue);
    expect(result, isNull);
  });
}
