import 'package:flutter/material.dart';

import '../target.dart';
import '../trust_store.dart';
import '../viewer_bridge.dart';

class StatsOverlay extends StatelessWidget {
  const StatsOverlay({
    super.key,
    required this.status,
    required this.target,
    this.path = '',
  });

  final ViewerStatus status;
  final ConnectTarget target;

  /// "直连"/"中继" when a tunnel agent is in front of the QUIC connection;
  /// empty for a plain host:port connection.
  final String path;

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
                status.sourceWidth > 0 && status.sourceWidth != status.width
                    ? '${status.width}×${status.height} · 源 ${status.sourceWidth}×${status.sourceHeight}'
                    : '${status.width}×${status.height}',
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
          if (path.isNotEmpty) _row('路径', path),
          _row(
            '身份',
            status.serverFingerprint.isEmpty
                ? '—'
                : 'SHA256 ${shortenFingerprint(status.serverFingerprint)}',
          ),
          _row('解码', status.decoder.isEmpty ? '—' : status.decoder),
          _row('编码', status.serverEncoder.isEmpty ? '—' : status.serverEncoder),
          _row(
            '目标',
            status.serverBitrate > 0
                ? '${(status.serverBitrate / 1000).round()} kbps'
                : '—',
          ),
          _row(
            '延迟',
            status.rttMs > 0
                ? 'RTT ${status.rttMs.toStringAsFixed(1)} ms'
                    '${status.latencyMs > 0 ? ' · 帧龄 ${status.latencyMs.toStringAsFixed(0)} ms' : ''}'
                : '—',
          ),
          _row('帧', '${status.decoded} 解码 / ${status.received} 接收'),
          _row(
            '输入',
            !status.inputEnabled
                ? '服务端未启用'
                : '${status.inputSent} 事件'
                    '${status.inputFailed > 0 ? ' · ${status.inputFailed} 失败' : ''}',
          ),
          _row(
            '剪贴板',
            !status.clipboardEnabled
                ? '服务端未启用'
                : '收 ${status.clipboardReceived} · 发 ${status.clipboardSent}'
                    '${status.clipboardFailed > 0 ? ' · ${status.clipboardFailed} 失败' : ''}',
          ),
          if (status.acceptsAudio) ...<Widget>[
            _row(
              '音频',
              '${status.audioMuted ? '已静音 · ' : ''}'
                  '${status.audioRecovering ? '设备恢复中 · ' : ''}'
                  'AAC-LC ${status.audioSampleRateLabel} ${status.audioChannelLabel}',
            ),
            _row(
              '缓冲',
              '${status.audioBufferMs} ms · 收 ${status.audioReceived}'
                  '${status.audioUnderruns > 0 ? ' · 欠载 ${status.audioUnderruns}' : ''}'
                  '${status.audioRecoveries > 0 ? ' · 恢复 ${status.audioRecoveries}' : ''}',
            ),
          ] else
            _row(
              '音频',
              status.audioAnnounced
                  ? (status.audioError.isEmpty ? '本机无法播放' : status.audioError)
                  : '服务端未启用',
            ),
          if (anomalies > 0 || status.audioErrors > 0)
            _row(
              '异常',
              'CRC ${status.crcErrors} · 解码 ${status.decodeErrors} · 缺口 ${status.gaps}'
                  '${status.audioErrors > 0 ? ' · 音频 ${status.audioErrors}' : ''}',
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
