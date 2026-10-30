import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'package:chast_viewer/l10n.dart';

import '../account_store.dart';
import '../address_book_sync.dart';
import '../app_info.dart';
import '../device_store.dart';
import '../lan_discovery.dart';
import '../security_key.dart';
import '../server_api.dart';
import '../server_identity.dart';
import '../server_settings.dart';
import '../target.dart';
import '../theme.dart';
import '../tunnel_agent.dart';
import '../trust_store.dart';
import '../viewer_bridge.dart';
import '../wake_on_lan.dart';
import 'session_page.dart';
import 'groups_dialog.dart';
import 'account_sessions_dialog.dart';
import 'change_password_dialog.dart';
import 'recording_archive_dialog.dart';
import 'recovery_dialog.dart';
import 'security_key_dialog.dart';
import 'shared_books_dialog.dart';
import 'two_factor_dialog.dart';

typedef HostLookup =
    Future<BatchLookupResult> Function(
      ServerSettings server,
      List<String> hostIds,
    );
typedef LabelsUpdater =
    Future<void> Function(
      ServerSettings server,
      String hostId, {
      required String alias,
      required List<String> tags,
    });
typedef AccountLogin =
    Future<AccountLoginResult> Function(
      ServerSettings server, {
      required String username,
      required String password,
      String totpCode,
      String recoveryCode,
      String smsCode,
      String emailCode,
    });
typedef LoginSMSRequester =
    Future<void> Function(
      ServerSettings server, {
      required String username,
      required String password,
    });
typedef LoginEmailRequester =
    Future<void> Function(
      ServerSettings server, {
      required String username,
      required String password,
    });
typedef AccountSecurityKeyLogin =
    Future<AccountLoginResult> Function(
      ServerSettings server, {
      required String username,
      required String password,
    });
typedef AccountPasswordlessSecurityKeyLogin =
    Future<AccountLoginResult> Function(
      ServerSettings server, {
      required String username,
    });
typedef AccountRefresh =
    Future<AccountLoginResult> Function(
      ServerSettings server,
      String refreshToken,
    );
typedef AccountLogout =
    Future<void> Function(ServerSettings server, String token);
typedef AddressBookFetcher =
    Future<AddressBookSnapshot> Function(
      ServerSettings server,
      String token,
      int since,
    );
typedef AddressBookPeerWriter =
    Future<AddressBookPeer> Function(
      ServerSettings server,
      String token,
      String peerId, {
      String? name,
      List<String>? tags,
      String? groupId,
      int? ifRevision,
    });
typedef AddressBookPeerDeleter =
    Future<void> Function(ServerSettings server, String token, String peerId);
typedef ViewerUpdateInstaller =
    Future<void> Function(SoftwareInfo update, String serverPublicKey);

class HomePage extends StatefulWidget {
  const HomePage({
    super.key,
    this.initialTarget,
    this.deviceStore,
    this.serverStore,
    this.trustStore,
    this.identityStore,
    this.accountStore,
    this.lookupBatch,
    this.labelsUpdater,
    this.accountLogin,
    this.accountLoginSMS,
    this.accountLoginEmail,
    this.accountSecurityKeyLogin,
    this.accountPasswordlessSecurityKeyLogin,
    this.accountRefresh,
    this.accountLogout,
    this.addressBookFetcher,
    this.addressBookPeerWriter,
    this.addressBookPeerDeleter,
    this.wakeSender,
    this.lanDiscovery,
    this.softwareUpdater,
    this.appExit,
  });

  final String? initialTarget;

  /// Test seams. Product code uses the files under %APPDATA%\chast.
  final DeviceStore? deviceStore;
  final ServerSettingsStore? serverStore;
  final TrustStore? trustStore;
  final ServerIdentityStore? identityStore;
  final AccountSessionStore? accountStore;
  final HostLookup? lookupBatch;
  final LabelsUpdater? labelsUpdater;
  final AccountLogin? accountLogin;
  final LoginSMSRequester? accountLoginSMS;
  final LoginEmailRequester? accountLoginEmail;
  final AccountSecurityKeyLogin? accountSecurityKeyLogin;
  final AccountPasswordlessSecurityKeyLogin? accountPasswordlessSecurityKeyLogin;
  final AccountRefresh? accountRefresh;
  final AccountLogout? accountLogout;
  final AddressBookFetcher? addressBookFetcher;
  final AddressBookPeerWriter? addressBookPeerWriter;
  final AddressBookPeerDeleter? addressBookPeerDeleter;
  final WakeSender? wakeSender;
  final LanDiscovery? lanDiscovery;
  final ViewerUpdateInstaller? softwareUpdater;
  final void Function(int code)? appExit;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  static const Duration _presenceInterval = Duration(seconds: 10);

  final TextEditingController _address = TextEditingController();
  final TextEditingController _token = TextEditingController();
  final TextEditingController _deviceSearch = TextEditingController();
  String _deviceQuery = '';
  final Set<String> _selectedTags = <String>{};
  late final DeviceStore _deviceStore;
  late final ServerSettingsStore _serverStore;
  late final TrustStore _trustStore;
  late final ServerIdentityStore _identityStore;
  late final AccountSessionStore _accountStore;
  late final HostLookup _lookupBatch;
  late final LabelsUpdater _updateLabels;
  late final AccountLogin _loginAccount;
  late final LoginSMSRequester _requestLoginSMS;
  late final LoginEmailRequester _requestLoginEmail;
  late final AccountSecurityKeyLogin _loginWithSecurityKey;
  late final AccountPasswordlessSecurityKeyLogin _loginWithSecurityKeyOnly;
  late final AccountRefresh _refreshAccount;
  late final AccountLogout _logoutAccount;
  late final AddressBookFetcher _fetchAddressBook;
  late final AddressBookPeerWriter _writeAddressBookPeer;
  late final AddressBookPeerDeleter _deleteAddressBookPeer;
  late final WakeSender _wakeSender;
  late final LanDiscovery _lanDiscovery;
  late final ViewerUpdateInstaller _installUpdate;
  late final void Function(int code) _exitApp;

  List<Device> _devices = <Device>[];
  List<AddressBookGroup> _groups = <AddressBookGroup>[];
  final Set<String> _collapsedGroups = <String>{};
  bool _groupsSynced = false;
  ServerSettings _server = const ServerSettings();
  Map<String, String> _trust = <String, String>{};
  Map<String, ServerIdentityPin> _serverPins = <String, ServerIdentityPin>{};
  AccountSession? _account;
  bool _syncing = false;
  final Map<String, DevicePresence> _presence = <String, DevicePresence>{};
  SoftwareInfo? _software;
  String? _softwareDismissed;
  Timer? _presenceTimer;
  Timer? _accountTimer;
  bool _renewing = false;
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
    _identityStore = widget.identityStore ?? ServerIdentityStore();
    _accountStore = widget.accountStore ??
        AccountSessionStore(
          protectSecret: ViewerBridge.protectSecret,
          unprotectSecret: ViewerBridge.unprotectSecret,
        );
    _lookupBatch = widget.lookupBatch ?? ServerApi.lookupBatch;
    _wakeSender = widget.wakeSender ?? WakeOnLan.send;
    _lanDiscovery = widget.lanDiscovery ?? const LanDiscovery();
    _installUpdate =
        widget.softwareUpdater ??
        (SoftwareInfo update, String serverPublicKey) =>
            TunnelAgent.applyUpdate(
              version: update.version,
              url: update.url,
              sha256: update.sha256,
              size: update.size,
              signature: update.signature,
              serverPublicKey: serverPublicKey,
            );
    _exitApp = widget.appExit ?? exit;
    _updateLabels = widget.labelsUpdater ?? ServerApi.updateLabels;
    _loginAccount =
        widget.accountLogin ??
        (
          ServerSettings server, {
          required String username,
          required String password,
          String totpCode = '',
          String recoveryCode = '',
          String smsCode = '',
          String emailCode = '',
        }) => ServerApi.login(
          server,
          username: username,
          password: password,
          totpCode: totpCode,
          recoveryCode: recoveryCode,
          smsCode: smsCode,
          emailCode: emailCode,
          deviceName: _hostname,
        );
    _requestLoginSMS =
        widget.accountLoginSMS ??
        (
          ServerSettings server, {
          required String username,
          required String password,
        }) => ServerApi.requestLoginSMS(
          server,
          username: username,
          password: password,
        );
    _requestLoginEmail =
        widget.accountLoginEmail ??
        (
          ServerSettings server, {
          required String username,
          required String password,
        }) => ServerApi.requestLoginEmail(
          server,
          username: username,
          password: password,
        );
    _loginWithSecurityKey =
        widget.accountSecurityKeyLogin ??
        (
          ServerSettings server, {
          required String username,
          required String password,
        }) => loginWithSecurityKey(
          server,
          username: username,
          password: password,
          deviceName: _hostname,
        );
    _loginWithSecurityKeyOnly =
        widget.accountPasswordlessSecurityKeyLogin ??
        (ServerSettings server, {required String username}) =>
            loginWithSecurityKeyOnly(
              server,
              username: username,
              deviceName: _hostname,
            );
    _logoutAccount =
        widget.accountLogout ??
        (ServerSettings server, String token) =>
            ServerApi.logout(server, token);
    _refreshAccount = widget.accountRefresh ?? ServerApi.refresh;
    _fetchAddressBook =
        widget.addressBookFetcher ??
        (ServerSettings server, String token, int since) =>
            ServerApi.addressBook(server, token, since: since);
    _writeAddressBookPeer =
        widget.addressBookPeerWriter ??
        (
          ServerSettings server,
          String token,
          String peerId, {
          String? name,
          List<String>? tags,
          String? groupId,
          int? ifRevision,
        }) {
          return ServerApi.updateAddressBookPeer(
            server,
            token,
            peerId,
            name: name,
            tags: tags,
            groupId: groupId,
            ifRevision: ifRevision,
          );
        };
    _deleteAddressBookPeer =
        widget.addressBookPeerDeleter ??
        (ServerSettings server, String token, String peerId) =>
            ServerApi.deleteAddressBookPeer(server, token, peerId);
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
    _accountTimer = Timer.periodic(
      const Duration(minutes: 1),
      (_) => unawaited(_renewAccountIfNeeded()),
    );
  }

  @override
  void dispose() {
    _presenceTimer?.cancel();
    _accountTimer?.cancel();
    _address.dispose();
    _token.dispose();
    _deviceSearch.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    final List<Device> devices = await _deviceStore.load();
    final ServerSettings server = await _serverStore.load();
    final Map<String, String> trust = await _trustStore.load();
    final Map<String, ServerIdentityPin> pins = await _identityStore.load();
    final AccountSession? storedAccount = await _accountStore.load();
    AccountSession? account;
    if (storedAccount != null) {
      if (!storedAccount.isUsableAt(DateTime.now()) ||
          storedAccount.server != server.url) {
        unawaited(_accountStore.clear());
      } else {
        account = storedAccount;
      }
    }
    if (!mounted) {
      return;
    }
    setState(() {
      _devices = devices;
      _server = server;
      _trust = trust;
      _serverPins = pins;
      _account = account;
      _loading = false;
    });
    unawaited(_refreshPresence());
    await _renewAccountIfNeeded();
    if (mounted && _account != null) {
      unawaited(_syncAddressBook(announce: false));
    }
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

  /// Renews the access token before it lapses. A rejected refresh token means
  /// the session is gone (logout, password change or replay detection), so the
  /// stored session is dropped and the account UI falls back to signed out.
  Future<void> _renewAccountIfNeeded() async {
    final AccountSession? account = _account;
    if (account == null || account.server != _server.url || _renewing) {
      return;
    }
    final DateTime now = DateTime.now().toUtc();
    if (account.isActiveAt(now.add(const Duration(minutes: 2)))) {
      return;
    }
    if (!account.canRefreshAt(now)) {
      return;
    }
    _renewing = true;
    try {
      final AccountLoginResult renewed = await _refreshAccount(
        _server,
        account.refreshToken,
      );
      if (!mounted || !identical(_account, account)) {
        return;
      }
      final DateTime issued = DateTime.now().toUtc();
      final AccountSession session = account.copyWith(
        token: renewed.token,
        expiresAt: issued.add(Duration(milliseconds: renewed.expiresInMs)),
        refreshToken: renewed.refreshToken.isEmpty
            ? null
            : renewed.refreshToken,
        refreshExpiresAt: renewed.refreshToken.isEmpty
            ? null
            : issued.add(Duration(milliseconds: renewed.refreshExpiresInMs)),
      );
      await _accountStore.save(session);
      if (mounted) {
        setState(() => _account = session);
      }
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.unauthorized &&
          mounted &&
          identical(_account, account)) {
        await _accountStore.clear();
        if (mounted) {
          setState(() => _account = null);
        }
      }
    } catch (_) {
      // A transient network failure leaves the stored refresh token intact;
      // the next tick retries before the access token expires.
    } finally {
      _renewing = false;
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
    final List<Device> devices = _devices
        .where((Device device) => device.viaServer)
        .toList();
    if (devices.isEmpty) {
      if ((_presence.isNotEmpty || _software != null) && mounted) {
        setState(() {
          _presence.clear();
          _software = null;
        });
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
    final Map<String, List<Device>> byServer = <String, List<Device>>{};
    for (final Device device in devices) {
      byServer.putIfAbsent(device.server, () => <Device>[]).add(device);
    }
    SoftwareInfo? software;
    await Future.wait(<Future<void>>[
      for (final MapEntry<String, List<Device>> group in byServer.entries)
        () async {
          final ServerSettings settings = ServerSettings(
            url: group.key,
            mode: _server.mode,
            key: _server.key,
          );
          try {
            final BatchLookupResult batch = await _lookupBatch(
              settings,
              <String>[for (final Device device in group.value) device.id],
            );
            // A pinned server answers every lookup with its key. A different
            // (or missing) key means this poll cannot be trusted, so the
            // presence states stay unknown instead of showing host data.
            final ServerIdentityPin? pin =
                _serverPins[normalizeServerUrl(group.key)];
            if (pin != null) {
              for (final HostInfo info in batch.hosts.values) {
                if (info.serverPublicKey != pin.identity.publicKey) {
                  throw ServerApiException(tr('服务器身份已变化，已拒绝查询'));
                }
              }
            }
            if (group.key == _server.url && batch.software != null) {
              software = batch.software;
            }
            for (final Device device in group.value) {
              final HostInfo? info = batch.hosts[device.id];
              if (info == null) {
                // The service answered but has no record for this id. That is
                // a stale address-book entry, not a server outage.
                result[device.key] = DevicePresence(
                  HostPresence.offline,
                  info: HostInfo(id: device.id, online: false),
                );
                continue;
              }
              result[device.key] = DevicePresence(
                info.online ? HostPresence.online : HostPresence.offline,
                info: info,
              );
            }
          } catch (error) {
            for (final Device device in group.value) {
              result[device.key] = DevicePresence(
                HostPresence.unreachable,
                error: '$error',
              );
            }
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
      _software = software;
    });
  }

  bool _looksLikeAddress(String text) {
    return text.startsWith('[') || text.contains(':');
  }

  Future<void> _connect(
    String input, {
    bool fileTransferOnly = false,
  }) async {
    if (_opening) {
      return;
    }
    final String text = input.trim();
    final String token = _token.text.trim();
    if (text.isEmpty) {
      setState(() => _error = tr('请输入设备 ID 或主机地址'));
      return;
    }
    if (!_looksLikeAddress(text) && !_server.configured) {
      setState(() => _error = tr('设备 ID 需要先配置连接服务，或输入 主机:端口'));
      return;
    }
    if (utf8ByteLength(token) > 128) {
      setState(() => _error = tr('访问令牌过长，最多 128 字节'));
      return;
    }
    final ConnectTarget? target = parseConnectInput(
      text,
      token: token,
      server: _server,
    );
    if (target == null) {
      setState(() => _error = tr('格式无效：请输入设备 ID 或 主机:端口'));
      return;
    }
    await _open(target, fileTransferOnly: fileTransferOnly);
  }

  Future<void> _connectDevice(Device device, {bool fileTransferOnly = false}) async {
    if (_opening) {
      return;
    }
    final String token = _token.text.trim();
    if (utf8ByteLength(token) > 128) {
      setState(() => _error = tr('访问令牌过长，最多 128 字节'));
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
        serverKey: _server.key,
      );
    } else {
      target = parseConnectTarget(device.id, token: token);
    }
    if (target == null) {
      setState(() => _error = tr('设备地址无效：{p1}', <String, Object?>{'p1': device.id}));
      return;
    }
    _address.text = target.label;
    await _open(target, fileTransferOnly: fileTransferOnly);
  }

  Future<void> _open(
    ConnectTarget target, {
    bool fileTransferOnly = false,
  }) async {
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
      MaterialPageRoute<void>(
        builder: (BuildContext context) {
          return SessionPage(
            target: target,
            account: _account,
            settings: _server,
            identityStore: _identityStore,
            fileTransferOnly: fileTransferOnly,
          );
        },
      ),
    );
    if (!mounted) {
      return;
    }
    setState(() => _opening = false);
    await _reloadTrust();
    await _reloadServerPins();
    unawaited(_refreshPresence());
  }

  Future<void> _reloadServerPins() async {
    final Map<String, ServerIdentityPin> pins = await _identityStore.load();
    if (mounted) {
      setState(() => _serverPins = pins);
    }
  }

  Device _deviceForTarget(ConnectTarget target) {
    if (target.viaServer) {
      final String key = Device(
        id: target.hostId,
        addedAt: DateTime.now(),
        server: target.server,
      ).key;
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
    unawaited(_pushAccountPeer(device));
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
    unawaited(_pushAccountPeer(device, allowEmptyName: true));
  }

  Future<Device?> _showDeviceDialog() {
    return showDialog<Device>(
      context: context,
      builder: (BuildContext context) {
        return _AddDeviceDialog(server: _server);
      },
    );
  }

  Future<void> _scanLan() async {
    final List<LanPeer>? peers = await showDialog<List<LanPeer>>(
      context: context,
      builder: (BuildContext context) {
        return _LanScanDialog(discovery: _lanDiscovery, server: _server);
      },
    );
    if (peers == null || peers.isEmpty || !mounted) {
      return;
    }
    List<Device> devices = _devices;
    for (final LanPeer peer in peers) {
      final Device device = Device(
        id: peer.id,
        name: peer.hostname,
        server: _server.url,
        mac: normalizeMac(peer.mac) ?? '',
        addedAt: DateTime.now(),
      );
      devices = await _deviceStore.save(device);
      unawaited(_pushAccountPeer(device, allowEmptyName: true));
    }
    if (!mounted) {
      return;
    }
    setState(() => _devices = devices);
    unawaited(_refreshPresence());
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(
          peers.length == 1
              ? tr('已添加 {p2}', <String, Object?>{'p2': peers.first.label})
              : tr('已添加 {p3} 台局域网设备', <String, Object?>{'p3': peers.length}),
        ),
      ),
    );
  }

  Future<void> _renameDevice(Device device) async {
    final String? result = await showDialog<String>(
      context: context,
      builder: (BuildContext context) {
        return _RenameDialog(device: device);
      },
    );
    if (result == null) {
      return;
    }
    final List<Device> devices = await _deviceStore.rename(device.key, result);
    if (mounted) {
      setState(() => _devices = devices);
      final Device updated = devices.firstWhere(
        (Device item) => item.key == device.key,
        orElse: () => device.copyWith(name: result.trim()),
      );
      unawaited(_pushAccountPeer(updated));
    }
  }

  Future<void> _wakeDevice(Device device) async {
    final String? mac = await showDialog<String>(
      context: context,
      builder: (BuildContext context) => _WakeDialog(device: device),
    );
    if (mac == null || !mounted) {
      return;
    }
    final List<Device> devices = await _deviceStore.updateMac(device.key, mac);
    if (!mounted) {
      return;
    }
    setState(() => _devices = devices);
    try {
      await _wakeSender(mac);
      if (mounted) {
        _showSnack(tr('已发送局域网唤醒包'));
      }
    } catch (error) {
      if (mounted) {
        _showSnack(tr('唤醒包发送失败：{error}', <String, Object?>{'error': error}));
      }
    }
  }

  Future<void> _editTags(Device device) async {
    final List<String>? tags = await showDialog<List<String>>(
      context: context,
      builder: (BuildContext context) {
        return _TagsDialog(tags: device.tags);
      },
    );
    if (tags == null) {
      return;
    }
    final List<Device> devices = await _deviceStore.updateTags(
      device.key,
      tags,
    );
    if (!mounted) {
      return;
    }
    setState(() => _devices = devices);
    final Device updated = devices.firstWhere(
      (Device item) => item.key == device.key,
      orElse: () => device.copyWith(tags: tags),
    );
    _showSnack(tr('账号标签已保存'));
    unawaited(_pushAccountPeer(updated, syncTags: true));
  }

  Future<void> _moveDeviceToGroup(Device device) async {
    final AccountSession? account = _account;
    if (account == null || account.server != _server.url) {
      _showSnack(tr('请先登录账号'));
      return;
    }
    final String? choice = await showDialog<String>(
      context: context,
      builder: (BuildContext context) =>
          _GroupPickerDialog(groups: _groups, selected: device.groupId),
    );
    if (choice == null || !mounted) {
      return;
    }
    final List<Device> devices = await _deviceStore.updateGroup(
      device.key,
      choice,
    );
    if (!mounted) {
      return;
    }
    setState(() => _devices = devices);
    final Device updated = devices.firstWhere(
      (Device item) => item.key == device.key,
      orElse: () => device.copyWith(groupId: choice),
    );
    try {
      final AddressBookPeer written = await _writeAddressBookPeer(
        _server,
        account.token,
        updated.id,
        name: updated.name.isEmpty ? null : updated.name,
        groupId: choice,
        ifRevision: updated.revision > 0 ? updated.revision : null,
      );
      List<Device> settled = await _deviceStore.setPeerRevision(
        device.key,
        written.revision,
      );
      if (choice.isEmpty) {
        settled = await _deviceStore.markPushSettled(device.key, group: true);
      }
      if (mounted) {
        setState(() => _devices = settled);
      }
      if (mounted) {
        _showSnack(choice.isEmpty ? tr('已移到未分组') : tr('已移动到分组'));
      }
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.unauthorized) {
        await _expireAccount();
        return;
      }
      if (error.statusCode == HttpStatus.conflict) {
        await _refreshAfterConflict(device.id);
        return;
      }
      if (mounted) {
        _showSnack(tr('分组已保存在本机，将在下次同步上传'));
      }
    } catch (_) {
      if (mounted) {
        _showSnack(tr('分组已保存在本机，将在下次同步上传'));
      }
    }
  }

  Future<void> _manageGroups() async {
    final AccountSession? account = _account;
    if (account == null || account.server != _server.url) {
      _showSnack(tr('请先登录账号'));
      return;
    }
    final Map<String, int> counts = <String, int>{};
    for (final Device device in _devices) {
      if (device.saved && device.server == _server.url) {
        counts[device.groupId] = (counts[device.groupId] ?? 0) + 1;
      }
    }
    final bool changed =
        await showDialog<bool>(
          context: context,
          builder: (BuildContext context) => GroupsDialog(
            server: _server,
            token: account.token,
            groups: _groups,
            peerCounts: counts,
            reload: () async {
              await _syncAddressBook(announce: false);
              return _groups;
            },
          ),
        ) ??
        false;
    if (changed && mounted) {
      await _syncAddressBook(announce: false);
    }
  }

  Future<void> _deleteDevice(Device device) async {
    final bool? confirmed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) {
        return AlertDialog(
          backgroundColor: Theme.of(context).colorScheme.surface,
          title: Text(tr('删除设备')),
          content: Text(tr('从地址簿删除“{p4}”？', <String, Object?>{'p4': device.displayName})),
          actions: <Widget>[
            TextButton(
              onPressed: () => Navigator.of(context).pop(false),
              child: Text(tr('取消')),
            ),
            FilledButton(
              onPressed: () => Navigator.of(context).pop(true),
              style: FilledButton.styleFrom(
                backgroundColor: Theme.of(context).colorScheme.error,
              ),
              child: Text(tr('删除')),
            ),
          ],
        );
      },
    );
    if (confirmed != true) {
      return;
    }
    final AccountSession? account = _account;
    final bool tracked =
        device.saved &&
        device.viaServer &&
        device.server == _server.url &&
        account != null &&
        account.server == _server.url &&
        account.isActiveAt(DateTime.now());
    if (tracked) {
      // Queue first so an offline delete is retried instead of being pulled
      // back as a server-only peer on the next sync.
      await _deviceStore.queueDeletion(
        PendingDeletion(
          server: account.server,
          username: account.username,
          id: device.id,
          deletedAt: DateTime.now(),
        ),
      );
    }
    final List<Device> devices = await _deviceStore.remove(device.key);
    if (mounted) {
      setState(() => _devices = devices);
      unawaited(_refreshPresence());
      unawaited(_deleteAccountPeer(device));
    }
  }

  Future<void> _editLabels(Device device) async {
    final DevicePresence presence =
        _presence[device.key] ?? DevicePresence.unknown;
    final bool saved =
        await showDialog<bool>(
          context: context,
          builder: (BuildContext context) {
            return _LabelsDialog(
              server: ServerSettings(
                url: device.server,
                mode: _server.mode,
                key: _server.key,
              ),
              hostId: device.id,
              alias: presence.info?.alias ?? '',
              tags: presence.info?.tags ?? const <String>[],
              updater: _updateLabels,
            );
          },
        ) ??
        false;
    if (!saved || !mounted) {
      return;
    }
    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text(tr('服务端别名与标签已保存'))));
    unawaited(_refreshPresence());
  }

  Future<void> _showAccount() async {
    final _AccountResult? result = await showDialog<_AccountResult>(
      context: context,
      builder: (BuildContext context) {
        return _AccountDialog(
          server: _server,
          account: _account,
          login: _loginAccount,
          requestLoginSMS: _requestLoginSMS,
          requestLoginEmail: _requestLoginEmail,
          securityKeyLogin: _loginWithSecurityKey,
          securityKeyPasswordlessLogin: _loginWithSecurityKeyOnly,
          syncing: _syncing,
          groups: _groups,
          uploadRecordings: _server.uploadRecordings,
          onUploadRecordingsChanged: _setUploadRecordings,
        );
      },
    );
    if (result == null || !mounted) {
      return;
    }
    if (result.session != null) {
      final AccountSession session = result.session!;
      await _accountStore.save(session);
      if (!mounted) {
        return;
      }
      setState(() => _account = session);
      await _syncAddressBook();
    } else if (result.logout) {
      await _signOutAccount();
    } else if (result.sync) {
      await _syncAddressBook();
    } else if (result.sharedPeer != null) {
      final AddressBookPeer peer = result.sharedPeer!;
      await _connectDevice(
        Device(
          id: peer.id,
          name: peer.name,
          server: _server.url,
          addedAt: DateTime.now(),
        ),
      );
    }
  }

  Future<void> _signOutAccount() async {
    final AccountSession? account = _account;
    if (account != null) {
      try {
        await _logoutAccount(_server, account.token);
      } catch (_) {
        // A token that cannot be revoked server-side still leaves this device.
      }
    }
    await _accountStore.clear();
    if (!mounted) {
      return;
    }
    setState(() {
      _account = null;
      _groups = <AddressBookGroup>[];
      _groupsSynced = false;
    });
    _showSnack(tr('已退出账号'));
  }

  Future<void> _expireAccount() async {
    await _accountStore.clear();
    if (!mounted) {
      return;
    }
    setState(() {
      _account = null;
      _groups = <AddressBookGroup>[];
      _groupsSynced = false;
    });
  }

  /// Pulls the account address book, merges it locally and uploads local
  /// entries the account does not have. A 401 drops the stored token so the
  /// next attempt goes through the login dialog instead of looping.
  Future<void> _syncAddressBook({bool announce = true}) async {
    final AccountSession? account = _account;
    if (account == null) {
      if (announce) {
        _showSnack(tr('请先登录账号'));
      }
      return;
    }
    if (!account.isActiveAt(DateTime.now())) {
      await _expireAccount();
      if (announce) {
        _showSnack(tr('账号登录已过期，请重新登录'));
      }
      return;
    }
    if (_syncing || account.server != _server.url) {
      return;
    }
    setState(() => _syncing = true);
    try {
      final List<PendingDeletion> queued = await _deviceStore
          .loadPendingDeletions();
      for (final PendingDeletion deletion in queued) {
        if (deletion.server != _server.url ||
            deletion.username != account.username) {
          continue;
        }
        try {
          await _deleteAddressBookPeer(_server, account.token, deletion.id);
        } on ServerApiException catch (error) {
          if (error.statusCode == HttpStatus.unauthorized) {
            await _expireAccount();
            if (announce) {
              _showSnack(tr('账号登录已过期，请重新登录'));
            }
            return;
          }
          if (error.statusCode != HttpStatus.notFound) {
            continue;
          }
        } catch (_) {
          continue;
        }
        await _deviceStore.removePendingDeletion(deletion.key);
      }
      final Set<String> pendingDeletions = <String>{
        for (final PendingDeletion deletion
            in await _deviceStore.loadPendingDeletions())
          if (deletion.server == _server.url &&
              deletion.username == account.username)
            deletion.id,
      };
      // The group cache lives in memory, so the first sync after launch reads
      // the whole book to learn group names even when the peer cursor is old.
      final int since = _groupsSynced ? account.bookRevision : 0;
      final AddressBookSnapshot snapshot = await _fetchAddressBook(
        _server,
        account.token,
        since,
      );
      final List<AddressBookGroup> groups = mergeAddressBookGroups(
        current: _groups,
        snapshot: snapshot,
        since: since,
      );
      final AddressBookPlan plan = planAddressBookSync(
        devices: await _deviceStore.load(),
        peers: snapshot.peers,
        groups: groups,
        tombstones: snapshot.tombstones,
        pendingDeletions: pendingDeletions,
        serverUrl: _server.url,
        now: DateTime.now(),
      );
      int failed = 0;
      final Map<String, AddressBookPush> uploaded = <String, AddressBookPush>{};
      final Map<String, AddressBookPeer> written = <String, AddressBookPeer>{};
      final Set<String> conflicts = <String>{};
      for (final AddressBookPush push in plan.pushes) {
        try {
          final AddressBookPeer peer = await _writeAddressBookPeer(
            _server,
            account.token,
            push.id,
            name: push.name,
            tags: push.tags,
            groupId: push.groupId,
            ifRevision: push.revision > 0 ? push.revision : null,
          );
          uploaded[push.id] = push;
          written[push.id] = peer;
        } on ServerApiException catch (error) {
          if (error.statusCode == HttpStatus.unauthorized) {
            await _expireAccount();
            if (announce) {
              _showSnack(tr('账号登录已过期，请重新登录'));
            }
            return;
          }
          if (error.statusCode == HttpStatus.conflict) {
            conflicts.add(push.id);
            continue;
          }
          failed++;
        } catch (_) {
          failed++;
        }
      }
      AddressBookSnapshot? refreshed;
      if (conflicts.isNotEmpty) {
        try {
          refreshed = await _fetchAddressBook(_server, account.token, 0);
        } catch (_) {
          // Without a fresh read the conflicting entries stay local and are
          // retried next sync; the server never applied the stale write.
        }
      }
      final Map<String, AddressBookPeer> refreshedById =
          <String, AddressBookPeer>{
            if (refreshed != null)
              for (final AddressBookPeer peer in refreshed.peers) peer.id: peer,
          };
      final List<Device> merged = <Device>[];
      for (final Device item in plan.devices) {
        final bool conflicted =
            refreshed != null &&
            conflicts.contains(item.id) &&
            item.saved &&
            item.server == _server.url;
        if (!conflicted) {
          merged.add(_settleUploadedClears(item, uploaded, written));
          continue;
        }
        final AddressBookPeer? peer = refreshedById[item.id];
        if (peer == null) {
          continue;
        }
        merged.add(
          item.copyWith(
            name: peer.name,
            tags: peer.tags,
            groupId: peer.groupId,
            revision: peer.revision,
            tagsCleared: false,
            groupCleared: false,
            saved: true,
          ),
        );
      }
      final List<Device> devices = await _deviceStore.replaceAll(merged);
      final AccountSession session = account.copyWith(
        syncedAt: DateTime.now(),
        bookRevision: refreshed?.revision ?? snapshot.revision,
      );
      await _accountStore.save(session);
      if (!mounted) {
        return;
      }
      setState(() {
        _devices = devices;
        _groups = groups;
        _groupsSynced = true;
        _account = session;
      });
      unawaited(_refreshPresence());
      if (announce) {
        _showSnack(
          _syncSummary(plan, failed, pendingDeletions.length, conflicts.length),
        );
      }
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.unauthorized) {
        await _expireAccount();
        if (announce) {
          _showSnack(tr('账号登录已过期，请重新登录'));
        }
      } else if (announce) {
        _showSnack(tr('地址簿同步失败：{p5}', <String, Object?>{'p5': error.message}));
      }
    } catch (error) {
      if (announce) {
        _showSnack(tr('地址簿同步失败：{error}', <String, Object?>{'error': error}));
      }
    } finally {
      if (mounted) {
        setState(() => _syncing = false);
      } else {
        _syncing = false;
      }
    }
  }

  String _syncSummary(
    AddressBookPlan plan,
    int failed,
    int pendingDeletes, [
    int conflicts = 0,
  ]) {
    final int uploaded = plan.pushes.length - failed;
    final List<String> details = <String>[
      if (plan.added > 0) tr('新增 {p6}', <String, Object?>{'p6': plan.added}),
      if (plan.updated > 0) tr('更新 {p7}', <String, Object?>{'p7': plan.updated}),
      if (plan.removed > 0) tr('移除 {p8}', <String, Object?>{'p8': plan.removed}),
      if (uploaded > 0) tr('上传 {uploaded}', <String, Object?>{'uploaded': uploaded}),
      if (conflicts > 0) tr('冲突 {conflicts} 已刷新', <String, Object?>{'conflicts': conflicts}),
      if (failed > 0) tr('{failed} 项上传失败', <String, Object?>{'failed': failed}),
      if (pendingDeletes > 0) tr('待删除重试 {pendingDeletes}', <String, Object?>{'pendingDeletes': pendingDeletes}),
    ];
    return details.isEmpty ? tr('地址簿已同步') : tr('地址簿已同步（{p9}）', <String, Object?>{'p9': details.join('，')});
  }

  /// A successful upload settles the offline clear intent it carried and
  /// records the revision the server returned, so a later edit does not send
  /// a token that is already stale.
  Device _settleUploadedClears(
    Device device,
    Map<String, AddressBookPush> uploaded,
    Map<String, AddressBookPeer> written,
  ) {
    final AddressBookPush? push = uploaded[device.id];
    if (push == null || !device.saved || device.server != _server.url) {
      return device;
    }
    return device.copyWith(
      tagsCleared: push.tags == null ? device.tagsCleared : false,
      groupCleared: push.groupId == null ? device.groupCleared : false,
      revision: written[device.id]?.revision ?? device.revision,
    );
  }

  /// Replaces a stale local entry with the server copy after a rejected write.
  /// A peer deleted elsewhere is removed locally instead of being recreated.
  Future<void> _refreshAfterConflict(String peerId) async {
    final AccountSession? account = _account;
    if (account == null || account.server != _server.url) {
      if (mounted) {
        _showSnack(tr('该条目已在其他设备修改，请重新同步'));
      }
      return;
    }
    try {
      final AddressBookSnapshot snapshot = await _fetchAddressBook(
        _server,
        account.token,
        0,
      );
      final Map<String, AddressBookPeer> byId = <String, AddressBookPeer>{
        for (final AddressBookPeer peer in snapshot.peers) peer.id: peer,
      };
      final List<Device> current = await _deviceStore.load();
      final List<Device> next = <Device>[];
      for (final Device item in current) {
        if (item.id != peerId || !item.saved || item.server != _server.url) {
          next.add(item);
          continue;
        }
        final AddressBookPeer? peer = byId[item.id];
        if (peer == null) {
          continue;
        }
        next.add(
          item.copyWith(
            name: peer.name,
            tags: peer.tags,
            groupId: peer.groupId,
            revision: peer.revision,
            tagsCleared: false,
            groupCleared: false,
            saved: true,
          ),
        );
      }
      final List<Device> devices = await _deviceStore.replaceAll(next);
      final AccountSession session = account.copyWith(
        syncedAt: DateTime.now(),
        bookRevision: snapshot.revision,
      );
      await _accountStore.save(session);
      if (!mounted) {
        return;
      }
      setState(() {
        _devices = devices;
        _groups = mergeAddressBookGroups(
          current: _groups,
          snapshot: snapshot,
          since: 0,
        );
        _groupsSynced = true;
        _account = session;
      });
      _showSnack(tr('该条目已在其他设备修改，已刷新为服务器版本'));
    } catch (_) {
      if (mounted) {
        _showSnack(tr('该条目已在其他设备修改，请重新同步'));
      }
    }
  }

  /// Best-effort upload after a local address-book change. The local edit is
  /// already durable, so a server failure must not undo it.
  Future<void> _pushAccountPeer(
    Device device, {
    bool allowEmptyName = false,
    bool syncTags = false,
  }) async {
    final AccountSession? account = _account;
    final String? name = device.name.isNotEmpty || allowEmptyName
        ? device.name
        : null;
    // A rename must not clear tags it never touched; an explicit tag edit
    // sends the list even when empty so clearing propagates.
    final List<String>? tags = device.tags.isNotEmpty || syncTags
        ? device.tags
        : null;
    if ((name == null && tags == null) ||
        !device.viaServer ||
        device.server != _server.url ||
        account == null ||
        account.server != _server.url ||
        !account.isActiveAt(DateTime.now())) {
      return;
    }
    try {
      final AddressBookPeer written = await _writeAddressBookPeer(
        _server,
        account.token,
        device.id,
        name: name,
        tags: tags,
        ifRevision: device.revision > 0 ? device.revision : null,
      );
      List<Device> settled = await _deviceStore.setPeerRevision(
        device.key,
        written.revision,
      );
      if (tags != null) {
        settled = await _deviceStore.markPushSettled(device.key, tags: true);
      }
      if (mounted) {
        setState(() => _devices = settled);
      }
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.unauthorized) {
        await _expireAccount();
        return;
      }
      if (error.statusCode == HttpStatus.conflict) {
        await _refreshAfterConflict(device.id);
      }
    } catch (_) {
      // Best effort; a later sync uploads the entry again.
    }
  }

  Future<void> _deleteAccountPeer(Device device) async {
    final AccountSession? account = _account;
    if (!device.saved ||
        !device.viaServer ||
        device.server != _server.url ||
        account == null ||
        account.server != _server.url ||
        !account.isActiveAt(DateTime.now())) {
      return;
    }
    final String pendingKey = PendingDeletion(
      server: account.server,
      username: account.username,
      id: device.id,
      deletedAt: DateTime.now(),
    ).key;
    try {
      await _deleteAddressBookPeer(_server, account.token, device.id);
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.unauthorized) {
        await _expireAccount();
        return;
      }
      if (error.statusCode != HttpStatus.notFound) {
        // Network or server failure: keep the outbox entry for the next sync.
        return;
      }
      // 404 means the peer is already gone, which is the desired state.
    } catch (_) {
      return;
    }
    await _deviceStore.removePendingDeletion(pendingKey);
  }

  void _showSnack(String text) {
    if (!mounted) {
      return;
    }
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(text)));
  }

  Future<void> _showServerSettings() async {
    final ServerSettings? settings = await showDialog<ServerSettings>(
      context: context,
      builder: (BuildContext context) {
        return _ServerDialog(initial: _server, identityStore: _identityStore);
      },
    );
    if (settings == null) {
      return;
    }
    await _serverStore.save(settings);
    if (!mounted) {
      return;
    }
    final bool serverChanged =
        _account != null && _account!.server != settings.url;
    if (serverChanged) {
      await _accountStore.clear();
    }
    setState(() {
      _server = settings;
      if (serverChanged) {
        _account = null;
      }
    });
    unawaited(_refreshPresence());
  }

  Future<void> _setUploadRecordings(bool enabled) async {
    final ServerSettings updated = _server.copyWith(
      uploadRecordings: enabled,
    );
    setState(() => _server = updated);
    await _serverStore.save(updated);
  }

  Future<void> _showUpdate(SoftwareInfo update) async {
    final ServerIdentityPin? pin = _serverPins[normalizeServerUrl(_server.url)];
    final String? pinKey = pin?.identity.publicKey;
    final bool canInstall = update.installable && pinKey != null;
    final String? action = await showDialog<String>(
      context: context,
      builder: (BuildContext context) {
        return AlertDialog(
          backgroundColor: Theme.of(context).colorScheme.surface,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(18),
          ),
          title: Text(tr('发现新版本')),
          content: SizedBox(
            width: 420,
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(tr('当前版本 {chastVersion}，连接服务提供 {p10}。', <String, Object?>{'chastVersion': chastVersion, 'p10': update.version})),
                const SizedBox(height: 12),
                SelectableText(
                  update.url,
                  style: const TextStyle(fontSize: 12),
                ),
                const SizedBox(height: 10),
                Text(
                  canInstall
                      ? tr('更新包带有服务端签名。校验通过后查看器退出，由 chast-tunnel 替换并重新启动。')
                      : update.installable
                      ? tr('尚未固定此服务器的身份，无法校验更新签名；连接一次后再试，或手动下载。')
                      : tr('更新由运营者发布，查看器不会自动下载或安装。'),
                  style: mutedStyle(context, size: 10.5),
                ),
              ],
            ),
          ),
          actions: <Widget>[
            TextButton(
              onPressed: () => Navigator.of(context).pop(),
              child: Text(tr('关闭')),
            ),
            TextButton(
              onPressed: () => Navigator.of(context).pop('copy'),
              child: Text(tr('复制下载地址')),
            ),
            if (canInstall)
              FilledButton(
                onPressed: () => Navigator.of(context).pop('install'),
                child: Text(tr('下载并安装')),
              ),
          ],
        );
      },
    );
    if (action == 'install' && pinKey != null) {
      await _installUpdateAndExit(update, pinKey);
      return;
    }
    if (action != 'copy') {
      return;
    }
    await Clipboard.setData(ClipboardData(text: update.url));
    if (mounted) {
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(tr('下载地址已复制'))));
    }
  }

  Future<void> _installUpdateAndExit(
    SoftwareInfo update,
    String serverPublicKey,
  ) async {
    try {
      await _installUpdate(update, serverPublicKey);
    } catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text(tr('无法启动更新：{error}', <String, Object?>{'error': error}))));
      }
      return;
    }
    _exitApp(0);
  }

  Future<void> _showAbout() async {
    await showDialog<void>(
      context: context,
      builder: (BuildContext context) {
        return AlertDialog(
          backgroundColor: Theme.of(context).colorScheme.surface,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(18),
          ),
          title: Text(tr('关于 Chast')),
          content: Text(
            tr('Chast {chastVersion}\n\nWindows 桌面采集、Intel 硬件编码、MsQuic 直连显示与键鼠控制。远端需以 chast-quic serve --input 启动才会接受输入；设备 ID 经 chast-server 解析，直连失败时自动回退中继。', <String, Object?>{'chastVersion': chastVersion}),
          ),
          actions: <Widget>[
            TextButton(
              onPressed: () => Navigator.of(context).pop(),
              child: Text(tr('关闭')),
            ),
          ],
        );
      },
    );
  }

  List<Device> get _savedDevices {
    final List<Device> devices = _devices
        .where((Device device) => device.saved)
        .toList();
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

  /// Tags shown on the device card: stored account tags win over the host's
  /// self-reported tags, matching what the user can actually see.
  List<String> _visibleTags(Device device) {
    final List<String> tags = device.tags.isNotEmpty
        ? device.tags
        : _presence[device.key]?.info?.tags ?? const <String>[];
    return Device.cleanTags(tags);
  }

  List<String> _availableFilterTags(List<Device> saved) {
    final List<String> tags =
        <String>{
          for (final Device device in saved) ..._visibleTags(device),
        }.toList()..sort(
          (String a, String b) => a.toLowerCase().compareTo(b.toLowerCase()),
        );
    return tags;
  }

  /// Selected tags that still exist; a tag whose last device disappeared must
  /// not keep hiding the whole address book behind an invisible chip.
  Set<String> _activeTags(List<Device> saved) {
    final Set<String> available = _availableFilterTags(saved).toSet();
    return _selectedTags.where(available.contains).toSet();
  }

  List<Device> _filteredSavedDevices(List<Device> saved, Set<String> tags) {
    final String query = _deviceQuery.trim().toLowerCase();
    if (query.isEmpty && tags.isEmpty) {
      return saved;
    }
    return <Device>[
      for (final Device device in saved)
        if (_matchesFilter(device, query, tags)) device,
    ];
  }

  bool _matchesFilter(Device device, String query, Set<String> tags) {
    final List<String> visibleTags = _visibleTags(device);
    if (tags.isNotEmpty && !visibleTags.any(tags.contains)) {
      return false;
    }
    if (query.isEmpty) {
      return true;
    }
    final DevicePresence? presence = _presence[device.key];
    final Iterable<String> fields = <String>[
      device.id,
      device.name,
      ...device.tags,
      ...visibleTags,
      presence?.info?.alias ?? '',
      ...?presence?.info?.tags,
    ];
    return fields.any((String value) => value.toLowerCase().contains(query));
  }

  void _clearDeviceFilters() {
    setState(() {
      _deviceSearch.clear();
      _deviceQuery = '';
      _selectedTags.clear();
    });
  }

  int get _onlineCount => _presence.values
      .where((DevicePresence presence) => presence.online)
      .length;

  /// A server offer is shown once; dismissing hides that version for this
  /// session, while a later version appears again.
  SoftwareInfo? get _availableUpdate {
    final SoftwareInfo? software = _software;
    if (software == null ||
        software.version == chastVersion ||
        software.version == _softwareDismissed) {
      return null;
    }
    return software;
  }

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
            account: _account,
            syncing: _syncing,
            onConfigureServer: _showServerSettings,
            onAccount: _showAccount,
          ),
          VerticalDivider(
            width: 1,
            thickness: 1,
            color: Theme.of(context).dividerColor,
          ),
          Expanded(child: _buildMain(context)),
        ],
      ),
    );
  }

  Widget _buildMain(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    final Color subtle = dark ? Colors.white12 : const Color(0xFFE4E6EB);
    final Color fieldFill = dark
        ? const Color(0xFF1F2026)
        : const Color(0xFFF7F8FA);
    final List<Device> saved = _savedDevices;
    final Set<String> activeTags = _activeTags(saved);
    final List<Device> filtered = _filteredSavedDevices(saved, activeTags);
    final bool filtering =
        _deviceQuery.trim().isNotEmpty || activeTags.isNotEmpty;
    final List<Device> recent = _recentDevices;
    final SoftwareInfo? update = _availableUpdate;

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
                            tr('控制远程桌面'),
                            style: theme.textTheme.titleLarge?.copyWith(
                              fontSize: 19,
                              fontWeight: FontWeight.w600,
                            ),
                          ),
                          const SizedBox(height: 6),
                          Text(
                            tr('输入设备 ID，或直接连接对端 chast QUIC 地址'),
                            style: mutedStyle(context),
                          ),
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
                              hintText: tr('设备 ID 或 主机:端口'),
                              hintStyle: TextStyle(
                                fontSize: 18,
                                fontWeight: FontWeight.w300,
                                color: dark
                                    ? Colors.white38
                                    : const Color(0xFF9AA0A6),
                              ),
                              prefixIcon: Icon(
                                Icons.link_rounded,
                                size: 20,
                                color: dark
                                    ? Colors.white54
                                    : const Color(0xFF7C8390),
                              ),
                              filled: true,
                              fillColor: fieldFill,
                              contentPadding: const EdgeInsets.symmetric(
                                horizontal: 12,
                                vertical: 14,
                              ),
                              enabledBorder: OutlineInputBorder(
                                borderRadius: BorderRadius.circular(8),
                                borderSide: BorderSide(color: subtle),
                              ),
                              focusedBorder: OutlineInputBorder(
                                borderRadius: BorderRadius.circular(8),
                                borderSide: const BorderSide(
                                  color: kAccent,
                                  width: 1.5,
                                ),
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
                              hintText: tr('访问令牌（主机启用时填写）'),
                              hintStyle: TextStyle(
                                fontSize: 13,
                                color: dark
                                    ? Colors.white38
                                    : const Color(0xFF9AA0A6),
                              ),
                              prefixIcon: Icon(
                                Icons.key_rounded,
                                size: 18,
                                color: dark
                                    ? Colors.white54
                                    : const Color(0xFF7C8390),
                              ),
                              filled: true,
                              fillColor: fieldFill,
                              contentPadding: const EdgeInsets.symmetric(
                                horizontal: 12,
                                vertical: 12,
                              ),
                              enabledBorder: OutlineInputBorder(
                                borderRadius: BorderRadius.circular(8),
                                borderSide: BorderSide(color: subtle),
                              ),
                              focusedBorder: OutlineInputBorder(
                                borderRadius: BorderRadius.circular(8),
                                borderSide: const BorderSide(
                                  color: kAccent,
                                  width: 1.5,
                                ),
                              ),
                            ),
                          ),
                          const SizedBox(height: 6),
                          Text(
                            _server.configured
                                ? tr('设备 ID 通过 {p11} 解析；令牌只用于这次连接。', <String, Object?>{'p11': _server.url})
                                : tr('带 : 的地址直接连接；设备 ID 需先配置连接服务。'),
                            style: mutedStyle(context, size: 10.5),
                          ),
                          if (_error != null) ...<Widget>[
                            const SizedBox(height: 10),
                            Row(
                              children: <Widget>[
                                Icon(
                                  Icons.error_outline,
                                  size: 15,
                                  color: theme.colorScheme.error,
                                ),
                                const SizedBox(width: 6),
                                Expanded(
                                  child: Text(
                                    _error!,
                                    style: TextStyle(
                                      fontSize: 12,
                                      color: theme.colorScheme.error,
                                    ),
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
                                    onPressed: _opening
                                        ? null
                                        : () => _connect(_address.text),
                                    style: ElevatedButton.styleFrom(
                                      backgroundColor: kAccent,
                                      foregroundColor: Colors.white,
                                      disabledBackgroundColor: kAccent
                                          .withValues(alpha: 0.5),
                                      elevation: 0,
                                      shape: RoundedRectangleBorder(
                                        borderRadius: BorderRadius.circular(8),
                                      ),
                                      textStyle: const TextStyle(
                                        fontSize: 14,
                                        fontWeight: FontWeight.w600,
                                      ),
                                    ),
                                    child: _opening
                                        ? const SizedBox(
                                            width: 16,
                                            height: 16,
                                            child: CircularProgressIndicator(
                                              strokeWidth: 2,
                                              valueColor:
                                                  AlwaysStoppedAnimation<Color>(
                                                    Colors.white,
                                                  ),
                                            ),
                                          )
                                        : Text(tr('连接')),
                                  ),
                                ),
                              ),
                              const SizedBox(width: 8),
                              Tooltip(
                                message: tr('仅文件传输'),
                                child: SizedBox(
                                  width: 40,
                                  height: 40,
                                  child: Material(
                                    color: fieldFill,
                                    borderRadius: BorderRadius.circular(8),
                                    child: InkWell(
                                      borderRadius: BorderRadius.circular(8),
                                      onTap: _opening
                                          ? null
                                          : () => _connect(
                                              _address.text,
                                              fileTransferOnly: true,
                                            ),
                                      child: Icon(
                                        Icons.folder_shared_rounded,
                                        size: 20,
                                        color: dark
                                            ? Colors.white70
                                            : const Color(0xFF5A6270),
                                      ),
                                    ),
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
                                      color: dark
                                          ? Colors.white70
                                          : const Color(0xFF5A6270),
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
                if (update != null) ...<Widget>[
                  const SizedBox(height: 18),
                  _UpdateBanner(
                    update: update,
                    current: chastVersion,
                    onDetails: () => _showUpdate(update),
                    onDismiss: () =>
                        setState(() => _softwareDismissed = update.version),
                  ),
                ],
                const SizedBox(height: 30),
                Row(
                  children: <Widget>[
                    Text(
                      tr('我的设备'),
                      style: theme.textTheme.titleMedium?.copyWith(
                        fontSize: 15,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                    const SizedBox(width: 8),
                    if (saved.isNotEmpty)
                      Text(
                        filtering
                            ? '${filtered.length}/${saved.length}'
                            : '${saved.length}',
                        key: const Key('saved-count'),
                        style: mutedStyle(context, size: 12),
                      ),
                    if (filtering)
                      TextButton(
                        key: const Key('device-filter-reset'),
                        onPressed: _clearDeviceFilters,
                        style: TextButton.styleFrom(
                          foregroundColor: kAccent,
                          minimumSize: Size.zero,
                          padding: const EdgeInsets.symmetric(
                            horizontal: 6,
                            vertical: 2,
                          ),
                          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                        ),
                        child: Text(
                          tr('清除筛选'),
                          style: TextStyle(fontSize: 11),
                        ),
                      ),
                    const Spacer(),
                    if (_account != null && _account!.server == _server.url)
                      TextButton.icon(
                        key: const Key('manage-groups'),
                        onPressed: _manageGroups,
                        style: TextButton.styleFrom(foregroundColor: kAccent),
                        icon: const Icon(Icons.folder_outlined, size: 16),
                        label: Text(tr('分组'), style: TextStyle(fontSize: 12)),
                      ),
                    TextButton.icon(
                      key: const Key('lan-scan'),
                      onPressed: _scanLan,
                      style: TextButton.styleFrom(foregroundColor: kAccent),
                      icon: const Icon(Icons.radar_rounded, size: 16),
                      label: Text(tr('扫描'), style: TextStyle(fontSize: 12)),
                    ),
                    TextButton.icon(
                      onPressed: _showAddDevice,
                      style: TextButton.styleFrom(foregroundColor: kAccent),
                      icon: const Icon(Icons.add_rounded, size: 16),
                      label: Text(tr('添加'), style: TextStyle(fontSize: 12)),
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
                        child: CircularProgressIndicator(
                          strokeWidth: 2,
                          color: kAccent,
                        ),
                      ),
                    ),
                  )
                else if (saved.isEmpty)
                  _emptyPanel(
                    context,
                    icon: Icons.bookmark_add_outlined,
                    text: tr('添加常用设备，输入设备 ID 即可一键连接'),
                    height: 104,
                  )
                else ...<Widget>[
                  _buildDeviceFilter(context, saved, activeTags),
                  const SizedBox(height: 12),
                  if (filtered.isEmpty)
                    _emptyPanel(
                      context,
                      icon: Icons.search_off_rounded,
                      text: tr('没有匹配的设备'),
                      height: 118,
                      action: TextButton(
                        key: const Key('device-filter-reset-empty'),
                        onPressed: _clearDeviceFilters,
                        style: TextButton.styleFrom(
                          foregroundColor: kAccent,
                          minimumSize: Size.zero,
                          padding: const EdgeInsets.symmetric(
                            horizontal: 8,
                            vertical: 4,
                          ),
                          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                        ),
                        child: Text(
                          tr('清除筛选'),
                          style: TextStyle(fontSize: 12),
                        ),
                      ),
                    )
                  else
                    _buildSavedDevices(context, filtered),
                ],
                const SizedBox(height: 30),
                Row(
                  children: <Widget>[
                    Text(
                      tr('最近连接'),
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
                        child: Text(tr('清空'), style: TextStyle(fontSize: 12)),
                      ),
                  ],
                ),
                const SizedBox(height: 12),
                if (recent.isEmpty)
                  _emptyPanel(
                    context,
                    icon: Icons.history_rounded,
                    text: tr('连接过的设备会出现在这里，保存后进入我的设备'),
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
                          presence:
                              _presence[device.key] ?? DevicePresence.unknown,
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

  Widget _buildDeviceFilter(
    BuildContext context,
    List<Device> saved,
    Set<String> activeTags,
  ) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    final Color subtle = dark ? Colors.white12 : const Color(0xFFE4E6EB);
    final Color fieldFill = dark
        ? const Color(0xFF1F2026)
        : const Color(0xFFF7F8FA);
    final List<String> tags = _availableFilterTags(saved);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        TextField(
          key: const Key('device-search'),
          controller: _deviceSearch,
          onChanged: (String value) => setState(() => _deviceQuery = value),
          style: TextStyle(fontSize: 12.5, color: theme.colorScheme.onSurface),
          decoration: InputDecoration(
            isDense: true,
            hintText: tr('搜索名称、ID 或标签'),
            hintStyle: TextStyle(
              fontSize: 12,
              color: dark ? Colors.white38 : const Color(0xFF9AA0A6),
            ),
            prefixIcon: Icon(
              Icons.search_rounded,
              size: 16,
              color: dark ? Colors.white54 : const Color(0xFF7C8390),
            ),
            prefixIconConstraints: const BoxConstraints(
              minWidth: 34,
              minHeight: 34,
            ),
            suffixIcon: _deviceQuery.isEmpty
                ? null
                : IconButton(
                    key: const Key('device-search-clear'),
                    tooltip: tr('清除搜索'),
                    splashRadius: 14,
                    icon: Icon(
                      Icons.close_rounded,
                      size: 15,
                      color: dark ? Colors.white54 : const Color(0xFF7C8390),
                    ),
                    onPressed: () {
                      _deviceSearch.clear();
                      setState(() => _deviceQuery = '');
                    },
                  ),
            suffixIconConstraints: const BoxConstraints(
              minWidth: 34,
              minHeight: 34,
            ),
            filled: true,
            fillColor: fieldFill,
            contentPadding: const EdgeInsets.symmetric(
              horizontal: 10,
              vertical: 10,
            ),
            enabledBorder: OutlineInputBorder(
              borderRadius: BorderRadius.circular(8),
              borderSide: BorderSide(color: subtle),
            ),
            focusedBorder: OutlineInputBorder(
              borderRadius: BorderRadius.circular(8),
              borderSide: const BorderSide(color: kAccent, width: 1.2),
            ),
          ),
        ),
        if (tags.isNotEmpty) ...<Widget>[
          const SizedBox(height: 8),
          Wrap(
            spacing: 6,
            runSpacing: 6,
            children: <Widget>[
              _filterChip(
                context,
                key: const Key('tag-filter-all'),
                label: tr('全部'),
                selected: activeTags.isEmpty,
                onTap: () => setState(() => _selectedTags.clear()),
              ),
              for (final String tag in tags)
                _filterChip(
                  context,
                  key: Key('tag-filter-$tag'),
                  label: tag,
                  selected: activeTags.contains(tag),
                  onTap: () => setState(() {
                    if (!_selectedTags.remove(tag)) {
                      _selectedTags.add(tag);
                    }
                  }),
                ),
            ],
          ),
        ],
      ],
    );
  }

  Widget _filterChip(
    BuildContext context, {
    required Key key,
    required String label,
    required bool selected,
    required VoidCallback onTap,
  }) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    return Material(
      key: key,
      color: selected ? kAccent.withValues(alpha: 0.12) : Colors.transparent,
      shape: StadiumBorder(
        side: BorderSide(
          color: selected
              ? kAccent.withValues(alpha: 0.55)
              : dark
              ? Colors.white12
              : const Color(0xFFE4E6EB),
        ),
      ),
      clipBehavior: Clip.antiAlias,
      child: InkWell(
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
          child: Text(
            label,
            style: TextStyle(
              fontSize: 11,
              fontWeight: selected ? FontWeight.w600 : FontWeight.w400,
              color: selected
                  ? kAccent
                  : theme.colorScheme.onSurface.withValues(alpha: 0.68),
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildSavedDevices(BuildContext context, List<Device> saved) {
    final bool accountReady =
        _account != null && _account!.server == _server.url;
    final List<AddressBookGroup> groups = accountReady
        ? <AddressBookGroup>[
            for (final AddressBookGroup group in _groups)
              if (!group.deleted) group,
          ]
        : const <AddressBookGroup>[];
    final Map<String, AddressBookGroup> groupsById = <String, AddressBookGroup>{
      for (final AddressBookGroup group in groups) group.id: group,
    };
    final Map<String, List<Device>> grouped = <String, List<Device>>{
      for (final String id in groupsById.keys) id: <Device>[],
    };
    final List<Device> ungrouped = <Device>[];
    for (final Device device in saved) {
      final bool inGroup =
          device.viaServer &&
          device.server == _server.url &&
          groupsById.containsKey(device.groupId);
      if (inGroup) {
        grouped[device.groupId]!.add(device);
      } else {
        ungrouped.add(device);
      }
    }
    for (final List<Device> devices in grouped.values) {
      devices.sort(_comparePeerOrder);
    }
    final List<AddressBookGroupNode> nodes = addressBookGroupTreeOrder(groups);
    final Map<String, int> subtreeCounts = <String, int>{};
    for (final AddressBookGroupNode node in nodes.reversed) {
      int count = grouped[node.group.id]?.length ?? 0;
      for (final AddressBookGroupNode other in nodes) {
        if (other.group.parentId == node.group.id) {
          count += subtreeCounts[other.group.id] ?? 0;
        }
      }
      subtreeCounts[node.group.id] = count;
    }
    final Set<String> hidden = <String>{};
    for (final AddressBookGroupNode node in nodes) {
      final String parent = node.group.parentId;
      if (parent.isNotEmpty &&
          (_collapsedGroups.contains(parent) || hidden.contains(parent))) {
        hidden.add(node.group.id);
      }
    }
    final List<AddressBookGroupNode> visible = <AddressBookGroupNode>[
      for (final AddressBookGroupNode node in nodes)
        if (!hidden.contains(node.group.id) &&
            (subtreeCounts[node.group.id] ?? 0) > 0)
          node,
    ];
    if (visible.isEmpty) {
      return Wrap(
        spacing: 12,
        runSpacing: 12,
        children: <Widget>[
          for (final Device device in saved) _deviceCard(device),
        ],
      );
    }
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        for (final AddressBookGroupNode node in visible) ...<Widget>[
          _groupHeader(
            context,
            key: Key('device-group-${node.group.id}'),
            label: node.group.name,
            count: subtreeCounts[node.group.id] ?? 0,
            depth: node.depth,
            collapsed: _collapsedGroups.contains(node.group.id),
            onToggle: () => setState(() {
              if (!_collapsedGroups.remove(node.group.id)) {
                _collapsedGroups.add(node.group.id);
              }
            }),
          ),
          if (!_collapsedGroups.contains(node.group.id))
            Padding(
              padding: EdgeInsets.only(top: 8, left: node.depth * 16.0),
              child: Wrap(
                spacing: 12,
                runSpacing: 12,
                children: <Widget>[
                  for (final Device device in grouped[node.group.id]!)
                    _deviceCard(
                      device,
                      onMoveUp: _canMovePeer(device, -1)
                          ? () => _movePeerInGroup(device, -1)
                          : null,
                      onMoveDown: _canMovePeer(device, 1)
                          ? () => _movePeerInGroup(device, 1)
                          : null,
                    ),
                ],
              ),
            ),
          const SizedBox(height: 14),
        ],
        if (ungrouped.isNotEmpty) ...<Widget>[
          _groupHeader(
            context,
            key: const Key('device-group-ungrouped'),
            label: tr('未分组'),
            count: ungrouped.length,
            collapsed: _collapsedGroups.contains(''),
            ungrouped: true,
            onToggle: () => setState(() {
              if (!_collapsedGroups.remove('')) {
                _collapsedGroups.add('');
              }
            }),
          ),
          if (!_collapsedGroups.contains(''))
            Padding(
              padding: const EdgeInsets.only(top: 8),
              child: Wrap(
                spacing: 12,
                runSpacing: 12,
                children: <Widget>[
                  for (final Device device in ungrouped) _deviceCard(device),
                ],
              ),
            ),
        ],
      ],
    );
  }

  static int _comparePeerOrder(Device a, Device b) {
    if (a.position != b.position) {
      return a.position.compareTo(b.position);
    }
    return a.id.compareTo(b.id);
  }

  /// Saved peers of the same server and group, in server order.
  List<Device> _groupPeers(String groupId) {
    return <Device>[
      for (final Device device in _devices)
        if (device.saved &&
            device.viaServer &&
            device.server == _server.url &&
            device.groupId == groupId)
          device,
    ]..sort(_comparePeerOrder);
  }

  bool _canMovePeer(Device device, int delta) {
    final List<Device> peers = _groupPeers(device.groupId);
    final int index =
        peers.indexWhere((Device item) => item.key == device.key);
    final int target = index + delta;
    return index >= 0 && target >= 0 && target < peers.length;
  }

  Future<void> _movePeerInGroup(Device device, int delta) async {
    final AccountSession? account = _account;
    if (account == null || account.server != _server.url) {
      return;
    }
    final List<Device> peers = _groupPeers(device.groupId);
    final int index =
        peers.indexWhere((Device item) => item.key == device.key);
    final int target = index + delta;
    if (index < 0 || target < 0 || target >= peers.length) {
      return;
    }
    final List<Device> reordered = List<Device>.of(peers)
      ..removeAt(index)
      ..insert(target, device);
    try {
      for (int i = 0; i < reordered.length; i++) {
        final Device item = reordered[i];
        final int position = i + 1;
        if (item.position == position) {
          continue;
        }
        final AddressBookPeer written = await ServerApi.updateAddressBookPeer(
          _server,
          account.token,
          item.id,
          position: position,
          ifRevision: item.revision > 0 ? item.revision : null,
        );
        final List<Device> settled = await _deviceStore.setPeerRevision(
          item.key,
          written.revision,
          position: written.position,
        );
        if (mounted) {
          setState(() => _devices = settled);
        }
      }
      if (mounted) {
        _showSnack(tr('分组内顺序已更新'));
      }
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.unauthorized) {
        await _expireAccount();
        return;
      }
      if (error.statusCode == HttpStatus.conflict) {
        await _refreshAfterConflict(device.id);
        return;
      }
      if (mounted) {
        _showSnack(tr('顺序未能上传，请稍后重试'));
      }
    } catch (_) {
      if (mounted) {
        _showSnack(tr('顺序未能上传，请稍后重试'));
      }
    }
  }

  Widget _deviceCard(
    Device device, {
    VoidCallback? onMoveUp,
    VoidCallback? onMoveDown,
  }) {
    final bool accountReady =
        _account != null &&
        _account!.server == _server.url &&
        device.viaServer &&
        device.server == _server.url;
    return _DeviceCard(
      device: device,
      presence: _presence[device.key] ?? DevicePresence.unknown,
      pinned: _trust.containsKey(device.trustKey),
      onTap: () => _connectDevice(device),
      onFilesOnly: () => _connectDevice(device, fileTransferOnly: true),
      onRename: () => _renameDevice(device),
      onWake: () => _wakeDevice(device),
      onLabels: device.viaServer ? () => _editLabels(device) : null,
      onTags: accountReady ? () => _editTags(device) : null,
      onGroup: accountReady ? () => _moveDeviceToGroup(device) : null,
      onMoveUp: onMoveUp,
      onMoveDown: onMoveDown,
      onDelete: () => _deleteDevice(device),
    );
  }

  Widget _groupHeader(
    BuildContext context, {
    required Key key,
    required String label,
    required int count,
    required bool collapsed,
    required VoidCallback onToggle,
    int depth = 0,
    bool ungrouped = false,
  }) {
    final ThemeData theme = Theme.of(context);
    return InkWell(
      key: key,
      borderRadius: BorderRadius.circular(8),
      onTap: onToggle,
      child: Padding(
        padding: EdgeInsets.fromLTRB(2 + depth * 16.0, 4, 2, 4),
        child: Row(
          children: <Widget>[
            Icon(
              ungrouped ? Icons.inbox_outlined : Icons.folder_open_rounded,
              size: 16,
              color: ungrouped
                  ? theme.colorScheme.onSurface.withValues(alpha: 0.5)
                  : kAccent,
            ),
            const SizedBox(width: 7),
            Text(
              label.isEmpty ? tr('未命名分组') : label,
              style: theme.textTheme.titleSmall?.copyWith(
                fontSize: 13,
                fontWeight: FontWeight.w600,
              ),
            ),
            const SizedBox(width: 6),
            Text('$count', style: mutedStyle(context, size: 11)),
            const Spacer(),
            Icon(
              collapsed
                  ? Icons.keyboard_arrow_right_rounded
                  : Icons.keyboard_arrow_down_rounded,
              size: 18,
              color: theme.colorScheme.onSurface.withValues(alpha: 0.45),
            ),
          ],
        ),
      ),
    );
  }

  Widget _emptyPanel(
    BuildContext context, {
    required IconData icon,
    required String text,
    required double height,
    Widget? action,
  }) {
    final bool dark = Theme.of(context).brightness == Brightness.dark;
    return Container(
      height: height,
      decoration: BoxDecoration(
        color: Theme.of(context).colorScheme.surface.withValues(alpha: 0.55),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(
          color: dark ? Colors.white12 : const Color(0xFFE4E6EB),
        ),
      ),
      child: Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Icon(
              icon,
              size: 25,
              color: dark ? Colors.white30 : const Color(0xFFA8AEB8),
            ),
            const SizedBox(height: 8),
            Text(text, style: mutedStyle(context)),
            if (action != null) ...<Widget>[const SizedBox(height: 4), action],
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
    required this.account,
    required this.syncing,
    required this.onConfigureServer,
    required this.onAccount,
  });

  final String hostname;
  final ServerSettings server;
  final int trustedCount;
  final int onlineCount;
  final AccountSession? account;
  final bool syncing;
  final VoidCallback onConfigureServer;
  final VoidCallback onAccount;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    final Color subtle = dark ? Colors.white12 : const Color(0xFFE2E4E9);
    final String service = server.configured
        ? (Uri.tryParse(server.url)?.authority ?? server.url)
        : tr('未配置');

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
                  child: const Icon(
                    Icons.desktop_windows_rounded,
                    color: Colors.white,
                    size: 20,
                  ),
                ),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(
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
                      Text(
                        tr('远程桌面'),
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: mutedStyle(context, size: 10),
                      ),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 34),
            Text(
              tr('本机信息'),
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
                            Text(
                              tr('查看器 · Windows'),
                              style: mutedStyle(context, size: 10),
                            ),
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
              tr('连接服务'),
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
                        server.configured
                            ? Icons.cloud_done_outlined
                            : Icons.cloud_off_outlined,
                        size: 17,
                        color: server.configured
                            ? kAccent
                            : (dark ? Colors.white38 : const Color(0xFF8A919C)),
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
                                  ? tr('{p17}{p18}', <String, Object?>{'p17': server.mode.shortLabel, 'p18': onlineCount > 0 ? tr(' · {onlineCount} 台在线', <String, Object?>{'onlineCount': onlineCount}) : ''})
                                  : tr('点击配置'),
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
                  color: trustedCount > 0
                      ? kAccent
                      : (dark ? Colors.white30 : const Color(0xFFA8AEB8)),
                ),
                const SizedBox(width: 5),
                Expanded(
                  child: Text(
                    trustedCount > 0
                        ? tr('已固定 {trustedCount} 个主机身份', <String, Object?>{'trustedCount': trustedCount})
                        : tr('首次连接时记录主机证书指纹'),
                    style: mutedStyle(context, size: 10),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 20),
            Text(
              tr('账号'),
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
                key: const Key('account-tile'),
                onTap: onAccount,
                child: Container(
                  padding: const EdgeInsets.fromLTRB(12, 10, 10, 10),
                  decoration: BoxDecoration(
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(color: subtle),
                  ),
                  child: Row(
                    children: <Widget>[
                      Icon(
                        account == null
                            ? Icons.person_outline_rounded
                            : Icons.person_rounded,
                        size: 17,
                        color: account == null
                            ? (dark ? Colors.white38 : const Color(0xFF8A919C))
                            : kAccent,
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: <Widget>[
                            Text(
                              account?.username ?? tr('登录账号'),
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
                              syncing
                                  ? tr('同步中…')
                                  : account == null
                                  ? tr('登录以同步地址簿')
                                  : tr('账号地址簿已连接'),
                              style: mutedStyle(context, size: 9.5),
                            ),
                          ],
                        ),
                      ),
                      if (syncing)
                        const SizedBox(
                          width: 13,
                          height: 13,
                          child: CircularProgressIndicator(
                            strokeWidth: 1.6,
                            color: kAccent,
                          ),
                        )
                      else
                        Icon(
                          Icons.sync_rounded,
                          size: 14,
                          color: dark
                              ? Colors.white38
                              : const Color(0xFF9AA0A6),
                        ),
                    ],
                  ),
                ),
              ),
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
                    child: const Icon(
                      Icons.bolt_rounded,
                      color: Colors.white,
                      size: 18,
                    ),
                  ),
                  const SizedBox(height: 10),
                  Text(
                    tr('直连优先'),
                    style: TextStyle(
                      color: Colors.white,
                      fontSize: 14,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                  const SizedBox(height: 3),
                  Text(
                    tr('打洞失败自动中继\n端到端加密'),
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
            PopupMenuButton<String>(
              key: const Key('language-menu'),
              tooltip: tr('界面语言'),
              initialValue: AppLanguage.code,
              onSelected: AppLanguage.set,
              itemBuilder: (BuildContext context) => <PopupMenuEntry<String>>[
                CheckedPopupMenuItem<String>(
                  value: kLanguageChinese,
                  checked: AppLanguage.code == kLanguageChinese,
                  child: Text(tr('简体中文')),
                ),
                CheckedPopupMenuItem<String>(
                  value: kLanguageEnglish,
                  checked: AppLanguage.code == kLanguageEnglish,
                  child: const Text('English'),
                ),
              ],
              child: Padding(
                padding: const EdgeInsets.symmetric(vertical: 6),
                child: Row(
                  children: <Widget>[
                    Icon(
                      Icons.translate_rounded,
                      size: 14,
                      color: dark ? Colors.white38 : const Color(0xFF8A919C),
                    ),
                    const SizedBox(width: 6),
                    Expanded(
                      child: Text(
                        tr('界面语言'),
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: mutedStyle(context, size: 10),
                      ),
                    ),
                    Text(
                      AppLanguage.label,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: mutedStyle(context, size: 10),
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 2),
            Row(
              children: <Widget>[
                Icon(
                  Icons.circle,
                  size: 7,
                  color: dark ? Colors.white24 : const Color(0xFFB6BCC6),
                ),
                const SizedBox(width: 6),
                Expanded(
                  child: Text(
                    'chast $chastVersion · Windows',
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

/// A local rename wins; otherwise the operator's alias, then the raw host id.
String _deviceTitle(Device device, DevicePresence presence) {
  if (device.name.isNotEmpty) {
    return device.name;
  }
  final String alias = presence.info?.alias ?? '';
  return alias.isEmpty ? device.id : alias;
}

String _deviceTags(Device device, DevicePresence presence) {
  final List<String> tags = device.tags.isNotEmpty
      ? device.tags
      : presence.info?.tags ?? const <String>[];
  return tags.map((String tag) => '#$tag').join(' ');
}

class _UpdateBanner extends StatelessWidget {
  const _UpdateBanner({
    required this.update,
    required this.current,
    required this.onDetails,
    required this.onDismiss,
  });

  final SoftwareInfo update;
  final String current;
  final VoidCallback onDetails;
  final VoidCallback onDismiss;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    return Container(
      padding: const EdgeInsets.fromLTRB(14, 8, 6, 8),
      decoration: BoxDecoration(
        color: kAccent.withValues(alpha: 0.08),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: kAccent.withValues(alpha: 0.28)),
      ),
      child: Row(
        children: <Widget>[
          const Icon(Icons.system_update_alt_rounded, size: 18, color: kAccent),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(
                  tr('查看器可更新到 {p19}', <String, Object?>{'p19': update.version}),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    fontSize: 12.5,
                    fontWeight: FontWeight.w600,
                    color: theme.colorScheme.onSurface,
                  ),
                ),
                const SizedBox(height: 2),
                Text(
                  tr('当前 {current} · 由连接服务通告', <String, Object?>{'current': current}),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: mutedStyle(context, size: 10.5),
                ),
              ],
            ),
          ),
          TextButton(
            onPressed: onDetails,
            style: TextButton.styleFrom(foregroundColor: kAccent),
            child: Text(tr('下载地址'), style: TextStyle(fontSize: 11.5)),
          ),
          IconButton(
            tooltip: tr('忽略此版本'),
            onPressed: onDismiss,
            icon: Icon(
              Icons.close_rounded,
              size: 16,
              color: dark ? Colors.white38 : const Color(0xFF9AA0A6),
            ),
          ),
        ],
      ),
    );
  }
}

class _LabelsDialog extends StatefulWidget {
  const _LabelsDialog({
    required this.server,
    required this.hostId,
    required this.alias,
    required this.tags,
    required this.updater,
  });

  final ServerSettings server;
  final String hostId;
  final String alias;
  final List<String> tags;
  final LabelsUpdater updater;

  @override
  State<_LabelsDialog> createState() => _LabelsDialogState();
}

class _LabelsDialogState extends State<_LabelsDialog> {
  static final RegExp _separators = RegExp(r'[,，]');

  late final TextEditingController _alias;
  late final TextEditingController _tags;
  String? _error;
  bool _saving = false;

  @override
  void initState() {
    super.initState();
    _alias = TextEditingController(text: widget.alias);
    _tags = TextEditingController(text: widget.tags.join(', '));
  }

  @override
  void dispose() {
    _alias.dispose();
    _tags.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    if (_saving) {
      return;
    }
    final String alias = _alias.text.trim();
    if (alias.runes.length > 128) {
      setState(() => _error = tr('别名最多 128 个字符'));
      return;
    }
    final List<String> tags = <String>[];
    final Set<String> seen = <String>{};
    for (final String raw in _tags.text.split(_separators)) {
      final String tag = raw.trim();
      if (tag.isEmpty || !seen.add(tag)) {
        continue;
      }
      if (tag.runes.length > 32) {
        setState(() => _error = tr('单个标签最多 32 个字符：{tag}', <String, Object?>{'tag': tag}));
        return;
      }
      tags.add(tag);
    }
    if (tags.length > 16) {
      setState(() => _error = tr('最多 16 个标签'));
      return;
    }
    setState(() {
      _saving = true;
      _error = null;
    });
    try {
      await widget.updater(
        widget.server,
        widget.hostId,
        alias: alias,
        tags: tags,
      );
    } catch (error) {
      if (mounted) {
        setState(() {
          _saving = false;
          _error = '$error';
        });
      }
      return;
    }
    if (mounted) {
      Navigator.of(context).pop(true);
    }
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      backgroundColor: Theme.of(context).colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: Text(tr('服务端别名与标签')),
      content: SizedBox(
        width: 420,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            TextField(
              key: const Key('labels-alias'),
              controller: _alias,
              autofocus: true,
              enabled: !_saving,
              maxLength: 128,
              decoration: InputDecoration(
                labelText: tr('别名'),
                hintText: tr('例如：设计室'),
                counterText: '',
              ),
            ),
            const SizedBox(height: 12),
            TextField(
              key: const Key('labels-tags'),
              controller: _tags,
              enabled: !_saving,
              decoration: InputDecoration(
                labelText: tr('标签'),
                hintText: tr('用逗号分隔，例如：office, design'),
              ),
            ),
            const SizedBox(height: 10),
            Text(
              tr('标签最多 16 个、每个 32 个字符；留空可清除。这些标签保存在连接服务上，本机重命名仍优先显示。'),
              style: mutedStyle(context, size: 11),
            ),
            if (_error != null) ...<Widget>[
              const SizedBox(height: 8),
              Text(
                _error!,
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
          onPressed: _saving ? null : () => Navigator.of(context).pop(false),
          child: Text(tr('取消')),
        ),
        FilledButton(
          key: const Key('labels-save'),
          onPressed: _saving ? null : _save,
          child: _saving
              ? const SizedBox(
                  width: 16,
                  height: 16,
                  child: CircularProgressIndicator(strokeWidth: 2),
                )
              : Text(tr('保存')),
        ),
      ],
    );
  }
}

/// Edits the viewer's account tags for one address-book entry.
///
/// Tags live in the account address book, not on the host, so they are
/// personal to the signed-in user and sync across that user's viewers.
class _TagsDialog extends StatefulWidget {
  const _TagsDialog({required this.tags});

  final List<String> tags;

  @override
  State<_TagsDialog> createState() => _TagsDialogState();
}

class _TagsDialogState extends State<_TagsDialog> {
  static final RegExp _separators = RegExp(r'[,，]');

  late final TextEditingController _tags;
  String? _error;

  @override
  void initState() {
    super.initState();
    _tags = TextEditingController(text: widget.tags.join(', '));
  }

  @override
  void dispose() {
    _tags.dispose();
    super.dispose();
  }

  void _save() {
    final List<String> tags = <String>[];
    final Set<String> seen = <String>{};
    for (final String raw in _tags.text.split(_separators)) {
      final String tag = raw.trim();
      if (tag.isEmpty || !seen.add(tag)) {
        continue;
      }
      if (tag.runes.length > 32) {
        setState(() => _error = tr('单个标签最多 32 个字符：{tag}', <String, Object?>{'tag': tag}));
        return;
      }
      tags.add(tag);
      if (tags.length > 16) {
        setState(() => _error = tr('最多 16 个标签'));
        return;
      }
    }
    Navigator.of(context).pop(tags);
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      backgroundColor: Theme.of(context).colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: Text(tr('账号标签')),
      content: SizedBox(
        width: 420,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            TextField(
              key: const Key('account-tags'),
              controller: _tags,
              autofocus: true,
              decoration: InputDecoration(
                labelText: tr('标签'),
                hintText: tr('用逗号分隔，例如：office, design'),
              ),
              onSubmitted: (_) => _save(),
            ),
            const SizedBox(height: 10),
            Text(
              tr('标签最多 16 个、每个 32 个字符；留空可清除。这些标签保存在账号地址簿，登录同一账号的其他查看器会同步。'),
              style: mutedStyle(context, size: 11),
            ),
            if (_error != null) ...<Widget>[
              const SizedBox(height: 8),
              Text(
                _error!,
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
          child: Text(tr('取消')),
        ),
        FilledButton(
          key: const Key('account-tags-save'),
          onPressed: _save,
          child: Text(tr('保存')),
        ),
      ],
    );
  }
}

/// Picks a group for one saved device; the empty id means ungrouped.
class _GroupPickerDialog extends StatelessWidget {
  const _GroupPickerDialog({required this.groups, required this.selected});

  final List<AddressBookGroup> groups;
  final String selected;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    Widget tile({
      required Key key,
      required String label,
      required String value,
      int depth = 0,
    }) {
      final bool active = value == selected;
      return ListTile(
        key: key,
        dense: true,
        contentPadding: EdgeInsets.only(left: 16 + depth * 18.0, right: 16),
        leading: Icon(
          value.isEmpty
              ? Icons.inbox_outlined
              : depth == 0
                  ? Icons.folder_outlined
                  : Icons.subdirectory_arrow_right_rounded,
          size: 18,
          color: active
              ? kAccent
              : theme.colorScheme.onSurface.withValues(alpha: 0.6),
        ),
        title: Text(
          label,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: const TextStyle(fontSize: 13),
        ),
        trailing: active
            ? Icon(Icons.check_rounded, size: 18, color: kAccent)
            : null,
        onTap: () => Navigator.of(context).pop(value),
      );
    }

    final List<AddressBookGroupNode> nodes = addressBookGroupTreeOrder(groups);
    return AlertDialog(
      backgroundColor: theme.colorScheme.surface,
      title: Text(tr('移动到分组')),
      content: SizedBox(
        width: 320,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            tile(key: const Key('group-pick-none'), label: tr('未分组'), value: ''),
            for (final AddressBookGroupNode node in nodes)
              tile(
                key: Key('group-pick-${node.group.id}'),
                label: node.group.name,
                value: node.group.id,
                depth: node.depth,
              ),
            if (nodes.isEmpty)
              Padding(
                padding: const EdgeInsets.only(top: 10),
                child: Text(
                  tr('还没有分组，可在“我的设备”标题栏的“分组”里创建。'),
                  style: mutedStyle(context, size: 11),
                ),
              ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
      ],
    );
  }
}

class _WakeDialog extends StatefulWidget {
  const _WakeDialog({required this.device});

  final Device device;

  @override
  State<_WakeDialog> createState() => _WakeDialogState();
}

class _WakeDialogState extends State<_WakeDialog> {
  late final TextEditingController _mac = TextEditingController(
    text: widget.device.mac,
  );
  String? _error;

  @override
  void dispose() {
    _mac.dispose();
    super.dispose();
  }

  void _submit() {
    final String? mac = normalizeMac(_mac.text);
    if (mac == null) {
      setState(() => _error = tr('请输入 12 位十六进制 MAC，例如 AA:BB:CC:DD:EE:FF'));
      return;
    }
    Navigator.of(context).pop(mac);
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      backgroundColor: Theme.of(context).colorScheme.surface,
      title: Text(tr('局域网唤醒')),
      content: SizedBox(
        width: 380,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text(
              tr('向“{p21}”发送 Wake-on-LAN 魔术包。查看器必须与目标处于同一局域网，目标需启用网络唤醒。', <String, Object?>{'p21': widget.device.displayName}),
              style: mutedStyle(context, size: 12),
            ),
            const SizedBox(height: 14),
            TextField(
              key: const Key('wake-mac'),
              controller: _mac,
              autofocus: true,
              decoration: InputDecoration(
                labelText: tr('MAC 地址'),
                hintText: 'AA:BB:CC:DD:EE:FF',
                errorText: _error,
              ),
              onSubmitted: (_) => _submit(),
            ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
        FilledButton(onPressed: _submit, child: Text(tr('唤醒'))),
      ],
    );
  }
}

class _DeviceCard extends StatelessWidget {
  const _DeviceCard({
    required this.device,
    required this.presence,
    required this.pinned,
    required this.onTap,
    required this.onFilesOnly,
    required this.onRename,
    required this.onWake,
    required this.onDelete,
    this.onLabels,
    this.onTags,
    this.onGroup,
    this.onMoveUp,
    this.onMoveDown,
  });

  final Device device;
  final DevicePresence presence;
  final bool pinned;
  final VoidCallback onTap;
  final VoidCallback onFilesOnly;
  final VoidCallback onRename;
  final VoidCallback onWake;
  final VoidCallback onDelete;
  final VoidCallback? onLabels;
  final VoidCallback? onTags;
  final VoidCallback? onGroup;
  final VoidCallback? onMoveUp;
  final VoidCallback? onMoveDown;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    final Color subtle = dark ? Colors.white12 : const Color(0xFFE4E6EB);
    final String title = _deviceTitle(device, presence);
    final String tags = _deviceTags(device, presence);
    final List<String> detail = <String>[
      if (title != device.id) device.id,
      if (tags.isNotEmpty) tags else if (device.viaServer) device.server,
    ];
    final String detailText = detail.isEmpty
        ? device.detail
        : detail.join(' · ');
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
                              title,
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
                            Icon(
                              Icons.verified_user_outlined,
                              size: 12,
                              color: kAccent,
                            ),
                          ],
                        ],
                      ),
                      const SizedBox(height: 4),
                      Text(
                        detailText,
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
                  tooltip: tr('设备操作'),
                  icon: Icon(
                    Icons.more_vert_rounded,
                    size: 17,
                    color: dark ? Colors.white38 : const Color(0xFF9AA0A6),
                  ),
                  onSelected: (String value) {
                    if (value == 'files') {
                      onFilesOnly();
                    } else if (value == 'rename') {
                      onRename();
                    } else if (value == 'wake') {
                      onWake();
                    } else if (value == 'labels') {
                      onLabels?.call();
                    } else if (value == 'tags') {
                      onTags?.call();
                    } else if (value == 'group') {
                      onGroup?.call();
                    } else if (value == 'move-up') {
                      onMoveUp?.call();
                    } else if (value == 'move-down') {
                      onMoveDown?.call();
                    } else if (value == 'delete') {
                      onDelete();
                    }
                  },
                  itemBuilder: (BuildContext context) =>
                      <PopupMenuEntry<String>>[
                        PopupMenuItem<String>(
                          value: 'files',
                          child: Text(tr('仅文件传输')),
                        ),
                        PopupMenuItem<String>(
                          value: 'rename',
                          child: Text(tr('重命名')),
                        ),
                        PopupMenuItem<String>(
                          value: 'wake',
                          child: Text(tr('唤醒')),
                        ),
                        if (onLabels != null)
                          PopupMenuItem<String>(
                            value: 'labels',
                            child: Text(tr('服务端别名/标签')),
                          ),
                        if (onTags != null)
                          PopupMenuItem<String>(
                            value: 'tags',
                            child: Text(tr('账号标签')),
                          ),
                        if (onGroup != null)
                          PopupMenuItem<String>(
                            value: 'group',
                            child: Text(tr('移动到分组')),
                          ),
                        if (onMoveUp != null)
                          PopupMenuItem<String>(
                            value: 'move-up',
                            child: Text(tr('上移')),
                          ),
                        if (onMoveDown != null)
                          PopupMenuItem<String>(
                            value: 'move-down',
                            child: Text(tr('下移')),
                          ),
                        PopupMenuItem<String>(
                          value: 'delete',
                          child: Text(tr('删除')),
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

  Widget _presenceLine(
    BuildContext context,
    DevicePresence presence,
    Device device,
  ) {
    final bool dark = Theme.of(context).brightness == Brightness.dark;
    Color color;
    String text;
    if (!device.viaServer) {
      color = dark ? Colors.white30 : const Color(0xFFA8AEB8);
      text = tr('直连地址');
    } else {
      switch (presence.state) {
        case HostPresence.online:
          color = const Color(0xFF4CAF50);
          final List<String> parts = <String>[tr('在线')];
          if (presence.info?.hasDirectCandidate ?? false) {
            parts.add(tr('可直连'));
          }
          if (presence.info?.hasRelay ?? false) {
            parts.add(tr('可中继'));
          }
          text = parts.join(' · ');
        case HostPresence.offline:
          color = const Color(0xFF9E9E9E);
          text = tr('离线');
        case HostPresence.unreachable:
          color = const Color(0xFFFFB74D);
          text = tr('服务不可达');
        case HostPresence.checking:
          color = kAccent;
          text = tr('查询中');
        case HostPresence.unknown:
          color = const Color(0xFF9E9E9E);
          text = tr('等待查询');
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
    final String title = _deviceTitle(device, presence);
    final String tags = _deviceTags(device, presence);
    final String subtitle = <String>[
      if (device.viaServer && title != device.id) device.id else device.detail,
      if (tags.isNotEmpty) tags,
    ].join(' · ');
    final String presenceText = !device.viaServer
        ? _formatTime(device.lastUsed!)
        : switch (presence.state) {
            HostPresence.online => tr('在线 · {p22}', <String, Object?>{'p22': _formatTime(device.lastUsed!)}),
            HostPresence.offline => tr('离线'),
            HostPresence.unreachable => tr('服务不可达'),
            HostPresence.checking => tr('查询中'),
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
                    device.viaServer
                        ? Icons.dns_outlined
                        : Icons.desktop_windows_rounded,
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
                        title,
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
                        subtitle,
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
                  tooltip: tr('保存到我的设备'),
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

class _LanScanDialog extends StatefulWidget {
  const _LanScanDialog({required this.discovery, required this.server});

  final LanDiscovery discovery;
  final ServerSettings server;

  @override
  State<_LanScanDialog> createState() => _LanScanDialogState();
}

class _LanScanDialogState extends State<_LanScanDialog> {
  List<LanPeer> _peers = <LanPeer>[];
  final Set<String> _selected = <String>{};
  bool _scanning = true;
  String _error = '';

  @override
  void initState() {
    super.initState();
    unawaited(_scan());
  }

  Future<void> _scan() async {
    setState(() {
      _scanning = true;
      _error = '';
    });
    try {
      final List<LanPeer> peers = await widget.discovery.scan();
      if (!mounted) {
        return;
      }
      setState(() {
        _peers = peers;
        _selected
          ..clear()
          ..addAll(peers.map((LanPeer peer) => peer.id));
        _scanning = false;
      });
    } on LanDiscoveryException catch (error) {
      if (!mounted) {
        return;
      }
      setState(() {
        _error = error.message;
        _scanning = false;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final bool canAdd = widget.server.configured;
    return AlertDialog(
      backgroundColor: Theme.of(context).colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: Text(tr('扫描局域网')),
      content: SizedBox(
        width: 460,
        height: 320,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            if (!canAdd)
              Padding(
                padding: const EdgeInsets.only(bottom: 8),
                child: Text(
                  tr('未配置连接服务，发现的 ID 不能保存为可连接设备。'),
                  style: TextStyle(
                    color: Theme.of(context).colorScheme.error,
                    fontSize: 11,
                  ),
                ),
              ),
            Expanded(child: _buildBody(context)),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          key: const Key('lan-scan-again'),
          onPressed: _scanning ? null : _scan,
          child: Text(tr('重新扫描')),
        ),
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
        FilledButton(
          key: const Key('lan-add'),
          onPressed: !canAdd || _selected.isEmpty
              ? null
              : () => Navigator.of(context).pop(
                  _peers
                      .where((LanPeer peer) => _selected.contains(peer.id))
                      .toList(),
                ),
          child: Text(_selected.isEmpty ? tr('添加') : tr('添加 {p23} 台', <String, Object?>{'p23': _selected.length})),
        ),
      ],
    );
  }

  Widget _buildBody(BuildContext context) {
    if (_scanning) {
      return const Center(
        child: SizedBox(
          width: 20,
          height: 20,
          child: CircularProgressIndicator(strokeWidth: 2, color: kAccent),
        ),
      );
    }
    if (_error.isNotEmpty) {
      return Center(
        child: Text(
          _error,
          textAlign: TextAlign.center,
          style: TextStyle(
            color: Theme.of(context).colorScheme.error,
            fontSize: 12,
          ),
        ),
      );
    }
    if (_peers.isEmpty) {
      return Center(
        child: Text(
          tr('没有发现局域网主机\n确认主机正在运行、允许局域网发现，并放行 UDP 21129。'),
          textAlign: TextAlign.center,
          style: mutedStyle(context, size: 12),
        ),
      );
    }
    return ListView.builder(
      itemCount: _peers.length,
      itemBuilder: (BuildContext context, int index) {
        final LanPeer peer = _peers[index];
        return CheckboxListTile(
          key: Key('lan-peer-${peer.id}'),
          value: _selected.contains(peer.id),
          dense: true,
          controlAffinity: ListTileControlAffinity.leading,
          onChanged: (bool? value) {
            setState(() {
              if (value == true) {
                _selected.add(peer.id);
              } else {
                _selected.remove(peer.id);
              }
            });
          },
          title: Text(
            peer.label,
            style: const TextStyle(fontSize: 13, fontWeight: FontWeight.w600),
          ),
          subtitle: Text(
            <String>[peer.detail, if (peer.mac.isNotEmpty) peer.mac].join('\n'),
            style: mutedStyle(context, size: 11),
          ),
          isThreeLine: peer.mac.isNotEmpty,
        );
      },
    );
  }
}

class _AddDeviceDialog extends StatefulWidget {
  const _AddDeviceDialog({required this.server});

  final ServerSettings server;

  @override
  State<_AddDeviceDialog> createState() => _AddDeviceDialogState();
}

class _AddDeviceDialogState extends State<_AddDeviceDialog> {
  final TextEditingController _name = TextEditingController();
  final TextEditingController _target = TextEditingController();
  String _error = '';

  @override
  void dispose() {
    _name.dispose();
    _target.dispose();
    super.dispose();
  }

  void _save() {
    final String text = _target.text.trim();
    if (text.isEmpty) {
      setState(() => _error = tr('请输入设备 ID 或地址'));
      return;
    }
    if (!text.startsWith('[') &&
        !text.contains(':') &&
        !widget.server.configured) {
      setState(() => _error = tr('设备 ID 需要先配置连接服务'));
      return;
    }
    final ConnectTarget? parsed = parseConnectInput(
      text,
      token: '',
      server: widget.server,
    );
    if (parsed == null) {
      setState(() => _error = tr('格式无效'));
      return;
    }
    final Device created = parsed.viaServer
        ? Device(
            id: parsed.hostId,
            name: _name.text.trim(),
            server: parsed.server,
            addedAt: DateTime.now(),
          )
        : Device(
            id: parsed.label,
            name: _name.text.trim(),
            addedAt: DateTime.now(),
          );
    Navigator.of(context).pop(created);
  }

  @override
  Widget build(BuildContext context) {
    final String service = widget.server.configured
        ? widget.server.url
        : tr('未配置连接服务');
    return AlertDialog(
      backgroundColor: Theme.of(context).colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: Text(tr('添加设备')),
      content: SizedBox(
        width: 420,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            TextField(
              controller: _name,
              decoration: InputDecoration(
                labelText: tr('名称（可选）'),
                hintText: tr('例如：开发机'),
              ),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _target,
              autofocus: true,
              decoration: InputDecoration(
                labelText: tr('设备 ID 或地址'),
                hintText: tr('devbox 或 192.168.1.10:4433'),
              ),
            ),
            const SizedBox(height: 10),
            Text(
              tr('设备 ID 通过 {service} 解析；带 : 的地址按直连保存。', <String, Object?>{'service': service}),
              style: mutedStyle(context, size: 11),
            ),
            if (_error.isNotEmpty) ...<Widget>[
              const SizedBox(height: 8),
              Text(
                _error,
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
          child: Text(tr('取消')),
        ),
        FilledButton(onPressed: _save, child: Text(tr('保存'))),
      ],
    );
  }
}

class _RenameDialog extends StatefulWidget {
  const _RenameDialog({required this.device});

  final Device device;

  @override
  State<_RenameDialog> createState() => _RenameDialogState();
}

class _RenameDialogState extends State<_RenameDialog> {
  late final TextEditingController _name = TextEditingController(
    text: widget.device.name,
  );

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      backgroundColor: Theme.of(context).colorScheme.surface,
      title: Text(tr('重命名设备')),
      content: TextField(
        controller: _name,
        autofocus: true,
        decoration: InputDecoration(hintText: widget.device.id),
        onSubmitted: (String value) => Navigator.of(context).pop(value),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
        FilledButton(
          onPressed: () => Navigator.of(context).pop(_name.text),
          child: Text(tr('保存')),
        ),
      ],
    );
  }
}

class _AccountResult {
  const _AccountResult({
    this.session,
    this.sync = false,
    this.logout = false,
    this.sharedPeer,
  });

  final AccountSession? session;
  final bool sync;
  final bool logout;
  final AddressBookPeer? sharedPeer;
}

class _AccountDialog extends StatefulWidget {
  const _AccountDialog({
    required this.server,
    required this.account,
    required this.login,
    required this.requestLoginSMS,
    required this.requestLoginEmail,
    required this.securityKeyLogin,
    required this.securityKeyPasswordlessLogin,
    required this.syncing,
    this.groups = const <AddressBookGroup>[],
    this.uploadRecordings = false,
    this.onUploadRecordingsChanged,
  });

  final ServerSettings server;
  final AccountSession? account;
  final AccountLogin login;
  final LoginSMSRequester requestLoginSMS;
  final LoginEmailRequester requestLoginEmail;
  final AccountSecurityKeyLogin securityKeyLogin;
  final AccountPasswordlessSecurityKeyLogin securityKeyPasswordlessLogin;
  final bool syncing;
  final List<AddressBookGroup> groups;
  final bool uploadRecordings;
  final Future<void> Function(bool enabled)? onUploadRecordingsChanged;

  @override
  State<_AccountDialog> createState() => _AccountDialogState();
}

class _AccountDialogState extends State<_AccountDialog> {
  final TextEditingController _username = TextEditingController();
  final TextEditingController _password = TextEditingController();
  final TextEditingController _totp = TextEditingController();
  final TextEditingController _recovery = TextEditingController();
  final TextEditingController _sms = TextEditingController();
  final TextEditingController _email = TextEditingController();
  bool _busy = false;
  bool _needsTotp = false;
  bool _useRecovery = false;
  bool _useSms = false;
  bool _useEmail = false;
  bool _sendingSms = false;
  bool _sendingEmail = false;
  late bool _uploadRecordings;
  String _error = '';
  String _notice = '';

  @override
  void initState() {
    super.initState();
    _uploadRecordings = widget.uploadRecordings;
  }

  @override
  void dispose() {
    _username.dispose();
    _password.dispose();
    _totp.dispose();
    _recovery.dispose();
    _sms.dispose();
    _email.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    final String username = _username.text.trim();
    final String password = _password.text;
    final String totp = _totp.text.trim();
    final String recovery = _recovery.text.trim();
    final String sms = _sms.text.trim();
    final String email = _email.text.trim();
    if (username.isEmpty || password.isEmpty) {
      setState(() => _error = tr('请输入用户名和密码'));
      return;
    }
    if (_useRecovery && recovery.isEmpty) {
      setState(() => _error = tr('请输入恢复码'));
      return;
    }
    if (_useSms && sms.isEmpty) {
      setState(() => _error = tr('请输入短信验证码'));
      return;
    }
    if (_useEmail && email.isEmpty) {
      setState(() => _error = tr('请输入邮件验证码'));
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
    });
    try {
      final AccountLoginResult result = await widget.login(
        widget.server,
        username: username,
        password: password,
        totpCode: _useRecovery || _useSms || _useEmail ? '' : totp,
        recoveryCode: _useRecovery ? recovery : '',
        smsCode: _useSms ? sms : '',
        emailCode: _useEmail ? email : '',
      );
      _finishLogin(username, result);
    } on ServerApiException catch (error) {
      if (mounted) {
        final bool twoFactor =
            error.code == 'totp_required' || error.code == 'totp_invalid';
        setState(() {
          _busy = false;
          _needsTotp = _needsTotp || twoFactor;
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = tr('登录失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  void _finishLogin(String fallbackUsername, AccountLoginResult result) {
    if (!mounted) {
      return;
    }
    final DateTime issued = DateTime.now().toUtc();
    Navigator.of(context).pop(
      _AccountResult(
        session: AccountSession(
          server: widget.server.url,
          username: result.username.isEmpty ? fallbackUsername : result.username,
          token: result.token,
          expiresAt: issued.add(Duration(milliseconds: result.expiresInMs)),
          refreshToken: result.refreshToken,
          refreshExpiresAt: result.refreshToken.isEmpty
              ? null
              : issued.add(Duration(milliseconds: result.refreshExpiresInMs)),
        ),
      ),
    );
  }

  Future<void> _submitSecurityKey() async {
    final String username = _username.text.trim();
    final String password = _password.text;
    if (username.isEmpty || password.isEmpty) {
      setState(() => _error = tr('请输入用户名和密码'));
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
      _notice = '';
    });
    try {
      final AccountLoginResult result = await widget.securityKeyLogin(
        widget.server,
        username: username,
        password: password,
      );
      _finishLogin(username, result);
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = error.message;
        });
      }
    } on SecurityKeyException catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = tr('安全密钥登录失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _submitPasswordlessSecurityKey() async {
    final String username = _username.text.trim();
    if (username.isEmpty) {
      setState(() => _error = tr('请输入用户名'));
      return;
    }
    setState(() {
      _busy = true;
      _error = '';
      _notice = '';
    });
    try {
      final AccountLoginResult result = await widget
          .securityKeyPasswordlessLogin(widget.server, username: username);
      _finishLogin(username, result);
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = error.message;
        });
      }
    } on SecurityKeyException catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = tr('安全密钥登录失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _sendLoginSMS() async {
    final String username = _username.text.trim();
    final String password = _password.text;
    if (username.isEmpty || password.isEmpty) {
      setState(() => _error = tr('请输入用户名和密码'));
      return;
    }
    setState(() {
      _sendingSms = true;
      _error = '';
      _notice = '';
    });
    try {
      await widget.requestLoginSMS(
        widget.server,
        username: username,
        password: password,
      );
      if (mounted) {
        setState(() {
          _sendingSms = false;
          _notice = tr('短信验证码已发送到绑定的手机号');
        });
      }
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _sendingSms = false;
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _sendingSms = false;
          _error = tr('发送短信验证码失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _sendLoginEmail() async {
    final String username = _username.text.trim();
    final String password = _password.text;
    if (username.isEmpty || password.isEmpty) {
      setState(() => _error = tr('请输入用户名和密码'));
      return;
    }
    setState(() {
      _sendingEmail = true;
      _error = '';
      _notice = '';
    });
    try {
      await widget.requestLoginEmail(
        widget.server,
        username: username,
        password: password,
      );
      if (mounted) {
        setState(() {
          _sendingEmail = false;
          _notice = tr('邮件验证码已发送到绑定的邮箱');
        });
      }
    } on ServerApiException catch (error) {
      if (mounted) {
        setState(() {
          _sendingEmail = false;
          _error = error.message;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _sendingEmail = false;
          _error = tr('发送邮件验证码失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  void _useSmsFactor() {
    setState(() {
      _useSms = true;
      _useRecovery = false;
      _useEmail = false;
      _error = '';
      _notice = '';
    });
    _sendLoginSMS();
  }

  void _useEmailFactor() {
    setState(() {
      _useEmail = true;
      _useSms = false;
      _useRecovery = false;
      _error = '';
      _notice = '';
    });
    _sendLoginEmail();
  }

  void _useTotpFactor() {
    setState(() {
      _useSms = false;
      _useEmail = false;
      _useRecovery = false;
      _error = '';
    });
  }

  Future<void> _openTwoFactor(AccountSession account) {
    return showDialog<void>(
      context: context,
      builder: (BuildContext context) {
        return TwoFactorDialog(server: widget.server, account: account);
      },
    );
  }

  Future<void> _openSecurityKeys(AccountSession account) {
    return showDialog<void>(
      context: context,
      builder: (BuildContext context) {
        return SecurityKeyDialog(server: widget.server, account: account);
      },
    );
  }

  Future<void> _openSharedBooks(AccountSession account) async {
    final AddressBookPeer? peer = await showDialog<AddressBookPeer>(
      context: context,
      builder: (BuildContext context) {
        return SharedBooksDialog(
          server: widget.server,
          account: account,
          groups: widget.groups,
        );
      },
    );
    if (peer != null && mounted) {
      Navigator.of(context).pop(_AccountResult(sharedPeer: peer));
    }
  }

  Future<void> _openSessions(AccountSession account) {
    return showDialog<void>(
      context: context,
      builder: (BuildContext context) {
        return AccountSessionsDialog(server: widget.server, account: account);
      },
    );
  }

  Future<void> _openRecordings(AccountSession account) {
    return showDialog<void>(
      context: context,
      builder: (BuildContext context) {
        return RecordingArchiveDialog(
          server: widget.server,
          account: account,
        );
      },
    );
  }

  Future<void> _toggleUploadRecordings(bool enabled) async {
    final Future<void> Function(bool)? changed =
        widget.onUploadRecordingsChanged;
    if (changed == null) {
      return;
    }
    setState(() => _uploadRecordings = enabled);
    try {
      await changed(enabled);
    } catch (error) {
      if (mounted) {
        setState(() {
          _uploadRecordings = !enabled;
          _error = tr('保存录制上传设置失败：{error}', <String, Object?>{'error': error});
        });
      }
    }
  }

  Future<void> _openChangePassword(AccountSession account) async {
    final bool? changed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) {
        return ChangePasswordDialog(server: widget.server, account: account);
      },
    );
    if (changed == true && mounted) {
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(tr('密码已修改，其他设备已退出'))));
    }
  }

  Future<void> _openRecoveryEmail(AccountSession account) async {
    final String? email = await showDialog<String>(
      context: context,
      builder: (BuildContext context) {
        return RecoveryEmailDialog(server: widget.server, account: account);
      },
    );
    if (email != null && mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(email.isEmpty ? tr('找回邮箱已清除') : tr('找回邮箱已更新'))),
      );
    }
  }

  Future<void> _openRecoveryPhone(AccountSession account) async {
    final String? phone = await showDialog<String>(
      context: context,
      builder: (BuildContext context) {
        return RecoveryPhoneDialog(server: widget.server, account: account);
      },
    );
    if (phone != null && mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(phone.isEmpty ? tr('找回手机已清除') : tr('找回手机已更新'))),
      );
    }
  }

  Future<void> _openForgotPassword() async {
    final String? username = await showDialog<String>(
      context: context,
      builder: (BuildContext context) {
        return ForgotPasswordDialog(
          server: widget.server,
          initialUsername: _username.text.trim(),
        );
      },
    );
    if (username == null || !mounted) {
      return;
    }
    setState(() {
      _username.text = username;
      _password.clear();
      _totp.clear();
      _recovery.clear();
      _sms.clear();
      _email.clear();
      _needsTotp = false;
      _useRecovery = false;
      _useSms = false;
      _useEmail = false;
      _sendingSms = false;
      _sendingEmail = false;
      _error = '';
      _notice = tr('密码已重置，请用新密码登录');
    });
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final AccountSession? account = widget.account;
    return AlertDialog(
      backgroundColor: theme.colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: Text(account == null ? tr('登录账号') : tr('账号')),
      content: SizedBox(
        width: 380,
        child: account == null
            ? _loginForm(context)
            : _accountView(context, account),
      ),
      actions: account == null
          ? <Widget>[
              TextButton(
                onPressed: _busy ? null : () => Navigator.of(context).pop(),
                child: Text(tr('取消')),
              ),
              FilledButton(
                key: const Key('account-login'),
                onPressed: _busy ? null : _submit,
                child: _busy
                    ? const SizedBox(
                        width: 15,
                        height: 15,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : Text(tr('登录')),
              ),
            ]
          : <Widget>[
              TextButton(
                onPressed: () => Navigator.of(context).pop(),
                child: Text(tr('关闭')),
              ),
              TextButton(
                key: const Key('account-logout'),
                onPressed: () => Navigator.of(
                  context,
                ).pop(const _AccountResult(logout: true)),
                style: TextButton.styleFrom(
                  foregroundColor: theme.colorScheme.error,
                ),
                child: Text(tr('退出登录')),
              ),
              FilledButton(
                key: const Key('account-sync'),
                onPressed: widget.syncing
                    ? null
                    : () => Navigator.of(
                        context,
                      ).pop(const _AccountResult(sync: true)),
                child: Text(widget.syncing ? tr('同步中…') : tr('立即同步')),
              ),
            ],
    );
  }

  Widget _loginForm(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        Text(
          tr('账号地址簿保存在连接服务上，登录后可在多台查看器之间同步设备。'),
          style: mutedStyle(context, size: 12),
        ),
        const SizedBox(height: 16),
        TextField(
          key: const Key('account-username'),
          controller: _username,
          autofocus: true,
          autocorrect: false,
          enableSuggestions: false,
          decoration: InputDecoration(
            labelText: tr('用户名'),
            prefixIcon: Icon(Icons.person_outline_rounded, size: 18),
          ),
        ),
        const SizedBox(height: 12),
        TextField(
          key: const Key('account-password'),
          controller: _password,
          obscureText: true,
          autocorrect: false,
          enableSuggestions: false,
          decoration: InputDecoration(
            labelText: tr('密码'),
            prefixIcon: Icon(Icons.lock_outline_rounded, size: 18),
          ),
          onSubmitted: (_) => _submit(),
        ),
        Align(
          alignment: Alignment.centerLeft,
          child: Wrap(
            spacing: 8,
            children: <Widget>[
              TextButton(
                key: const Key('account-forgot-password'),
                onPressed: _busy ? null : _openForgotPassword,
                child: Text(tr('忘记密码？')),
              ),
              TextButton(
                key: const Key('account-security-key-login'),
                onPressed: _busy ? null : _submitPasswordlessSecurityKey,
                child: Text(tr('使用安全密钥登录')),
              ),
            ],
          ),
        ),
        if (_needsTotp) ...<Widget>[
          const SizedBox(height: 12),
          if (_useRecovery)
            TextField(
              key: const Key('account-recovery-code'),
              controller: _recovery,
              autofocus: true,
              autocorrect: false,
              enableSuggestions: false,
              decoration: InputDecoration(
                labelText: tr('恢复码'),
                hintText: 'ABCD-EFGH-IJKL-MNOP',
                prefixIcon: Icon(Icons.key_outlined, size: 18),
              ),
              onSubmitted: (_) => _submit(),
            )
          else if (_useSms)
            TextField(
              key: const Key('account-sms'),
              controller: _sms,
              autofocus: true,
              keyboardType: TextInputType.number,
              inputFormatters: <TextInputFormatter>[
                FilteringTextInputFormatter.digitsOnly,
                LengthLimitingTextInputFormatter(8),
              ],
              autocorrect: false,
              enableSuggestions: false,
              decoration: InputDecoration(
                labelText: tr('短信验证码'),
                hintText: tr('8 位数字'),
                prefixIcon: Icon(Icons.sms_outlined, size: 18),
              ),
              onSubmitted: (_) => _submit(),
            )
          else if (_useEmail)
            TextField(
              key: const Key('account-email-code'),
              controller: _email,
              autofocus: true,
              keyboardType: TextInputType.number,
              inputFormatters: <TextInputFormatter>[
                FilteringTextInputFormatter.digitsOnly,
                LengthLimitingTextInputFormatter(8),
              ],
              autocorrect: false,
              enableSuggestions: false,
              decoration: InputDecoration(
                labelText: tr('邮件验证码'),
                hintText: tr('8 位数字'),
                prefixIcon: Icon(Icons.mail_outlined, size: 18),
              ),
              onSubmitted: (_) => _submit(),
            )
          else
            TextField(
              key: const Key('account-totp'),
              controller: _totp,
              autofocus: true,
              keyboardType: TextInputType.number,
              maxLength: 6,
              autocorrect: false,
              enableSuggestions: false,
              decoration: InputDecoration(
                labelText: tr('两步验证码'),
                counterText: '',
                prefixIcon: Icon(Icons.shield_outlined, size: 18),
              ),
              onSubmitted: (_) => _submit(),
            ),
          Wrap(
            spacing: 8,
            children: <Widget>[
              if (_useRecovery || _useSms || _useEmail)
                TextButton(
                  key: const Key('account-use-totp'),
                  onPressed: _busy ? null : _useTotpFactor,
                  child: Text(tr('使用身份验证器验证码')),
                )
              else
                TextButton(
                  key: const Key('account-use-recovery'),
                  onPressed: _busy
                      ? null
                      : () => setState(() {
                          _useRecovery = true;
                          _useSms = false;
                          _useEmail = false;
                          _error = '';
                        }),
                  child: Text(tr('验证器丢失？使用恢复码')),
                ),
              if (_useSms)
                TextButton(
                  key: const Key('account-send-sms'),
                  onPressed: _busy || _sendingSms ? null : _sendLoginSMS,
                  child: Text(_sendingSms ? tr('发送中…') : tr('重新发送短信验证码')),
                )
              else
                TextButton(
                  key: const Key('account-use-sms'),
                  onPressed: _busy || _sendingSms ? null : _useSmsFactor,
                  child: Text(tr('使用短信验证码')),
                ),
              if (_useEmail)
                TextButton(
                  key: const Key('account-send-email'),
                  onPressed: _busy || _sendingEmail ? null : _sendLoginEmail,
                  child: Text(_sendingEmail ? tr('发送中…') : tr('重新发送邮件验证码')),
                )
              else
                TextButton(
                  key: const Key('account-use-email'),
                  onPressed: _busy || _sendingEmail ? null : _useEmailFactor,
                  child: Text(tr('使用邮件验证码')),
                ),
              TextButton(
                key: const Key('account-use-security-key'),
                onPressed: _busy ? null : _submitSecurityKey,
                child: Text(tr('使用安全密钥')),
              ),
            ],
          ),
        ],
        const SizedBox(height: 10),
        Text(
          widget.server.configured
              ? tr('登录到 {p25}', <String, Object?>{'p25': widget.server.url})
              : tr('请先在连接服务中填写服务地址'),
          style: mutedStyle(context, size: 10.5),
        ),
        if (_notice.isNotEmpty) ...<Widget>[
          const SizedBox(height: 10),
          Text(
            _notice,
            key: const Key('account-notice'),
            style: TextStyle(
              color: Theme.of(context).colorScheme.primary,
              fontSize: 12,
            ),
          ),
        ],
        if (_error.isNotEmpty) ...<Widget>[
          const SizedBox(height: 10),
          Text(
            _error,
            style: TextStyle(
              color: Theme.of(context).colorScheme.error,
              fontSize: 12,
            ),
          ),
        ],
      ],
    );
  }

  Widget _accountView(BuildContext context, AccountSession account) {
    final ThemeData theme = Theme.of(context);
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        Row(
          children: <Widget>[
            Container(
              width: 38,
              height: 38,
              decoration: BoxDecoration(
                color: kAccent.withValues(alpha: 0.10),
                borderRadius: BorderRadius.circular(10),
              ),
              child: const Icon(Icons.person_rounded, color: kAccent, size: 20),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  Text(
                    account.username,
                    key: const Key('account-status'),
                    style: TextStyle(
                      fontSize: 15,
                      fontWeight: FontWeight.w600,
                      color: theme.colorScheme.onSurface,
                    ),
                  ),
                  const SizedBox(height: 2),
                  Text(account.server, style: mutedStyle(context, size: 10.5)),
                ],
              ),
            ),
          ],
        ),
        const SizedBox(height: 16),
        Text(tr('账号地址簿会在此设备与其他已登录设备之间同步。'), style: mutedStyle(context, size: 12)),
        const SizedBox(height: 8),
        Text(
          tr('登录有效期至 {p26}', <String, Object?>{'p26': _formatTime(account.expiresAt.toLocal())}),
          style: mutedStyle(context, size: 11),
        ),
        const SizedBox(height: 4),
        Text(
          account.syncedAt == null
              ? tr('尚未同步')
              : tr('上次同步：{p27}', <String, Object?>{'p27': _formatTime(account.syncedAt!.toLocal())}),
          style: mutedStyle(context, size: 11),
        ),
        const SizedBox(height: 14),
        OutlinedButton.icon(
          key: const Key('account-shared-books'),
          onPressed: _busy ? null : () => _openSharedBooks(account),
          icon: const Icon(Icons.folder_shared_outlined, size: 17),
          label: Text(tr('共享地址簿')),
        ),
        const SizedBox(height: 8),
        OutlinedButton.icon(
          key: const Key('account-sessions'),
          onPressed: _busy ? null : () => _openSessions(account),
          icon: const Icon(Icons.devices_outlined, size: 17),
          label: Text(tr('设备会话')),
        ),
        const SizedBox(height: 8),
        OutlinedButton.icon(
          key: const Key('account-recordings'),
          onPressed: _busy ? null : () => _openRecordings(account),
          icon: const Icon(Icons.cloud_download_outlined, size: 17),
          label: Text(tr('云端录制')),
        ),
        SwitchListTile(
          key: const Key('account-upload-recordings'),
          contentPadding: EdgeInsets.zero,
          value: _uploadRecordings,
          onChanged: _busy || widget.onUploadRecordingsChanged == null
              ? null
              : _toggleUploadRecordings,
          title: Text(tr('自动上传录制'), style: const TextStyle(fontSize: 13.5)),
          subtitle: Text(
            tr('会话录制结束后，把 MP4 上传到当前账号，可在“云端录制”中下载或删除。'),
            style: mutedStyle(context, size: 10.5),
          ),
        ),
        const SizedBox(height: 4),
        OutlinedButton.icon(
          key: const Key('account-change-password'),
          onPressed: _busy ? null : () => _openChangePassword(account),
          icon: const Icon(Icons.password_rounded, size: 17),
          label: Text(tr('修改密码')),
        ),
        const SizedBox(height: 8),
        OutlinedButton.icon(
          key: const Key('account-recovery-email'),
          onPressed: _busy ? null : () => _openRecoveryEmail(account),
          icon: const Icon(Icons.alternate_email_rounded, size: 17),
          label: Text(tr('找回邮箱')),
        ),
        const SizedBox(height: 8),
        OutlinedButton.icon(
          key: const Key('account-recovery-phone'),
          onPressed: _busy ? null : () => _openRecoveryPhone(account),
          icon: const Icon(Icons.smartphone_rounded, size: 17),
          label: Text(tr('找回手机')),
        ),
        const SizedBox(height: 8),
        OutlinedButton.icon(
          key: const Key('account-two-factor'),
          onPressed: _busy ? null : () => _openTwoFactor(account),
          icon: const Icon(Icons.shield_outlined, size: 17),
          label: Text(tr('两步验证')),
        ),
        const SizedBox(height: 8),
        OutlinedButton.icon(
          key: const Key('account-security-keys'),
          onPressed: _busy ? null : () => _openSecurityKeys(account),
          icon: const Icon(Icons.fingerprint_rounded, size: 17),
          label: Text(tr('安全密钥（Windows Hello）')),
        ),
      ],
    );
  }
}

class _ServerDialog extends StatefulWidget {
  const _ServerDialog({required this.initial, required this.identityStore});

  final ServerSettings initial;
  final ServerIdentityStore identityStore;

  @override
  State<_ServerDialog> createState() => _ServerDialogState();
}

class _ServerDialogState extends State<_ServerDialog> {
  late final TextEditingController _url;
  late final TextEditingController _key;
  late TunnelMode _mode;
  bool _testing = false;
  bool _showKey = false;
  String _status = '';
  bool _statusOk = false;
  ServerIdentityPin? _pin;

  @override
  void initState() {
    super.initState();
    _url = TextEditingController(text: widget.initial.url);
    _key = TextEditingController(text: widget.initial.key);
    _mode = widget.initial.mode;
    _url.addListener(_loadPin);
    unawaited(_loadPin());
  }

  @override
  void dispose() {
    _url.removeListener(_loadPin);
    _url.dispose();
    _key.dispose();
    super.dispose();
  }

  Future<void> _loadPin() async {
    final String url = normalizeServerUrl(_url.text);
    final ServerIdentityPin? pin = url.isEmpty
        ? null
        : await widget.identityStore.pinFor(url);
    if (mounted) {
      setState(() => _pin = pin);
    }
  }

  Future<void> _forgetPin() async {
    await widget.identityStore.forget(normalizeServerUrl(_url.text));
    await _loadPin();
  }

  Future<void> _test() async {
    final String url = normalizeServerUrl(_url.text);
    if (url.isEmpty) {
      setState(() {
        _status = tr('请输入有效的 http(s) 地址');
        _statusOk = false;
      });
      return;
    }
    setState(() {
      _testing = true;
      _status = tr('正在检查…');
      _statusOk = false;
    });
    try {
      await ServerApi.check(
        ServerSettings(url: url, mode: _mode, key: _key.text.trim()),
      );
      if (mounted) {
        setState(() {
          _status = tr('服务可用，密钥有效');
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
        _status = tr('地址无效：示例 http://192.168.1.10:8443');
        _statusOk = false;
      });
      return;
    }
    Navigator.of(
      context,
    ).pop(ServerSettings(url: url, mode: _mode, key: _key.text.trim()));
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      backgroundColor: Theme.of(context).colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: Text(tr('连接服务')),
      content: SizedBox(
        width: 420,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            TextField(
              controller: _url,
              autofocus: true,
              decoration: InputDecoration(
                labelText: tr('服务地址'),
                hintText: 'http://127.0.0.1:8443',
              ),
              onSubmitted: (_) => _save(),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _key,
              obscureText: !_showKey,
              autocorrect: false,
              enableSuggestions: false,
              decoration: InputDecoration(
                labelText: tr('服务端密钥（可选）'),
                hintText: tr('与 --server-key 相同'),
                suffixIcon: IconButton(
                  tooltip: _showKey ? tr('隐藏密钥') : tr('显示密钥'),
                  icon: Icon(
                    _showKey ? Icons.visibility_off : Icons.visibility,
                  ),
                  onPressed: () => setState(() => _showKey = !_showKey),
                ),
              ),
              onSubmitted: (_) => _save(),
            ),
            const SizedBox(height: 12),
            DropdownButtonFormField<TunnelMode>(
              initialValue: _mode,
              decoration: InputDecoration(labelText: tr('路径策略')),
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
                  child: Text(_testing ? tr('检查中…') : tr('测试连接')),
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
                          : (_statusOk
                                ? const Color(0xFF4CAF50)
                                : Theme.of(context).colorScheme.error),
                    ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 8),
            Row(
              children: <Widget>[
                Icon(
                  _pin == null
                      ? Icons.verified_user_outlined
                      : Icons.verified_user,
                  size: 15,
                  color: _pin == null
                      ? mutedStyle(context).color
                      : const Color(0xFF4CAF50),
                ),
                const SizedBox(width: 6),
                Expanded(
                  child: Text(
                    _pin == null
                        ? tr('服务器身份尚未固定：首次连接时会展示指纹')
                        : tr('服务器身份 {p28}（已固定）', <String, Object?>{'p28': shortenServerFingerprint(_pin!.identity.fingerprint)}),
                    overflow: TextOverflow.ellipsis,
                    style: mutedStyle(context, size: 11),
                  ),
                ),
                if (_pin != null)
                  TextButton(
                    key: const Key('server-identity-forget'),
                    onPressed: _forgetPin,
                    child: Text(tr('清除固定')),
                  ),
              ],
            ),
            Text(
              tr('服务端配置 --server-key 后，查看者解析设备 ID 和打洞也要带同一密钥；留空表示服务端未启用密钥。留空服务地址即可关闭设备 ID 解析。'),
              style: mutedStyle(context, size: 10.5),
            ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(tr('取消')),
        ),
        FilledButton(onPressed: _save, child: Text(tr('保存'))),
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
    return tr('今天 {hm}', <String, Object?>{'hm': hm});
  }
  if (today.difference(day).inDays == 1) {
    return tr('昨天 {hm}', <String, Object?>{'hm': hm});
  }
  return '${time.month.toString().padLeft(2, '0')}-${time.day.toString().padLeft(2, '0')} $hm';
}
