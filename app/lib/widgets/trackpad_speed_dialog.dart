import 'package:flutter/material.dart';

import '../l10n.dart';
import '../trackpad_speed.dart';

class TrackpadSpeedDialog extends StatefulWidget {
  const TrackpadSpeedDialog({super.key, required this.initialValue});

  final int initialValue;

  @override
  State<TrackpadSpeedDialog> createState() => _TrackpadSpeedDialogState();
}

class _TrackpadSpeedDialogState extends State<TrackpadSpeedDialog> {
  late int _value = normalizeTrackpadSpeedPercent(widget.initialValue);

  @override
  Widget build(BuildContext context) => AlertDialog(
    title: Text(tr('触控板速度倍率')),
    content: SizedBox(
      width: 360,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          Text(tr('调整双指平移速度，不影响缩放')),
          Slider(
            value: _value.toDouble(),
            min: kMinTrackpadSpeedPercent.toDouble(),
            max: kMaxTrackpadSpeedPercent.toDouble(),
            divisions:
                (kMaxTrackpadSpeedPercent - kMinTrackpadSpeedPercent) ~/ 10,
            label: '$_value%',
            onChanged: (double next) => setState(() => _value = next.round()),
          ),
          Text('$_value%'),
        ],
      ),
    ),
    actions: <Widget>[
      TextButton(
        onPressed: () => Navigator.of(context).pop(),
        child: Text(tr('取消')),
      ),
      FilledButton(
        onPressed: () => Navigator.of(context).pop(_value),
        child: Text(tr('应用')),
      ),
    ],
  );
}
