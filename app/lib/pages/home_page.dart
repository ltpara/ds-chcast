import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../account_store.dart';
import '../address_book_sync.dart';
import '../app_info.dart';
import '../device_store.dart';
import '../server_api.dart';
import '../server_settings.dart';
import '../target.dart';
import '../theme.dart';
import '../trust_store.dart';
import '../wake_on_lan.dart';
import 'session_page.dart';
import 'groups_dialog.dart';
import 'account_sessions_dialog.dart';
import 'change_password_dialog.dart';
import 'shared_books_dialog.dart';
import 'two_factor_dialog.dart';

typedef HostLookup = Future<BatchLookupResult> Function(
    ServerSettings server, List<String> hostIds);
typedef LabelsUpdater = Future<void> Function(
  ServerSettings server,
  String hostId, {
  required String alias,
  required List<String> tags,
});
typedef AccountLogin = Future<AccountLoginResult> Function(
  ServerSettings server, {
  required String username,
  required String password,
  String totpCode,
  String recoveryCode,
});
typedef AccountLogout = Future<void> Function(
    ServerSettings server, String token);
typedef AddressBookFetcher = Future<AddressBookSnapshot> Function(
    ServerSettings server, String token, int since);
typedef AddressBookPeerWriter = Future<AddressBookPeer> Function(
  ServerSettings server,
  String token,
  String peerId, {
  String? name,
  List<String>? tags,
  String? groupId,
  int? ifRevision,
});
typedef AddressBookPeerDeleter = Future<void> Function(
    ServerSettings server, String token, String peerId);

class HomePage extends StatefulWidget {
  const HomePage({
    super.key,
    this.initialTarget,
    this.deviceStore,
    this.serverStore,
    this.trustStore,
    this.accountStore,
    this.lookupBatch,
    this.labelsUpdater,
    this.accountLogin,
    this.accountLogout,
    this.addressBookFetcher,
    this.addressBookPeerWriter,
    this.addressBookPeerDeleter,
    this.wakeSender,
  });

  final String? initialTarget;

  /// Test seams. Product code uses the files under %APPDATA%\chast.
  final DeviceStore? deviceStore;
  final ServerSettingsStore? serverStore;
  final TrustStore? trustStore;
  final AccountSessionStore? accountStore;
  final HostLookup? lookupBatch;
  final LabelsUpdater? labelsUpdater;
  final AccountLogin? accountLogin;
  final AccountLogout? accountLogout;
  final AddressBookFetcher? addressBookFetcher;
  final AddressBookPeerWriter? addressBookPeerWriter;
  final AddressBookPeerDeleter? addressBookPeerDeleter;
  final WakeSender? wakeSender;

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
  late final AccountSessionStore _accountStore;
  late final HostLookup _lookupBatch;
  late final LabelsUpdater _updateLabels;
  late final AccountLogin _loginAccount;
  late final AccountLogout _logoutAccount;
  late final AddressBookFetcher _fetchAddressBook;
  late final AddressBookPeerWriter _writeAddressBookPeer;
  late final AddressBookPeerDeleter _deleteAddressBookPeer;
  late final WakeSender _wakeSender;

  List<Device> _devices = <Device>[];
  List<AddressBookGroup> _groups = <AddressBookGroup>[];
  final Set<String> _collapsedGroups = <String>{};
  bool _groupsSynced = false;
  ServerSettings _server = const ServerSettings();
  Map<String, String> _trust = <String, String>{};
  AccountSession? _account;
  bool _syncing = false;
  final Map<String, DevicePresence> _presence = <String, DevicePresence>{};
  SoftwareInfo? _software;
  String? _softwareDismissed;
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
    _accountStore = widget.accountStore ?? AccountSessionStore();
    _lookupBatch = widget.lookupBatch ?? ServerApi.lookupBatch;
    _wakeSender = widget.wakeSender ?? WakeOnLan.send;
    _updateLabels = widget.labelsUpdater ?? ServerApi.updateLabels;
    _loginAccount = widget.accountLogin ??
        (ServerSettings server,
                {required String username,
                required String password,
                String totpCode = '',
                String recoveryCode = ''}) =>
            ServerApi.login(
              server,
              username: username,
              password: password,
              totpCode: totpCode,
              recoveryCode: recoveryCode,
              deviceName: _hostname,
            );
    _logoutAccount = widget.accountLogout ??
        (ServerSettings server, String token) => ServerApi.logout(server, token);
    _fetchAddressBook = widget.addressBookFetcher ??
        (ServerSettings server, String token, int since) =>
            ServerApi.addressBook(server, token, since: since);
    _writeAddressBookPeer = widget.addressBookPeerWriter ??
        (ServerSettings server, String token, String peerId,
            {String? name,
            List<String>? tags,
            String? groupId,
            int? ifRevision}) {
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
    _deleteAddressBookPeer = widget.addressBookPeerDeleter ??
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
  }

  @override
  void dispose() {
    _presenceTimer?.cancel();
    _address.dispose();
    _token.dispose();
    _deviceSearch.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    final List<Device> devices = await _deviceStore.load();
    final ServerSettings server = await _serverStore.load();
    final Map<String, String> trust = await _trustStore.load();
    final AccountSession? storedAccount = await _accountStore.load();
    AccountSession? account;
    if (storedAccount != null) {
      if (!storedAccount.isActiveAt(DateTime.now()) ||
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
      _account = account;
      _loading = false;
    });
    unawaited(_refreshPresence());
    if (account != null) {
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
        serverKey: _server.key,
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
        return SessionPage(target: target, account: _account);
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
        _showSnack('已发送局域网唤醒包');
      }
    } catch (error) {
      if (mounted) {
        _showSnack('唤醒包发送失败：$error');
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
    final List<Device> devices = await _deviceStore.updateTags(device.key, tags);
    if (!mounted) {
      return;
    }
    setState(() => _devices = devices);
    final Device updated = devices.firstWhere(
      (Device item) => item.key == device.key,
      orElse: () => device.copyWith(tags: tags),
    );
    _showSnack('账号标签已保存');
    unawaited(_pushAccountPeer(updated, syncTags: true));
  }

  Future<void> _moveDeviceToGroup(Device device) async {
    final AccountSession? account = _account;
    if (account == null || account.server != _server.url) {
      _showSnack('请先登录账号');
      return;
    }
    final String? choice = await showDialog<String>(
      context: context,
      builder: (BuildContext context) => _GroupPickerDialog(
        groups: _groups,
        selected: device.groupId,
      ),
    );
    if (choice == null || !mounted) {
      return;
    }
    final List<Device> devices =
        await _deviceStore.updateGroup(device.key, choice);
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
      List<Device> settled =
          await _deviceStore.setPeerRevision(device.key, written.revision);
      if (choice.isEmpty) {
        settled = await _deviceStore.markPushSettled(device.key, group: true);
      }
      if (mounted) {
        setState(() => _devices = settled);
      }
      if (mounted) {
        _showSnack(choice.isEmpty ? '已移到未分组' : '已移动到分组');
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
        _showSnack('分组已保存在本机，将在下次同步上传');
      }
    } catch (_) {
      if (mounted) {
        _showSnack('分组已保存在本机，将在下次同步上传');
      }
    }
  }

  Future<void> _manageGroups() async {
    final AccountSession? account = _account;
    if (account == null || account.server != _server.url) {
      _showSnack('请先登录账号');
      return;
    }
    final Map<String, int> counts = <String, int>{};
    for (final Device device in _devices) {
      if (device.saved && device.server == _server.url) {
        counts[device.groupId] = (counts[device.groupId] ?? 0) + 1;
      }
    }
    final bool changed = await showDialog<bool>(
          context: context,
          builder: (BuildContext context) => GroupsDialog(
            server: _server,
            token: account.token,
            groups: _groups,
            peerCounts: counts,
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
    final AccountSession? account = _account;
    final bool tracked = device.saved &&
        device.viaServer &&
        device.server == _server.url &&
        account != null &&
        account.server == _server.url &&
        account.isActiveAt(DateTime.now());
    if (tracked) {
      // Queue first so an offline delete is retried instead of being pulled
      // back as a server-only peer on the next sync.
      await _deviceStore.queueDeletion(PendingDeletion(
        server: account.server,
        username: account.username,
        id: device.id,
        deletedAt: DateTime.now(),
      ));
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
    final bool saved = await showDialog<bool>(
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
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('服务端别名与标签已保存')),
    );
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
          syncing: _syncing,
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
      await _connectDevice(Device(
        id: peer.id,
        name: peer.name,
        server: _server.url,
        addedAt: DateTime.now(),
      ));
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
    _showSnack('已退出账号');
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
        _showSnack('请先登录账号');
      }
      return;
    }
    if (!account.isActiveAt(DateTime.now())) {
      await _expireAccount();
      if (announce) {
        _showSnack('账号登录已过期，请重新登录');
      }
      return;
    }
    if (_syncing || account.server != _server.url) {
      return;
    }
    setState(() => _syncing = true);
    try {
      final List<PendingDeletion> queued =
          await _deviceStore.loadPendingDeletions();
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
              _showSnack('账号登录已过期，请重新登录');
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
      final AddressBookSnapshot snapshot =
          await _fetchAddressBook(_server, account.token, since);
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
              _showSnack('账号登录已过期，请重新登录');
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
      final Map<String, AddressBookPeer> refreshedById = <String, AddressBookPeer>{
        if (refreshed != null)
          for (final AddressBookPeer peer in refreshed.peers) peer.id: peer,
      };
      final List<Device> merged = <Device>[];
      for (final Device item in plan.devices) {
        final bool conflicted = refreshed != null &&
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
        merged.add(item.copyWith(
          name: peer.name,
          tags: peer.tags,
          groupId: peer.groupId,
          revision: peer.revision,
          tagsCleared: false,
          groupCleared: false,
          saved: true,
        ));
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
        _showSnack(_syncSummary(
          plan,
          failed,
          pendingDeletions.length,
          conflicts.length,
        ));
      }
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.unauthorized) {
        await _expireAccount();
        if (announce) {
          _showSnack('账号登录已过期，请重新登录');
        }
      } else if (announce) {
        _showSnack('地址簿同步失败：${error.message}');
      }
    } catch (error) {
      if (announce) {
        _showSnack('地址簿同步失败：$error');
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
      if (plan.added > 0) '新增 ${plan.added}',
      if (plan.updated > 0) '更新 ${plan.updated}',
      if (plan.removed > 0) '移除 ${plan.removed}',
      if (uploaded > 0) '上传 $uploaded',
      if (conflicts > 0) '冲突 $conflicts 已刷新',
      if (failed > 0) '$failed 项上传失败',
      if (pendingDeletes > 0) '待删除重试 $pendingDeletes',
    ];
    return details.isEmpty ? '地址簿已同步' : '地址簿已同步（${details.join('，')}）';
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
        _showSnack('该条目已在其他设备修改，请重新同步');
      }
      return;
    }
    try {
      final AddressBookSnapshot snapshot =
          await _fetchAddressBook(_server, account.token, 0);
      final Map<String, AddressBookPeer> byId = <String, AddressBookPeer>{
        for (final AddressBookPeer peer in snapshot.peers) peer.id: peer,
      };
      final List<Device> current = await _deviceStore.load();
      final List<Device> next = <Device>[];
      for (final Device item in current) {
        if (item.id != peerId ||
            !item.saved ||
            item.server != _server.url) {
          next.add(item);
          continue;
        }
        final AddressBookPeer? peer = byId[item.id];
        if (peer == null) {
          continue;
        }
        next.add(item.copyWith(
          name: peer.name,
          tags: peer.tags,
          groupId: peer.groupId,
          revision: peer.revision,
          tagsCleared: false,
          groupCleared: false,
          saved: true,
        ));
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
      _showSnack('该条目已在其他设备修改，已刷新为服务器版本');
    } catch (_) {
      if (mounted) {
        _showSnack('该条目已在其他设备修改，请重新同步');
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
    final String? name =
        device.name.isNotEmpty || allowEmptyName ? device.name : null;
    // A rename must not clear tags it never touched; an explicit tag edit
    // sends the list even when empty so clearing propagates.
    final List<String>? tags =
        device.tags.isNotEmpty || syncTags ? device.tags : null;
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
      List<Device> settled =
          await _deviceStore.setPeerRevision(device.key, written.revision);
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

  Future<void> _showUpdate(SoftwareInfo update) async {
    final bool? copy = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) {
        return AlertDialog(
          backgroundColor: Theme.of(context).colorScheme.surface,
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
          title: const Text('发现新版本'),
          content: SizedBox(
            width: 420,
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text('当前版本 $chastVersion，连接服务提供 ${update.version}。'),
                const SizedBox(height: 12),
                SelectableText(
                  update.url,
                  style: const TextStyle(fontSize: 12),
                ),
                const SizedBox(height: 10),
                Text(
                  '更新由运营者发布，查看器不会自动下载或安装。',
                  style: mutedStyle(context, size: 10.5),
                ),
              ],
            ),
          ),
          actions: <Widget>[
            TextButton(
              onPressed: () => Navigator.of(context).pop(false),
              child: const Text('关闭'),
            ),
            FilledButton(
              onPressed: () => Navigator.of(context).pop(true),
              child: const Text('复制下载地址'),
            ),
          ],
        );
      },
    );
    if (copy != true) {
      return;
    }
    await Clipboard.setData(ClipboardData(text: update.url));
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('下载地址已复制')),
      );
    }
  }

  Future<void> _showAbout() async {
    await showDialog<void>(
      context: context,
      builder: (BuildContext context) {
        return AlertDialog(
          backgroundColor: Theme.of(context).colorScheme.surface,
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
          title: const Text('关于 Chast'),
          content: Text(
            'Chast $chastVersion\n\n'
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

  /// Tags shown on the device card: stored account tags win over the host's
  /// self-reported tags, matching what the user can actually see.
  List<String> _visibleTags(Device device) {
    final List<String> tags = device.tags.isNotEmpty
        ? device.tags
        : _presence[device.key]?.info?.tags ?? const <String>[];
    return Device.cleanTags(tags);
  }

  List<String> _availableFilterTags(List<Device> saved) {
    final List<String> tags = <String>{
      for (final Device device in saved) ..._visibleTags(device),
    }.toList()
      ..sort((String a, String b) => a.toLowerCase().compareTo(b.toLowerCase()));
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
                      '我的设备',
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
                        child: const Text('清除筛选',
                            style: TextStyle(fontSize: 11)),
                      ),
                    const Spacer(),
                    if (_account != null && _account!.server == _server.url)
                      TextButton.icon(
                        key: const Key('manage-groups'),
                        onPressed: _manageGroups,
                        style: TextButton.styleFrom(foregroundColor: kAccent),
                        icon: const Icon(Icons.folder_outlined, size: 16),
                        label: const Text('分组',
                            style: TextStyle(fontSize: 12)),
                      ),
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
                else ...<Widget>[
                  _buildDeviceFilter(context, saved, activeTags),
                  const SizedBox(height: 12),
                  if (filtered.isEmpty)
                    _emptyPanel(
                      context,
                      icon: Icons.search_off_rounded,
                      text: '没有匹配的设备',
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
                        child: const Text('清除筛选',
                            style: TextStyle(fontSize: 12)),
                      ),
                    )
                  else
                    _buildSavedDevices(context, filtered),
                ],
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

  Widget _buildDeviceFilter(
    BuildContext context,
    List<Device> saved,
    Set<String> activeTags,
  ) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    final Color subtle = dark ? Colors.white12 : const Color(0xFFE4E6EB);
    final Color fieldFill = dark ? const Color(0xFF1F2026) : const Color(0xFFF7F8FA);
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
            hintText: '搜索名称、ID 或标签',
            hintStyle: TextStyle(
              fontSize: 12,
              color: dark ? Colors.white38 : const Color(0xFF9AA0A6),
            ),
            prefixIcon: Icon(
              Icons.search_rounded,
              size: 16,
              color: dark ? Colors.white54 : const Color(0xFF7C8390),
            ),
            prefixIconConstraints:
                const BoxConstraints(minWidth: 34, minHeight: 34),
            suffixIcon: _deviceQuery.isEmpty
                ? null
                : IconButton(
                    key: const Key('device-search-clear'),
                    tooltip: '清除搜索',
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
            suffixIconConstraints:
                const BoxConstraints(minWidth: 34, minHeight: 34),
            filled: true,
            fillColor: fieldFill,
            contentPadding:
                const EdgeInsets.symmetric(horizontal: 10, vertical: 10),
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
                label: '全部',
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
    final Map<String, AddressBookGroup> groupsById = <String, AddressBookGroup>{
      if (accountReady)
        for (final AddressBookGroup group in _groups) group.id: group,
    };
    final Map<String, List<Device>> grouped = <String, List<Device>>{
      for (final String id in groupsById.keys) id: <Device>[],
    };
    final List<Device> ungrouped = <Device>[];
    for (final Device device in saved) {
      final bool inGroup = device.viaServer &&
          device.server == _server.url &&
          groupsById.containsKey(device.groupId);
      if (inGroup) {
        grouped[device.groupId]!.add(device);
      } else {
        ungrouped.add(device);
      }
    }
    final List<AddressBookGroup> visible = <AddressBookGroup>[
      for (final AddressBookGroup group in _groups)
        if (groupsById.containsKey(group.id) &&
            (grouped[group.id]?.isNotEmpty ?? false))
          group,
    ];
    if (visible.isEmpty) {
      return Wrap(
        spacing: 12,
        runSpacing: 12,
        children: <Widget>[for (final Device device in saved) _deviceCard(device)],
      );
    }
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        for (final AddressBookGroup group in visible) ...<Widget>[
          _groupHeader(
            context,
            key: Key('device-group-${group.id}'),
            label: group.name,
            count: grouped[group.id]!.length,
            collapsed: _collapsedGroups.contains(group.id),
            onToggle: () => setState(() {
              if (!_collapsedGroups.remove(group.id)) {
                _collapsedGroups.add(group.id);
              }
            }),
          ),
          if (!_collapsedGroups.contains(group.id))
            Padding(
              padding: const EdgeInsets.only(top: 8),
              child: Wrap(
                spacing: 12,
                runSpacing: 12,
                children: <Widget>[
                  for (final Device device in grouped[group.id]!)
                    _deviceCard(device),
                ],
              ),
            ),
          const SizedBox(height: 14),
        ],
        if (ungrouped.isNotEmpty) ...<Widget>[
          _groupHeader(
            context,
            key: const Key('device-group-ungrouped'),
            label: '未分组',
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

  Widget _deviceCard(Device device) {
    final bool accountReady = _account != null &&
        _account!.server == _server.url &&
        device.viaServer &&
        device.server == _server.url;
    return _DeviceCard(
      device: device,
      presence: _presence[device.key] ?? DevicePresence.unknown,
      pinned: _trust.containsKey(device.trustKey),
      onTap: () => _connectDevice(device),
      onRename: () => _renameDevice(device),
      onWake: () => _wakeDevice(device),
      onLabels: device.viaServer ? () => _editLabels(device) : null,
      onTags: accountReady ? () => _editTags(device) : null,
      onGroup: accountReady ? () => _moveDeviceToGroup(device) : null,
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
    bool ungrouped = false,
  }) {
    final ThemeData theme = Theme.of(context);
    return InkWell(
      key: key,
      borderRadius: BorderRadius.circular(8),
      onTap: onToggle,
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 4, horizontal: 2),
        child: Row(
          children: <Widget>[
            Icon(
              ungrouped ? Icons.inbox_outlined : Icons.folder_open_rounded,
              size: 16,
              color: ungrouped ? theme.colorScheme.onSurface.withValues(alpha: 0.5) : kAccent,
            ),
            const SizedBox(width: 7),
            Text(
              label.isEmpty ? '未命名分组' : label,
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
        border: Border.all(color: dark ? Colors.white12 : const Color(0xFFE4E6EB)),
      ),
      child: Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Icon(icon, size: 25, color: dark ? Colors.white30 : const Color(0xFFA8AEB8)),
            const SizedBox(height: 8),
            Text(text, style: mutedStyle(context)),
            if (action != null) ...<Widget>[
              const SizedBox(height: 4),
              action,
            ],
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
            const SizedBox(height: 20),
            Text(
              '账号',
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
                              account?.username ?? '登录账号',
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
                                  ? '同步中…'
                                  : account == null
                                      ? '登录以同步地址簿'
                                      : '账号地址簿已连接',
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
                          color: dark ? Colors.white38 : const Color(0xFF9AA0A6),
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
          const Icon(
            Icons.system_update_alt_rounded,
            size: 18,
            color: kAccent,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(
                  '查看器可更新到 ${update.version}',
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
                  '当前 $current · 由连接服务通告',
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
            child: const Text('下载地址', style: TextStyle(fontSize: 11.5)),
          ),
          IconButton(
            tooltip: '忽略此版本',
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
      setState(() => _error = '别名最多 128 个字符');
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
        setState(() => _error = '单个标签最多 32 个字符：$tag');
        return;
      }
      tags.add(tag);
    }
    if (tags.length > 16) {
      setState(() => _error = '最多 16 个标签');
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
      title: const Text('服务端别名与标签'),
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
              decoration: const InputDecoration(
                labelText: '别名',
                hintText: '例如：设计室',
                counterText: '',
              ),
            ),
            const SizedBox(height: 12),
            TextField(
              key: const Key('labels-tags'),
              controller: _tags,
              enabled: !_saving,
              decoration: const InputDecoration(
                labelText: '标签',
                hintText: '用逗号分隔，例如：office, design',
              ),
            ),
            const SizedBox(height: 10),
            Text(
              '标签最多 16 个、每个 32 个字符；留空可清除。'
              '这些标签保存在连接服务上，本机重命名仍优先显示。',
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
          child: const Text('取消'),
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
              : const Text('保存'),
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
        setState(() => _error = '单个标签最多 32 个字符：$tag');
        return;
      }
      tags.add(tag);
      if (tags.length > 16) {
        setState(() => _error = '最多 16 个标签');
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
      title: const Text('账号标签'),
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
              decoration: const InputDecoration(
                labelText: '标签',
                hintText: '用逗号分隔，例如：office, design',
              ),
              onSubmitted: (_) => _save(),
            ),
            const SizedBox(height: 10),
            Text(
              '标签最多 16 个、每个 32 个字符；留空可清除。'
              '这些标签保存在账号地址簿，登录同一账号的其他查看器会同步。',
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
          child: const Text('取消'),
        ),
        FilledButton(
          key: const Key('account-tags-save'),
          onPressed: _save,
          child: const Text('保存'),
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
    }) {
      final bool active = value == selected;
      return ListTile(
        key: key,
        dense: true,
        leading: Icon(
          value.isEmpty ? Icons.inbox_outlined : Icons.folder_outlined,
          size: 18,
          color: active ? kAccent : theme.colorScheme.onSurface.withValues(alpha: 0.6),
        ),
        title: Text(
          label,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: const TextStyle(fontSize: 13),
        ),
        trailing: active ? Icon(Icons.check_rounded, size: 18, color: kAccent) : null,
        onTap: () => Navigator.of(context).pop(value),
      );
    }

    return AlertDialog(
      backgroundColor: theme.colorScheme.surface,
      title: const Text('移动到分组'),
      content: SizedBox(
        width: 320,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            tile(key: const Key('group-pick-none'), label: '未分组', value: ''),
            for (final AddressBookGroup group in groups)
              tile(
                key: Key('group-pick-${group.id}'),
                label: group.name,
                value: group.id,
              ),
            if (groups.isEmpty)
              Padding(
                padding: const EdgeInsets.only(top: 10),
                child: Text(
                  '还没有分组，可在“我的设备”标题栏的“分组”里创建。',
                  style: mutedStyle(context, size: 11),
                ),
              ),
          ],
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('取消'),
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
  late final TextEditingController _mac =
      TextEditingController(text: widget.device.mac);
  String? _error;

  @override
  void dispose() {
    _mac.dispose();
    super.dispose();
  }

  void _submit() {
    final String? mac = normalizeMac(_mac.text);
    if (mac == null) {
      setState(() => _error = '请输入 12 位十六进制 MAC，例如 AA:BB:CC:DD:EE:FF');
      return;
    }
    Navigator.of(context).pop(mac);
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      backgroundColor: Theme.of(context).colorScheme.surface,
      title: const Text('局域网唤醒'),
      content: SizedBox(
        width: 380,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text(
              '向“${widget.device.displayName}”发送 Wake-on-LAN 魔术包。'
              '查看器必须与目标处于同一局域网，目标需启用网络唤醒。',
              style: mutedStyle(context, size: 12),
            ),
            const SizedBox(height: 14),
            TextField(
              key: const Key('wake-mac'),
              controller: _mac,
              autofocus: true,
              decoration: InputDecoration(
                labelText: 'MAC 地址',
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
          child: const Text('取消'),
        ),
        FilledButton(
          onPressed: _submit,
          child: const Text('唤醒'),
        ),
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
    required this.onRename,
    required this.onWake,
    required this.onDelete,
    this.onLabels,
    this.onTags,
    this.onGroup,
  });

  final Device device;
  final DevicePresence presence;
  final bool pinned;
  final VoidCallback onTap;
  final VoidCallback onRename;
  final VoidCallback onWake;
  final VoidCallback onDelete;
  final VoidCallback? onLabels;
  final VoidCallback? onTags;
  final VoidCallback? onGroup;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final bool dark = theme.brightness == Brightness.dark;
    final Color subtle = dark ? Colors.white12 : const Color(0xFFE4E6EB);
    final String title = _deviceTitle(device, presence);
    final String tags = _deviceTags(device, presence);
    final List<String> detail = <String>[
      if (title != device.id) device.id,
      if (tags.isNotEmpty)
        tags
      else if (device.viaServer)
        device.server,
    ];
    final String detailText = detail.isEmpty ? device.detail : detail.join(' · ');
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
                            Icon(Icons.verified_user_outlined, size: 12, color: kAccent),
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
                  tooltip: '设备操作',
                  icon: Icon(
                    Icons.more_vert_rounded,
                    size: 17,
                    color: dark ? Colors.white38 : const Color(0xFF9AA0A6),
                  ),
                  onSelected: (String value) {
                    if (value == 'rename') {
                      onRename();
                    } else if (value == 'wake') {
                      onWake();
                    } else if (value == 'labels') {
                      onLabels?.call();
                    } else if (value == 'tags') {
                      onTags?.call();
                    } else if (value == 'group') {
                      onGroup?.call();
                    } else if (value == 'delete') {
                      onDelete();
                    }
                  },
                  itemBuilder: (BuildContext context) => <PopupMenuEntry<String>>[
                    const PopupMenuItem<String>(value: 'rename', child: Text('重命名')),
                    const PopupMenuItem<String>(value: 'wake', child: Text('唤醒')),
                    if (onLabels != null)
                      const PopupMenuItem<String>(
                        value: 'labels',
                        child: Text('服务端别名/标签'),
                      ),
                    if (onTags != null)
                      const PopupMenuItem<String>(
                        value: 'tags',
                        child: Text('账号标签'),
                      ),
                    if (onGroup != null)
                      const PopupMenuItem<String>(
                        value: 'group',
                        child: Text('移动到分组'),
                      ),
                    const PopupMenuItem<String>(value: 'delete', child: Text('删除')),
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
    final String title = _deviceTitle(device, presence);
    final String tags = _deviceTags(device, presence);
    final String subtitle = <String>[
      if (device.viaServer && title != device.id) device.id else device.detail,
      if (tags.isNotEmpty) tags,
    ].join(' · ');
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
      setState(() => _error = '请输入设备 ID 或地址');
      return;
    }
    if (!text.startsWith('[') &&
        !text.contains(':') &&
        !widget.server.configured) {
      setState(() => _error = '设备 ID 需要先配置连接服务');
      return;
    }
    final ConnectTarget? parsed = parseConnectInput(
      text,
      token: '',
      server: widget.server,
    );
    if (parsed == null) {
      setState(() => _error = '格式无效');
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
    final String service =
        widget.server.configured ? widget.server.url : '未配置连接服务';
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
              controller: _name,
              decoration: const InputDecoration(
                labelText: '名称（可选）',
                hintText: '例如：开发机',
              ),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _target,
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
          child: const Text('取消'),
        ),
        FilledButton(onPressed: _save, child: const Text('保存')),
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
  late final TextEditingController _name =
      TextEditingController(text: widget.device.name);

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      backgroundColor: Theme.of(context).colorScheme.surface,
      title: const Text('重命名设备'),
      content: TextField(
        controller: _name,
        autofocus: true,
        decoration: InputDecoration(hintText: widget.device.id),
        onSubmitted: (String value) => Navigator.of(context).pop(value),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('取消'),
        ),
        FilledButton(
          onPressed: () => Navigator.of(context).pop(_name.text),
          child: const Text('保存'),
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
    required this.syncing,
  });

  final ServerSettings server;
  final AccountSession? account;
  final AccountLogin login;
  final bool syncing;

  @override
  State<_AccountDialog> createState() => _AccountDialogState();
}

class _AccountDialogState extends State<_AccountDialog> {
  final TextEditingController _username = TextEditingController();
  final TextEditingController _password = TextEditingController();
  final TextEditingController _totp = TextEditingController();
  final TextEditingController _recovery = TextEditingController();
  bool _busy = false;
  bool _needsTotp = false;
  bool _useRecovery = false;
  String _error = '';

  @override
  void dispose() {
    _username.dispose();
    _password.dispose();
    _totp.dispose();
    _recovery.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    final String username = _username.text.trim();
    final String password = _password.text;
    final String totp = _totp.text.trim();
    final String recovery = _recovery.text.trim();
    if (username.isEmpty || password.isEmpty) {
      setState(() => _error = '请输入用户名和密码');
      return;
    }
    if (_useRecovery && recovery.isEmpty) {
      setState(() => _error = '请输入恢复码');
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
        totpCode: _useRecovery ? '' : totp,
        recoveryCode: _useRecovery ? recovery : '',
      );
      if (!mounted) {
        return;
      }
      Navigator.of(context).pop(_AccountResult(
        session: AccountSession(
          server: widget.server.url,
          username: result.username.isEmpty ? username : result.username,
          token: result.token,
          expiresAt: DateTime.now()
              .toUtc()
              .add(Duration(milliseconds: result.expiresInMs)),
        ),
      ));
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
          _error = '登录失败：$error';
        });
      }
    }
  }

  Future<void> _openTwoFactor(AccountSession account) {
    return showDialog<void>(
      context: context,
      builder: (BuildContext context) {
        return TwoFactorDialog(server: widget.server, account: account);
      },
    );
  }

  Future<void> _openSharedBooks(AccountSession account) async {
    final AddressBookPeer? peer = await showDialog<AddressBookPeer>(
      context: context,
      builder: (BuildContext context) {
        return SharedBooksDialog(server: widget.server, account: account);
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

  Future<void> _openChangePassword(AccountSession account) async {
    final bool? changed = await showDialog<bool>(
      context: context,
      builder: (BuildContext context) {
        return ChangePasswordDialog(server: widget.server, account: account);
      },
    );
    if (changed == true && mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('密码已修改，其他设备已退出')),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final AccountSession? account = widget.account;
    return AlertDialog(
      backgroundColor: theme.colorScheme.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      title: Text(account == null ? '登录账号' : '账号'),
      content: SizedBox(
        width: 380,
        child: account == null ? _loginForm(context) : _accountView(context, account),
      ),
      actions: account == null
          ? <Widget>[
              TextButton(
                onPressed: _busy ? null : () => Navigator.of(context).pop(),
                child: const Text('取消'),
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
                    : const Text('登录'),
              ),
            ]
          : <Widget>[
              TextButton(
                onPressed: () => Navigator.of(context).pop(),
                child: const Text('关闭'),
              ),
              TextButton(
                key: const Key('account-logout'),
                onPressed: () => Navigator.of(context)
                    .pop(const _AccountResult(logout: true)),
                style: TextButton.styleFrom(
                  foregroundColor: theme.colorScheme.error,
                ),
                child: const Text('退出登录'),
              ),
              FilledButton(
                key: const Key('account-sync'),
                onPressed: widget.syncing
                    ? null
                    : () => Navigator.of(context)
                        .pop(const _AccountResult(sync: true)),
                child: Text(widget.syncing ? '同步中…' : '立即同步'),
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
          '账号地址簿保存在连接服务上，登录后可在多台查看器之间同步设备。',
          style: mutedStyle(context, size: 12),
        ),
        const SizedBox(height: 16),
        TextField(
          key: const Key('account-username'),
          controller: _username,
          autofocus: true,
          autocorrect: false,
          enableSuggestions: false,
          decoration: const InputDecoration(
            labelText: '用户名',
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
          decoration: const InputDecoration(
            labelText: '密码',
            prefixIcon: Icon(Icons.lock_outline_rounded, size: 18),
          ),
          onSubmitted: (_) => _submit(),
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
              decoration: const InputDecoration(
                labelText: '恢复码',
                hintText: 'ABCD-EFGH-IJKL-MNOP',
                prefixIcon: Icon(Icons.key_outlined, size: 18),
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
              decoration: const InputDecoration(
                labelText: '两步验证码',
                counterText: '',
                prefixIcon: Icon(Icons.shield_outlined, size: 18),
              ),
              onSubmitted: (_) => _submit(),
            ),
          Align(
            alignment: Alignment.centerLeft,
            child: TextButton(
              key: const Key('account-use-recovery'),
              onPressed: _busy
                  ? null
                  : () => setState(() {
                        _useRecovery = !_useRecovery;
                        _error = '';
                      }),
              child: Text(_useRecovery ? '使用身份验证器验证码' : '验证器丢失？使用恢复码'),
            ),
          ),
        ],
        const SizedBox(height: 10),
        Text(
          widget.server.configured
              ? '登录到 ${widget.server.url}'
              : '请先在连接服务中填写服务地址',
          style: mutedStyle(context, size: 10.5),
        ),
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
        Text(
          '账号地址簿会在此设备与其他已登录设备之间同步。',
          style: mutedStyle(context, size: 12),
        ),
        const SizedBox(height: 8),
        Text(
          '登录有效期至 ${_formatTime(account.expiresAt.toLocal())}',
          style: mutedStyle(context, size: 11),
        ),
        const SizedBox(height: 4),
        Text(
          account.syncedAt == null
              ? '尚未同步'
              : '上次同步：${_formatTime(account.syncedAt!.toLocal())}',
          style: mutedStyle(context, size: 11),
        ),
        const SizedBox(height: 14),
        OutlinedButton.icon(
          key: const Key('account-shared-books'),
          onPressed: _busy ? null : () => _openSharedBooks(account),
          icon: const Icon(Icons.folder_shared_outlined, size: 17),
          label: const Text('共享地址簿'),
        ),
        const SizedBox(height: 8),
        OutlinedButton.icon(
          key: const Key('account-sessions'),
          onPressed: _busy ? null : () => _openSessions(account),
          icon: const Icon(Icons.devices_outlined, size: 17),
          label: const Text('设备会话'),
        ),
        const SizedBox(height: 8),
        OutlinedButton.icon(
          key: const Key('account-change-password'),
          onPressed: _busy ? null : () => _openChangePassword(account),
          icon: const Icon(Icons.password_rounded, size: 17),
          label: const Text('修改密码'),
        ),
        const SizedBox(height: 8),
        OutlinedButton.icon(
          key: const Key('account-two-factor'),
          onPressed: _busy ? null : () => _openTwoFactor(account),
          icon: const Icon(Icons.shield_outlined, size: 17),
          label: const Text('两步验证'),
        ),
      ],
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
  late final TextEditingController _key;
  late TunnelMode _mode;
  bool _testing = false;
  bool _showKey = false;
  String _status = '';
  bool _statusOk = false;

  @override
  void initState() {
    super.initState();
    _url = TextEditingController(text: widget.initial.url);
    _key = TextEditingController(text: widget.initial.key);
    _mode = widget.initial.mode;
  }

  @override
  void dispose() {
    _url.dispose();
    _key.dispose();
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
      await ServerApi.check(ServerSettings(
        url: url,
        mode: _mode,
        key: _key.text.trim(),
      ));
      if (mounted) {
        setState(() {
          _status = '服务可用，密钥有效';
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
    Navigator.of(context).pop(ServerSettings(
      url: url,
      mode: _mode,
      key: _key.text.trim(),
    ));
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
            TextField(
              controller: _key,
              obscureText: !_showKey,
              autocorrect: false,
              enableSuggestions: false,
              decoration: InputDecoration(
                labelText: '服务端密钥（可选）',
                hintText: '与 --server-key 相同',
                suffixIcon: IconButton(
                  tooltip: _showKey ? '隐藏密钥' : '显示密钥',
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
              '服务端配置 --server-key 后，查看者解析设备 ID 和打洞也要带同一密钥；'
              '留空表示服务端未启用密钥。留空服务地址即可关闭设备 ID 解析。',
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
