import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';

import '../device_store.dart';
import '../server_api.dart';
import '../server_settings.dart';
import '../target.dart';
import '../theme.dart';
import '../trust_store.dart';
import 'session_page.dart';

typedef HostLookup = Future<HostInfo> Function(ServerSettings server, String hostId);

class HomePage extends StatefulWidget {
  const HomePage({
    super.key,
    this.initialTarget,
    this.deviceStore,
    this.serverStore,
    this.trustStore,
    this.lookup,
  });

  final String? initialTarget;

  /// Test seams. Product code uses the files under %APPDATA%\chast.
  final DeviceStore? deviceStore;
  final ServerSettingsStore? serverStore;
  final TrustStore? trustStore;
  final HostLookup? lookup;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  static const Duration _presenceInterval = Duration(seconds: 10);

  final TextEditingController _address = TextEditingController();
  final TextEditingController _token = TextEditingController();
  late final DeviceStore _deviceStore;
  late final ServerSettingsStore _serverStore;
  late final TrustStore _trustStore;
  late final HostLookup _lookup;

  List<Device> _devices = <Device>[];
  ServerSettings _server = const ServerSettings();
  Map<String, String> _trust = <String, String>{};
  final Map<String, DevicePresence> _presence = <String, DevicePresence>{};
  Timer? _presenceTimer;
  bool _presenceRunning = false;
  int _presenceGeneration = 0;
  bool _loading = true;
  bool _opening = false;
  bool _initialHandled = false;
  String? _error;
  String _hostname = 'Windows';

  @override
  void initState() {
    super.initState();
    _deviceStore = widget.deviceStore ?? DeviceStore();
    _serverStore = widget.serverStore ?? ServerSettingsStore();
    _trustStore = widget.trustStore ?? TrustStore();
    _lookup = widget.lookup ?? ServerApi.lookup;
    try {
      _hostname = Platform.localHostname;
    } catch (_) {
      // Hostname is decorative; keep the fallback.
    }
    unawaited(_load());
    _presenceTimer = Timer.periodic(
      _presenceInterval,
      (_) => unawaited(_refreshPresence()),
    );
  }

  @override
  void dispose() {
    _presenceTimer?.cancel();
    _address.dispose();
    _token.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    final List<Device> devices = await _deviceStore.load();
    final ServerSettings server = await _serverStore.load();
    final Map<String, String> trust = await _trustStore.load();
    if (!mounted) {
      return;
    }
    setState(() {
      _devices = devices;
      _server = server;
      _trust = trust;
      _loading = false;
    });
    unawaited(_refreshPresence());
    if (!_initialHandled && widget.initialTarget != null) {
      _initialHandled = true;
      final String target = widget.initialTarget!;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) {
          _connect(target);
        }
      });
    }
  }

  Future<void> _reloadDevices() async {
    final List<Device> devices = await _deviceStore.load();
    if (mounted) {
      setState(() => _devices = devices);
    }
  }

  Future<void> _reloadTrust() async {
    final Map<String, String> trust = await _trustStore.load();
    if (mounted) {
      setState(() => _trust = trust);
    }
  }

  Future<void> _refreshPresence() async {
    final List<Device> devices =
        _devices.where((Device device) => device.viaServer).toList();
    if (devices.isEmpty) {
      if (_presence.isNotEmpty && mounted) {
        setState(() => _presence.clear());
      }
      return;
    }
    if (_presenceRunning) {
      return;
    }
    _presenceRunning = true;
    final int generation = ++_presenceGeneration;
    final Map<String, DevicePresence> result = <String, DevicePresence>{};
    if (mounted) {
      setState(() {
        for (final Device device in devices) {
          _presence.putIfAbsent(device.key, () => DevicePresence.unknown);
        }
      });
    }
    await Future.wait(<Future<void>>[
      for (final Device device in devices)
        () async {
          try {
            final HostInfo info = await _lookup(
              ServerSettings(url: device.server, mode: _server.mode),
              device.id,
            );
            result[device.key] = info.online
                ? DevicePresence(HostPresence.online, info: info)
                : DevicePresence(HostPresence.offline, info: info);
          } catch (error) {
            result[device.key] = DevicePresence(
              HostPresence.unreachable,
              error: '$error',
            );
          }
        }(),
    ]);
    _presenceRunning = false;
    if (!mounted || generation != _presenceGeneration) {
      return;
    }
    setState(() {
      _presence
        ..clear()
        ..addAll(result);
    });
  }

  bool _looksLikeAddress(String text) {
    return text.startsWith('[') || text.contains(':');
  }

  Future<void> _connect(String input) async {
    if (_opening) {
      return;
    }
    final String text = input.trim();
    final String token = _token.text.trim();
    if (text.isEmpty) {
      setState(() => _error = '请输入设备 ID 或主机地址');
      return;
    }
    if (!_looksLikeAddress(text) && !_server.configured) {
      setState(() => _error = '设备 ID 需要先配置连接服务，或输入 主机:端口');
      return;
    }
    if (utf8ByteLength(token) > 128) {
      setState(() => _error = '访问令牌过长，最多 128 字节');
      return;
    }
    final ConnectTarget? target =
        parseConnectInput(text, token: token, server: _server);
    if (target == null) {
      setState(() => _error = '格式无效：请输入设备 ID 或 主机:端口');
      return;
    }
    await _open(target);
  }

  Future<void> _connectDevice(Device device) async {
    if (_opening) {
      return;
    }
    final String token = _token.text.trim();
    if (utf8ByteLength(token) > 128) {
      setState(() => _error = '访问令牌过长，最多 128 字节');
      return;
    }
    final ConnectTarget? target;
    if (device.viaServer) {
      target = ConnectTarget(
        '',
        0,
        token: token,
        server: device.server,
        hostId: device.id,
        displayName: device.name,
        mode: _server.mode,
      );
    } else {
      target = parseConnectTarget(device.id, token: token);
    }
    if (target == null) {
      setState(() => _error = '设备地址无效：${device.id}');
      return;
    }
    _address.text = target.label;
    await _open(target);
  }

  Future<void> _open(ConnectTarget target) async {
    if (_opening) {
      return;
    }
    setState(() {
      _error = null;
      _opening = true;
    });
    _address.text = target.label;
    final Device device = _deviceForTarget(target);
    await _deviceStore.touch(device);
    await _reloadDevices();
    if (!mounted) {
      return;
    }
    await Navigator.of(context).push<void>(
      MaterialPageRoute<void>(builder: (BuildContext context) {
        return SessionPage(target: target);
      }),
    );
    if (!mounted) {
      return;
    }
    setState(() => _opening = false);
    await _reloadTrust();
    unawaited(_refreshPresence());
  }

  Device _deviceForTarget(ConnectTarget target) {
    if (target.viaServer) {
      final String key =
          Device(id: target.hostId, addedAt: DateTime.now(), server: target.server).key;
      for (final Device device in _devices) {
        if (device.key == key) {
          return device;
        }
      }
      return Device(
        id: target.hostId,
        name: target.displayName,
        server: target.server,
        addedAt: DateTime.now(),
      );
    }
    return Device(id: target.label, addedAt: DateTime.now());
  }

  Future<void> _clearRecent() async {
    final List<Device> devices = await _deviceStore.clearRecent();
    if (!mounted) {
      return;
    }
    setState(() => _devices = devices);
    unawaited(_refreshPresence());
  }

  Future<void> _promote(Device device) async {
    final List<Device> devices = await _deviceStore.promote(device.key);
    if (!mounted) {
      return;
    }
    setState(() => _devices = devices);
  }

  Future<void> _showAddDevice() async {
    final Device? device = await _showDeviceDialog();
    if (device == null) {
      return;
    }
    final List<Device> devices = await _deviceStore.save(device);
    if (!mounted) {
      return;
    }
    setState(() => _devices = devices);
    unawaited(_refreshPresence());
  }

  Future<Device?> _showDeviceDialog() async {
    final TextEditingController name = TextEditingController();
    final TextEditingController target = TextEditingController();
    String? error;
    final Device? device = await showDialog<Device>(
      context: context,
      builder: (BuildContext context) {
        return StatefulBuilder(
          builder: (BuildContext context, StateSetter setDialogState) {
            final String service = _server.configured ? _server.url : '未配置连接服务';
            return AlertDialog(
              backgroundColor: Theme.of(context).colorScheme.surface,
              shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
              title: const Text('添加设备'),
              content: SizedBox(
                width: 420,
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: <Widget>[
                    TextField(
                      controller: name,
                      decoration: const InputDecoration(
                        labelText: '名称（可选）',
                        hintText: '例如：开发机',
                      ),
                    ),
                    const SizedBox(height: 12),
                    TextField(
                      controller: target,
                      autofocus: true,
                      decoration: const InputDecoration(
                        labelText: '设备 ID 或地址',
                        hintText: 'devbox 或 192.168.1.10:4433',
                      ),
                    ),
                    const SizedBox(height: 10),
                    Text(
                      '设备 ID 通过 $service 解析；带 : 的地址按直连保存。',
                      style: mutedStyle(context, size: 11),
                    ),
                    if (error != null) ...<Widget>[
                      const SizedBox(height: 8),
                      Text(
                        error!,
                        style: TextStyle(
                          color: Theme.of(context).colorScheme.error,
                          fontSize: 12,
                        ),
                      ),
                    ],
                  ],
                ),
              ),
              actions: <Widget>[
                TextButton(
                  onPressed: () => Navigator.of(context).pop(),
                  child: const Text('取消'),
                ),
                FilledButton(
                  onPressed: () {
                    final String text = target.text.trim();
                    if (text.isEmpty) {
                      setDialogState(() => error = '请输入设备 ID 或地址');
                      return;
                    }
                    if (!_looksLikeAddress(text) && !_server.configured) {
                      setDialogState(() => error = '设备 ID 需要先配置连接服务');
                      return;
                    }
                    final ConnectTarget? parsed = parseConnectInput(
                      text,
                      token: '',
                      server: _server,
                    );
                    if (parsed == null) {
                      setDialogState(() => error = '格式无效');
                      return;
                    }
                    final Device created = parsed.viaServer
                        ? Device(
                            id: parsed.hostId,
                            name: name.text.trim(),
                            server: parsed.server,
                            addedAt: DateTime.now(),
                          )
                        : Device(
                            id: parsed.label,
                            name: name.text.trim(),
                            addedAt: DateTime.now(),
                          );
                    Navigator.of(context).pop(created);
                  },
                  child: const Text('保存'),
                ),
              ],
            );
          },
        );
      },
    );
    name.dispose();
    target.dispose();
    return device;
  }

  Future<void> _renameDevice(Device device) async {
    final TextEditingController name = TextEditingController(text: device.name);
    final String? result = await showDialog<String>(
      context: context,
      builder: (BuildContext context) {
        return AlertDialog(
          backgroundColor: Theme.of(context).colorScheme.surface,
          title: const Text('重命名设备'),
          content: TextField(
            controller: name,
            autofocus: true,
            decoration: InputDecoration(hintText: device.id),
            onSubmitted: (String value) => Navigator.of(context).pop(value),
          ),
          actions: <Widget>[
            TextButton(
              onPressed: () => Navigator.of(context).pop(),
              child: const Text('取消'),
            ),
            FilledButton(
              onPressed: () => Navigator.of(context).pop(name.text),
              child: const Text('保存'),
            ),
          ],
        );
      },
    );
    name.dispose();
    if (result == null) {
      return;
    }
    final List<Device> devices = await _deviceStore.rename(device.key, result);
    if (mounted) {
      setState(() => _devices = devices);
    }
  }

  Future<void> _deleteDevice(Device device) async {
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) {
        return AlertDialog(
          backgroundColor: Theme.of(context).colorScheme.surface,
          title: const Text('删除设备'),
          content: Text('从地址簿删除“${device.displayName}”？'),
          actions: <Widget>[
            TextButton(
              onPressed: () => Navigator.of(context).pop(false),
              child: const Text('取消'),
            ),
            FilledButton(
              onPressed: () => Navigator.of(context).pop(true),
              style: FilledButton.styleFrom(backgroundColor: Theme.of(context).colorScheme.error),
              child: const Text('删除'),
            ),
          ],
        );
      },
    );
    if (confirmed != true) {
      return;
    }
    final List<Device> devices = await _deviceStore.remove(device.key);
    if (mounted) {
      setState(() => _devices = devices);
      unawaited(_refreshPresence());
    }
  }

  Future<void> _showServerSettings() async {
    final ServerSettings? settings = await showDialog<ServerSettings>(
      context: context,
      builder: (BuildContext context) {
        return _ServerDialog(initial: _server);
      },
    );
    if (settings == null) {
      return;
    }
    await _serverStore.save(settings);
    if (!mounted) {
      return;
    }
    setState(() => _server = settings);
    unawaited(_refreshPresence());
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
            'Windows 桌面采集、Intel 硬件编码、MsQuic 直连显示与键鼠控制。'
            '远端需以 chast-quic serve --input 启动才会接受输入；'
            '设备 ID 经 chast-server 解析，直连失败时自动回退中继。',
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

  List<Device> get _savedDevices {
    final List<Device> devices = _devices.where((Device device) => device.saved).toList();
    devices.sort((Device a, Device b) => b.sortTime.compareTo(a.sortTime));
    return devices;
  }

  List<Device> get _recentDevices {
    final List<Device> devices = _devices
        .where((Device device) => !device.saved && device.lastUsed != null)
        .toList();
    devices.sort((Device a, Device b) => b.sortTime.compareTo(a.sortTime));
    return devices;
  }

  int get _onlineCount => _presence.values
      .where((DevicePresence presence) => presence.online)
      .length;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          _Sidebar(
            hostname: _hostname,
            server: _server,
            trustedCount: _trust.length,
            onlineCount: _onlineCount,
            onConfigureServer: _showServerSettings,
          ),
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
    final List<Device> saved = _savedDevices;
    final List<Device> recent = _recentDevices;

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
                          Text('输入设备 ID，或直接连接对端 chast QUIC 地址',
                              style: mutedStyle(context)),
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
                              hintText: '设备 ID 或 主机:端口',
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
                          const SizedBox(height: 10),
                          TextField(
                            controller: _token,
                            onSubmitted: _connect,
                            style: TextStyle(
                              fontSize: 14,
                              letterSpacing: 0.6,
                              color: theme.colorScheme.onSurface,
                            ),
                            decoration: InputDecoration(
                              hintText: '访问令牌（主机启用时填写）',
                              hintStyle: TextStyle(
                                fontSize: 13,
                                color: dark ? Colors.white38 : const Color(0xFF9AA0A6),
                              ),
                              prefixIcon: Icon(
                                Icons.key_rounded,
                                size: 18,
                                color: dark ? Colors.white54 : const Color(0xFF7C8390),
                              ),
                              filled: true,
                              fillColor: fieldFill,
                              contentPadding: const EdgeInsets.symmetric(horizontal: 12, vertical: 12),
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
                          const SizedBox(height: 6),
                          Text(
                            _server.configured
                                ? '设备 ID 通过 ${_server.url} 解析；令牌只用于这次连接。'
                                : '带 : 的地址直接连接；设备 ID 需先配置连接服务。',
                            style: mutedStyle(context, size: 10.5),
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
                const SizedBox(height: 30),
                Row(
                  children: <Widget>[
                    Text(
                      '我的设备',
                      style: theme.textTheme.titleMedium?.copyWith(
                        fontSize: 15,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                    const SizedBox(width: 8),
                    if (saved.isNotEmpty)
                      Text(
                        '${saved.length}',
                        style: mutedStyle(context, size: 12),
                      ),
                    const Spacer(),
                    TextButton.icon(
                      onPressed: _showAddDevice,
                      style: TextButton.styleFrom(foregroundColor: kAccent),
                      icon: const Icon(Icons.add_rounded, size: 16),
                      label: const Text('添加', style: TextStyle(fontSize: 12)),
                    ),
                  ],
                ),
                const SizedBox(height: 12),
                if (_loading)
                  const Padding(
                    padding: EdgeInsets.symmetric(vertical: 22),
                    child: Center(
                      child: SizedBox(
                        width: 18,
                        height: 18,
                        child: CircularProgressIndicator(strokeWidth: 2, color: kAccent),
                      ),
                    ),
                  )
                else if (saved.isEmpty)
                  _emptyPanel(
                    context,
                    icon: Icons.bookmark_add_outlined,
                    text: '添加常用设备，输入设备 ID 即可一键连接',
                    height: 104,
                  )
                else
                  Wrap(
                    spacing: 12,
                    runSpacing: 12,
                    children: <Widget>[
                      for (final Device device in saved)
                        _DeviceCard(
                          device: device,
                          presence: _presence[device.key] ?? DevicePresence.unknown,
                          pinned: _trust.containsKey(device.trustKey),
                          onTap: () => _connectDevice(device),
                          onRename: () => _renameDevice(device),
                          onDelete: () => _deleteDevice(device),
                        ),
                    ],
                  ),
                const SizedBox(height: 30),
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
                    if (recent.isNotEmpty)
                      Text(
                        '${recent.length}',
                        style: mutedStyle(context, size: 12),
                      ),
                    const Spacer(),
                    if (recent.isNotEmpty)
                      TextButton(
                        onPressed: _clearRecent,
                        style: TextButton.styleFrom(foregroundColor: kAccent),
                        child: const Text('清空', style: TextStyle(fontSize: 12)),
                      ),
                  ],
                ),
                const SizedBox(height: 12),
                if (recent.isEmpty)
                  _emptyPanel(
                    context,
                    icon: Icons.history_rounded,
                    text: '连接过的设备会出现在这里，保存后进入我的设备',
                    height: 112,
                  )
                else
                  Wrap(
                    spacing: 12,
                    runSpacing: 12,
                    children: <Widget>[
                      for (final Device device in recent)
                        _RecentCard(
                          device: device,
                          presence: _presence[device.key] ?? DevicePresence.unknown,
                          onTap: () => _connectDevice(device),
                          onSave: () => _promote(device),
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

  Widget _emptyPanel(
    BuildContext context, {
    required IconData icon,
    required String text,
    required double height,
  }) {
    final bool dark = Theme.of(context).brightness == Brightness.dark;
    return Container(
      height: height,
      decoration: BoxDecoration(
        color: Theme.of(context).colorScheme.surface.withValues(alpha: 0.55),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: dark ? Colors.white12 : const Color(0xFFE4E6EB)),
      ),
      child: Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Icon(icon, size: 25, color: dark ? Colors.white30 : const Color(0xFFA8AEB8)),
            const SizedBox(height: 8),
            Text(text, style: mutedStyle(context)),
          ],
        ),
      ),
    );
  }
}

int utf8ByteLength(String text) {
  // Tokens are ASCII in practice; counting UTF-16 code units would reject a
  // Chinese passphrase only after the host already accepted it. The protocol
  // limit is bytes, so encode once here.
  return text.codeUnits.isEmpty ? 0 : _utf8Length(text);
}

int _utf8Length(String text) {
  int bytes = 0;
  for (final int rune in text.runes) {
    if (rune <= 0x7F) {
      bytes += 1;
    } else if (rune <= 0x7FF) {
      bytes += 2;
    } else if (rune <= 0xFFFF) {
      bytes += 3;
    } else {
      bytes += 4;
    }
  }
  return bytes;
}

class _Sidebar extends StatelessWidget {
  const _Sidebar({
    required this.hostname,
    required this.server,
    required this.trustedCount,
    required this.onlineCount,
    required this.onConfigureServer,
  });

  final String hostname;
  final ServerSettings server;
  final int trustedCount;
  final int onlineCount;
  final VoidCallback onConfigureServer;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    final Color subtle = dark ? Colors.white12 : const Color(0xFFE2E4E9);
    final String service = server.configured
        ? (Uri.tryParse(server.url)?.authority ?? server.url)
        : '未配置';

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
                            Text('查看器 · Windows', style: mutedStyle(context, size: 10)),
                          ],
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 20),
            Text(
              '连接服务',
              style: TextStyle(
                fontSize: 10,
                fontWeight: FontWeight.w600,
                letterSpacing: 1.1,
                color: dark ? Colors.white38 : const Color(0xFF8A919C),
              ),
            ),
            const SizedBox(height: 8),
            Material(
              color: theme.colorScheme.surface,
              borderRadius: BorderRadius.circular(8),
              clipBehavior: Clip.antiAlias,
              child: InkWell(
                onTap: onConfigureServer,
                child: Container(
                  padding: const EdgeInsets.fromLTRB(12, 10, 6, 10),
                  decoration: BoxDecoration(
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(color: subtle),
                  ),
                  child: Row(
                    children: <Widget>[
                      Icon(
                        server.configured ? Icons.cloud_done_outlined : Icons.cloud_off_outlined,
                        size: 17,
                        color: server.configured ? kAccent : (dark ? Colors.white38 : const Color(0xFF8A919C)),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: <Widget>[
                            Text(
                              service,
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                              style: TextStyle(
                                fontSize: 11.5,
                                fontWeight: FontWeight.w600,
                                color: theme.colorScheme.onSurface,
                              ),
                            ),
                            const SizedBox(height: 2),
                            Text(
                              server.configured
                                  ? '${server.mode.shortLabel}'
                                      '${onlineCount > 0 ? ' · $onlineCount 台在线' : ''}'
                                  : '点击配置',
                              style: mutedStyle(context, size: 9.5),
                            ),
                          ],
                        ),
                      ),
                      Icon(
                        Icons.settings_outlined,
                        size: 15,
                        color: dark ? Colors.white38 : const Color(0xFF9AA0A6),
                      ),
                    ],
                  ),
                ),
              ),
            ),
            const SizedBox(height: 10),
            Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Icon(
                  Icons.shield_outlined,
                  size: 13,
                  color: trustedCount > 0 ? kAccent : (dark ? Colors.white30 : const Color(0xFFA8AEB8)),
                ),
                const SizedBox(width: 5),
                Expanded(
                  child: Text(
                    trustedCount > 0 ? '已固定 $trustedCount 个主机身份' : '首次连接时记录主机证书指纹',
                    style: mutedStyle(context, size: 10),
                  ),
                ),
              ],
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
                    '直连优先',
                    style: TextStyle(color: Colors.white, fontSize: 14, fontWeight: FontWeight.w600),
                  ),
                  const SizedBox(height: 3),
                  Text(
                    '打洞失败自动中继\n端到端加密',
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
                Expanded(
                  child: Text(
                    'chast 0.1.0 · Windows',
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: mutedStyle(context, size: 10),
                  ),
                ),
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

class _DeviceCard extends StatelessWidget {
  const _DeviceCard({
    required this.device,
    required this.presence,
    required this.pinned,
    required this.onTap,
    required this.onRename,
    required this.onDelete,
  });

  final Device device;
  final DevicePresence presence;
  final bool pinned;
  final VoidCallback onTap;
  final VoidCallback onRename;
  final VoidCallback onDelete;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    final Color subtle = dark ? Colors.white12 : const Color(0xFFE4E6EB);
    return SizedBox(
      width: 250,
      height: 92,
      child: Material(
        color: theme.colorScheme.surface,
        borderRadius: BorderRadius.circular(14),
        clipBehavior: Clip.antiAlias,
        child: InkWell(
          onTap: onTap,
          hoverColor: kAccent.withValues(alpha: 0.05),
          child: Container(
            padding: const EdgeInsets.fromLTRB(14, 10, 4, 10),
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
                  child: Icon(
                    device.viaServer ? Icons.dns_outlined : Icons.lan_outlined,
                    color: kAccent,
                    size: 20,
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Row(
                        children: <Widget>[
                          Flexible(
                            child: Text(
                              device.displayName,
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                              style: TextStyle(
                                fontSize: 13,
                                fontWeight: FontWeight.w600,
                                color: theme.colorScheme.onSurface,
                              ),
                            ),
                          ),
                          if (pinned) ...<Widget>[
                            const SizedBox(width: 5),
                            Icon(Icons.verified_user_outlined, size: 12, color: kAccent),
                          ],
                        ],
                      ),
                      const SizedBox(height: 4),
                      Text(
                        device.detail,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: mutedStyle(context, size: 10),
                      ),
                      const SizedBox(height: 3),
                      _presenceLine(context, presence, device),
                    ],
                  ),
                ),
                PopupMenuButton<String>(
                  tooltip: '设备操作',
                  icon: Icon(
                    Icons.more_vert_rounded,
                    size: 17,
                    color: dark ? Colors.white38 : const Color(0xFF9AA0A6),
                  ),
                  onSelected: (String value) {
                    if (value == 'rename') {
                      onRename();
                    } else if (value == 'delete') {
                      onDelete();
                    }
                  },
                  itemBuilder: (BuildContext context) => const <PopupMenuEntry<String>>[
                    PopupMenuItem<String>(value: 'rename', child: Text('重命名')),
                    PopupMenuItem<String>(value: 'delete', child: Text('删除')),
                  ],
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _presenceLine(BuildContext context, DevicePresence presence, Device device) {
    final bool dark = Theme.of(context).brightness == Brightness.dark;
    Color color;
    String text;
    if (!device.viaServer) {
      color = dark ? Colors.white30 : const Color(0xFFA8AEB8);
      text = '直连地址';
    } else {
      switch (presence.state) {
        case HostPresence.online:
          color = const Color(0xFF4CAF50);
          final List<String> parts = <String>['在线'];
          if (presence.info?.hasDirectCandidate ?? false) {
            parts.add('可直连');
          }
          if (presence.info?.hasRelay ?? false) {
            parts.add('可中继');
          }
          text = parts.join(' · ');
        case HostPresence.offline:
          color = const Color(0xFF9E9E9E);
          text = '离线';
        case HostPresence.unreachable:
          color = const Color(0xFFFFB74D);
          text = '服务不可达';
        case HostPresence.checking:
          color = kAccent;
          text = '查询中';
        case HostPresence.unknown:
          color = const Color(0xFF9E9E9E);
          text = '等待查询';
      }
    }
    return Row(
      children: <Widget>[
        Container(
          width: 6,
          height: 6,
          decoration: BoxDecoration(color: color, shape: BoxShape.circle),
        ),
        const SizedBox(width: 5),
        Flexible(
          child: Text(
            text,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: TextStyle(fontSize: 9.5, color: color),
          ),
        ),
      ],
    );
  }
}

class _RecentCard extends StatelessWidget {
  const _RecentCard({
    required this.device,
    required this.presence,
    required this.onTap,
    required this.onSave,
  });

  final Device device;
  final DevicePresence presence;
  final VoidCallback onTap;
  final VoidCallback onSave;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    final Color subtle = dark ? Colors.white12 : const Color(0xFFE4E6EB);
    final String presenceText = !device.viaServer
        ? _formatTime(device.lastUsed!)
        : switch (presence.state) {
            HostPresence.online => '在线 · ${_formatTime(device.lastUsed!)}',
            HostPresence.offline => '离线',
            HostPresence.unreachable => '服务不可达',
            HostPresence.checking => '查询中',
            HostPresence.unknown => _formatTime(device.lastUsed!),
          };
    return SizedBox(
      width: 250,
      height: 86,
      child: Material(
        color: theme.colorScheme.surface,
        borderRadius: BorderRadius.circular(14),
        clipBehavior: Clip.antiAlias,
        child: InkWell(
          onTap: onTap,
          hoverColor: kAccent.withValues(alpha: 0.05),
          child: Container(
            padding: const EdgeInsets.fromLTRB(14, 8, 4, 8),
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(14),
              border: Border.all(color: subtle),
            ),
            child: Row(
              children: <Widget>[
                Container(
                  width: 38,
                  height: 38,
                  decoration: BoxDecoration(
                    color: kAccent.withValues(alpha: 0.10),
                    borderRadius: BorderRadius.circular(10),
                  ),
                  child: Icon(
                    device.viaServer ? Icons.dns_outlined : Icons.desktop_windows_rounded,
                    color: kAccent,
                    size: 19,
                  ),
                ),
                const SizedBox(width: 11),
                Expanded(
                  child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Text(
                        device.displayName,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: TextStyle(
                          fontSize: 13,
                          fontWeight: FontWeight.w600,
                          color: theme.colorScheme.onSurface,
                        ),
                      ),
                      const SizedBox(height: 3),
                      Text(
                        device.viaServer && device.name.isNotEmpty ? device.id : device.detail,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: mutedStyle(context, size: 10),
                      ),
                      const SizedBox(height: 2),
                      Text(
                        presenceText,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: mutedStyle(context, size: 9.5),
                      ),
                    ],
                  ),
                ),
                IconButton(
                  tooltip: '保存到我的设备',
                  onPressed: onSave,
                  icon: Icon(
                    Icons.bookmark_add_outlined,
                    size: 17,
                    color: dark ? Colors.white54 : const Color(0xFF7C8390),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _ServerDialog extends StatefulWidget {
  const _ServerDialog({required this.initial});

  final ServerSettings initial;

  @override
  State<_ServerDialog> createState() => _ServerDialogState();
}

class _ServerDialogState extends State<_ServerDialog> {
  late final TextEditingController _url;
  late TunnelMode _mode;
  bool _testing = false;
  String _status = '';
  bool _statusOk = false;

  @override
  void initState() {
    super.initState();
    _url = TextEditingController(text: widget.initial.url);
    _mode = widget.initial.mode;
  }

  @override
  void dispose() {
    _url.dispose();
    super.dispose();
  }

  Future<void> _test() async {
    final String url = normalizeServerUrl(_url.text);
    if (url.isEmpty) {
      setState(() {
        _status = '请输入有效的 http(s) 地址';
        _statusOk = false;
      });
      return;
    }
    setState(() {
      _testing = true;
      _status = '正在检查…';
      _statusOk = false;
    });
    try {
      await ServerApi.health(ServerSettings(url: url, mode: _mode));
      if (mounted) {
        setState(() {
          _status = '服务可用';
          _statusOk = true;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _status = '$error';
          _statusOk = false;
        });
      }
    } finally {
      if (mounted) {
        setState(() => _testing = false);
      }
    }
  }

  void _save() {
    final String text = _url.text.trim();
    final String url = normalizeServerUrl(text);
    if (text.isNotEmpty && url.isEmpty) {
      setState(() {
        _status = '地址无效：示例 http://192.168.1.10:8443';
        _statusOk = false;
      });
      return;
    }
    Navigator.of(context).pop(ServerSettings(url: url, mode: _mode));
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      backgroundColor: Theme.of(context).colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: const Text('连接服务'),
      content: SizedBox(
        width: 420,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            TextField(
              controller: _url,
              autofocus: true,
              decoration: const InputDecoration(
                labelText: '服务地址',
                hintText: 'http://127.0.0.1:8443',
              ),
              onSubmitted: (_) => _save(),
            ),
            const SizedBox(height: 12),
            DropdownButtonFormField<TunnelMode>(
              initialValue: _mode,
              decoration: const InputDecoration(labelText: '路径策略'),
              items: <DropdownMenuItem<TunnelMode>>[
                for (final TunnelMode mode in TunnelMode.values)
                  DropdownMenuItem<TunnelMode>(
                    value: mode,
                    child: Text(mode.label),
                  ),
              ],
              onChanged: (TunnelMode? value) {
                if (value != null) {
                  setState(() => _mode = value);
                }
              },
            ),
            const SizedBox(height: 10),
            Row(
              children: <Widget>[
                TextButton(
                  onPressed: _testing ? null : _test,
                  child: Text(_testing ? '检查中…' : '测试连接'),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    _status,
                    maxLines: 2,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(
                      fontSize: 11,
                      color: _status.isEmpty
                          ? Colors.grey
                          : (_statusOk ? const Color(0xFF4CAF50) : Theme.of(context).colorScheme.error),
                    ),
                  ),
                ),
              ],
            ),
            Text(
              '查看者不需要服务端密钥；注册主机时才需要。留空即可关闭设备 ID 解析。',
              style: mutedStyle(context, size: 10.5),
            ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('取消'),
        ),
        FilledButton(onPressed: _save, child: const Text('保存')),
      ],
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
