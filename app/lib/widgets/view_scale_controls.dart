import 'package:flutter/material.dart';

import '../l10n.dart';
import '../view_scale.dart';
import 'remote_view.dart';

class ViewScaleControlsButton extends StatelessWidget {
  const ViewScaleControlsButton({
    super.key,
    required this.fit,
    required this.zoomPercent,
    required this.cursorZoomPercent,
    required this.onFitChanged,
    required this.onZoomPercentChanged,
    required this.onCursorZoomPercentChanged,
  });

  final ViewFit fit;
  final int zoomPercent;
  final int cursorZoomPercent;
  final ValueChanged<ViewFit> onFitChanged;
  final ValueChanged<int> onZoomPercentChanged;
  final ValueChanged<int> onCursorZoomPercentChanged;

  Future<void> _open(BuildContext context) async {
    final _ViewScaleSelection? selection =
        await showDialog<_ViewScaleSelection>(
          context: context,
          builder: (BuildContext context) => _ViewScaleControlsDialog(
            fit: fit,
            zoomPercent: zoomPercent,
            cursorZoomPercent: cursorZoomPercent,
          ),
        );
    if (!context.mounted || selection == null) return;
    if (selection.fit != fit) onFitChanged(selection.fit);
    if (selection.zoomPercent != zoomPercent) {
      onZoomPercentChanged(selection.zoomPercent);
    }
    if (selection.cursorZoomPercent != cursorZoomPercent) {
      onCursorZoomPercentChanged(selection.cursorZoomPercent);
    }
  }

  @override
  Widget build(BuildContext context) => Tooltip(
    message: tr('视图与光标缩放'),
    waitDuration: const Duration(milliseconds: 450),
    child: SizedBox(
      width: 32,
      height: 32,
      child: IconButton(
        tooltip: '',
        padding: EdgeInsets.zero,
        visualDensity: VisualDensity.compact,
        constraints: const BoxConstraints.tightFor(width: 32, height: 32),
        iconSize: 18,
        onPressed: () => _open(context),
        icon: const Icon(Icons.zoom_in_map_rounded),
      ),
    ),
  );
}

class _ViewScaleSelection {
  const _ViewScaleSelection({
    required this.fit,
    required this.zoomPercent,
    required this.cursorZoomPercent,
  });

  final ViewFit fit;
  final int zoomPercent;
  final int cursorZoomPercent;
}

class _ViewScaleControlsDialog extends StatefulWidget {
  const _ViewScaleControlsDialog({
    required this.fit,
    required this.zoomPercent,
    required this.cursorZoomPercent,
  });

  final ViewFit fit;
  final int zoomPercent;
  final int cursorZoomPercent;

  @override
  State<_ViewScaleControlsDialog> createState() =>
      _ViewScaleControlsDialogState();
}

class _ViewScaleControlsDialogState extends State<_ViewScaleControlsDialog> {
  late ViewFit _fit = widget.fit;
  late int _zoomPercent = widget.zoomPercent;
  late int _cursorZoomPercent = widget.cursorZoomPercent;

  void _reset() {
    setState(() {
      _fit = ViewFit.adaptive;
      _zoomPercent = defaultViewZoomPercent;
      _cursorZoomPercent = defaultCursorZoomPercent;
    });
  }

  @override
  Widget build(BuildContext context) => AlertDialog(
    title: Text(tr('视图与光标缩放')),
    content: SizedBox(
      width: 380,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          SegmentedButton<ViewFit>(
            segments: <ButtonSegment<ViewFit>>[
              ButtonSegment<ViewFit>(
                value: ViewFit.adaptive,
                icon: const Icon(Icons.fit_screen_rounded),
                label: Text(tr('自适应窗口')),
              ),
              ButtonSegment<ViewFit>(
                value: ViewFit.original,
                icon: const Icon(Icons.crop_original_rounded),
                label: Text(tr('原始大小')),
              ),
            ],
            selected: <ViewFit>{_fit},
            showSelectedIcon: false,
            onSelectionChanged: (Set<ViewFit> value) {
              setState(() => _fit = value.single);
            },
          ),
          const SizedBox(height: 18),
          _ScaleSlider(
            label: tr('视图缩放'),
            percent: _zoomPercent,
            min: minViewZoomPercent,
            pivot: defaultViewZoomPercent,
            max: maxViewZoomPercent,
            pivotPosition: viewZoomPivotPosition,
            onChanged: (int value) => setState(() => _zoomPercent = value),
          ),
          const SizedBox(height: 12),
          _ScaleSlider(
            label: tr('光标缩放'),
            percent: _cursorZoomPercent,
            min: minCursorZoomPercent,
            pivot: defaultCursorZoomPercent,
            max: maxCursorZoomPercent,
            pivotPosition: cursorZoomPivotPosition,
            onChanged: (int value) =>
                setState(() => _cursorZoomPercent = value),
          ),
        ],
      ),
    ),
    actions: <Widget>[
      TextButton(onPressed: _reset, child: Text(tr('重置'))),
      TextButton(
        onPressed: () => Navigator.of(context).pop(),
        child: Text(tr('取消')),
      ),
      FilledButton(
        onPressed: () => Navigator.of(context).pop(
          _ViewScaleSelection(
            fit: _fit,
            zoomPercent: _zoomPercent,
            cursorZoomPercent: _cursorZoomPercent,
          ),
        ),
        child: Text(tr('应用')),
      ),
    ],
  );
}

class _ScaleSlider extends StatelessWidget {
  const _ScaleSlider({
    required this.label,
    required this.percent,
    required this.min,
    required this.pivot,
    required this.max,
    required this.pivotPosition,
    required this.onChanged,
  });

  final String label;
  final int percent;
  final int min;
  final int pivot;
  final int max;
  final double pivotPosition;
  final ValueChanged<int> onChanged;

  @override
  Widget build(BuildContext context) => Column(
    crossAxisAlignment: CrossAxisAlignment.stretch,
    children: <Widget>[
      Row(
        children: <Widget>[
          Expanded(child: Text(label)),
          Text('$percent%'),
        ],
      ),
      Slider(
        value: scalePercentToSlider(
          percent,
          min: min,
          pivot: pivot,
          max: max,
          pivotPosition: pivotPosition,
        ),
        min: 0,
        max: 1,
        onChanged: (double value) => onChanged(
          scaleSliderToPercent(
            value,
            min: min,
            pivot: pivot,
            max: max,
            pivotPosition: pivotPosition,
          ),
        ),
      ),
    ],
  );
}
