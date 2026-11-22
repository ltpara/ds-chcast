import 'package:flutter/material.dart';

import '../l10n.dart';
import '../view_scale.dart';
import '../view_scroll.dart';
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
    this.scrollStyle = defaultViewScrollStyle,
    this.scrollEdge = defaultViewScrollEdge,
    this.onScrollChanged,
  });

  final ViewFit fit;
  final int zoomPercent;
  final int cursorZoomPercent;
  final ValueChanged<ViewFit> onFitChanged;
  final ValueChanged<int> onZoomPercentChanged;
  final ValueChanged<int> onCursorZoomPercentChanged;
  final ViewScrollStyle scrollStyle;
  final int scrollEdge;
  final void Function(ViewScrollStyle, int)? onScrollChanged;

  Future<void> _open(BuildContext context) async {
    final _ViewScaleSelection? selection =
        await showDialog<_ViewScaleSelection>(
          context: context,
          builder: (BuildContext context) => _ViewScaleControlsDialog(
            fit: fit,
            zoomPercent: zoomPercent,
            cursorZoomPercent: cursorZoomPercent,
            scrollStyle: scrollStyle,
            scrollEdge: scrollEdge,
            showScrollControls: onScrollChanged != null,
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
    if (selection.scrollStyle != scrollStyle ||
        selection.scrollEdge != scrollEdge) {
      onScrollChanged?.call(selection.scrollStyle, selection.scrollEdge);
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
    required this.scrollStyle,
    required this.scrollEdge,
  });

  final ViewFit fit;
  final int zoomPercent;
  final int cursorZoomPercent;
  final ViewScrollStyle scrollStyle;
  final int scrollEdge;
}

class _ViewScaleControlsDialog extends StatefulWidget {
  const _ViewScaleControlsDialog({
    required this.fit,
    required this.zoomPercent,
    required this.cursorZoomPercent,
    required this.scrollStyle,
    required this.scrollEdge,
    required this.showScrollControls,
  });

  final ViewFit fit;
  final int zoomPercent;
  final int cursorZoomPercent;
  final ViewScrollStyle scrollStyle;
  final int scrollEdge;
  final bool showScrollControls;

  @override
  State<_ViewScaleControlsDialog> createState() =>
      _ViewScaleControlsDialogState();
}

class _ViewScaleControlsDialogState extends State<_ViewScaleControlsDialog> {
  late ViewFit _fit = widget.fit;
  late int _zoomPercent = widget.zoomPercent;
  late int _cursorZoomPercent = widget.cursorZoomPercent;
  late ViewScrollStyle _scrollStyle = widget.scrollStyle;
  late int _scrollEdge = widget.scrollEdge;

  void _reset() {
    setState(() {
      _fit = ViewFit.adaptive;
      _zoomPercent = defaultViewZoomPercent;
      _cursorZoomPercent = defaultCursorZoomPercent;
      _scrollStyle = defaultViewScrollStyle;
      _scrollEdge = defaultViewScrollEdge;
    });
  }

  @override
  Widget build(BuildContext context) => AlertDialog(
    title: Text(tr('视图与光标缩放')),
    scrollable: true,
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
          if (widget.showScrollControls) ...<Widget>[
            const SizedBox(height: 12),
            Text(tr('画布滚动方式')),
            SegmentedButton<ViewScrollStyle>(
              style: const ButtonStyle(
                padding: WidgetStatePropertyAll(
                  EdgeInsets.symmetric(horizontal: 8),
                ),
              ),
              segments: <ButtonSegment<ViewScrollStyle>>[
                ButtonSegment(
                  value: ViewScrollStyle.scrollbar,
                  label: Text(tr('滚动条')),
                ),
                ButtonSegment(
                  value: ViewScrollStyle.auto,
                  label: Text(tr('自动滚动')),
                ),
                ButtonSegment(
                  value: ViewScrollStyle.edge,
                  label: Text(tr('边缘滚动')),
                ),
              ],
              selected: <ViewScrollStyle>{_scrollStyle},
              showSelectedIcon: false,
              onSelectionChanged: (value) =>
                  setState(() => _scrollStyle = value.single),
            ),
            if (_scrollStyle == ViewScrollStyle.edge) ...<Widget>[
              Row(
                children: <Widget>[
                  Expanded(child: Text(tr('边缘感应范围'))),
                  Text('$_scrollEdge px'),
                ],
              ),
              Slider(
                value: _scrollEdge.toDouble(),
                min: minViewScrollEdge.toDouble(),
                max: maxViewScrollEdge.toDouble(),
                divisions: maxViewScrollEdge - minViewScrollEdge,
                onChanged: (value) =>
                    setState(() => _scrollEdge = value.round()),
              ),
            ],
          ],
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
            scrollStyle: _scrollStyle,
            scrollEdge: _scrollEdge,
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
