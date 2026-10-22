import 'package:flutter/material.dart';

import '../theme.dart';
import '../viewer_bridge.dart';
import 'remote_view.dart';

class RemoteToolbar extends StatelessWidget {
  const RemoteToolbar({
    super.key,
    required this.collapsed,
    required this.pinned,
    required this.fullscreen,
    required this.statsVisible,
    required this.fit,
    required this.controlEnabled,
    required this.inputAvailable,
    required this.audioAvailable,
    required this.audioMuted,
    required this.displays,
    required this.activeDisplay,
    required this.onSelectDisplay,
    required this.onToggleControl,
    required this.onToggleAudio,
    required this.onTogglePin,
    required this.onToggleStats,
    required this.onToggleFullscreen,
    required this.onCollapse,
    required this.onExpand,
    required this.onClose,
    required this.onFitChanged,
  });

  final bool collapsed;
  final bool pinned;
  final bool fullscreen;
  final bool statsVisible;
  final ViewFit fit;
  final bool controlEnabled;
  final bool inputAvailable;
  final bool audioAvailable;
  final bool audioMuted;
  final List<ViewerDisplay> displays;
  final int activeDisplay;
  final ValueChanged<int> onSelectDisplay;
  final VoidCallback onToggleControl;
  final VoidCallback onToggleAudio;
  final VoidCallback onTogglePin;
  final VoidCallback onToggleStats;
  final VoidCallback onToggleFullscreen;
  final VoidCallback onCollapse;
  final VoidCallback onExpand;
  final VoidCallback onClose;
  final ValueChanged<ViewFit> onFitChanged;

  @override
  Widget build(BuildContext context) {
    if (collapsed) {
      return _CollapsedBar(
        onExpand: onExpand,
        onToggleFullscreen: onToggleFullscreen,
        fullscreen: fullscreen,
      );
    }
    return Material(
      color: kToolbarDark,
      elevation: 3,
      borderRadius: BorderRadius.circular(4),
      child: Container(
        height: 40,
        padding: const EdgeInsets.symmetric(horizontal: 4),
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(4),
          border: Border.all(color: Colors.white24),
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            _BarButton(
              icon: pinned ? Icons.push_pin_rounded : Icons.push_pin_outlined,
              tooltip: pinned ? '取消固定' : '固定工具栏',
              active: pinned,
              onTap: onTogglePin,
            ),
            _FitButton(fit: fit, onChanged: onFitChanged),
            const _BarSeparator(),
            _DisplayButton(
              displays: displays,
              active: activeDisplay,
              onSelected: onSelectDisplay,
            ),
            _BarButton(
              icon: Icons.keyboard_alt_outlined,
              tooltip: !inputAvailable
                  ? '服务端未开放输入'
                  : controlEnabled
                      ? '关闭远程键鼠'
                      : '开启远程键鼠',
              disabled: !inputAvailable,
              active: controlEnabled,
              onTap: onToggleControl,
            ),
            _BarButton(
              icon: audioMuted ? Icons.volume_off_rounded : Icons.volume_up_rounded,
              tooltip: !audioAvailable
                  ? '服务端未启用声音'
                  : audioMuted
                      ? '取消静音'
                      : '静音',
              disabled: !audioAvailable,
              active: audioAvailable && !audioMuted,
              onTap: onToggleAudio,
            ),
            _BarButton(
              icon: Icons.speed_rounded,
              tooltip: '显示或隐藏统计',
              active: statsVisible,
              onTap: onToggleStats,
            ),
            const _BarSeparator(),
            _BarButton(
              icon: fullscreen ? Icons.fullscreen_exit_rounded : Icons.fullscreen_rounded,
              tooltip: fullscreen ? '退出全屏' : '全屏',
              active: fullscreen,
              onTap: onToggleFullscreen,
            ),
            _BarButton(
              icon: Icons.expand_less_rounded,
              tooltip: '收起工具栏',
              onTap: onCollapse,
            ),
            const SizedBox(width: 2),
            _BarButton(
              icon: Icons.close_rounded,
              tooltip: '断开连接',
              danger: true,
              onTap: onClose,
            ),
          ],
        ),
      ),
    );
  }
}

class _CollapsedBar extends StatelessWidget {
  const _CollapsedBar({
    required this.onExpand,
    required this.onToggleFullscreen,
    required this.fullscreen,
  });

  final VoidCallback onExpand;
  final VoidCallback onToggleFullscreen;
  final bool fullscreen;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: kToolbarDark,
      elevation: 3,
      borderRadius: BorderRadius.circular(5),
      child: Container(
        width: 136,
        height: 20,
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(5),
          border: Border.all(color: Colors.white24),
        ),
        child: Row(
          children: <Widget>[
            const SizedBox(width: 6),
            const Icon(Icons.drag_indicator_rounded, size: 12, color: Colors.white30),
            Expanded(
              child: InkWell(
                borderRadius: BorderRadius.circular(4),
                onTap: onExpand,
                child: const Center(
                  child: Icon(Icons.expand_more_rounded, size: 15, color: Colors.grey),
                ),
              ),
            ),
            InkWell(
              borderRadius: BorderRadius.circular(4),
              onTap: onToggleFullscreen,
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 3),
                child: Icon(
                  fullscreen ? Icons.fullscreen_exit_rounded : Icons.fullscreen_rounded,
                  size: 13,
                  color: Colors.grey,
                ),
              ),
            ),
            const SizedBox(width: 4),
          ],
        ),
      ),
    );
  }
}

class _BarButton extends StatelessWidget {
  const _BarButton({
    required this.icon,
    required this.tooltip,
    this.onTap,
    this.active = false,
    this.danger = false,
    this.disabled = false,
  });

  final IconData icon;
  final String tooltip;
  final VoidCallback? onTap;
  final bool active;
  final bool danger;
  final bool disabled;

  @override
  Widget build(BuildContext context) {
    final Color color = disabled
        ? Colors.white24
        : danger
            ? Colors.redAccent
            : active
                ? kAccentBright
                : Colors.grey.shade300;
    return Tooltip(
      message: tooltip,
      waitDuration: const Duration(milliseconds: 450),
      child: SizedBox(
        width: 32,
        height: 32,
        child: Material(
          color: Colors.transparent,
          borderRadius: BorderRadius.circular(4),
          child: InkWell(
            borderRadius: BorderRadius.circular(4),
            hoverColor: Colors.white.withValues(alpha: 0.08),
            onTap: disabled ? null : onTap,
            child: Icon(icon, size: 18, color: color),
          ),
        ),
      ),
    );
  }
}

class _FitButton extends StatelessWidget {
  const _FitButton({required this.fit, required this.onChanged});

  final ViewFit fit;
  final ValueChanged<ViewFit> onChanged;

  @override
  Widget build(BuildContext context) {
    return Tooltip(
      message: '画面适应方式',
      waitDuration: const Duration(milliseconds: 450),
      child: PopupMenuButton<ViewFit>(
        tooltip: '',
        color: const Color(0xFF24252B),
        elevation: 8,
        padding: EdgeInsets.zero,
        onSelected: onChanged,
        itemBuilder: (BuildContext context) {
          return <PopupMenuEntry<ViewFit>>[
            _fitItem(ViewFit.adaptive, '自适应窗口', Icons.fit_screen_rounded),
            _fitItem(ViewFit.original, '原始大小', Icons.crop_original_rounded),
          ];
        },
        child: SizedBox(
          width: 32,
          height: 32,
          child: Icon(
            fit == ViewFit.original ? Icons.crop_original_rounded : Icons.fit_screen_rounded,
            size: 18,
            color: Colors.grey.shade300,
          ),
        ),
      ),
    );
  }

  PopupMenuItem<ViewFit> _fitItem(ViewFit value, String label, IconData icon) {
    final bool selected = fit == value;
    return PopupMenuItem<ViewFit>(
      value: value,
      height: 38,
      child: Row(
        children: <Widget>[
          Icon(icon, size: 16, color: selected ? kAccentBright : Colors.white54),
          const SizedBox(width: 10),
          Text(
            label,
            style: TextStyle(
              fontSize: 12,
              color: selected ? kAccentBright : Colors.white70,
            ),
          ),
          const Spacer(),
          if (selected) const Icon(Icons.check_rounded, size: 15, color: kAccentBright),
        ],
      ),
    );
  }
}

class _DisplayButton extends StatelessWidget {
  const _DisplayButton({
    required this.displays,
    required this.active,
    required this.onSelected,
  });

  final List<ViewerDisplay> displays;
  final int active;
  final ValueChanged<int> onSelected;

  @override
  Widget build(BuildContext context) {
    final bool available = displays.isNotEmpty;
    return Tooltip(
      message: available
          ? '切换显示器（重新连接）'
          : '服务端尚未公布显示器列表',
      waitDuration: const Duration(milliseconds: 450),
      child: PopupMenuButton<int>(
        enabled: available,
        tooltip: '',
        color: const Color(0xFF24252B),
        elevation: 8,
        padding: EdgeInsets.zero,
        constraints: const BoxConstraints(minWidth: 210, maxWidth: 300),
        onSelected: onSelected,
        itemBuilder: (BuildContext context) {
          return <PopupMenuEntry<int>>[
            _displayItem(kViewerDisplayAuto, '服务端默认', '由服务端选择输出',
                Icons.auto_mode_rounded),
            for (final ViewerDisplay display in displays)
              _displayItem(
                display.index,
                display.label.isEmpty ? '显示器 ${display.index}' : display.label,
                _detail(display),
                Icons.monitor_rounded,
              ),
          ];
        },
        child: SizedBox(
          width: 32,
          height: 32,
          child: Icon(
            Icons.monitor_rounded,
            size: 18,
            color: available ? Colors.grey.shade300 : Colors.white24,
          ),
        ),
      ),
    );
  }

  static String _detail(ViewerDisplay display) {
    final StringBuffer text = StringBuffer('${display.width}×${display.height}');
    if (display.primary) {
      text.write(' · 主屏');
    }
    if (display.rotation != 0) {
      text.write(' · ${display.rotation}°');
    }
    return text.toString();
  }

  PopupMenuItem<int> _displayItem(int value, String title, String detail, IconData icon) {
    final bool selected = value == active;
    return PopupMenuItem<int>(
      value: value,
      height: 46,
      child: Row(
        children: <Widget>[
          Icon(icon, size: 16, color: selected ? kAccentBright : Colors.white54),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                Text(
                  title,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    fontSize: 12,
                    color: selected ? kAccentBright : Colors.white70,
                  ),
                ),
                Text(
                  detail,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(fontSize: 10, color: Colors.white38),
                ),
              ],
            ),
          ),
          if (selected) const Icon(Icons.check_rounded, size: 15, color: kAccentBright),
        ],
      ),
    );
  }
}

class _BarSeparator extends StatelessWidget {
  const _BarSeparator();

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 1,
      height: 18,
      margin: const EdgeInsets.symmetric(horizontal: 4),
      color: Colors.white12,
    );
  }
}
