import 'package:flutter/material.dart';

import '../target.dart';
import '../trust_store.dart';
import '../viewer_bridge.dart';
import 'package:chast_viewer/l10n.dart';

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
                    ? tr('{p1}×{p2} · 源 {p3}×{p4}', <String, Object?>{'p1': status.width, 'p2': status.height, 'p3': status.sourceWidth, 'p4': status.sourceHeight})
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
          _row(tr('链路'), target.label),
          if (path.isNotEmpty) _row(tr('路径'), path),
          _row(tr('传输'), status.transportLabel),
          _row(
            tr('身份'),
            status.serverFingerprint.isEmpty
                ? '—'
                : 'SHA256 ${shortenFingerprint(status.serverFingerprint)}',
          ),
          _row(tr('解码'), status.decoder.isEmpty ? '—' : status.decoder),
          _row(tr('编码'), status.serverEncoder.isEmpty ? '—' : status.serverEncoder),
          _row(
            tr('目标'),
            status.serverBitrate > 0
                ? '${(status.serverBitrate / 1000).round()} kbps'
                : '—',
          ),
          _row(
            tr('延迟'),
            status.rttMs > 0
                ? tr('RTT {p11} ms{p13}', <String, Object?>{'p11': status.rttMs.toStringAsFixed(1), 'p13': status.latencyMs > 0 ? tr(' · 帧龄 {p12} ms', <String, Object?>{'p12': status.latencyMs.toStringAsFixed(0)}) : ''})
                : '—',
          ),
          _row(tr('帧'), tr('{p14} 解码 / {p15} 接收', <String, Object?>{'p14': status.decoded, 'p15': status.received})),
          _row(
            tr('输入'),
            !status.inputEnabled
                ? tr('服务端未启用')
                : tr('{p16} 事件{p18}', <String, Object?>{'p16': status.inputSent, 'p18': status.inputFailed > 0 ? tr(' · {p17} 失败', <String, Object?>{'p17': status.inputFailed}) : ''}),
          ),
          _row(
            tr('剪贴板'),
            !status.clipboardEnabled
                ? tr('服务端未启用')
                : tr('收 {p19} · 发 {p20}{p22}', <String, Object?>{'p19': status.clipboardReceived, 'p20': status.clipboardSent, 'p22': status.clipboardFailed > 0 ? tr(' · {p21} 失败', <String, Object?>{'p21': status.clipboardFailed}) : ''}),
          ),
          if (status.acceptsAudio) ...<Widget>[
            _row(
              tr('音频'),
              tr('{p23}{p24}{p25} {p26}', <String, Object?>{'p23': status.audioMuted ? tr('已静音 · ') : '', 'p24': status.audioRecovering ? tr('设备恢复中 · ') : '', 'p25': status.audioSampleRateLabel, 'p26': status.audioChannelLabel}),
            ),
            _row(
              tr('缓冲'),
              tr('{p27} ms · 收 {p28}{p30}{p32}', <String, Object?>{'p27': status.audioBufferMs, 'p28': status.audioReceived, 'p30': status.audioUnderruns > 0 ? tr(' · 欠载 {p29}', <String, Object?>{'p29': status.audioUnderruns}) : '', 'p32': status.audioRecoveries > 0 ? tr(' · 恢复 {p31}', <String, Object?>{'p31': status.audioRecoveries}) : ''}),
            ),
          ] else
            _row(
              tr('音频'),
              status.audioAnnounced
                  ? (status.audioError.isEmpty ? tr('本机无法播放') : status.audioError)
                  : tr('服务端未启用'),
            ),
          if (status.microphoneEnabled)
            _row(
              tr('麦克风'),
              tr('{p33}{p34} · 发 {p35}{p37}', <String, Object?>{'p33': status.microphoneRecovering ? tr('设备恢复中 · ') : '', 'p34': status.microphoneLabel, 'p35': status.microphonePackets, 'p37': status.microphoneDropped > 0 ? tr(' · 丢 {p36}', <String, Object?>{'p36': status.microphoneDropped}) : ''}),
            )
          else if (!status.acceptsMicrophone)
            _row(tr('麦克风'), tr('服务端未开放'))
          else
            _row(
              tr('麦克风'),
              status.microphoneError.isEmpty ? tr('已关闭') : status.microphoneError,
              color: status.microphoneError.isEmpty
                  ? const Color(0xB3FFFFFF)
                  : const Color(0xFFFFB74D),
            ),
          if (anomalies > 0 || status.audioErrors > 0 || status.microphoneErrors > 0)
            _row(
              tr('异常'),
              tr('CRC {p38} · 解码 {p39} · 缺口 {p40}{p42}{p44}', <String, Object?>{'p38': status.crcErrors, 'p39': status.decodeErrors, 'p40': status.gaps, 'p42': status.audioErrors > 0 ? tr(' · 音频 {p41}', <String, Object?>{'p41': status.audioErrors}) : '', 'p44': status.microphoneErrors > 0 ? tr(' · 麦克风 {p43}', <String, Object?>{'p43': status.microphoneErrors}) : ''}),
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
