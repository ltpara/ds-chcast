import 'package:flutter/material.dart';

import '../target.dart';
import '../viewer_bridge.dart';

class StatsOverlay extends StatelessWidget {
  const StatsOverlay({
    super.key,
    required this.status,
    required this.target,
  });

  final ViewerStatus status;
  final ConnectTarget target;

  @override
  Widget build(BuildContext context) {
    const Color labelColor = Color(0x99FFFFFF);
    const Color valueColor = Color(0xE6FFFFFF);
    final int anomalies =
        status.crcErrors + status.decodeErrors + status.protocolErrors + status.gaps;

    return Container(
      width: 226,
      padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.58),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: Colors.white12),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(
            crossAxisAlignment: CrossAxisAlignment.end,
            children: <Widget>[
              Text(
                status.fps >= 10 ? status.fps.toStringAsFixed(0) : status.fps.toStringAsFixed(1),
                style: const TextStyle(
                  color: valueColor,
                  fontSize: 19,
                  fontWeight: FontWeight.w600,
                  height: 1.0,
                  fontFeatures: <FontFeature>[FontFeature.tabularFigures()],
                ),
              ),
              const Padding(
                padding: EdgeInsets.only(left: 3, bottom: 2),
                child: Text('fps', style: TextStyle(color: labelColor, fontSize: 10)),
              ),
              const Spacer(),
              Text(
                _rate(status.kbps),
                style: const TextStyle(
                  color: valueColor,
                  fontSize: 11,
                  fontFeatures: <FontFeature>[FontFeature.tabularFigures()],
                ),
              ),
            ],
          ),
          const SizedBox(height: 5),
          Row(
            children: <Widget>[
              Text(
                '${status.width}×${status.height}',
                style: const TextStyle(
                  color: labelColor,
                  fontSize: 11,
                  fontFeatures: <FontFeature>[FontFeature.tabularFigures()],
                ),
              ),
              const Spacer(),
              Text(
                '${status.codecLabel} · ${status.framerate}fps',
                style: const TextStyle(color: labelColor, fontSize: 10),
              ),
            ],
          ),
          const SizedBox(height: 8),
          Container(height: 1, color: Colors.white12),
          const SizedBox(height: 7),
          _row('链路', target.label),
          _row('解码', status.decoder.isEmpty ? '—' : status.decoder),
          _row('编码', status.serverEncoder.isEmpty ? '—' : status.serverEncoder),
          _row('帧', '${status.decoded} 解码 / ${status.received} 接收'),
          if (anomalies > 0)
            _row(
              '异常',
              'CRC ${status.crcErrors} · 解码 ${status.decodeErrors} · 缺口 ${status.gaps}',
              color: const Color(0xFFFFB74D),
            ),
        ],
      ),
    );
  }

  Widget _row(String label, String value, {Color color = const Color(0xB3FFFFFF)}) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 3),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          SizedBox(
            width: 32,
            child: Text(
              label,
              style: const TextStyle(color: Color(0x7AFFFFFF), fontSize: 10, height: 1.3),
            ),
          ),
          Expanded(
            child: Text(
              value,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: TextStyle(color: color, fontSize: 10, height: 1.3),
            ),
          ),
        ],
      ),
    );
  }

  String _rate(double kbps) {
    if (kbps >= 1000) {
      return '${(kbps / 1000).toStringAsFixed(1)} Mbps';
    }
    return '${kbps.round()} kbps';
  }
}
