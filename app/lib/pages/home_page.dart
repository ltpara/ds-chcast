import 'dart:io';

import 'package:flutter/material.dart';

import '../recent_store.dart';
import '../target.dart';
import '../theme.dart';
import 'session_page.dart';

class HomePage extends StatefulWidget {
  const HomePage({super.key, this.initialTarget});

  final String? initialTarget;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  final TextEditingController _address = TextEditingController();
  final RecentStore _recentStore = RecentStore();
  List<RecentEntry> _recent = <RecentEntry>[];
  String? _error;
  bool _opening = false;
  String _hostname = 'Windows';

  @override
  void initState() {
    super.initState();
    try {
      _hostname = Platform.localHostname;
    } catch (_) {
      // Hostname is decorative; keep the fallback.
    }
    _loadRecent();
    final String? initial = widget.initialTarget;
    if (initial != null && initial.trim().isNotEmpty) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) {
          _connect(initial);
        }
      });
    }
  }

  @override
  void dispose() {
    _address.dispose();
    super.dispose();
  }

  Future<void> _loadRecent() async {
    final List<RecentEntry> entries = await _recentStore.load();
    if (!mounted) {
      return;
    }
    setState(() => _recent = entries);
  }

  Future<void> _connect(String input) async {
    if (_opening) {
      return;
    }
    final ConnectTarget? target = parseConnectTarget(input);
    if (target == null) {
      setState(() => _error = '请输入有效地址，例如 192.168.1.10:4433');
      return;
    }
    setState(() {
      _error = null;
      _opening = true;
    });
    _address.text = target.label;

    final List<RecentEntry> entries = await _recentStore.add(target.label);
    if (!mounted) {
      return;
    }
    setState(() => _recent = entries);

    await Navigator.of(context).push<void>(
      MaterialPageRoute<void>(builder: (BuildContext context) {
        return SessionPage(target: target);
      }),
    );
    if (!mounted) {
      return;
    }
    setState(() => _opening = false);
  }

  Future<void> _clearRecent() async {
    final List<RecentEntry> entries = await _recentStore.clear();
    if (!mounted) {
      return;
    }
    setState(() => _recent = entries);
  }

  Future<void> _showAbout() async {
    await showDialog<void>(
      context: context,
      builder: (BuildContext context) {
        return AlertDialog(
          backgroundColor: Theme.of(context).colorScheme.surface,
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
          title: const Text('关于 Chast'),
          content: const Text(
            'Chast 0.1.0\n\n'
            'Windows 桌面采集、Intel 硬件编码与 MsQuic 直连显示。'
            '当前阶段只接收画布，键鼠控制与多显示器切换将在后续接入。',
          ),
          actions: <Widget>[
            TextButton(
              onPressed: () => Navigator.of(context).pop(),
              child: const Text('关闭'),
            ),
          ],
        );
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          _Sidebar(hostname: _hostname),
          VerticalDivider(width: 1, thickness: 1, color: Theme.of(context).dividerColor),
          Expanded(child: _buildMain(context)),
        ],
      ),
    );
  }

  Widget _buildMain(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    final Color subtle = dark ? Colors.white12 : const Color(0xFFE4E6EB);
    final Color fieldFill = dark ? const Color(0xFF1F2026) : const Color(0xFFF7F8FA);

    return ScrollConfiguration(
      behavior: ScrollConfiguration.of(context).copyWith(scrollbars: false),
      child: SingleChildScrollView(
        padding: const EdgeInsets.fromLTRB(36, 48, 36, 40),
        child: Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 820),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: <Widget>[
                Center(
                  child: ConstrainedBox(
                    constraints: const BoxConstraints(maxWidth: 420),
                    child: Container(
                      padding: const EdgeInsets.fromLTRB(24, 24, 24, 22),
                      decoration: panelDecoration(context, radius: 14),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: <Widget>[
                          Text(
                            '控制远程桌面',
                            style: theme.textTheme.titleLarge?.copyWith(
                              fontSize: 19,
                              fontWeight: FontWeight.w600,
                            ),
                          ),
                          const SizedBox(height: 6),
                          Text('输入对端的 chast QUIC 直连地址', style: mutedStyle(context)),
                          const SizedBox(height: 18),
                          TextField(
                            controller: _address,
                            autofocus: widget.initialTarget == null,
                            onSubmitted: _connect,
                            style: TextStyle(
                              fontSize: 20,
                              fontWeight: FontWeight.w300,
                              letterSpacing: 0.6,
                              color: theme.colorScheme.onSurface,
                            ),
                            decoration: InputDecoration(
                              hintText: '主机:端口',
                              hintStyle: TextStyle(
                                fontSize: 18,
                                fontWeight: FontWeight.w300,
                                color: dark ? Colors.white38 : const Color(0xFF9AA0A6),
                              ),
                              prefixIcon: Icon(
                                Icons.link_rounded,
                                size: 20,
                                color: dark ? Colors.white54 : const Color(0xFF7C8390),
                              ),
                              filled: true,
                              fillColor: fieldFill,
                              contentPadding: const EdgeInsets.symmetric(horizontal: 12, vertical: 14),
                              enabledBorder: OutlineInputBorder(
                                borderRadius: BorderRadius.circular(8),
                                borderSide: BorderSide(color: subtle),
                              ),
                              focusedBorder: OutlineInputBorder(
                                borderRadius: BorderRadius.circular(8),
                                borderSide: const BorderSide(color: kAccent, width: 1.5),
                              ),
                            ),
                          ),
                          if (_error != null) ...<Widget>[
                            const SizedBox(height: 10),
                            Row(
                              children: <Widget>[
                                Icon(Icons.error_outline, size: 15, color: theme.colorScheme.error),
                                const SizedBox(width: 6),
                                Expanded(
                                  child: Text(
                                    _error!,
                                    style: TextStyle(fontSize: 12, color: theme.colorScheme.error),
                                  ),
                                ),
                              ],
                            ),
                          ],
                          const SizedBox(height: 16),
                          Row(
                            children: <Widget>[
                              Expanded(
                                child: SizedBox(
                                  height: 40,
                                  child: ElevatedButton(
                                    onPressed: _opening ? null : () => _connect(_address.text),
                                    style: ElevatedButton.styleFrom(
                                      backgroundColor: kAccent,
                                      foregroundColor: Colors.white,
                                      disabledBackgroundColor: kAccent.withValues(alpha: 0.5),
                                      elevation: 0,
                                      shape: RoundedRectangleBorder(
                                        borderRadius: BorderRadius.circular(8),
                                      ),
                                      textStyle: const TextStyle(fontSize: 14, fontWeight: FontWeight.w600),
                                    ),
                                    child: _opening
                                        ? const SizedBox(
                                            width: 16,
                                            height: 16,
                                            child: CircularProgressIndicator(
                                              strokeWidth: 2,
                                              valueColor: AlwaysStoppedAnimation<Color>(Colors.white),
                                            ),
                                          )
                                        : const Text('连接'),
                                  ),
                                ),
                              ),
                              const SizedBox(width: 8),
                              SizedBox(
                                width: 40,
                                height: 40,
                                child: Material(
                                  color: fieldFill,
                                  borderRadius: BorderRadius.circular(8),
                                  child: InkWell(
                                    borderRadius: BorderRadius.circular(8),
                                    onTap: _showAbout,
                                    child: Icon(
                                      Icons.more_horiz_rounded,
                                      size: 20,
                                      color: dark ? Colors.white70 : const Color(0xFF5A6270),
                                    ),
                                  ),
                                ),
                              ),
                            ],
                          ),
                        ],
                      ),
                    ),
                  ),
                ),
                const SizedBox(height: 34),
                Row(
                  children: <Widget>[
                    Text(
                      '最近连接',
                      style: theme.textTheme.titleMedium?.copyWith(
                        fontSize: 15,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                    const SizedBox(width: 8),
                    if (_recent.isNotEmpty)
                      Text(
                        '${_recent.length}',
                        style: mutedStyle(context, size: 12),
                      ),
                    const Spacer(),
                    if (_recent.isNotEmpty)
                      TextButton(
                        onPressed: _clearRecent,
                        style: TextButton.styleFrom(foregroundColor: kAccent),
                        child: const Text('清空', style: TextStyle(fontSize: 12)),
                      ),
                  ],
                ),
                const SizedBox(height: 12),
                if (_recent.isEmpty)
                  Container(
                    height: 132,
                    decoration: BoxDecoration(
                      color: theme.colorScheme.surface.withValues(alpha: 0.55),
                      borderRadius: BorderRadius.circular(14),
                      border: Border.all(color: subtle),
                    ),
                    child: Center(
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        children: <Widget>[
                          Icon(Icons.history_rounded, size: 26, color: dark ? Colors.white30 : const Color(0xFFA8AEB8)),
                          const SizedBox(height: 8),
                          Text('连接过的地址会出现在这里', style: mutedStyle(context)),
                        ],
                      ),
                    ),
                  )
                else
                  Wrap(
                    spacing: 12,
                    runSpacing: 12,
                    children: <Widget>[
                      for (final RecentEntry entry in _recent)
                        _RecentCard(
                          entry: entry,
                          onTap: () {
                            _address.text = entry.target;
                            _connect(entry.target);
                          },
                        ),
                    ],
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _Sidebar extends StatelessWidget {
  const _Sidebar({required this.hostname});

  final String hostname;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    final Color subtle = dark ? Colors.white12 : const Color(0xFFE2E4E9);

    return SizedBox(
      width: 208,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(20, 26, 20, 20),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Row(
              children: <Widget>[
                Container(
                  width: 36,
                  height: 36,
                  decoration: BoxDecoration(
                    gradient: const LinearGradient(
                      colors: <Color>[kAccentBright, kAccent],
                      begin: Alignment.topLeft,
                      end: Alignment.bottomRight,
                    ),
                    borderRadius: BorderRadius.circular(10),
                  ),
                  child: const Icon(Icons.desktop_windows_rounded, color: Colors.white, size: 20),
                ),
                const SizedBox(width: 10),
                Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    Text(
                      'chast',
                      style: theme.textTheme.titleLarge?.copyWith(
                        fontSize: 20,
                        fontWeight: FontWeight.w700,
                        letterSpacing: -0.4,
                        height: 1.0,
                      ),
                    ),
                    const SizedBox(height: 3),
                    Text('远程桌面', style: mutedStyle(context, size: 10)),
                  ],
                ),
              ],
            ),
            const SizedBox(height: 34),
            Text(
              '本机信息',
              style: TextStyle(
                fontSize: 10,
                fontWeight: FontWeight.w600,
                letterSpacing: 1.1,
                color: dark ? Colors.white38 : const Color(0xFF8A919C),
              ),
            ),
            const SizedBox(height: 8),
            ClipRRect(
              borderRadius: BorderRadius.circular(8),
              child: Container(
                height: 58,
                color: theme.colorScheme.surface,
                child: Row(
                  children: <Widget>[
                    Container(width: 3, color: kAccent),
                    Expanded(
                      child: Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 12),
                        child: Column(
                          mainAxisAlignment: MainAxisAlignment.center,
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: <Widget>[
                            Text(
                              hostname,
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                              style: TextStyle(
                                fontSize: 13,
                                fontWeight: FontWeight.w600,
                                color: theme.colorScheme.onSurface,
                              ),
                            ),
                            const SizedBox(height: 3),
                            Text('直连模式 · 未启用监听', style: mutedStyle(context, size: 10)),
                          ],
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 12),
            Text(
              '身份、中继与地址簿将在下一阶段接入。',
              style: mutedStyle(context, size: 11),
            ),
            const Spacer(),
            Container(
              padding: const EdgeInsets.all(16),
              decoration: BoxDecoration(
                gradient: const LinearGradient(
                  colors: <Color>[Color(0xFFE242BC), Color(0xFFF4727C)],
                  begin: Alignment.topLeft,
                  end: Alignment.bottomRight,
                ),
                borderRadius: BorderRadius.circular(12),
              ),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  Container(
                    width: 30,
                    height: 30,
                    decoration: BoxDecoration(
                      color: Colors.white.withValues(alpha: 0.22),
                      shape: BoxShape.circle,
                    ),
                    child: const Icon(Icons.bolt_rounded, color: Colors.white, size: 18),
                  ),
                  const SizedBox(height: 10),
                  const Text(
                    'QUIC 直连',
                    style: TextStyle(color: Colors.white, fontSize: 14, fontWeight: FontWeight.w600),
                  ),
                  const SizedBox(height: 3),
                  Text(
                    '端到端加密\nGPU 硬件编解码',
                    style: TextStyle(
                      color: Colors.white.withValues(alpha: 0.85),
                      fontSize: 11,
                      height: 1.4,
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 14),
            Row(
              children: <Widget>[
                Icon(Icons.circle, size: 7, color: dark ? Colors.white24 : const Color(0xFFB6BCC6)),
                const SizedBox(width: 6),
                Text('chast 0.1.0 · Windows', style: mutedStyle(context, size: 10)),
              ],
            ),
            const SizedBox(height: 4),
            Container(height: 1, color: subtle),
          ],
        ),
      ),
    );
  }
}

class _RecentCard extends StatelessWidget {
  const _RecentCard({required this.entry, required this.onTap});

  final RecentEntry entry;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    final Color subtle = dark ? Colors.white12 : const Color(0xFFE4E6EB);

    return SizedBox(
      width: 240,
      height: 86,
      child: Material(
        color: theme.colorScheme.surface,
        borderRadius: BorderRadius.circular(14),
        clipBehavior: Clip.antiAlias,
        child: InkWell(
          onTap: onTap,
          hoverColor: kAccent.withValues(alpha: 0.05),
          child: Container(
            padding: const EdgeInsets.symmetric(horizontal: 14),
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(14),
              border: Border.all(color: subtle),
            ),
            child: Row(
              children: <Widget>[
                Container(
                  width: 40,
                  height: 40,
                  decoration: BoxDecoration(
                    color: kAccent.withValues(alpha: 0.10),
                    borderRadius: BorderRadius.circular(10),
                  ),
                  child: const Icon(Icons.desktop_windows_rounded, color: kAccent, size: 20),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Text(
                        entry.target,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: TextStyle(
                          fontSize: 13,
                          fontWeight: FontWeight.w600,
                          color: theme.colorScheme.onSurface,
                        ),
                      ),
                      const SizedBox(height: 4),
                      Text(
                        '最近连接 · ${_formatTime(entry.lastUsed)}',
                        style: mutedStyle(context, size: 10),
                      ),
                    ],
                  ),
                ),
                Icon(Icons.chevron_right_rounded, size: 18, color: dark ? Colors.white30 : const Color(0xFFB0B6C0)),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

String _formatTime(DateTime time) {
  final DateTime now = DateTime.now();
  final DateTime today = DateTime(now.year, now.month, now.day);
  final DateTime day = DateTime(time.year, time.month, time.day);
  final String hm =
      '${time.hour.toString().padLeft(2, '0')}:${time.minute.toString().padLeft(2, '0')}';
  if (day == today) {
    return '今天 $hm';
  }
  if (today.difference(day).inDays == 1) {
    return '昨天 $hm';
  }
  return '${time.month.toString().padLeft(2, '0')}-${time.day.toString().padLeft(2, '0')} $hm';
}
