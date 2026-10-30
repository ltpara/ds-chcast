import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'server_identity.dart';
import 'server_settings.dart';
import 'package:chast_viewer/l10n.dart';

/// One host as reported by `GET /v1/hosts/{id}`.
class HostInfo {
  const HostInfo({
    required this.id,
    required this.online,
    this.candidate = '',
    this.relay = '',
    this.punch = '',
    this.fingerprint = '',
    this.serverPublicKey = '',
    this.alias = '',
    this.tags = const <String>[],
    this.lastSeenMs = 0,
    this.expiresInMs = 0,
  });

  final String id;
  final bool online;
  final String candidate;
  final String relay;
  final String punch;
  final String fingerprint;

  /// The key of the server that answered this lookup. [ServerIdentityStore]
  /// compares it with the pinned key; an empty value on a pinned server is a
  /// mismatch, not a reason to skip the check.
  final String serverPublicKey;

  /// Operator-maintained display name and tags. They live on the server, so a
  /// viewer sees the same labels on every machine and a local rename still
  /// wins in the address book.
  final String alias;
  final List<String> tags;
  final int lastSeenMs;
  final int expiresInMs;

  bool get hasRelay => relay.isNotEmpty;
  bool get hasDirectCandidate => candidate.isNotEmpty;

  static HostInfo fromJson(Map<Object?, Object?> map, String fallbackId) {
    String text(Object? value) => value?.toString() ?? '';
    int number(Object? value) => (value as num?)?.toInt() ?? 0;
    final Object? rawTags = map['tags'];
    return HostInfo(
      id: text(map['id']).isEmpty ? fallbackId : text(map['id']),
      online: map['online'] == true,
      candidate: text(map['candidate']),
      relay: text(map['relay']),
      punch: text(map['punch']),
      fingerprint: text(map['fingerprint']),
      serverPublicKey: text(map['server_public_key']),
      alias: text(map['alias']),
      tags: <String>[
        if (rawTags is List)
          for (final Object? tag in rawTags)
            if (tag is String && tag.trim().isNotEmpty) tag.trim(),
      ],
      lastSeenMs: number(map['last_seen_ms']),
      expiresInMs: number(map['expires_in_ms']),
    );
  }
}

/// Server-side SoftwareUpdate equivalent: the newest viewer build and where
/// the operator publishes it. An incomplete pair is ignored rather than shown
/// as an unusable offer.
class SoftwareInfo {
  const SoftwareInfo({
    required this.version,
    required this.url,
    this.sha256 = '',
    this.size = 0,
    this.signature = '',
  });

  final String version;
  final String url;

  /// Present when the server hosts the file itself and signs the notice with
  /// its identity, which is what makes an in-place update verifiable.
  final String sha256;
  final int size;
  final String signature;

  bool get installable =>
      sha256.isNotEmpty && size > 0 && signature.isNotEmpty;

  static SoftwareInfo? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final String version = value['version']?.toString().trim() ?? '';
    final String url = value['url']?.toString().trim() ?? '';
    if (version.isEmpty || url.isEmpty) {
      return null;
    }
    final Object? rawSize = value['size'];
    return SoftwareInfo(
      version: version,
      url: url,
      sha256: value['sha256']?.toString().trim() ?? '',
      size: rawSize is num && rawSize > 0 ? rawSize.toInt() : 0,
      signature: value['signature']?.toString().trim() ?? '',
    );
  }
}

class BatchLookupResult {
  const BatchLookupResult({required this.hosts, this.software});

  final Map<String, HostInfo> hosts;
  final SoftwareInfo? software;
}

class AccountLoginResult {
  const AccountLoginResult({
    required this.token,
    required this.username,
    required this.expiresInMs,
    this.refreshToken = '',
    this.refreshExpiresInMs = 0,
  });

  final String token;
  final String username;
  final int expiresInMs;

  /// Rotates [token] without a password. Empty when the server still issues
  /// long-lived access tokens only; such a session simply ends at [expiresInMs].
  final String refreshToken;
  final int refreshExpiresInMs;
}

/// One server-side login session of the signed-in account. [id] is an opaque
/// public identifier: the bearer token itself is never sent back by the server.
class AccountDeviceSession {
  const AccountDeviceSession({
    required this.id,
    this.deviceId = '',
    this.deviceName = '',
    required this.createdAt,
    required this.expiresAt,
    this.current = false,
    this.loginIp = '',
    this.location = '',
  });

  final String id;
  final String deviceId;
  final String deviceName;
  final DateTime createdAt;
  final DateTime expiresAt;
  final bool current;
  final String loginIp;
  final String location;

  AccountDeviceSession copyWith({String? deviceName}) {
    return AccountDeviceSession(
      id: id,
      deviceId: deviceId,
      deviceName: deviceName ?? this.deviceName,
      createdAt: createdAt,
      expiresAt: expiresAt,
      current: current,
      loginIp: loginIp,
      location: location,
    );
  }

  static AccountDeviceSession? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final String id = value['id']?.toString().trim() ?? '';
    final Object? created = value['created_at_ms'];
    final Object? expires = value['expires_at_ms'];
    if (id.isEmpty || created is! num || expires is! num) {
      return null;
    }
    return AccountDeviceSession(
      id: id,
      deviceId: value['device_id']?.toString().trim() ?? '',
      deviceName: value['device_name']?.toString().trim() ?? '',
      createdAt: DateTime.fromMillisecondsSinceEpoch(
        created.toInt(),
        isUtc: true,
      ),
      expiresAt: DateTime.fromMillisecondsSinceEpoch(
        expires.toInt(),
        isUtc: true,
      ),
      current: value['current'] == true,
      loginIp: value['login_ip']?.toString().trim() ?? '',
      location: value['location']?.toString().trim() ?? '',
    );
  }
}

/// One viewer recording stored in the signed-in account's server archive.
/// The bytes live on the server; [sha256] lets a download be verified against
/// the digest computed while it was uploaded.
class RemoteRecording {
  const RemoteRecording({
    required this.id,
    required this.name,
    this.peerId = '',
    this.size = 0,
    this.sha256 = '',
    this.durationMs = 0,
    this.startedAtMs = 0,
    this.createdAtMs = 0,
  });

  final String id;
  final String name;
  final String peerId;
  final int size;
  final String sha256;
  final int durationMs;
  final int startedAtMs;
  final int createdAtMs;

  DateTime? get createdAt => createdAtMs > 0
      ? DateTime.fromMillisecondsSinceEpoch(createdAtMs, isUtc: true).toLocal()
      : null;

  static RemoteRecording? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final String id = value['id']?.toString().trim() ?? '';
    if (id.isEmpty) {
      return null;
    }
    int number(Object? raw) => raw is num ? raw.toInt() : 0;
    String text(Object? raw) => raw?.toString().trim() ?? '';
    return RemoteRecording(
      id: id,
      name: text(value['name']),
      peerId: text(value['peer_id']),
      size: number(value['size']),
      sha256: text(value['sha256']),
      durationMs: number(value['duration_ms']),
      startedAtMs: number(value['started_at_ms']),
      createdAtMs: number(value['created_at_ms']),
    );
  }
}

/// The signed-in account's recording archive plus the limits that apply to
/// the next upload. A zero quota means the server does not enforce one.
class RecordingArchive {
  const RecordingArchive({
    this.recordings = const <RemoteRecording>[],
    this.usageBytes = 0,
    this.quotaBytes = 0,
    this.maxUploadBytes = 0,
  });

  final List<RemoteRecording> recordings;
  final int usageBytes;
  final int quotaBytes;
  final int maxUploadBytes;

  static RecordingArchive? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final Object? raw = value['recordings'];
    if (raw is! List) {
      return null;
    }
    final List<RemoteRecording> recordings = <RemoteRecording>[];
    for (final Object? item in raw) {
      final RemoteRecording? recording = RemoteRecording.fromJson(item);
      if (recording == null) {
        return null;
      }
      recordings.add(recording);
    }
    int number(Object? raw) => raw is num ? raw.toInt() : 0;
    return RecordingArchive(
      recordings: recordings,
      usageBytes: number(value['usage_bytes']),
      quotaBytes: number(value['quota_bytes']),
      maxUploadBytes: number(value['max_upload_bytes']),
    );
  }
}

/// Account-wide settings read from the signed-in user's server record.
class AccountInfo {
  const AccountInfo({
    required this.username,
    this.email = '',
    this.phone = '',
    this.twoFactor = false,
    this.recoveryCodes = 0,
  });

  final String username;

  /// Optional recovery address; empty means password reset by mail is off.
  final String email;

  /// Optional recovery number, normalized to digits; empty means SMS reset is
  /// off.
  final String phone;
  final bool twoFactor;

  /// Unused one-time backup codes; zero means only the authenticator works.
  final int recoveryCodes;

  static AccountInfo? fromJson(Object? value) {
    if (value is! Map || value['user'] is! Map) {
      return null;
    }
    final Map<Object?, Object?> user = value['user'] as Map<Object?, Object?>;
    final String username = user['username']?.toString().trim() ?? '';
    if (username.isEmpty) {
      return null;
    }
    final Object? rawRecovery = value['recovery_codes'];
    return AccountInfo(
      username: username,
      email: user['email']?.toString().trim() ?? '',
      phone: user['phone']?.toString().trim() ?? '',
      twoFactor: value['two_factor'] == true,
      recoveryCodes: rawRecovery is num ? rawRecovery.toInt() : 0,
    );
  }
}

/// A pending authenticator enrollment. The secret is shown once and the URI
/// can be pasted into an authenticator app that does not scan QR codes.
class TwoFactorEnrollment {
  const TwoFactorEnrollment({required this.secret, required this.uri});

  final String secret;
  final String uri;

  static TwoFactorEnrollment? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final String secret = value['secret']?.toString().trim() ?? '';
    final String uri = value['otpauth_uri']?.toString().trim() ?? '';
    if (secret.isEmpty || uri.isEmpty) {
      return null;
    }
    return TwoFactorEnrollment(secret: secret, uri: uri);
  }
}

/// One Windows Hello / FIDO2 credential registered on the account. [id] is
/// the unpadded base64url credential id; the private key never leaves the
/// platform authenticator.
class WebAuthnCredential {
  const WebAuthnCredential({
    required this.id,
    this.alg = 0,
    this.name = '',
    this.createdAtMs = 0,
    this.lastUsedAtMs = 0,
  });

  final String id;
  final int alg;
  final String name;
  final int createdAtMs;
  final int lastUsedAtMs;

  static List<WebAuthnCredential> listFromJson(Object? value) {
    if (value is! List) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return <WebAuthnCredential>[
      for (final Object? entry in value)
        if (entry is Map)
          WebAuthnCredential(
            id: entry['id']?.toString().trim() ?? '',
            alg: (entry['alg'] as num?)?.toInt() ?? 0,
            name: entry['name']?.toString().trim() ?? '',
            createdAtMs: (entry['created_at_ms'] as num?)?.toInt() ?? 0,
            lastUsedAtMs: (entry['last_used_at_ms'] as num?)?.toInt() ?? 0,
          ),
    ];
  }
}

/// A pending registration challenge. [userId] and [challenge] are the
/// unpadded base64url values the native ceremony must sign verbatim.
class WebAuthnRegisterOptions {
  const WebAuthnRegisterOptions({
    required this.challenge,
    required this.rpId,
    required this.userId,
    required this.username,
    this.displayName = '',
    this.timeoutMs = 120000,
    this.excludeCredentials = const <WebAuthnCredential>[],
  });

  final String challenge;
  final String rpId;
  final String userId;
  final String username;
  final String displayName;
  final int timeoutMs;
  final List<WebAuthnCredential> excludeCredentials;

  static WebAuthnRegisterOptions? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final String challenge = value['challenge']?.toString().trim() ?? '';
    final String rpId = value['rp_id']?.toString().trim() ?? '';
    final String userId = value['user_id']?.toString().trim() ?? '';
    final String username = value['username']?.toString().trim() ?? '';
    if (challenge.isEmpty || rpId.isEmpty || userId.isEmpty || username.isEmpty) {
      return null;
    }
    final int timeout = (value['timeout_ms'] as num?)?.toInt() ?? 0;
    return WebAuthnRegisterOptions(
      challenge: challenge,
      rpId: rpId,
      userId: userId,
      username: username,
      displayName: value['display_name']?.toString().trim() ?? '',
      timeoutMs: timeout > 0 ? timeout : 120000,
      excludeCredentials: WebAuthnCredential.listFromJson(
        value['exclude_credentials'] ?? const <Object?>[],
      ),
    );
  }
}

/// A pending assertion challenge for a security-key login.
class WebAuthnLoginOptions {
  const WebAuthnLoginOptions({
    required this.challenge,
    required this.rpId,
    required this.username,
    this.timeoutMs = 120000,
    this.credentials = const <WebAuthnCredential>[],
  });

  final String challenge;
  final String rpId;
  final String username;
  final int timeoutMs;
  final List<WebAuthnCredential> credentials;

  static WebAuthnLoginOptions? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final String challenge = value['challenge']?.toString().trim() ?? '';
    final String rpId = value['rp_id']?.toString().trim() ?? '';
    final String username = value['username']?.toString().trim() ?? '';
    if (challenge.isEmpty || rpId.isEmpty || username.isEmpty) {
      return null;
    }
    final int timeout = (value['timeout_ms'] as num?)?.toInt() ?? 0;
    return WebAuthnLoginOptions(
      challenge: challenge,
      rpId: rpId,
      username: username,
      timeoutMs: timeout > 0 ? timeout : 120000,
      credentials: WebAuthnCredential.listFromJson(
        value['credentials'] ?? const <Object?>[],
      ),
    );
  }
}

/// One entry in the signed-in user's server-side address book.
class AddressBookPeer {
  const AddressBookPeer({
    required this.id,
    this.name = '',
    this.tags = const <String>[],
    this.groupId = '',
    this.position = 0,
    this.updatedAtMs = 0,
    this.revision = 0,
    this.deleted = false,
  });

  final String id;
  final String name;
  final List<String> tags;

  /// Address-book group this peer belongs to; empty means ungrouped.
  final String groupId;

  /// Manual sort position inside the group; lower values come first and ties
  /// fall back to the peer id.
  final int position;
  final int updatedAtMs;

  /// Per-user server sequence number of the last write to this peer.
  final int revision;

  /// Tombstone marker. Tombstones only arrive through
  /// [AddressBookSnapshot.changes], never through the alive peer list.
  final bool deleted;

  static AddressBookPeer? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final String id = value['id']?.toString().trim() ?? '';
    if (id.isEmpty) {
      return null;
    }
    final Object? rawTags = value['tags'];
    final Object? rawUpdated = value['updated_at_ms'];
    final Object? rawRevision = value['revision'];
    final Object? rawPosition = value['position'];
    return AddressBookPeer(
      id: id,
      name: value['name']?.toString().trim() ?? '',
      tags: <String>[
        if (rawTags is List)
          for (final Object? tag in rawTags)
            if (tag is String && tag.trim().isNotEmpty) tag.trim(),
      ],
      groupId: value['group_id']?.toString().trim() ?? '',
      position: rawPosition is num && rawPosition >= 0
          ? rawPosition.toInt()
          : 0,
      updatedAtMs: rawUpdated is num ? rawUpdated.toInt() : 0,
      revision: rawRevision is num ? rawRevision.toInt() : 0,
      deleted: value['deleted'] == true,
    );
  }
}

/// One named group inside the signed-in user's address book.
class AddressBookGroup {
  const AddressBookGroup({
    required this.id,
    this.name = '',
    this.parentId = '',
    this.position = 0,
    this.updatedAtMs = 0,
    this.revision = 0,
    this.deleted = false,
  });

  final String id;
  final String name;

  /// Parent group id; empty means the group sits at the root.
  final String parentId;

  /// Manual sort position among sibling groups; lower values come first and
  /// ties fall back to the name, then the id.
  final int position;
  final int updatedAtMs;
  final int revision;
  final bool deleted;

  static AddressBookGroup? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final String id = value['id']?.toString().trim() ?? '';
    if (id.isEmpty) {
      return null;
    }
    final Object? rawUpdated = value['updated_at_ms'];
    final Object? rawRevision = value['revision'];
    final Object? rawPosition = value['position'];
    final String parentId = value['parent_id']?.toString().trim() ?? '';
    return AddressBookGroup(
      id: id,
      name: value['name']?.toString().trim() ?? '',
      parentId: parentId == id ? '' : parentId,
      position: rawPosition is num && rawPosition >= 0
          ? rawPosition.toInt()
          : 0,
      updatedAtMs: rawUpdated is num ? rawUpdated.toInt() : 0,
      revision: rawRevision is num ? rawRevision.toInt() : 0,
      deleted: value['deleted'] == true,
    );
  }
}

/// One address-book read: the alive book plus the deletion tombstones written
/// after the caller's last seen [AddressBookSnapshot.revision].
class AddressBookSnapshot {
  const AddressBookSnapshot({
    required this.revision,
    required this.peers,
    this.changes = const <AddressBookPeer>[],
    this.groups = const <AddressBookGroup>[],
    this.groupChanges = const <AddressBookGroup>[],
  });

  final int revision;
  final List<AddressBookPeer> peers;
  final List<AddressBookPeer> changes;
  final List<AddressBookGroup> groups;
  final List<AddressBookGroup> groupChanges;

  List<AddressBookPeer> get tombstones => <AddressBookPeer>[
    for (final AddressBookPeer peer in changes)
      if (peer.deleted) peer,
  ];

  List<AddressBookGroup> get groupTombstones => <AddressBookGroup>[
    for (final AddressBookGroup group in groupChanges)
      if (group.deleted) group,
  ];
}

const List<String> addressBookPermissions = <String>[
  'read',
  'write',
  'control',
];

bool addressBookPermissionAllows(String granted, String required) {
  int rank(String permission) {
    switch (permission) {
      case 'control':
        return 3;
      case 'write':
        return 2;
      case 'read':
        return 1;
    }
    return 0;
  }

  return rank(granted) >= rank(required);
}

/// One user allowed to access an address book, or one book shared with the
/// signed-in user. [permission] is read, write or control; an empty
/// [groupId] means the whole book and a non-empty one scopes the grant to a
/// single group.
class AddressBookGrant {
  const AddressBookGrant({
    required this.username,
    this.permission = 'read',
    this.groupId = '',
    this.createdAtMs = 0,
  });

  final String username;
  final String permission;
  final String groupId;
  final int createdAtMs;

  bool get wholeBook => groupId.isEmpty;

  static AddressBookGrant? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final String username = value['username']?.toString().trim() ?? '';
    if (username.isEmpty) {
      return null;
    }
    final String rawPermission = value['permission']?.toString().trim() ?? '';
    final Object? rawCreated = value['created_at_ms'];
    return AddressBookGrant(
      username: username,
      permission: rawPermission.isEmpty ? 'read' : rawPermission,
      groupId: value['group_id']?.toString().trim() ?? '',
      createdAtMs: rawCreated is num ? rawCreated.toInt() : 0,
    );
  }
}

class AddressBookGrants {
  const AddressBookGrants({
    this.outgoing = const <AddressBookGrant>[],
    this.incoming = const <AddressBookGrant>[],
  });

  /// Users who may access this account's address book.
  final List<AddressBookGrant> outgoing;

  /// Owners whose address books this account may access.
  final List<AddressBookGrant> incoming;
}

/// One grant the signed-in user holds on another account's book. An empty
/// [groupId] means the whole book.
class AddressBookScope {
  const AddressBookScope({this.groupId = '', this.permission = 'read'});

  final String groupId;
  final String permission;

  bool get wholeBook => groupId.isEmpty;

  static AddressBookScope? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final String rawPermission = value['permission']?.toString().trim() ?? '';
    return AddressBookScope(
      groupId: value['group_id']?.toString().trim() ?? '',
      permission: rawPermission.isEmpty ? 'read' : rawPermission,
    );
  }
}

/// Another account's live address book, fetched whole by a grantee. [peers]
/// and [groups] only contain what the grants expose; [scopes] lists the
/// whole-book and group-level permissions behind that view.
class SharedAddressBook {
  const SharedAddressBook({
    required this.owner,
    this.permission = 'read',
    this.revision = 0,
    this.peers = const <AddressBookPeer>[],
    this.groups = const <AddressBookGroup>[],
    this.scopes = const <AddressBookScope>[],
  });

  final String owner;
  final String permission;
  final int revision;
  final List<AddressBookPeer> peers;
  final List<AddressBookGroup> groups;
  final List<AddressBookScope> scopes;

  /// Permission covering one group, or '' when no grant reaches it. Servers
  /// that predate scopes only report a book-wide permission.
  String permissionFor(String groupId) {
    if (scopes.isEmpty) {
      return permission;
    }
    int rank(String permission) => switch (permission) {
      'control' => 3,
      'write' => 2,
      'read' => 1,
      _ => 0,
    };
    String best = '';
    for (final AddressBookScope scope in scopes) {
      if (scope.groupId == groupId && rank(scope.permission) > rank(best)) {
        best = scope.permission;
      }
    }
    return best;
  }

  static SharedAddressBook? fromJson(Object? value) {
    if (value is! Map) {
      return null;
    }
    final String owner = value['owner']?.toString().trim() ?? '';
    final Object? rawPeers = value['peers'];
    if (owner.isEmpty || rawPeers is! List) {
      return null;
    }
    final List<AddressBookPeer> peers = <AddressBookPeer>[];
    for (final Object? item in rawPeers) {
      final AddressBookPeer? peer = AddressBookPeer.fromJson(item);
      if (peer == null) {
        return null;
      }
      peers.add(peer);
    }
    final List<AddressBookGroup> groups = <AddressBookGroup>[];
    final Object? rawGroups = value['groups'];
    if (rawGroups is List) {
      for (final Object? item in rawGroups) {
        final AddressBookGroup? group = AddressBookGroup.fromJson(item);
        if (group == null) {
          return null;
        }
        groups.add(group);
      }
    }
    final List<AddressBookScope> scopes = <AddressBookScope>[];
    final Object? rawScopes = value['scopes'];
    if (rawScopes is List) {
      for (final Object? item in rawScopes) {
        final AddressBookScope? scope = AddressBookScope.fromJson(item);
        if (scope == null) {
          return null;
        }
        scopes.add(scope);
      }
    }
    final String rawPermission = value['permission']?.toString().trim() ?? '';
    final Object? rawRevision = value['revision'];
    return SharedAddressBook(
      owner: owner,
      permission: rawPermission.isEmpty ? 'read' : rawPermission,
      revision: rawRevision is num ? rawRevision.toInt() : 0,
      peers: peers,
      groups: groups,
      scopes: scopes,
    );
  }
}

class ServerApiException implements Exception {
  const ServerApiException(this.message, {this.statusCode, this.code = ''});

  final String message;
  final int? statusCode;

  /// Machine-readable reason from the server, such as `totp_required`.
  final String code;

  @override
  String toString() => message;
}

enum HostPresence { unknown, checking, online, offline, unreachable }

class DevicePresence {
  const DevicePresence(this.state, {this.info, this.error = ''});

  static const DevicePresence unknown = DevicePresence(HostPresence.unknown);

  final HostPresence state;
  final HostInfo? info;
  final String error;

  bool get online => state == HostPresence.online;
}

/// Lookup-only client for the HTTP service. When the server runs with a key,
/// lookup must carry it too: the host id and punch proof are only as private
/// as the registry that hands them out.
class ServerApi {
  ServerApi._();

  /// Server-side limit for `GET /v1/hosts?ids=...`.
  static const int maxBatchIds = 200;

  static Future<HostInfo> lookup(
    ServerSettings settings,
    String hostId, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String id = hostId.trim();
    if (id.isEmpty) {
      throw ServerApiException(tr('设备 ID 为空'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/hosts/${Uri.encodeComponent(id)}');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _getJson(uri, timeout, key: settings.key);
    if (decoded is! Map) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return HostInfo.fromJson(decoded, id);
  }

  /// One address-book presence refresh for [hostIds] in a single round trip.
  ///
  /// Ids the server does not know are absent from [BatchLookupResult.hosts]
  /// instead of failing the whole batch, so a stale entry cannot hide the
  /// peers that do exist. The list is split into [maxBatchIds] chunks because
  /// the server rejects larger requests.
  static Future<BatchLookupResult> lookupBatch(
    ServerSettings settings,
    List<String> hostIds, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final List<String> ids = <String>[];
    final Set<String> seen = <String>{};
    for (final String raw in hostIds) {
      final String id = raw.trim();
      if (id.isNotEmpty && seen.add(id)) {
        ids.add(id);
      }
    }
    final Map<String, HostInfo> hosts = <String, HostInfo>{};
    SoftwareInfo? software;
    for (int start = 0; start < ids.length; start += maxBatchIds) {
      final int end = start + maxBatchIds < ids.length
          ? start + maxBatchIds
          : ids.length;
      final BatchLookupResult chunk = await _lookupBatchChunk(
        settings,
        ids.sublist(start, end),
        timeout,
      );
      hosts.addAll(chunk.hosts);
      software ??= chunk.software;
    }
    return BatchLookupResult(hosts: hosts, software: software);
  }

  static Future<BatchLookupResult> _lookupBatchChunk(
    ServerSettings settings,
    List<String> ids,
    Duration timeout,
  ) async {
    final Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}/v1/hosts',
      ).replace(queryParameters: <String, String>{'ids': ids.join(',')});
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _getJson(uri, timeout, key: settings.key);
    final Object? rawHosts = decoded is Map ? decoded['hosts'] : null;
    if (rawHosts is! List) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    final Map<String, HostInfo> hosts = <String, HostInfo>{};
    for (final Object? item in rawHosts) {
      if (item is! Map) {
        throw ServerApiException(tr('连接服务返回了无效数据'));
      }
      final String id = item['id']?.toString() ?? '';
      if (id.isEmpty) {
        throw ServerApiException(tr('连接服务返回了无效数据'));
      }
      hosts[id] = HostInfo.fromJson(item, id);
    }
    return BatchLookupResult(
      hosts: hosts,
      software: decoded is Map
          ? SoftwareInfo.fromJson(decoded['software'])
          : null,
    );
  }

  /// Replaces the operator-managed display labels for [hostId].
  ///
  /// Empty values clear the field, matching `PATCH /v1/identities/{id}`. Tags
  /// are trimmed and de-duplicated here as well so an accidental
  /// `office, office` does not reach the server; the server still enforces its
  /// own length limits.
  static Future<void> updateLabels(
    ServerSettings settings,
    String hostId, {
    required String alias,
    required List<String> tags,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String id = hostId.trim();
    if (id.isEmpty) {
      throw ServerApiException(tr('设备 ID 为空'));
    }
    final Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}/v1/identities/${Uri.encodeComponent(id)}',
      );
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final List<String> cleaned = <String>[];
    final Set<String> seen = <String>{};
    for (final String raw in tags) {
      final String tag = raw.trim();
      if (tag.isNotEmpty && seen.add(tag)) {
        cleaned.add(tag);
      }
    }
    final Object? decoded = await _requestJson(
      'PATCH',
      uri,
      timeout,
      key: settings.key,
      body: <String, Object?>{'alias': alias.trim(), 'tags': cleaned},
    );
    if (decoded is! Map || decoded['id']?.toString() != id) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
  }

  /// Exchanges a viewer account name and password for a bearer token.
  ///
  /// The token belongs to the account API only; the operator server key is
  /// never sent on this route and cannot be used in place of the token.
  static Future<AccountLoginResult> login(
    ServerSettings settings, {
    required String username,
    required String password,
    String deviceName = '',
    String totpCode = '',
    String recoveryCode = '',
    String smsCode = '',
    String emailCode = '',
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String name = username.trim();
    final String device = deviceName.trim();
    final String otp = totpCode.trim();
    final String recovery = recoveryCode.trim();
    final String sms = _numericCode(smsCode);
    final String email = _numericCode(emailCode);
    if (name.isEmpty || password.isEmpty) {
      throw ServerApiException(tr('请输入用户名和密码'));
    }
    // Precedence mirrors the server: authenticator code, then recovery code,
    // then the SMS backup code, then the email backup code, so only one is
    // sent.
    final bool useSms = otp.isEmpty && recovery.isEmpty && sms.isNotEmpty;
    final bool useEmail =
        otp.isEmpty && recovery.isEmpty && sms.isEmpty && email.isNotEmpty;
    if (useSms && sms.length != 8) {
      throw ServerApiException(tr('短信验证码需为 8 位数字'));
    }
    if (useEmail && email.length != 8) {
      throw ServerApiException(tr('邮件验证码需为 8 位数字'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/login');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded;
    try {
      decoded = await _requestJson(
        'POST',
        uri,
        timeout,
        body: <String, Object?>{
          'username': name,
          'password': password,
          if (device.isNotEmpty) 'device_name': device,
          if (otp.isNotEmpty) 'totp_code': otp,
          if (otp.isEmpty && recovery.isNotEmpty) 'recovery_code': recovery,
          if (useSms) 'sms_code': sms,
          if (useEmail) 'email_code': email,
        },
        unauthorized: tr('用户名或密码不正确'),
      );
    } on ServerApiException catch (error) {
      if (error.code == 'totp_required') {
        throw ServerApiException(
          tr('请输入两步验证码'),
          statusCode: HttpStatus.unauthorized,
          code: 'totp_required',
        );
      }
      if (error.code == 'totp_invalid') {
        throw ServerApiException(
          tr('验证码或恢复码不正确'),
          statusCode: HttpStatus.unauthorized,
          code: 'totp_invalid',
        );
      }
      rethrow;
    }
    final AccountLoginResult? result = _parseLoginReply(decoded);
    if (result == null) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return result;
  }

  /// Asks the server to text a single-use login code to the account's
  /// recovery number. The password is checked first, and accounts without
  /// two-factor or a number still answer 202, so the route cannot be used to
  /// discover either. Repeated sends are throttled to one per minute.
  static Future<void> requestLoginSMS(
    ServerSettings settings, {
    required String username,
    required String password,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String name = username.trim();
    if (name.isEmpty || password.isEmpty) {
      throw ServerApiException(tr('请输入用户名和密码'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/login/sms');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'POST',
      uri,
      timeout,
      body: <String, Object?>{'username': name, 'password': password},
      success: const <int>{HttpStatus.accepted},
      unauthorized: tr('用户名或密码不正确'),
      forbidden: tr('账号已被封禁'),
      badRequest: tr('用户名或密码无效'),
      unavailable: tr('连接服务未配置账号或短信登录'),
    );
  }

  /// Asks the server to email a single-use login code to the account's
  /// recovery address. The password is checked first, and accounts without
  /// two-factor or an address still answer 202, so the route cannot be used to
  /// discover either. Repeated sends are throttled to one per minute.
  static Future<void> requestLoginEmail(
    ServerSettings settings, {
    required String username,
    required String password,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String name = username.trim();
    if (name.isEmpty || password.isEmpty) {
      throw ServerApiException(tr('请输入用户名和密码'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/login/email');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'POST',
      uri,
      timeout,
      body: <String, Object?>{'username': name, 'password': password},
      success: const <int>{HttpStatus.accepted},
      unauthorized: tr('用户名或密码不正确'),
      forbidden: tr('账号已被封禁'),
      badRequest: tr('用户名或密码无效'),
      unavailable: tr('连接服务未配置账号或邮件登录'),
    );
  }

  /// Trades a refresh token for a fresh access/refresh pair. The server
  /// rotates the pair and revokes the session when a retired token is
  /// presented again, so a failed renewal always means a new password login.
  static Future<AccountLoginResult> refresh(
    ServerSettings settings,
    String refreshToken, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String token = refreshToken.trim();
    if (token.isEmpty) {
      throw ServerApiException(tr('登录已过期，请重新登录'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/refresh');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded;
    try {
      decoded = await _requestJson(
        'POST',
        uri,
        timeout,
        body: <String, Object?>{'refresh_token': token},
        unauthorized: tr('登录已过期，请重新登录'),
      );
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.unauthorized) {
        throw ServerApiException(
          tr('登录已过期，请重新登录'),
          statusCode: HttpStatus.unauthorized,
        );
      }
      rethrow;
    }
    final AccountLoginResult? result = _parseLoginReply(decoded);
    if (result == null) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return result;
  }

  static AccountLoginResult? _parseLoginReply(Object? decoded) {
    if (decoded is! Map || decoded['user'] is! Map) {
      return null;
    }
    final Object? rawToken = decoded['token'];
    final Object? rawExpires = decoded['expires_in_ms'];
    final String username =
        (decoded['user'] as Map)['username']?.toString().trim() ?? '';
    if (rawToken is! String ||
        rawToken.trim().isEmpty ||
        rawExpires is! num ||
        username.isEmpty) {
      return null;
    }
    final String refreshToken =
        decoded['refresh_token']?.toString().trim() ?? '';
    final Object? rawRefreshExpires = decoded['refresh_expires_in_ms'];
    return AccountLoginResult(
      token: rawToken.trim(),
      username: username,
      expiresInMs: rawExpires.toInt(),
      refreshToken: refreshToken,
      refreshExpiresInMs: refreshToken.isEmpty || rawRefreshExpires is! num
          ? 0
          : rawRefreshExpires.toInt(),
    );
  }

  /// Reads the signed-in account's server state, including whether logins
  /// require a two-factor code.
  static Future<AccountInfo> accountInfo(
    ServerSettings settings,
    String token, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'GET',
      uri,
      timeout,
      accountToken: accountToken,
      unauthorized: tr('账号登录已失效，请重新登录'),
    );
    final AccountInfo? info = AccountInfo.fromJson(decoded);
    if (info == null) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return info;
  }

  /// Creates a pending authenticator secret. Logins keep working with the
  /// password alone until [enableTwoFactor] confirms a code from it.
  static Future<TwoFactorEnrollment> startTwoFactor(
    ServerSettings settings,
    String token, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/2fa/start');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded;
    try {
      decoded = await _requestJson(
        'POST',
        uri,
        timeout,
        accountToken: accountToken,
        unauthorized: tr('账号登录已失效，请重新登录'),
      );
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.conflict) {
        throw ServerApiException(tr('两步验证已启用，请先关闭后再重新设置'));
      }
      rethrow;
    }
    final TwoFactorEnrollment? enrollment = TwoFactorEnrollment.fromJson(
      decoded,
    );
    if (enrollment == null) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return enrollment;
  }

  static Future<void> enableTwoFactor(
    ServerSettings settings,
    String token,
    String code, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final String otp = code.trim();
    if (otp.isEmpty) {
      throw ServerApiException(tr('请输入两步验证码'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/2fa/enable');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    try {
      await _requestJson(
        'POST',
        uri,
        timeout,
        accountToken: accountToken,
        body: <String, Object?>{'code': otp},
        success: const <int>{HttpStatus.ok, HttpStatus.noContent},
        unauthorized: tr('账号登录已失效，请重新登录'),
        forbidden: tr('两步验证码不正确'),
      );
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.badRequest) {
        throw ServerApiException(tr('请先获取两步验证密钥'));
      }
      rethrow;
    }
  }

  /// Turns two-factor off. The server requires both the password and a
  /// current authenticator code.
  static Future<void> disableTwoFactor(
    ServerSettings settings,
    String token, {
    required String password,
    required String code,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    if (password.isEmpty) {
      throw ServerApiException(tr('请输入当前密码'));
    }
    final String otp = code.trim();
    if (otp.isEmpty) {
      throw ServerApiException(tr('请输入两步验证码'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/2fa/disable');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    try {
      await _requestJson(
        'POST',
        uri,
        timeout,
        accountToken: accountToken,
        body: <String, Object?>{'password': password, 'code': otp},
        success: const <int>{HttpStatus.ok, HttpStatus.noContent},
        unauthorized: tr('账号登录已失效，请重新登录'),
        forbidden: tr('密码或两步验证码不正确'),
      );
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.badRequest) {
        throw ServerApiException(tr('两步验证未启用'));
      }
      rethrow;
    }
  }

  /// Issues a fresh set of single-use backup codes and returns the plaintext
  /// once. The password and a current authenticator code are required; any
  /// previously issued codes stop working.
  static Future<List<String>> generateRecoveryCodes(
    ServerSettings settings,
    String token, {
    required String password,
    required String code,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    if (password.isEmpty) {
      throw ServerApiException(tr('请输入当前密码'));
    }
    final String otp = code.trim();
    if (otp.isEmpty) {
      throw ServerApiException(tr('请输入两步验证码'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/2fa/recovery');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded;
    try {
      decoded = await _requestJson(
        'POST',
        uri,
        timeout,
        accountToken: accountToken,
        body: <String, Object?>{'password': password, 'code': otp},
        success: const <int>{HttpStatus.ok, HttpStatus.noContent},
        unauthorized: tr('账号登录已失效，请重新登录'),
        forbidden: tr('密码或两步验证码不正确'),
      );
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.badRequest) {
        throw ServerApiException(tr('两步验证未启用'));
      }
      rethrow;
    }
    final Object? raw = decoded is Map ? decoded['codes'] : null;
    if (raw is! List || raw.isEmpty) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    final List<String> codes = <String>[];
    for (final Object? entry in raw) {
      final String value = entry?.toString().trim() ?? '';
      if (value.isEmpty) {
        throw ServerApiException(tr('连接服务返回了无效数据'));
      }
      codes.add(value);
    }
    return codes;
  }

  /// Starts registering a Windows Hello / FIDO2 credential. The password is
  /// re-checked so a borrowed token cannot add a key.
  static Future<WebAuthnRegisterOptions> webauthnRegisterBegin(
    ServerSettings settings,
    String token, {
    required String password,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    if (password.isEmpty) {
      throw ServerApiException(tr('请输入当前密码'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/webauthn/register/begin');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'POST',
      uri,
      timeout,
      accountToken: accountToken,
      body: <String, Object?>{'password': password},
      unauthorized: tr('账号登录已失效，请重新登录'),
      forbidden: tr('当前密码不正确'),
      unavailable: tr('连接服务未启用安全密钥'),
    );
    final WebAuthnRegisterOptions? options = WebAuthnRegisterOptions.fromJson(
      decoded,
    );
    if (options == null) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return options;
  }

  /// Sends the attestation produced by Windows Hello. [name] is the label the
  /// account dialog shows, such as "此电脑".
  static Future<WebAuthnCredential> webauthnRegisterFinish(
    ServerSettings settings,
    String token, {
    required String name,
    required String clientDataJSON,
    required String attestationObject,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    if (clientDataJSON.isEmpty || attestationObject.isEmpty) {
      throw ServerApiException(tr('安全密钥数据不完整'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/webauthn/register/finish');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'POST',
      uri,
      timeout,
      accountToken: accountToken,
      body: <String, Object?>{
        'name': name.trim(),
        'client_data_json': clientDataJSON,
        'attestation_object': attestationObject,
      },
      unauthorized: tr('账号登录已失效，请重新登录'),
      forbidden: tr('注册校验未通过，请重新开始'),
      badRequest: tr('安全密钥数据无效'),
      conflict: tr('该安全密钥已经注册'),
      unavailable: tr('连接服务未启用安全密钥'),
    );
    final List<WebAuthnCredential> stored = WebAuthnCredential.listFromJson(
      decoded == null ? const <Object?>[] : <Object?>[decoded],
    );
    if (stored.isEmpty || stored.first.id.isEmpty) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return stored.first;
  }

  /// Lists the security keys registered on the signed-in account.
  static Future<List<WebAuthnCredential>> webauthnCredentials(
    ServerSettings settings,
    String token, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/webauthn/credentials');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'GET',
      uri,
      timeout,
      accountToken: accountToken,
      unauthorized: tr('账号登录已失效，请重新登录'),
      unavailable: tr('连接服务未启用安全密钥'),
    );
    final Object? raw = decoded is Map ? decoded['credentials'] : null;
    return WebAuthnCredential.listFromJson(raw ?? const <Object?>[]);
  }

  /// Removes a registered security key. The password is required again.
  static Future<void> webauthnRemove(
    ServerSettings settings,
    String token, {
    required String password,
    required String credentialId,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    if (password.isEmpty) {
      throw ServerApiException(tr('请输入当前密码'));
    }
    if (credentialId.isEmpty) {
      throw ServerApiException(tr('安全密钥标识为空'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/webauthn/credentials/remove');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'POST',
      uri,
      timeout,
      accountToken: accountToken,
      body: <String, Object?>{
        'password': password,
        'credential_id': credentialId,
      },
      success: const <int>{HttpStatus.ok, HttpStatus.noContent},
      unauthorized: tr('账号登录已失效，请重新登录'),
      forbidden: tr('当前密码不正确'),
      notFound: tr('安全密钥不存在或已删除'),
      unavailable: tr('连接服务未启用安全密钥'),
    );
  }

  /// Starts a security-key login after the password has been checked. A null
  /// result means the password matched but the account has no registered key;
  /// the server answers 202 in that case so the route cannot map accounts.
  ///
  /// The account's two-factor state is not bypassed: this route is the
  /// second factor itself.
  static Future<WebAuthnLoginOptions?> loginWebAuthnBegin(
    ServerSettings settings, {
    required String username,
    required String password,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String name = username.trim();
    if (name.isEmpty || password.isEmpty) {
      throw ServerApiException(tr('请输入用户名和密码'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/login/webauthn/begin');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'POST',
      uri,
      timeout,
      body: <String, Object?>{'username': name, 'password': password},
      success: const <int>{HttpStatus.ok, HttpStatus.accepted},
      unauthorized: tr('用户名或密码不正确'),
      forbidden: tr('账号已被封禁'),
      badRequest: tr('用户名或密码无效'),
      unavailable: tr('连接服务未启用安全密钥'),
    );
    if (decoded is Map && decoded['status'] == 'accepted') {
      return null;
    }
    final WebAuthnLoginOptions? options = WebAuthnLoginOptions.fromJson(
      decoded,
    );
    if (options == null) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return options;
  }

  /// Starts a passwordless security-key login for a username. No password is
  /// checked, so the returned credential list is the only thing Windows Hello
  /// needs. A null result means the account has no registered key; the server
  /// answers 202 for both an unknown account and one without keys so this
  /// route cannot map accounts.
  static Future<WebAuthnLoginOptions?> loginWebAuthnPasswordlessBegin(
    ServerSettings settings, {
    required String username,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String name = username.trim();
    if (name.isEmpty) {
      throw ServerApiException(tr('请输入用户名'));
    }
    final Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}/v1/account/login/webauthn/passwordless/begin',
      );
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'POST',
      uri,
      timeout,
      body: <String, Object?>{'username': name},
      success: const <int>{HttpStatus.ok, HttpStatus.accepted},
      badRequest: tr('用户名无效'),
      unavailable: tr('连接服务未启用安全密钥'),
    );
    if (decoded is Map && decoded['status'] == 'accepted') {
      return null;
    }
    final WebAuthnLoginOptions? options = WebAuthnLoginOptions.fromJson(
      decoded,
    );
    if (options == null) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return options;
  }

  /// Exchanges a Windows Hello assertion produced for the password-gated
  /// challenge for an account session.
  static Future<AccountLoginResult> loginWebAuthnFinish(
    ServerSettings settings, {
    required String username,
    required String credentialId,
    required String clientDataJSON,
    required String authenticatorData,
    required String signature,
    String deviceName = '',
    Duration timeout = const Duration(seconds: 6),
  }) => _loginWebAuthnFinish(
    '/v1/account/login/webauthn/finish',
    settings,
    username: username,
    credentialId: credentialId,
    clientDataJSON: clientDataJSON,
    authenticatorData: authenticatorData,
    signature: signature,
    deviceName: deviceName,
    timeout: timeout,
  );

  /// Exchanges a Windows Hello assertion produced for a passwordless
  /// challenge for an account session.
  static Future<AccountLoginResult> loginWebAuthnPasswordlessFinish(
    ServerSettings settings, {
    required String username,
    required String credentialId,
    required String clientDataJSON,
    required String authenticatorData,
    required String signature,
    String deviceName = '',
    Duration timeout = const Duration(seconds: 6),
  }) => _loginWebAuthnFinish(
    '/v1/account/login/webauthn/passwordless/finish',
    settings,
    username: username,
    credentialId: credentialId,
    clientDataJSON: clientDataJSON,
    authenticatorData: authenticatorData,
    signature: signature,
    deviceName: deviceName,
    timeout: timeout,
  );

  static Future<AccountLoginResult> _loginWebAuthnFinish(
    String path,
    ServerSettings settings, {
    required String username,
    required String credentialId,
    required String clientDataJSON,
    required String authenticatorData,
    required String signature,
    String deviceName = '',
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String name = username.trim();
    final String device = deviceName.trim();
    if (name.isEmpty) {
      throw ServerApiException(tr('请输入用户名'));
    }
    if (credentialId.isEmpty ||
        clientDataJSON.isEmpty ||
        authenticatorData.isEmpty ||
        signature.isEmpty) {
      throw ServerApiException(tr('安全密钥数据不完整'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}$path');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'POST',
      uri,
      timeout,
      body: <String, Object?>{
        'username': name,
        'credential_id': credentialId,
        'client_data_json': clientDataJSON,
        'authenticator_data': authenticatorData,
        'signature': signature,
        if (device.isNotEmpty) 'device_name': device,
      },
      unauthorized: tr('安全密钥验证未通过'),
      forbidden: tr('账号已被封禁'),
      badRequest: tr('安全密钥数据无效'),
      unavailable: tr('连接服务未启用安全密钥'),
    );
    final AccountLoginResult? result = _parseLoginReply(decoded);
    if (result == null) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return result;
  }

  /// Reports one finished remote session to the account's server. The server
  /// derives the username and client address from the token and request, so
  /// callers only describe the peer and timing. Audit delivery is best-effort;
  /// callers should ignore failures rather than disturb the viewer.
  static Future<void> reportConnection(
    ServerSettings settings,
    String token, {
    required String peerId,
    required DateTime startedAt,
    required DateTime endedAt,
    bool success = true,
    String detail = '',
    String auditRef = '',
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final String peer = peerId.trim();
    if (peer.isEmpty) {
      return;
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/connections');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'POST',
      uri,
      timeout,
      accountToken: accountToken,
      body: <String, Object?>{
        'peer_id': peer,
        'started_at_ms': startedAt.toUtc().millisecondsSinceEpoch,
        'ended_at_ms': endedAt.toUtc().millisecondsSinceEpoch,
        'success': success,
        if (detail.trim().isNotEmpty) 'detail': detail.trim(),
        if (auditRef.trim().isNotEmpty) 'audit_ref': auditRef.trim(),
      },
      success: const <int>{HttpStatus.created},
      unauthorized: tr('账号登录已失效，请重新登录'),
    );
  }

  /// Reports files that finished between two viewer polls. One call can cover
  /// several files, in which case [name] and [size] describe the last one.
  static Future<void> reportFileTransfers(
    ServerSettings settings,
    String token, {
    required String peerId,
    required String direction,
    String name = '',
    int size = 0,
    int files = 1,
    bool success = true,
    String detail = '',
    String auditRef = '',
    DateTime? occurredAt,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final String peer = peerId.trim();
    if (peer.isEmpty || (direction != 'upload' && direction != 'download')) {
      return;
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/file-transfers');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'POST',
      uri,
      timeout,
      accountToken: accountToken,
      body: <String, Object?>{
        'peer_id': peer,
        'direction': direction,
        'files': files < 1 ? 1 : files,
        if (name.trim().isNotEmpty) 'name': name.trim(),
        if (size > 0) 'size': size,
        'success': success,
        if (detail.trim().isNotEmpty) 'detail': detail.trim(),
        if (auditRef.trim().isNotEmpty) 'audit_ref': auditRef.trim(),
        if (occurredAt != null)
          'occurred_at_ms': occurredAt.toUtc().millisecondsSinceEpoch,
      },
      success: const <int>{HttpStatus.created},
      unauthorized: tr('账号登录已失效，请重新登录'),
    );
  }

  static Future<void> logout(
    ServerSettings settings,
    String token, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = token.trim();
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    if (accountToken.isEmpty) {
      throw ServerApiException(tr('账号未登录'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/logout');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'POST',
      uri,
      timeout,
      accountToken: accountToken,
      success: const <int>{HttpStatus.ok, HttpStatus.noContent},
      unauthorized: tr('账号登录已失效，请重新登录'),
    );
  }

  /// Lists the account's active login sessions across devices.
  static Future<List<AccountDeviceSession>> accountSessions(
    ServerSettings settings,
    String token, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/sessions');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'GET',
      uri,
      timeout,
      accountToken: accountToken,
      unauthorized: tr('账号登录已失效，请重新登录'),
    );
    final Object? raw = decoded is Map ? decoded['sessions'] : null;
    if (raw is! List) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    final List<AccountDeviceSession> sessions = <AccountDeviceSession>[];
    for (final Object? item in raw) {
      final AccountDeviceSession? session = AccountDeviceSession.fromJson(item);
      if (session == null) {
        throw ServerApiException(tr('连接服务返回了无效数据'));
      }
      sessions.add(session);
    }
    return sessions;
  }

  /// Revokes one login session by its public session id. Revoking the current
  /// session invalidates this device's token too.
  static Future<void> revokeAccountSession(
    ServerSettings settings,
    String token,
    String sessionId, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final String id = sessionId.trim();
    if (id.isEmpty) {
      throw ServerApiException(tr('会话 ID 为空'));
    }
    final Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}/v1/account/sessions/${Uri.encodeComponent(id)}',
      );
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'DELETE',
      uri,
      timeout,
      accountToken: accountToken,
      success: const <int>{HttpStatus.ok, HttpStatus.noContent},
      unauthorized: tr('账号登录已失效，请重新登录'),
      notFound: tr('会话不存在或已退出'),
    );
  }

  /// Renames one login session so the device list is easier to recognise.
  /// Renaming the current session does not touch its token.
  static Future<void> renameAccountSession(
    ServerSettings settings,
    String token,
    String sessionId, {
    required String deviceName,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final String id = sessionId.trim();
    if (id.isEmpty) {
      throw ServerApiException(tr('会话 ID 为空'));
    }
    final String name = deviceName.trim();
    if (name.isEmpty) {
      throw ServerApiException(tr('请输入设备名称'));
    }
    final Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}/v1/account/sessions/${Uri.encodeComponent(id)}',
      );
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'PATCH',
      uri,
      timeout,
      accountToken: accountToken,
      body: <String, Object?>{'device_name': name},
      success: const <int>{HttpStatus.ok, HttpStatus.noContent},
      unauthorized: tr('账号登录已失效，请重新登录'),
      notFound: tr('会话不存在或已退出'),
    );
  }

  /// Signs every other device out and keeps this session active. Returns the
  /// number of sessions the server removed.
  static Future<int> revokeOtherAccountSessions(
    ServerSettings settings,
    String token, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/sessions/revoke-others');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'POST',
      uri,
      timeout,
      accountToken: accountToken,
      success: const <int>{HttpStatus.ok},
      unauthorized: tr('账号登录已失效，请重新登录'),
    );
    final Object? raw = decoded is Map ? decoded['revoked'] : null;
    if (raw is! int || raw < 0) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return raw;
  }

  /// Changes the signed-in account's password. Other devices are signed out;
  /// this device's token keeps working.
  static Future<void> changePassword(
    ServerSettings settings,
    String token, {
    required String currentPassword,
    required String newPassword,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    if (currentPassword.isEmpty) {
      throw ServerApiException(tr('请输入当前密码'));
    }
    if (newPassword.isEmpty) {
      throw ServerApiException(tr('请输入新密码'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/password');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'POST',
      uri,
      timeout,
      accountToken: accountToken,
      body: <String, Object?>{
        'current_password': currentPassword,
        'new_password': newPassword,
      },
      success: const <int>{HttpStatus.ok, HttpStatus.noContent},
      unauthorized: tr('账号登录已失效，请重新登录'),
      forbidden: tr('当前密码不正确'),
    );
  }

  /// Attaches or clears the account recovery email. The server also requires
  /// the current password so a borrowed token cannot redirect future resets.
  /// An empty [email] clears the address; the stored value is returned.
  static Future<String> changeRecoveryEmail(
    ServerSettings settings,
    String token, {
    required String email,
    required String password,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    if (password.isEmpty) {
      throw ServerApiException(tr('请输入当前密码'));
    }
    final String value = email.trim();
    if (value.isNotEmpty && !value.contains('@')) {
      throw ServerApiException(tr('请输入有效的邮箱地址'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/email');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'POST',
      uri,
      timeout,
      accountToken: accountToken,
      body: <String, Object?>{'email': value, 'password': password},
      unauthorized: tr('账号登录已失效，请重新登录'),
      forbidden: tr('当前密码不正确'),
      badRequest: tr('邮箱格式无效'),
      conflict: tr('该邮箱已被其他账号使用'),
    );
    final Object? stored = decoded is Map ? decoded['email'] : null;
    if (decoded is! Map || (stored != null && stored is! String)) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return stored?.toString().trim() ?? '';
  }

  /// Asks the server to mail a reset token. Known and unknown names both
  /// receive HTTP 202, so the result cannot reveal whether [username] exists
  /// or has a recovery address.
  static Future<void> requestPasswordRecovery(
    ServerSettings settings, {
    required String username,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String name = username.trim();
    if (name.isEmpty) {
      throw ServerApiException(tr('请输入用户名'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/recover');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'POST',
      uri,
      timeout,
      body: <String, Object?>{'username': name},
      success: const <int>{HttpStatus.accepted},
      badRequest: tr('用户名无效'),
      unavailable: tr('连接服务未配置账号或邮件找回'),
    );
  }

  /// Spends a reset token from the recovery mail and replaces the password.
  /// The server revokes every existing session of the account.
  static Future<String> completePasswordRecovery(
    ServerSettings settings, {
    required String token,
    required String password,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String value = token.trim();
    if (value.isEmpty) {
      throw ServerApiException(tr('请输入邮件中的重置令牌'));
    }
    final int bytes = utf8.encode(password).length;
    if (bytes < 8 || bytes > 128) {
      throw ServerApiException(tr('新密码长度需为 8–128 个字节'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/recover/complete');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'POST',
      uri,
      timeout,
      body: <String, Object?>{'token': value, 'password': password},
      badRequest: tr('重置令牌无效、已过期或密码不符合要求'),
    );
    final Object? username = decoded is Map ? decoded['username'] : null;
    if (username is! String || username.trim().isEmpty) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return username.trim();
  }

  /// Sets or clears the number that receives SMS reset codes. The current
  /// password is required, matching the email change.
  static Future<String> changeRecoveryPhone(
    ServerSettings settings,
    String token, {
    required String phone,
    required String password,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    if (password.isEmpty) {
      throw ServerApiException(tr('请输入当前密码'));
    }
    final String value = phone.trim();
    if (value.isNotEmpty && !_validPhone(value)) {
      throw ServerApiException(tr('手机号需为 6–15 位数字'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/phone');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'POST',
      uri,
      timeout,
      accountToken: accountToken,
      body: <String, Object?>{'phone': value, 'password': password},
      unauthorized: tr('账号登录已失效，请重新登录'),
      forbidden: tr('当前密码不正确'),
      badRequest: tr('手机号格式无效'),
      conflict: tr('该手机号已被其他账号使用'),
    );
    final Object? stored = decoded is Map ? decoded['phone'] : null;
    if (decoded is! Map || (stored != null && stored is! String)) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return stored?.toString().trim() ?? '';
  }

  /// Asks the server to text a reset code. Known and unknown names both
  /// receive HTTP 202, so the result cannot reveal whether [username] exists
  /// or has a recovery number.
  static Future<void> requestSMSRecovery(
    ServerSettings settings, {
    required String username,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String name = username.trim();
    if (name.isEmpty) {
      throw ServerApiException(tr('请输入用户名'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/recover/sms');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'POST',
      uri,
      timeout,
      body: <String, Object?>{'username': name},
      success: const <int>{HttpStatus.accepted},
      badRequest: tr('用户名无效'),
      unavailable: tr('连接服务未配置账号或短信找回'),
    );
  }

  /// Spends an SMS reset code and replaces the password. The server retires
  /// the code after five wrong attempts and revokes every existing session.
  static Future<String> completeSMSRecovery(
    ServerSettings settings, {
    required String username,
    required String code,
    required String password,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String name = username.trim();
    if (name.isEmpty) {
      throw ServerApiException(tr('请输入用户名'));
    }
    final String value = _numericCode(code);
    if (value.length != 8) {
      throw ServerApiException(tr('短信验证码需为 8 位数字'));
    }
    final int bytes = utf8.encode(password).length;
    if (bytes < 8 || bytes > 128) {
      throw ServerApiException(tr('新密码长度需为 8–128 个字节'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/recover/sms/complete');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'POST',
      uri,
      timeout,
      body: <String, Object?>{
        'username': name,
        'code': value,
        'password': password,
      },
      badRequest: tr('短信验证码无效、已过期或密码不符合要求'),
    );
    final Object? resolved = decoded is Map ? decoded['username'] : null;
    if (resolved is! String || resolved.trim().isEmpty) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return resolved.trim();
  }

  static bool _validPhone(String value) {
    if (!RegExp(r'^\+?[0-9 ()\-\.]+$').hasMatch(value)) {
      return false;
    }
    final String digits = value.replaceAll(RegExp(r'[^0-9]'), '');
    return digits.length >= 6 && digits.length <= 15;
  }

  static String _numericCode(String value) =>
      value.replaceAll(RegExp(r'[\s-]'), '');

  /// Reads the account address book. [since] is the revision of the last
  /// successful sync; the server then also returns later deletion tombstones.
  /// A zero or stale value asks for a full snapshot.
  static Future<AddressBookSnapshot> addressBook(
    ServerSettings settings,
    String token, {
    int since = 0,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = token.trim();
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    if (accountToken.isEmpty) {
      throw ServerApiException(tr('账号未登录'));
    }
    final Uri uri;
    try {
      final String query = since > 0 ? '?since=$since' : '';
      uri = Uri.parse('${settings.url}/v1/addressbook$query');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'GET',
      uri,
      timeout,
      accountToken: accountToken,
      unauthorized: tr('账号登录已失效，请重新登录'),
    );
    final Object? peers = decoded is Map ? decoded['peers'] : null;
    final Object? rawChanges = decoded is Map ? decoded['changes'] : null;
    if (peers is! List || (rawChanges != null && rawChanges is! List)) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    final List<AddressBookPeer> alive = <AddressBookPeer>[];
    for (final Object? item in peers) {
      final AddressBookPeer? peer = AddressBookPeer.fromJson(item);
      if (peer == null) {
        throw ServerApiException(tr('连接服务返回了无效数据'));
      }
      alive.add(peer);
    }
    final List<AddressBookPeer> changes = <AddressBookPeer>[];
    for (final Object? item
        in rawChanges is List ? rawChanges : <Object?>[]) {
      final AddressBookPeer? peer = AddressBookPeer.fromJson(item);
      if (peer == null) {
        throw ServerApiException(tr('连接服务返回了无效数据'));
      }
      changes.add(peer);
    }
    final Object? rawRevision = decoded is Map ? decoded['revision'] : null;
    return AddressBookSnapshot(
      revision: rawRevision is num ? rawRevision.toInt() : 0,
      peers: alive,
      changes: changes,
      groups: _parseGroups(decoded is Map ? decoded['groups'] : null),
      groupChanges: _parseGroups(
        decoded is Map ? decoded['group_changes'] : null,
      ),
    );
  }

  static List<AddressBookGroup> _parseGroups(Object? raw) {
    if (raw == null) {
      return <AddressBookGroup>[];
    }
    if (raw is! List) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    final List<AddressBookGroup> groups = <AddressBookGroup>[];
    for (final Object? item in raw) {
      final AddressBookGroup? group = AddressBookGroup.fromJson(item);
      if (group == null) {
        throw ServerApiException(tr('连接服务返回了无效数据'));
      }
      groups.add(group);
    }
    return groups;
  }

  /// Creates or renames one address-book group. Reusing a deleted group id
  /// revives it on the server. A positive [ifRevision] must match the live
  /// group and makes a stale rename fail with a 409.
  /// Creates or partially updates an address-book group.
  ///
  /// Omitted fields keep the server value. A null [parentId] leaves the
  /// parent alone while an empty string moves the group to the root; a null
  /// [name] leaves the name alone. At least one field must be present.
  static Future<AddressBookGroup> updateAddressBookGroup(
    ServerSettings settings,
    String token,
    String groupId,
    String? name, {
    String? parentId,
    int? position,
    int? ifRevision,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final String id = groupId.trim();
    if (id.isEmpty) {
      throw ServerApiException(tr('分组 ID 为空'));
    }
    final String? cleanName = name?.trim();
    if (cleanName != null && cleanName.isEmpty) {
      throw ServerApiException(tr('分组名称不能为空'));
    }
    if (position != null && position < 0) {
      throw ServerApiException(tr('排序位置不能为负数'));
    }
    if (cleanName == null && parentId == null && position == null) {
      throw ServerApiException(tr('分组名称、上级分组或排序位置至少需要一项'));
    }
    final Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}/v1/addressbook/groups/${Uri.encodeComponent(id)}',
      );
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Map<String, Object?> body = <String, Object?>{};
    if (cleanName != null) {
      body['name'] = cleanName;
    }
    if (parentId != null) {
      body['parent_id'] = parentId.trim();
    }
    if (position != null) {
      body['position'] = position;
    }
    if (ifRevision != null && ifRevision > 0) {
      body['if_revision'] = ifRevision;
    }
    final Object? decoded = await _requestJson(
      'PATCH',
      uri,
      timeout,
      accountToken: accountToken,
      body: body,
      unauthorized: tr('账号登录已失效，请重新登录'),
    );
    final AddressBookGroup? group = AddressBookGroup.fromJson(decoded);
    if (group == null || group.id != id) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return group;
  }

  /// Deletes an address-book group. The server moves its peers back to the
  /// ungrouped list before tombstoning the group. A positive [ifRevision]
  /// rejects a stale delete with a 409.
  static Future<void> deleteAddressBookGroup(
    ServerSettings settings,
    String token,
    String groupId, {
    int? ifRevision,
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final String id = groupId.trim();
    if (id.isEmpty) {
      throw ServerApiException(tr('分组 ID 为空'));
    }
    Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}/v1/addressbook/groups/${Uri.encodeComponent(id)}',
      );
      if (ifRevision != null && ifRevision > 0) {
        uri = uri.replace(
          queryParameters: <String, String>{'if_revision': '$ifRevision'},
        );
      }
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'DELETE',
      uri,
      timeout,
      accountToken: accountToken,
      success: <int>{HttpStatus.ok, HttpStatus.noContent},
      unauthorized: tr('账号登录已失效，请重新登录'),
      notFound: tr('地址簿分组不存在'),
    );
  }

  /// Creates or partially updates an address-book entry.
  ///
  /// Omitted fields keep the server value; an empty [name] or [tags] clears
  /// that field, and an empty [groupId] moves the peer to the ungrouped list.
  /// At least one field must be present. [ifRevision] is the revision the edit
  /// is based on; the server rejects a stale value with a 409.
  static Future<AddressBookPeer> updateAddressBookPeer(
    ServerSettings settings,
    String token,
    String peerId, {
    String? name,
    List<String>? tags,
    String? groupId,
    int? position,
    int? ifRevision,
    Duration timeout = const Duration(seconds: 6),
  }) {
    return _updatePeer(
      settings,
      token,
      '/v1/addressbook',
      peerId,
      name: name,
      tags: tags,
      groupId: groupId,
      position: position,
      ifRevision: ifRevision,
      timeout: timeout,
      notFound: tr('地址簿条目不存在'),
    );
  }

  /// Updates a peer in a shared book. The server requires write or control
  /// permission and applies the edit to the owner's book.
  static Future<AddressBookPeer> updateSharedAddressBookPeer(
    ServerSettings settings,
    String token,
    String owner,
    String peerId, {
    String? name,
    List<String>? tags,
    int? ifRevision,
    Duration timeout = const Duration(seconds: 6),
  }) {
    final String ownerName = owner.trim();
    if (ownerName.isEmpty) {
      throw ServerApiException(tr('用户名不能为空'));
    }
    return _updatePeer(
      settings,
      token,
      '/v1/addressbook/shared/${Uri.encodeComponent(ownerName)}',
      peerId,
      name: name,
      tags: tags,
      ifRevision: ifRevision,
      timeout: timeout,
      notFound: tr('地址簿条目不存在'),
      forbidden: tr('共享地址簿权限不足'),
    );
  }

  /// Adds a peer to a shared book. The server requires write or control
  /// permission and writes it into the owner's book; [groupId] places it in
  /// one group and a group grant may only target its own group.
  static Future<AddressBookPeer> addSharedAddressBookPeer(
    ServerSettings settings,
    String token,
    String owner,
    String peerId, {
    String? name,
    List<String>? tags,
    String groupId = '',
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final String ownerName = owner.trim();
    if (ownerName.isEmpty) {
      throw ServerApiException(tr('用户名不能为空'));
    }
    final String id = peerId.trim();
    if (id.isEmpty) {
      throw ServerApiException(tr('设备 ID 为空'));
    }
    final Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}/v1/addressbook/shared/${Uri.encodeComponent(ownerName)}/peers',
      );
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final List<String> cleaned = <String>[];
    final Set<String> seen = <String>{};
    for (final String raw in tags ?? <String>[]) {
      final String tag = raw.trim();
      if (tag.isNotEmpty && seen.add(tag)) {
        cleaned.add(tag);
      }
    }
    final Object? decoded = await _requestJson(
      'POST',
      uri,
      timeout,
      accountToken: accountToken,
      body: <String, Object?>{
        'id': id,
        'name': name?.trim() ?? '',
        'tags': cleaned,
        if (groupId.trim().isNotEmpty) 'group_id': groupId.trim(),
      },
      unauthorized: tr('账号登录已失效，请重新登录'),
      notFound: tr('地址簿未共享给当前账号'),
      forbidden: tr('共享地址簿权限不足'),
    );
    final AddressBookPeer? peer = AddressBookPeer.fromJson(decoded);
    if (peer == null || peer.id != id) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return peer;
  }

  static Future<AddressBookPeer> _updatePeer(
    ServerSettings settings,
    String token,
    String bookPath,
    String peerId, {
    String? name,
    List<String>? tags,
    String? groupId,
    int? position,
    int? ifRevision,
    required Duration timeout,
    required String notFound,
    String? forbidden,
  }) async {
    forbidden ??= tr('没有权限执行此操作');
    final String accountToken = _accountToken(settings, token);
    final String id = peerId.trim();
    if (id.isEmpty) {
      throw ServerApiException(tr('设备 ID 为空'));
    }
    if (name == null && tags == null && groupId == null && position == null) {
      throw ServerApiException(tr('没有可更新的地址簿字段'));
    }
    if (position != null && position < 0) {
      throw ServerApiException(tr('排序位置不能为负数'));
    }
    final Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}$bookPath/peers/${Uri.encodeComponent(id)}',
      );
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Map<String, Object?> body = <String, Object?>{};
    if (name != null) {
      body['name'] = name.trim();
    }
    if (tags != null) {
      final List<String> cleaned = <String>[];
      final Set<String> seen = <String>{};
      for (final String raw in tags) {
        final String tag = raw.trim();
        if (tag.isNotEmpty && seen.add(tag)) {
          cleaned.add(tag);
        }
      }
      body['tags'] = cleaned;
    }
    if (groupId != null) {
      body['group_id'] = groupId.trim();
    }
    if (position != null) {
      body['position'] = position;
    }
    if (ifRevision != null && ifRevision > 0) {
      body['if_revision'] = ifRevision;
    }
    final Object? decoded = await _requestJson(
      'PATCH',
      uri,
      timeout,
      accountToken: accountToken,
      body: body,
      unauthorized: tr('账号登录已失效，请重新登录'),
      notFound: notFound,
      forbidden: forbidden,
    );
    final AddressBookPeer? peer = AddressBookPeer.fromJson(decoded);
    if (peer == null || peer.id != id) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return peer;
  }

  static Future<void> deleteAddressBookPeer(
    ServerSettings settings,
    String token,
    String peerId, {
    Duration timeout = const Duration(seconds: 6),
  }) {
    return _deletePeer(
      settings,
      token,
      '/v1/addressbook',
      peerId,
      timeout: timeout,
      notFound: tr('地址簿条目不存在'),
    );
  }

  /// Deletes a peer from a shared book. The server requires write or control
  /// permission and writes a tombstone into the owner's book.
  static Future<void> deleteSharedAddressBookPeer(
    ServerSettings settings,
    String token,
    String owner,
    String peerId, {
    int? ifRevision,
    Duration timeout = const Duration(seconds: 6),
  }) {
    final String ownerName = owner.trim();
    if (ownerName.isEmpty) {
      throw ServerApiException(tr('用户名不能为空'));
    }
    return _deletePeer(
      settings,
      token,
      '/v1/addressbook/shared/${Uri.encodeComponent(ownerName)}',
      peerId,
      ifRevision: ifRevision,
      timeout: timeout,
      notFound: tr('地址簿条目不存在'),
      forbidden: tr('共享地址簿权限不足'),
    );
  }

  static Future<void> _deletePeer(
    ServerSettings settings,
    String token,
    String bookPath,
    String peerId, {
    int? ifRevision,
    required Duration timeout,
    required String notFound,
    String? forbidden,
  }) async {
    forbidden ??= tr('没有权限执行此操作');
    final String accountToken = _accountToken(settings, token);
    final String id = peerId.trim();
    if (id.isEmpty) {
      throw ServerApiException(tr('设备 ID 为空'));
    }
    Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}$bookPath/peers/${Uri.encodeComponent(id)}',
      );
      if (ifRevision != null && ifRevision > 0) {
        uri = uri.replace(
          queryParameters: <String, String>{'if_revision': '$ifRevision'},
        );
      }
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'DELETE',
      uri,
      timeout,
      accountToken: accountToken,
      success: const <int>{HttpStatus.ok, HttpStatus.noContent},
      unauthorized: tr('账号登录已失效，请重新登录'),
      notFound: notFound,
      forbidden: forbidden,
    );
  }

  /// Lists users who may read this address book and books shared with this
  /// account.
  static Future<AddressBookGrants> addressBookGrants(
    ServerSettings settings,
    String token, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/addressbook/grants');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'GET',
      uri,
      timeout,
      accountToken: accountToken,
      unauthorized: tr('账号登录已失效，请重新登录'),
    );
    return _parseGrants(decoded);
  }

  /// Lists the grants on a shared book the signed-in user controls.
  static Future<AddressBookGrants> sharedAddressBookGrants(
    ServerSettings settings,
    String token,
    String owner, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final String ownerName = owner.trim();
    if (ownerName.isEmpty) {
      throw ServerApiException(tr('用户名不能为空'));
    }
    final Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}/v1/addressbook/shared/${Uri.encodeComponent(ownerName)}/grants',
      );
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'GET',
      uri,
      timeout,
      accountToken: accountToken,
      unauthorized: tr('账号登录已失效，请重新登录'),
      notFound: tr('地址簿未共享给当前账号'),
      forbidden: tr('共享地址簿权限不足'),
    );
    return _parseGrants(decoded);
  }

  static AddressBookGrants _parseGrants(Object? decoded) {
    final Object? rawOutgoing = decoded is Map ? decoded['outgoing'] : null;
    final Object? rawIncoming = decoded is Map ? decoded['incoming'] : null;
    if (rawOutgoing is! List || rawIncoming is! List) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    final List<AddressBookGrant> outgoing = <AddressBookGrant>[];
    final List<AddressBookGrant> incoming = <AddressBookGrant>[];
    for (final Object? item in rawOutgoing) {
      final AddressBookGrant? grant = AddressBookGrant.fromJson(item);
      if (grant == null) {
        throw ServerApiException(tr('连接服务返回了无效数据'));
      }
      outgoing.add(grant);
    }
    for (final Object? item in rawIncoming) {
      final AddressBookGrant? grant = AddressBookGrant.fromJson(item);
      if (grant == null) {
        throw ServerApiException(tr('连接服务返回了无效数据'));
      }
      incoming.add(grant);
    }
    return AddressBookGrants(outgoing: outgoing, incoming: incoming);
  }

  /// Shares this account's address book with [username] at [permission].
  /// A non-empty [groupId] limits the grant to that group.
  static Future<AddressBookGrant> shareAddressBook(
    ServerSettings settings,
    String token,
    String username, {
    String permission = 'read',
    String groupId = '',
    Duration timeout = const Duration(seconds: 6),
  }) {
    return _shareBook(
      settings,
      token,
      '/v1/addressbook/grants',
      username,
      permission,
      groupId,
      timeout,
    );
  }

  /// Grants access to a shared book the signed-in user controls.
  static Future<AddressBookGrant> shareSharedAddressBook(
    ServerSettings settings,
    String token,
    String owner,
    String username, {
    String permission = 'read',
    String groupId = '',
    Duration timeout = const Duration(seconds: 6),
  }) {
    final String ownerName = owner.trim();
    if (ownerName.isEmpty) {
      throw ServerApiException(tr('用户名不能为空'));
    }
    return _shareBook(
      settings,
      token,
      '/v1/addressbook/shared/${Uri.encodeComponent(ownerName)}/grants',
      username,
      permission,
      groupId,
      timeout,
    );
  }

  static Future<AddressBookGrant> _shareBook(
    ServerSettings settings,
    String token,
    String grantsPath,
    String username,
    String permission,
    String groupId,
    Duration timeout,
  ) async {
    final String accountToken = _accountToken(settings, token);
    final String target = username.trim();
    if (target.isEmpty) {
      throw ServerApiException(tr('用户名不能为空'));
    }
    final String level = permission.trim().toLowerCase();
    if (!addressBookPermissions.contains(level)) {
      throw ServerApiException(tr('地址簿权限无效'));
    }
    final String scope = groupId.trim();
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}$grantsPath');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Map<String, Object?> body = <String, Object?>{
      'username': target,
      'permission': level,
    };
    if (scope.isNotEmpty) {
      body['group_id'] = scope;
    }
    final Object? decoded = await _requestJson(
      'POST',
      uri,
      timeout,
      accountToken: accountToken,
      body: body,
      unauthorized: tr('账号登录已失效，请重新登录'),
      notFound: tr('用户或分组不存在'),
      forbidden: tr('共享地址簿权限不足'),
    );
    final AddressBookGrant? grant = AddressBookGrant.fromJson(decoded);
    if (grant == null) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return grant;
  }

  /// Revokes a share previously granted to [username]. Without [groupId] every
  /// scope is revoked; with one (an empty string included) only that scope is.
  static Future<void> unshareAddressBook(
    ServerSettings settings,
    String token,
    String username, {
    String? groupId,
    Duration timeout = const Duration(seconds: 6),
  }) {
    return _unshareBook(
      settings,
      token,
      '/v1/addressbook/grants',
      username,
      groupId,
      timeout,
    );
  }

  /// Revokes a grant on a shared book the signed-in user controls.
  static Future<void> unshareSharedAddressBook(
    ServerSettings settings,
    String token,
    String owner,
    String username, {
    String? groupId,
    Duration timeout = const Duration(seconds: 6),
  }) {
    final String ownerName = owner.trim();
    if (ownerName.isEmpty) {
      throw ServerApiException(tr('用户名不能为空'));
    }
    return _unshareBook(
      settings,
      token,
      '/v1/addressbook/shared/${Uri.encodeComponent(ownerName)}/grants',
      username,
      groupId,
      timeout,
    );
  }

  static Future<void> _unshareBook(
    ServerSettings settings,
    String token,
    String grantsPath,
    String username,
    String? groupId,
    Duration timeout,
  ) async {
    final String accountToken = _accountToken(settings, token);
    final String target = username.trim();
    if (target.isEmpty) {
      throw ServerApiException(tr('用户名不能为空'));
    }
    final Uri uri;
    try {
      final Uri base = Uri.parse(
        '${settings.url}$grantsPath/${Uri.encodeComponent(target)}',
      );
      uri = groupId == null
          ? base
          : base.replace(
              queryParameters: <String, String>{'group_id': groupId.trim()},
            );
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'DELETE',
      uri,
      timeout,
      accountToken: accountToken,
      success: <int>{HttpStatus.ok, HttpStatus.noContent},
      unauthorized: tr('账号登录已失效，请重新登录'),
      notFound: tr('该用户没有共享地址簿'),
      forbidden: tr('共享地址簿权限不足'),
    );
  }

  /// Fetches every address book shared with this account, whole rather than
  /// through an incremental cursor.
  static Future<List<SharedAddressBook>> sharedAddressBooks(
    ServerSettings settings,
    String token, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/addressbook/shared');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'GET',
      uri,
      timeout,
      accountToken: accountToken,
      unauthorized: tr('账号登录已失效，请重新登录'),
    );
    final Object? rawBooks = decoded is Map ? decoded['books'] : null;
    if (rawBooks is! List) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    final List<SharedAddressBook> books = <SharedAddressBook>[];
    for (final Object? item in rawBooks) {
      final SharedAddressBook? book = SharedAddressBook.fromJson(item);
      if (book == null) {
        throw ServerApiException(tr('连接服务返回了无效数据'));
      }
      books.add(book);
    }
    return books;
  }

  /// Lists the signed-in account's uploaded recordings with its quota state.
  static Future<RecordingArchive> listRecordings(
    ServerSettings settings,
    String token, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/account/recordings');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson(
      'GET',
      uri,
      timeout,
      accountToken: accountToken,
      unauthorized: tr('账号登录已失效，请重新登录'),
      unavailable: tr('连接服务未启用录制归档'),
    );
    final RecordingArchive? archive = RecordingArchive.fromJson(decoded);
    if (archive == null) {
      throw ServerApiException(tr('连接服务返回了无效数据'));
    }
    return archive;
  }

  /// Streams one local MP4 into the account archive. Metadata travels in the
  /// query string so a UTF-8 name survives; the body stays raw bytes, which
  /// keeps the server from buffering a multi-gigabyte recording.
  static Future<RemoteRecording> uploadRecording(
    ServerSettings settings,
    String token, {
    required File file,
    String name = '',
    String peerId = '',
    int startedAtMs = 0,
    int durationMs = 0,
    void Function(int sent, int total)? onProgress,
    Duration timeout = const Duration(minutes: 10),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final int total = await file.length();
    if (total <= 0) {
      throw ServerApiException(tr('录制文件为空'));
    }
    final String fileName = name.trim().isEmpty
        ? _fileName(file.path)
        : name.trim();
    final Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}/v1/account/recordings',
      ).replace(queryParameters: <String, String>{
        'name': fileName,
        if (peerId.trim().isNotEmpty) 'peer_id': peerId.trim(),
        if (startedAtMs > 0) 'started_ms': '$startedAtMs',
        if (durationMs > 0) 'duration_ms': '$durationMs',
      });
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final HttpClient client = HttpClient()..connectionTimeout = timeout;
    try {
      final HttpClientRequest request = await client
          .postUrl(uri)
          .timeout(timeout);
      request.headers.set(HttpHeaders.acceptHeader, 'application/json');
      request.headers.set(
        HttpHeaders.authorizationHeader,
        'Bearer $accountToken',
      );
      request.headers.contentType = ContentType('video', 'mp4');
      request.contentLength = total;
      int sent = 0;
      await request.addStream(file.openRead().map((List<int> chunk) {
        sent += chunk.length;
        onProgress?.call(sent, total);
        return chunk;
      }));
      final HttpClientResponse response = await request.close().timeout(
        timeout,
      );
      final String text = await response
          .transform(utf8.decoder)
          .join()
          .timeout(timeout);
      if (response.statusCode != HttpStatus.created) {
        throw _recordingFailure(response.statusCode, text);
      }
      final Object? decoded = text.trim().isEmpty ? null : jsonDecode(text);
      final RemoteRecording? recording = decoded is Map
          ? RemoteRecording.fromJson(decoded['recording'])
          : null;
      if (recording == null) {
        throw ServerApiException(tr('连接服务返回了无效数据'));
      }
      return recording;
    } on ServerApiException {
      rethrow;
    } on TimeoutException {
      throw ServerApiException(tr('连接服务超时'));
    } on SocketException catch (error) {
      final String detail = error.message.trim();
      throw ServerApiException(
        detail.isEmpty
            ? tr('无法连接连接服务')
            : tr('无法连接连接服务：{detail}', <String, Object?>{'detail': detail}),
      );
    } on HttpException catch (error) {
      throw ServerApiException(
        tr('连接服务请求失败：{p62}', <String, Object?>{'p62': error.message}),
      );
    } on FileSystemException catch (error) {
      throw ServerApiException(
        tr('读取录制文件失败：{error}', <String, Object?>{'error': error.message}),
      );
    } catch (error) {
      throw ServerApiException(
        tr('连接服务请求失败：{error}', <String, Object?>{'error': error}),
      );
    } finally {
      client.close(force: true);
    }
  }

  /// Saves one archived recording under [directory], picking a non-existing
  /// name so a second download cannot overwrite the first.
  static Future<File> downloadRecording(
    ServerSettings settings,
    String token,
    RemoteRecording recording, {
    required Directory directory,
    Duration timeout = const Duration(minutes: 10),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final String id = recording.id.trim();
    if (id.isEmpty) {
      throw ServerApiException(tr('录制不存在或已被删除'));
    }
    final Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}/v1/account/recordings/${Uri.encodeComponent(id)}',
      );
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final HttpClient client = HttpClient()..connectionTimeout = timeout;
    try {
      final HttpClientRequest request = await client
          .getUrl(uri)
          .timeout(timeout);
      request.headers.set(
        HttpHeaders.authorizationHeader,
        'Bearer $accountToken',
      );
      final HttpClientResponse response = await request.close().timeout(
        timeout,
      );
      if (response.statusCode != HttpStatus.ok) {
        final String text = await response
            .transform(utf8.decoder)
            .join()
            .timeout(timeout);
        throw _recordingFailure(response.statusCode, text);
      }
      await directory.create(recursive: true);
      final File target = await _newRecordingFile(directory, recording.name);
      final IOSink sink = target.openWrite();
      try {
        await for (final List<int> chunk in response.timeout(timeout)) {
          sink.add(chunk);
        }
        await sink.flush();
        await sink.close();
      } catch (_) {
        await sink.close();
        try {
          await target.delete();
        } catch (_) {
          // The partial file is reported by the original error.
        }
        rethrow;
      }
      return target;
    } on ServerApiException {
      rethrow;
    } on TimeoutException {
      throw ServerApiException(tr('连接服务超时'));
    } on SocketException catch (error) {
      final String detail = error.message.trim();
      throw ServerApiException(
        detail.isEmpty
            ? tr('无法连接连接服务')
            : tr('无法连接连接服务：{detail}', <String, Object?>{'detail': detail}),
      );
    } on FileSystemException catch (error) {
      throw ServerApiException(
        tr('保存录制文件失败：{error}', <String, Object?>{'error': error.message}),
      );
    } catch (error) {
      throw ServerApiException(
        tr('下载录制失败：{error}', <String, Object?>{'error': error}),
      );
    } finally {
      client.close(force: true);
    }
  }

  static Future<void> deleteRecording(
    ServerSettings settings,
    String token,
    String recordingId, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    final String accountToken = _accountToken(settings, token);
    final String id = recordingId.trim();
    if (id.isEmpty) {
      throw ServerApiException(tr('录制不存在或已被删除'));
    }
    final Uri uri;
    try {
      uri = Uri.parse(
        '${settings.url}/v1/account/recordings/${Uri.encodeComponent(id)}',
      );
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _requestJson(
      'DELETE',
      uri,
      timeout,
      accountToken: accountToken,
      success: const <int>{HttpStatus.noContent},
      unauthorized: tr('账号登录已失效，请重新登录'),
      notFound: tr('录制不存在或已被删除'),
      unavailable: tr('连接服务未启用录制归档'),
    );
  }

  /// Turns a failed recording request into the same user-facing reasons the
  /// upload and archive dialogs show. The server answers 413 for both the
  /// single-file limit and the account quota, so the body decides.
  static ServerApiException _recordingFailure(int status, String body) {
    String detail = '';
    try {
      final Object? decoded = jsonDecode(body);
      if (decoded is Map) {
        detail = decoded['error']?.toString().trim() ?? '';
      }
    } on FormatException {
      // A proxy may answer with HTML; the status alone still explains it.
    }
    switch (status) {
      case HttpStatus.badRequest:
        return ServerApiException(tr('录制文件无效'), statusCode: status);
      case HttpStatus.unauthorized:
        return ServerApiException(
          tr('账号登录已失效，请重新登录'),
          statusCode: status,
        );
      case HttpStatus.notFound:
        return ServerApiException(
          tr('录制不存在或已被删除'),
          statusCode: status,
        );
      case HttpStatus.requestEntityTooLarge:
        if (detail.contains('quota')) {
          return ServerApiException(
            tr('录制存储配额已用尽'),
            statusCode: status,
          );
        }
        return ServerApiException(
          tr('录制文件超过单次上传上限'),
          statusCode: status,
        );
      case HttpStatus.serviceUnavailable:
        return ServerApiException(
          tr('连接服务未启用录制归档'),
          statusCode: status,
        );
      default:
        return ServerApiException(
          tr('连接服务返回 HTTP {p61}', <String, Object?>{'p61': status}),
          statusCode: status,
        );
    }
  }

  static String _fileName(String path) {
    final String normalized = path.replaceAll('\\', '/');
    final int slash = normalized.lastIndexOf('/');
    final String name = slash < 0 ? normalized : normalized.substring(slash + 1);
    return name.trim().isEmpty ? 'recording.mp4' : name;
  }

  static Future<File> _newRecordingFile(Directory directory, String name) async {
    String safe = name.replaceAll(RegExp(r'[\\/:*?"<>|\x00-\x1f]'), '_').trim();
    if (safe.isEmpty) {
      safe = 'recording.mp4';
    }
    if (!safe.toLowerCase().endsWith('.mp4')) {
      safe = '$safe.mp4';
    }
    final String separator = Platform.pathSeparator;
    File file = File('${directory.path}$separator$safe');
    if (!await file.exists()) {
      return file;
    }
    final int dot = safe.lastIndexOf('.');
    final String stem = dot > 0 ? safe.substring(0, dot) : safe;
    final String extension = dot > 0 ? safe.substring(dot) : '';
    for (int index = 1; index < 1000; index++) {
      file = File('${directory.path}$separator$stem ($index)$extension');
      if (!await file.exists()) {
        return file;
      }
    }
    return File(
      '${directory.path}$separator$stem-'
      '${DateTime.now().millisecondsSinceEpoch}$extension',
    );
  }

  static String _accountToken(ServerSettings settings, String token) {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final String accountToken = token.trim();
    if (accountToken.isEmpty) {
      throw ServerApiException(tr('账号未登录'));
    }
    return accountToken;
  }

  /// Reads the server's long-term Ed25519 key from `/v1/server`. The route is
  /// deliberately unauthenticated: a viewer can refuse an unknown server
  /// before it presents a key or lookup answer.
  static Future<ServerIdentity> serverIdentity(
    ServerSettings settings, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/server');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    final Object? decoded = await _requestJson('GET', uri, timeout);
    final Object? raw = decoded is Map ? decoded['public_key'] : null;
    final ServerIdentity? identity = ServerIdentity.parse(
      raw?.toString() ?? '',
    );
    if (identity == null) {
      throw ServerApiException(tr('连接服务未提供有效的服务器身份'));
    }
    return identity;
  }

  /// Looks up a reserved id to verify reachability and [ServerSettings.key] in
  /// one call. The server authorizes before touching the registry, so a 404
  /// proves the key was accepted; a 401 means it was missing or wrong. The
  /// id is valid but not a host this project registers, so no real device is
  /// exposed to the check.
  static Future<void> check(
    ServerSettings settings, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    try {
      await lookup(settings, 'chast-probe', timeout: timeout);
    } on ServerApiException catch (error) {
      if (error.statusCode == HttpStatus.notFound) {
        return;
      }
      rethrow;
    }
  }

  static Future<void> health(
    ServerSettings settings, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw ServerApiException(tr('未配置连接服务'));
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/healthz');
    } on FormatException {
      throw ServerApiException(tr('连接服务地址无效'));
    }
    await _getJson(uri, timeout);
  }

  static Future<Object?> _getJson(
    Uri uri,
    Duration timeout, {
    String key = '',
  }) {
    return _requestJson('GET', uri, timeout, key: key);
  }

  static Future<Object?> _requestJson(
    String method,
    Uri uri,
    Duration timeout, {
    String key = '',
    String accountToken = '',
    Object? body,
    Set<int> success = const <int>{HttpStatus.ok},
    String? unauthorized,
    String? notFound,
    String? forbidden,
    String badRequest = '',
    String conflict = '',
    String? unavailable,
  }) async {
    unauthorized ??= tr('连接服务密钥无效');
    notFound ??= tr('设备未注册或已下线');
    forbidden ??= tr('没有权限执行此操作');
    unavailable ??= tr('连接服务未启用账号存储');
    final HttpClient client = HttpClient()..connectionTimeout = timeout;
    try {
      final HttpClientRequest request = await client
          .openUrl(method, uri)
          .timeout(timeout);
      request.headers.set(HttpHeaders.acceptHeader, 'application/json');
      final String bearer = accountToken.isNotEmpty ? accountToken : key;
      if (bearer.isNotEmpty) {
        request.headers.set(HttpHeaders.authorizationHeader, 'Bearer $bearer');
      }
      if (body != null) {
        request.headers.contentType = ContentType.json;
        request.write(jsonEncode(body));
      }
      final HttpClientResponse response = await request.close().timeout(
        timeout,
      );
      final String text = await response
          .transform(utf8.decoder)
          .join()
          .timeout(timeout);
      if (!success.contains(response.statusCode)) {
        final String? secondFactor = response.headers.value('x-chast-2fa');
        throw ServerApiException(
          response.statusCode == HttpStatus.notFound
              ? notFound
              : response.statusCode == HttpStatus.unauthorized
              ? unauthorized
              : response.statusCode == HttpStatus.forbidden
              ? forbidden
              : response.statusCode == HttpStatus.tooManyRequests
              ? tr('请求过于频繁，请稍后再试')
              : response.statusCode == HttpStatus.serviceUnavailable
              ? unavailable
              : response.statusCode == HttpStatus.badRequest &&
                    badRequest.isNotEmpty
              ? badRequest
              : response.statusCode == HttpStatus.conflict &&
                    conflict.isNotEmpty
              ? conflict
              : tr('连接服务返回 HTTP {p61}', <String, Object?>{'p61': response.statusCode}),
          statusCode: response.statusCode,
          code: secondFactor == 'required'
              ? 'totp_required'
              : secondFactor == 'invalid'
              ? 'totp_invalid'
              : '',
        );
      }
      if (text.trim().isEmpty) {
        return null;
      }
      try {
        return jsonDecode(text);
      } on FormatException {
        throw ServerApiException(tr('连接服务返回了无效 JSON'));
      }
    } on ServerApiException {
      rethrow;
    } on TimeoutException {
      throw ServerApiException(tr('连接服务超时'));
    } on SocketException catch (error) {
      final String detail = error.message.trim();
      throw ServerApiException(
        detail.isEmpty ? tr('无法连接连接服务') : tr('无法连接连接服务：{detail}', <String, Object?>{'detail': detail}),
      );
    } on HttpException catch (error) {
      throw ServerApiException(tr('连接服务请求失败：{p62}', <String, Object?>{'p62': error.message}));
    } catch (error) {
      throw ServerApiException(tr('连接服务请求失败：{error}', <String, Object?>{'error': error}));
    } finally {
      client.close(force: true);
    }
  }
}
