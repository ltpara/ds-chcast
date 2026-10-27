import 'dart:convert';
import 'dart:io';

import 'package:chast_viewer/account_store.dart';
import 'package:chast_viewer/address_book_sync.dart';
import 'package:chast_viewer/device_store.dart';
import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';

/// Live probe for the viewer account client against a running chast-server.
///
/// Run with: dart run tool/account_sync_smoke.dart [url] [username] [password]
Future<void> main(List<String> args) async {
  final String base = args.isNotEmpty ? args[0] : 'http://127.0.0.1:18643';
  final String username = args.length > 1 ? args[1] : 'viewer';
  final String password = args.length > 2 ? args[2] : 'ViewerPass123';
  if (args.length >= 8 && args[3] == 'share') {
    await _shareProbe(
      base: base,
      owner: username,
      ownerPassword: password,
      grantee: args[4],
      granteePassword: args[5],
      control: args[6],
      controlPassword: args[7],
    );
    return;
  }
  if (args.length >= 5 && (args[3] == 'prepare' || args[3] == 'check')) {
    await _restartProbe(
      prepare: args[3] == 'prepare',
      base: base,
      username: username,
      password: password,
      sessionPath: args[4],
    );
    return;
  }
  final ServerSettings settings = ServerSettings(url: base);
  int failed = 0;
  void check(bool ok, String message) {
    stdout.writeln(ok ? 'ok $message' : 'FAIL $message');
    if (!ok) {
      failed++;
    }
  }

  final AccountLoginResult login = await ServerApi.login(
    settings,
    username: username,
    password: password,
  );
  check(
    login.token.isNotEmpty && login.username == username,
    'login returns a token and the canonical username',
  );
  check(
    login.refreshToken.isNotEmpty &&
        login.refreshExpiresInMs > login.expiresInMs,
    'login returns a refresh token that outlives the access token',
  );
  final String token = login.token;

  final Directory temp = await Directory.systemTemp.createTemp(
    'chast-account-smoke-',
  );
  try {
    final AccountSessionStore store = AccountSessionStore(
      file: File('${temp.path}/account.json'),
    );
    final AccountSession session = AccountSession(
      server: base,
      username: login.username,
      token: token,
      expiresAt: DateTime.now().toUtc().add(
        Duration(milliseconds: login.expiresInMs),
      ),
      refreshToken: login.refreshToken,
      refreshExpiresAt: login.refreshToken.isEmpty
          ? null
          : DateTime.now().toUtc().add(
              Duration(milliseconds: login.refreshExpiresInMs),
            ),
    );
    await store.save(session);
    final AccountSession? loaded = await store.load();
    check(
      loaded != null &&
          loaded.token == token &&
          loaded.refreshToken == login.refreshToken &&
          loaded.isActiveAt(DateTime.now()),
      'account session round-trips through account.json',
    );

    try {
      await ServerApi.addressBook(settings, 'not-a-token');
      check(false, 'an invalid account token is rejected');
    } on ServerApiException catch (error) {
      check(error.statusCode == 401, 'an invalid account token is rejected');
    }

    final String stamp = DateTime.now().millisecondsSinceEpoch.toString();
    final String peerId = 'smoke-$stamp';
    final String remoteId = 'smoke-remote-$stamp';
    final String localId = 'smoke-local-$stamp';
    await ServerApi.updateAddressBookPeer(
      settings,
      token,
      peerId,
      name: '工作区',
      tags: <String>['lab', 'lab', 'vip'],
    );
    await ServerApi.updateAddressBookPeer(
      settings,
      token,
      remoteId,
      name: '远端设备',
      tags: <String>['shared'],
    );

    final AddressBookSnapshot booked = await ServerApi.addressBook(
      settings,
      token,
    );
    final List<AddressBookPeer> peers = booked.peers;
    final AddressBookPeer saved = peers.firstWhere(
      (AddressBookPeer peer) => peer.id == peerId,
    );
    check(
      saved.name == '工作区' && saved.tags.join(',') == 'lab,vip',
      'peer list parses the name and de-duplicated tags',
    );
    check(
      booked.revision > 0 && saved.revision > 0,
      'the address book exposes a positive revision',
    );

    final DateTime now = DateTime.now();
    final AddressBookPlan plan = planAddressBookSync(
      devices: <Device>[
        Device(
          id: peerId,
          name: '本机命名',
          server: base,
          tags: <String>['vip-local'],
          saved: true,
          addedAt: now,
        ),
        Device(
          id: localId,
          name: '仅本机',
          server: base,
          tags: <String>['local'],
          saved: true,
          addedAt: now,
        ),
      ],
      peers: peers,
      serverUrl: base,
      now: now,
    );
    check(
      plan.added == 1 &&
          plan.devices.any(
            (Device device) =>
                device.id == remoteId &&
                device.name == '远端设备' &&
                device.tags.join(',') == 'shared',
          ),
      'a server-only peer is merged with its name and tags',
    );
    check(
      plan.pushes.length == 2 &&
          plan.pushes
                  .firstWhere((AddressBookPush push) => push.id == peerId)
                  .tags
                  ?.join(',') ==
              'vip-local',
      'a conflicting name/tags and a local-only peer are queued for upload',
    );
    for (final AddressBookPush push in plan.pushes) {
      await ServerApi.updateAddressBookPeer(
        settings,
        token,
        push.id,
        name: push.name,
        tags: push.tags,
      );
    }

    final AddressBookSnapshot after = await ServerApi.addressBook(
      settings,
      token,
    );
    final AddressBookPeer renamed = after.peers.firstWhere(
      (AddressBookPeer peer) => peer.id == peerId,
    );
    check(renamed.name == '本机命名', 'the local name wins on upload');
    check(
      renamed.tags.join(',') == 'vip-local',
      'the local tag list wins on upload',
    );
    final AddressBookPeer localOnly = after.peers.firstWhere(
      (AddressBookPeer peer) => peer.id == localId,
    );
    check(
      localOnly.name == '仅本机' && localOnly.tags.join(',') == 'local',
      'the local-only peer reaches the server with its tags',
    );

    final AddressBookSnapshot delta = await ServerApi.addressBook(
      settings,
      token,
      since: booked.revision,
    );
    check(
      delta.revision > booked.revision &&
          delta.changes.any((AddressBookPeer peer) => peer.id == peerId) &&
          delta.changes.any((AddressBookPeer peer) => peer.id == localId) &&
          delta.tombstones.isEmpty,
      'an incremental read returns only the changes after the stored revision',
    );

    await ServerApi.updateAddressBookPeer(
      settings,
      token,
      localId,
      tags: const <String>[],
    );
    final AddressBookSnapshot tagCleared = await ServerApi.addressBook(
      settings,
      token,
    );
    final AddressBookPeer clearedLocal = tagCleared.peers.firstWhere(
      (AddressBookPeer peer) => peer.id == localId,
    );
    check(
      clearedLocal.name == '仅本机' && clearedLocal.tags.isEmpty,
      'clearing tags keeps the peer name',
    );

    final String groupId = 'smoke-group-$stamp';
    final AddressBookGroup createdGroup = await ServerApi.updateAddressBookGroup(
      settings,
      token,
      groupId,
      ' 实验组 ',
    );
    check(
      createdGroup.id == groupId &&
          createdGroup.name == '实验组' &&
          createdGroup.revision > 0,
      'a group is created with a trimmed name',
    );
    final AddressBookGroup renamedGroup = await ServerApi.updateAddressBookGroup(
      settings,
      token,
      groupId,
      '实验室',
    );
    check(
      renamedGroup.name == '实验室' &&
          renamedGroup.revision > createdGroup.revision,
      'renaming a group advances the book revision',
    );
    await ServerApi.updateAddressBookPeer(
      settings,
      token,
      peerId,
      groupId: groupId,
    );
    final AddressBookSnapshot grouped = await ServerApi.addressBook(
      settings,
      token,
    );
    final AddressBookPeer groupedPeer = grouped.peers.firstWhere(
      (AddressBookPeer peer) => peer.id == peerId,
    );
    check(
      groupedPeer.groupId == groupId &&
          grouped.groups.length == 1 &&
          grouped.groups.single.name == '实验室',
      'a peer move and its group reach the address book snapshot',
    );
    final AddressBookPlan groupPlan = planAddressBookSync(
      devices: <Device>[
        Device(
          id: peerId,
          name: '本机命名',
          server: base,
          tags: <String>['vip-local'],
          groupId: groupId,
          saved: true,
          addedAt: now,
        ),
        Device(
          id: localId,
          name: '仅本机',
          server: base,
          groupId: 'gone-group',
          saved: true,
          addedAt: now,
        ),
      ],
      peers: grouped.peers,
      groups: grouped.groups,
      serverUrl: base,
      now: now,
    );
    check(
      groupPlan.pushes.isEmpty &&
          groupPlan.devices
                  .firstWhere((Device device) => device.id == localId)
                  .groupId ==
              '',
      'a live group is kept and a stale local group is cleared without a push',
    );
    final AddressBookSnapshot groupDelta = await ServerApi.addressBook(
      settings,
      token,
      since: grouped.revision,
    );
    check(
      groupDelta.groupChanges.isEmpty && groupDelta.revision == grouped.revision,
      'a caught-up cursor returns no further group changes',
    );
    await ServerApi.deleteAddressBookGroup(settings, token, groupId);
    final AddressBookSnapshot ungroupedDelta = await ServerApi.addressBook(
      settings,
      token,
      since: grouped.revision,
    );
    final AddressBookSnapshot ungroupedAll = await ServerApi.addressBook(
      settings,
      token,
    );
    check(
      ungroupedDelta.groupChanges.any(
            (AddressBookGroup group) => group.id == groupId && group.deleted,
          ) &&
          ungroupedDelta.changes.any(
            (AddressBookPeer peer) => peer.id == peerId && peer.groupId.isEmpty,
          ) &&
          ungroupedAll.groups.every(
            (AddressBookGroup group) => group.id != groupId,
          ) &&
          ungroupedAll.peers.firstWhere(
                (AddressBookPeer peer) => peer.id == peerId,
              ).groupId ==
              '',
      'deleting a group tombstones it and ungroups its peers',
    );
    try {
      await ServerApi.deleteAddressBookGroup(settings, token, groupId);
      check(false, 'deleting a missing group is refused');
    } on ServerApiException catch (error) {
      check(error.statusCode == 404, 'deleting a missing group is refused');
    }
    try {
      await ServerApi.updateAddressBookGroup(
        settings,
        token,
        'bad group',
        '无效',
      );
      check(false, 'an invalid group id is rejected');
    } on ServerApiException catch (error) {
      check(error.statusCode == 400, 'an invalid group id is rejected');
    }

    await ServerApi.deleteAddressBookPeer(settings, token, remoteId);
    final AddressBookSnapshot deleted = await ServerApi.addressBook(
      settings,
      token,
      since: delta.revision,
    );
    check(
      deleted.revision > delta.revision &&
          deleted.tombstones.length == 1 &&
          deleted.tombstones.single.id == remoteId,
      'a deletion reaches another device as a tombstone',
    );
    final AddressBookPlan tombstonePlan = planAddressBookSync(
      devices: <Device>[
        Device(
          id: remoteId,
          name: '远端设备',
          server: base,
          saved: true,
          addedAt: now,
        ),
        Device(
          id: peerId,
          name: '本机命名',
          server: base,
          saved: true,
          addedAt: now,
        ),
      ],
      peers: deleted.peers,
      tombstones: deleted.tombstones,
      serverUrl: base,
      now: now,
    );
    check(
      tombstonePlan.removed == 1 &&
          tombstonePlan.devices.every(
            (Device device) => device.id != remoteId,
          ) &&
          tombstonePlan.pushes.isEmpty,
      'the tombstone removes the stale local peer and is not re-uploaded',
    );
    final AddressBookSnapshot uptodate = await ServerApi.addressBook(
      settings,
      token,
      since: deleted.revision,
    );
    check(
      uptodate.changes.isEmpty && uptodate.revision == deleted.revision,
      'a caught-up cursor returns no further changes',
    );

    final String offlineId = 'smoke-offline-$stamp';
    await ServerApi.updateAddressBookPeer(
      settings,
      token,
      offlineId,
      name: '离线删除',
    );
    final File devicesFile = File('${temp.path}/devices.json');
    final DeviceStore outbox = DeviceStore(file: devicesFile);
    await outbox.save(
      Device(
        id: offlineId,
        name: '离线删除',
        server: base,
        saved: true,
        addedAt: now,
      ),
    );
    await outbox.queueDeletion(
      PendingDeletion(
        server: base,
        username: login.username,
        id: offlineId,
        deletedAt: now,
      ),
    );
    final DeviceStore reopened = DeviceStore(file: devicesFile);
    final List<PendingDeletion> queued = await reopened.loadPendingDeletions();
    check(
      queued.length == 1 &&
          queued.single.id == offlineId &&
          queued.single.username == login.username &&
          (await reopened.load()).any(
            (Device device) => device.id == offlineId,
          ),
      'an offline deletion persists next to the address book',
    );
    final AddressBookSnapshot withOffline = await ServerApi.addressBook(
      settings,
      token,
    );
    final AddressBookPlan offlinePlan = planAddressBookSync(
      devices: await reopened.load(),
      peers: withOffline.peers,
      pendingDeletions: queued.map((PendingDeletion item) => item.id).toSet(),
      serverUrl: base,
      now: now,
    );
    check(
      offlinePlan.removed == 1 &&
          offlinePlan.devices.every(
            (Device device) => device.id != offlineId,
          ) &&
          offlinePlan.pushes.isEmpty,
      'a queued deletion hides the alive peer and blocks re-upload',
    );
    await ServerApi.deleteAddressBookPeer(settings, token, offlineId);
    await reopened.removePendingDeletion(queued.single.key);
    check(
      (await reopened.loadPendingDeletions()).isEmpty,
      'a successful retry clears the queued deletion',
    );

    await ServerApi.deleteAddressBookPeer(settings, token, peerId);
    await ServerApi.deleteAddressBookPeer(settings, token, localId);
    final AddressBookSnapshot cleared = await ServerApi.addressBook(
      settings,
      token,
    );
    check(
      cleared.peers.every(
        (AddressBookPeer peer) => !peer.id.startsWith('smoke-'),
      ),
      'peers created by this probe are deleted',
    );

    final DateTime startedAt = DateTime.utc(2026, 10, 25, 10);
    final DateTime endedAt = startedAt.add(const Duration(minutes: 3));
    await ServerApi.reportConnection(
      settings,
      token,
      peerId: 'smoke-peer',
      startedAt: startedAt,
      endedAt: endedAt,
      detail: 'probe session',
    );
    try {
      await ServerApi.reportConnection(
        settings,
        token,
        peerId: 'smoke-peer',
        startedAt: endedAt,
        endedAt: startedAt,
      );
      check(false, 'a reversed connection report is refused');
    } on ServerApiException catch (error) {
      check(
        error.statusCode == 400,
        'a reversed connection report is refused',
      );
    }
    final HttpClient http = HttpClient();
    try {
      final HttpClientRequest request = await http.getUrl(
        Uri.parse('$base/v1/account/connections?limit=10'),
      );
      request.headers.set(HttpHeaders.authorizationHeader, 'Bearer $token');
      final HttpClientResponse response = await request.close();
      final Object? decoded = jsonDecode(await utf8.decodeStream(response));
      final List<Object?> events =
          decoded is Map<String, Object?> &&
                  decoded['connections'] is List<Object?>
              ? decoded['connections']! as List<Object?>
              : const <Object?>[];
      Map<String, Object?>? recorded;
      for (final Object? item in events) {
        if (item is Map<String, Object?> && item['peer_id'] == 'smoke-peer') {
          recorded = item;
          break;
        }
      }
      check(
        response.statusCode == 200 &&
            recorded != null &&
            recorded['username'] == login.username &&
            recorded['direction'] == 'viewer' &&
            recorded['started_at_ms'] == startedAt.millisecondsSinceEpoch &&
            recorded['ended_at_ms'] == endedAt.millisecondsSinceEpoch &&
            recorded['duration_ms'] == 180000 &&
            recorded['success'] == true &&
            recorded['detail'] == 'probe session' &&
            recorded['client_ip'] == '127.0.0.1',
        'a reported connection is persisted and readable by the account',
      );
    } finally {
      http.close(force: true);
    }

    final DateTime transferAt = startedAt.add(const Duration(hours: 1));
    await ServerApi.reportFileTransfers(
      settings,
      token,
      peerId: 'smoke-peer',
      direction: 'download',
      name: 'photos.zip',
      size: 900,
      files: 3,
      occurredAt: transferAt,
    );
    final HttpClient filesHttp = HttpClient();
    try {
      final HttpClientRequest request = await filesHttp.getUrl(
        Uri.parse('$base/v1/account/file-transfers?limit=10'),
      );
      request.headers.set(HttpHeaders.authorizationHeader, 'Bearer $token');
      final HttpClientResponse response = await request.close();
      final Object? decoded = jsonDecode(await utf8.decodeStream(response));
      final List<Object?> events =
          decoded is Map<String, Object?> &&
                  decoded['transfers'] is List<Object?>
              ? decoded['transfers']! as List<Object?>
              : const <Object?>[];
      Map<String, Object?>? recorded;
      for (final Object? item in events) {
        if (item is Map<String, Object?> && item['peer_id'] == 'smoke-peer') {
          recorded = item;
          break;
        }
      }
      check(
        response.statusCode == 200 &&
            recorded != null &&
            recorded['username'] == login.username &&
            recorded['direction'] == 'download' &&
            recorded['name'] == 'photos.zip' &&
            recorded['size'] == 900 &&
            recorded['files'] == 3 &&
            recorded['occurred_at_ms'] ==
                transferAt.millisecondsSinceEpoch &&
            recorded['success'] == true &&
            recorded['client_ip'] == '127.0.0.1',
        'a reported file transfer is persisted and readable by the account',
      );
    } finally {
      filesHttp.close(force: true);
    }

    await ServerApi.logout(settings, token);
    try {
      await ServerApi.addressBook(settings, token);
      check(false, 'the token is rejected after logout');
    } on ServerApiException catch (error) {
      check(error.statusCode == 401, 'the token is rejected after logout');
    }

    final AccountLoginResult lifecycle = await ServerApi.login(
      settings,
      username: username,
      password: password,
    );
    try {
      await ServerApi.refresh(settings, login.refreshToken);
      check(false, 'a logged-out refresh token is rejected');
    } on ServerApiException catch (error) {
      check(error.statusCode == 401, 'a logged-out refresh token is rejected');
    }
    final AccountLoginResult renewed =
        await ServerApi.refresh(settings, lifecycle.refreshToken);
    check(
      renewed.token.isNotEmpty &&
          renewed.token != lifecycle.token &&
          renewed.refreshToken.isNotEmpty &&
          renewed.refreshToken != lifecycle.refreshToken,
      'refresh rotates both tokens',
    );
    final AccountInfo info = await ServerApi.accountInfo(settings, renewed.token);
    check(info.username == login.username, 'the renewed token reads the account');
    try {
      await ServerApi.addressBook(settings, lifecycle.token);
      check(false, 'the old access token is rejected after rotation');
    } on ServerApiException catch (error) {
      check(
        error.statusCode == 401,
        'the old access token is rejected after rotation',
      );
    }
    try {
      await ServerApi.refresh(settings, lifecycle.refreshToken);
      check(false, 'a replayed refresh token is rejected');
    } on ServerApiException catch (error) {
      check(error.statusCode == 401, 'a replayed refresh token is rejected');
    }
    try {
      await ServerApi.accountInfo(settings, renewed.token);
      check(false, 'a replay revokes the rotated session');
    } on ServerApiException catch (error) {
      check(error.statusCode == 401, 'a replay revokes the rotated session');
    }

    await store.clear();
    check(
      await store.load() == null,
      'clearing the session removes account.json',
    );
  } finally {
    try {
      await temp.delete(recursive: true);
    } catch (_) {}
  }

  if (failed > 0) {
    stdout.writeln('$failed checks failed');
    exitCode = 1;
  } else {
    stdout.writeln('account sync smoke checks passed');
  }
}

Future<void> _shareProbe({
  required String base,
  required String owner,
  required String ownerPassword,
  required String grantee,
  required String granteePassword,
  required String control,
  required String controlPassword,
}) async {
  final ServerSettings settings = ServerSettings(url: base);
  int failed = 0;
  void check(bool ok, String message) {
    stdout.writeln(ok ? 'ok $message' : 'FAIL $message');
    if (!ok) {
      failed++;
    }
  }

  Future<int> denied(Future<void> Function() action) async {
    try {
      await action();
    } on ServerApiException catch (error) {
      return error.statusCode ?? -1;
    }
    return 0;
  }

  final AccountLoginResult ownerLogin = await ServerApi.login(
    settings,
    username: owner,
    password: ownerPassword,
  );
  final AccountLoginResult granteeLogin = await ServerApi.login(
    settings,
    username: grantee,
    password: granteePassword,
  );
  final AccountLoginResult controlLogin = await ServerApi.login(
    settings,
    username: control,
    password: controlPassword,
  );
  check(
    ownerLogin.token.isNotEmpty &&
        granteeLogin.token.isNotEmpty &&
        controlLogin.token.isNotEmpty &&
        <String>{
              ownerLogin.username,
              granteeLogin.username,
              controlLogin.username,
            }.length ==
            3,
    'three accounts log in with distinct canonical usernames',
  );

  final String stamp = DateTime.now().millisecondsSinceEpoch.toString();
  final String peerId = 'smoke-share-$stamp';
  final String ownPeerId = 'smoke-own-$stamp';
  final String addedPeerId = 'smoke-added-$stamp';
  await ServerApi.updateAddressBookPeer(
    settings,
    ownerLogin.token,
    peerId,
    name: '共享工作区',
    tags: <String>['shared', 'vip'],
  );
  final AddressBookSnapshot beforeShare = await ServerApi.addressBook(
    settings,
    ownerLogin.token,
  );
  final AddressBookGrant grant = await ServerApi.shareAddressBook(
    settings,
    ownerLogin.token,
    granteeLogin.username.toUpperCase(),
  );
  check(
    grant.username == granteeLogin.username &&
        grant.permission == 'read' &&
        grant.createdAtMs > 0,
    'the owner grants read access by a case-insensitive username',
  );
  final AddressBookGrant repeat = await ServerApi.shareAddressBook(
    settings,
    ownerLogin.token,
    granteeLogin.username,
  );
  check(
    repeat.username == granteeLogin.username && repeat.permission == 'read',
    'sharing the same book twice stays idempotent at read permission',
  );

  final AddressBookGrants ownerGrants = await ServerApi.addressBookGrants(
    settings,
    ownerLogin.token,
  );
  final AddressBookGrants granteeGrants = await ServerApi.addressBookGrants(
    settings,
    granteeLogin.token,
  );
  check(
    ownerGrants.outgoing.length == 1 &&
        ownerGrants.outgoing.single.username == granteeLogin.username &&
        ownerGrants.outgoing.single.permission == 'read' &&
        ownerGrants.incoming.isEmpty,
    'the owner sees one outgoing read grant and no incoming grant',
  );
  check(
    granteeGrants.incoming.length == 1 &&
        granteeGrants.incoming.single.username == ownerLogin.username &&
        granteeGrants.incoming.single.permission == 'read' &&
        granteeGrants.outgoing.isEmpty,
    'the grantee sees the owner as an incoming read grant',
  );

  List<SharedAddressBook> shared = await ServerApi.sharedAddressBooks(
    settings,
    granteeLogin.token,
  );
  List<AddressBookPeer> sharedPeers = shared.length == 1
      ? shared.single.peers
            .where((AddressBookPeer peer) => peer.id == peerId)
            .toList()
      : <AddressBookPeer>[];
  check(
    shared.length == 1 &&
        shared.single.owner == ownerLogin.username &&
        shared.single.revision > 0 &&
        shared.single.permission == 'read' &&
        sharedPeers.length == 1 &&
        sharedPeers.single.name == '共享工作区' &&
        sharedPeers.single.tags.join(',') == 'shared,vip',
    'the grantee reads the shared peer with its name, tags and permission',
  );

  final AddressBookSnapshot granteeBook = await ServerApi.addressBook(
    settings,
    granteeLogin.token,
  );
  check(
    granteeBook.peers.every((AddressBookPeer peer) => peer.id != peerId),
    'a shared book does not leak into the grantee address book',
  );
  await ServerApi.updateAddressBookPeer(
    settings,
    granteeLogin.token,
    ownPeerId,
    name: '自己的条目',
  );
  final AddressBookSnapshot ownerBook = await ServerApi.addressBook(
    settings,
    ownerLogin.token,
  );
  check(
    ownerBook.peers.every((AddressBookPeer peer) => peer.id != ownPeerId),
    'a grantee write stays in their own book and cannot alter the shared one',
  );

  check(
    await denied(
          () => ServerApi.updateSharedAddressBookPeer(
            settings,
            granteeLogin.token,
            ownerLogin.username,
            peerId,
            name: '篡改',
          ),
        ) ==
        403,
    'a read-only grantee cannot edit the shared peer',
  );
  check(
    await denied(
          () => ServerApi.deleteSharedAddressBookPeer(
            settings,
            granteeLogin.token,
            ownerLogin.username,
            peerId,
          ),
        ) ==
        403,
    'a read-only grantee cannot delete the shared peer',
  );
  check(
    await denied(
          () => ServerApi.addSharedAddressBookPeer(
            settings,
            granteeLogin.token,
            ownerLogin.username,
            'smoke-denied-$stamp',
            name: '只读新增',
          ),
        ) ==
        403,
    'a read-only grantee cannot add peers to the shared book',
  );
  check(
    await denied(
          () => ServerApi.sharedAddressBookGrants(
            settings,
            granteeLogin.token,
            ownerLogin.username,
          ),
        ) ==
        403,
    'a read-only grantee cannot list the owner grants',
  );
  final AddressBookPeer untouched = (await ServerApi.addressBook(
    settings,
    ownerLogin.token,
  )).peers.firstWhere((AddressBookPeer peer) => peer.id == peerId);
  check(
    untouched.name == '共享工作区' && untouched.tags.join(',') == 'shared,vip',
    'refused shared writes leave the owner peer unchanged',
  );

  final AddressBookGrant writeGrant = await ServerApi.shareAddressBook(
    settings,
    ownerLogin.token,
    granteeLogin.username,
    permission: 'write',
  );
  check(
    writeGrant.permission == 'write',
    'the owner upgrades the grant to read-write',
  );
  shared = await ServerApi.sharedAddressBooks(settings, granteeLogin.token);
  check(
    shared.single.permission == 'write',
    'the grantee sees the upgraded read-write permission',
  );
  final AddressBookPeer edited = await ServerApi.updateSharedAddressBookPeer(
    settings,
    granteeLogin.token,
    ownerLogin.username,
    peerId,
    name: '共享工作区（改）',
    tags: <String>['shared', 'edited'],
  );
  check(
    edited.id == peerId &&
        edited.name == '共享工作区（改）' &&
        edited.tags.join(',') == 'shared,edited',
    'a read-write grantee edits the shared peer',
  );
  final AddressBookSnapshot ownerAfterEdit = await ServerApi.addressBook(
    settings,
    ownerLogin.token,
  );
  final AddressBookPeer ownerEdited = ownerAfterEdit.peers.firstWhere(
    (AddressBookPeer peer) => peer.id == peerId,
  );
  check(
    ownerAfterEdit.revision > beforeShare.revision &&
        ownerEdited.name == '共享工作区（改）' &&
        ownerEdited.tags.join(',') == 'shared,edited',
    'a shared edit reaches the owner book and advances its revision',
  );

  final AddressBookPeer added = await ServerApi.addSharedAddressBookPeer(
    settings,
    granteeLogin.token,
    ownerLogin.username,
    addedPeerId,
    name: '共享新增',
    tags: <String>['added', ' added '],
  );
  final AddressBookSnapshot ownerAfterAdd = await ServerApi.addressBook(
    settings,
    ownerLogin.token,
  );
  final AddressBookSnapshot granteeAfterAdd = await ServerApi.addressBook(
    settings,
    granteeLogin.token,
  );
  check(
    added.id == addedPeerId &&
        added.name == '共享新增' &&
        added.tags.join(',') == 'added' &&
        ownerAfterAdd.peers.any(
          (AddressBookPeer peer) => peer.id == addedPeerId,
        ) &&
        granteeAfterAdd.peers.every(
          (AddressBookPeer peer) => peer.id != addedPeerId,
        ),
    'a read-write grantee adds a peer to the owner book without copying it',
  );

  await ServerApi.deleteSharedAddressBookPeer(
    settings,
    granteeLogin.token,
    ownerLogin.username,
    peerId,
  );
  final AddressBookSnapshot ownerAfterDelete = await ServerApi.addressBook(
    settings,
    ownerLogin.token,
  );
  final AddressBookSnapshot deletedDelta = await ServerApi.addressBook(
    settings,
    ownerLogin.token,
    since: ownerAfterEdit.revision,
  );
  check(
    ownerAfterDelete.peers.every((AddressBookPeer peer) => peer.id != peerId) &&
        deletedDelta.tombstones.any(
          (AddressBookPeer peer) => peer.id == peerId,
        ),
    'a read-write grantee deletes the shared peer as a tombstone',
  );

  await ServerApi.updateAddressBookPeer(
    settings,
    ownerLogin.token,
    peerId,
    name: '共享工作区',
    tags: <String>['shared', 'vip'],
  );
  shared = await ServerApi.sharedAddressBooks(settings, granteeLogin.token);
  check(
    shared.single.peers.any(
      (AddressBookPeer peer) => peer.id == peerId && peer.name == '共享工作区',
    ),
    'the owner re-adds the shared peer after the shared deletion',
  );

  final AddressBookGrant controlGrant = await ServerApi.shareAddressBook(
    settings,
    ownerLogin.token,
    granteeLogin.username,
    permission: 'control',
  );
  check(
    controlGrant.permission == 'control',
    'the owner upgrades the grant to full control',
  );
  shared = await ServerApi.sharedAddressBooks(settings, granteeLogin.token);
  check(
    shared.single.permission == 'control',
    'the grantee sees the full-control permission',
  );
  final AddressBookGrants managed = await ServerApi.sharedAddressBookGrants(
    settings,
    granteeLogin.token,
    ownerLogin.username,
  );
  check(
    managed.outgoing.length == 1 &&
        managed.outgoing.single.username == granteeLogin.username &&
        managed.outgoing.single.permission == 'control',
    'a full-control grantee lists the grants on the shared book',
  );

  final AddressBookGrant delegated = await ServerApi.shareSharedAddressBook(
    settings,
    granteeLogin.token,
    ownerLogin.username,
    controlLogin.username,
    permission: 'read',
  );
  check(
    delegated.username == controlLogin.username &&
        delegated.permission == 'read',
    'a full-control grantee delegates read access to a third account',
  );
  List<SharedAddressBook> controlBooks = await ServerApi.sharedAddressBooks(
    settings,
    controlLogin.token,
  );
  check(
    controlBooks.length == 1 &&
        controlBooks.single.owner == ownerLogin.username &&
        controlBooks.single.permission == 'read' &&
        controlBooks.single.peers.any(
          (AddressBookPeer peer) => peer.id == peerId && peer.name == '共享工作区',
        ),
    'the delegated account reads the shared book at read permission',
  );
  check(
    await denied(
          () => ServerApi.updateSharedAddressBookPeer(
            settings,
            controlLogin.token,
            ownerLogin.username,
            peerId,
            name: '篡改',
          ),
        ) ==
        403,
    'a delegated read grant cannot edit the shared peer',
  );
  check(
    await denied(
          () => ServerApi.sharedAddressBookGrants(
            settings,
            controlLogin.token,
            ownerLogin.username,
          ),
        ) ==
        403,
    'a delegated read grant cannot list the owner grants',
  );
  await ServerApi.unshareSharedAddressBook(
    settings,
    granteeLogin.token,
    ownerLogin.username,
    controlLogin.username,
  );
  controlBooks = await ServerApi.sharedAddressBooks(
    settings,
    controlLogin.token,
  );
  check(
    controlBooks.isEmpty,
    'a full-control grantee revokes the delegated grant',
  );

  await ServerApi.unshareSharedAddressBook(
    settings,
    granteeLogin.token,
    ownerLogin.username,
    granteeLogin.username,
  );
  final List<SharedAddressBook> selfRevoked =
      await ServerApi.sharedAddressBooks(settings, granteeLogin.token);
  final AddressBookGrants ownerAfterSelfRevoke =
      await ServerApi.addressBookGrants(settings, ownerLogin.token);
  check(
    selfRevoked.isEmpty && ownerAfterSelfRevoke.outgoing.isEmpty,
    'a full-control grantee revokes their own access',
  );
  final AddressBookGrant restored = await ServerApi.shareAddressBook(
    settings,
    ownerLogin.token,
    granteeLogin.username,
  );
  check(
    restored.permission == 'read' && restored.username == granteeLogin.username,
    'the owner re-shares at read permission after the self-revoke',
  );

  try {
    await ServerApi.shareAddressBook(
      settings,
      ownerLogin.token,
      ownerLogin.username,
    );
    check(false, 'sharing an address book with yourself is refused');
  } on ServerApiException catch (error) {
    check(
      error.statusCode == 400,
      'sharing an address book with yourself is refused',
    );
  }
  try {
    await ServerApi.shareAddressBook(
      settings,
      ownerLogin.token,
      'missing-$stamp',
    );
    check(false, 'sharing with an unknown user is refused');
  } on ServerApiException catch (error) {
    check(error.statusCode == 404, 'sharing with an unknown user is refused');
  }

  await ServerApi.unshareAddressBook(
    settings,
    ownerLogin.token,
    granteeLogin.username.toUpperCase(),
  );
  final List<SharedAddressBook> revoked = await ServerApi.sharedAddressBooks(
    settings,
    granteeLogin.token,
  );
  final AddressBookGrants afterRevoke = await ServerApi.addressBookGrants(
    settings,
    granteeLogin.token,
  );
  check(
    revoked.isEmpty && afterRevoke.incoming.isEmpty,
    'revoking the grant removes the shared book from the grantee',
  );
  try {
    await ServerApi.unshareAddressBook(
      settings,
      ownerLogin.token,
      granteeLogin.username,
    );
    check(false, 'revoking the same grant twice is refused');
  } on ServerApiException catch (error) {
    check(error.statusCode == 404, 'revoking the same grant twice is refused');
  }

  // Group-scoped grants: only the granted group is visible and writable.
  final String labGroup = 'lab-$stamp';
  final String workGroup = 'work-$stamp';
  final String labPeerId = 'smoke-lab-$stamp';
  final String workPeerId = 'smoke-work-$stamp';
  final String labAddedId = 'smoke-lab-add-$stamp';
  await ServerApi.updateAddressBookGroup(
    settings,
    ownerLogin.token,
    labGroup,
    '实验室',
  );
  await ServerApi.updateAddressBookGroup(
    settings,
    ownerLogin.token,
    workGroup,
    '办公室',
  );
  await ServerApi.updateAddressBookPeer(
    settings,
    ownerLogin.token,
    labPeerId,
    name: '实验机',
    groupId: labGroup,
  );
  await ServerApi.updateAddressBookPeer(
    settings,
    ownerLogin.token,
    workPeerId,
    name: '办公机',
    groupId: workGroup,
  );

  final AddressBookGrant scoped = await ServerApi.shareAddressBook(
    settings,
    ownerLogin.token,
    granteeLogin.username,
    groupId: labGroup,
  );
  check(
    scoped.groupId == labGroup && !scoped.wholeBook,
    'the owner grants one group at read permission',
  );
  List<SharedAddressBook> scopedBooks = await ServerApi.sharedAddressBooks(
    settings,
    granteeLogin.token,
  );
  check(
    scopedBooks.length == 1 &&
        scopedBooks.single.scopes.length == 1 &&
        scopedBooks.single.permissionFor(labGroup) == 'read' &&
        scopedBooks.single.permissionFor(workGroup).isEmpty &&
        scopedBooks.single.peers
                .map((AddressBookPeer peer) => peer.id)
                .join(',') ==
            labPeerId &&
        scopedBooks.single.groups
                .map((AddressBookGroup group) => group.id)
                .join(',') ==
            labGroup,
    'a group grant hides peers and groups outside its scope',
  );
  check(
    await denied(
          () => ServerApi.updateSharedAddressBookPeer(
            settings,
            granteeLogin.token,
            ownerLogin.username,
            labPeerId,
            name: '篡改',
          ),
        ) ==
        403,
    'a scoped read grant cannot edit peers in its own group',
  );
  check(
    await denied(
          () => ServerApi.addSharedAddressBookPeer(
            settings,
            granteeLogin.token,
            ownerLogin.username,
            'smoke-scope-deny-$stamp',
            name: '越权新增',
            groupId: workGroup,
          ),
        ) ==
        403,
    'a scoped grant cannot add peers into another group',
  );

  final AddressBookGrant scopedWrite = await ServerApi.shareAddressBook(
    settings,
    ownerLogin.token,
    granteeLogin.username,
    permission: 'write',
    groupId: labGroup,
  );
  check(
    scopedWrite.groupId == labGroup && scopedWrite.permission == 'write',
    'the owner upgrades the group grant to read-write',
  );
  final AddressBookPeer scopedEdit = await ServerApi.updateSharedAddressBookPeer(
    settings,
    granteeLogin.token,
    ownerLogin.username,
    labPeerId,
    name: '实验机（改）',
  );
  check(
    scopedEdit.name == '实验机（改）',
    'a scoped read-write grant edits peers in its group',
  );
  check(
    await denied(
          () => ServerApi.updateSharedAddressBookPeer(
            settings,
            granteeLogin.token,
            ownerLogin.username,
            workPeerId,
            name: '越权修改',
          ),
        ) ==
        403,
    'a scoped read-write grant cannot edit another group',
  );
  final AddressBookPeer scopedAdded = await ServerApi.addSharedAddressBookPeer(
    settings,
    granteeLogin.token,
    ownerLogin.username,
    labAddedId,
    name: '组内新增',
    groupId: labGroup,
  );
  check(
    scopedAdded.groupId == labGroup,
    'a scoped read-write grant adds peers into its group',
  );

  await ServerApi.shareAddressBook(
    settings,
    ownerLogin.token,
    granteeLogin.username,
    permission: 'control',
    groupId: labGroup,
  );
  check(
    await denied(
          () => ServerApi.sharedAddressBookGrants(
            settings,
            granteeLogin.token,
            ownerLogin.username,
          ),
        ) ==
        403,
    'a group-level control grant cannot manage the book grants',
  );

  await ServerApi.deleteAddressBookGroup(
    settings,
    ownerLogin.token,
    labGroup,
  );
  scopedBooks = await ServerApi.sharedAddressBooks(
    settings,
    granteeLogin.token,
  );
  final AddressBookGrants scopedAfterDelete = await ServerApi.addressBookGrants(
    settings,
    granteeLogin.token,
  );
  check(
    scopedBooks.isEmpty && scopedAfterDelete.incoming.isEmpty,
    'deleting a group revokes its scoped grants',
  );

  await ServerApi.deleteAddressBookPeer(settings, ownerLogin.token, labPeerId);
  await ServerApi.deleteAddressBookPeer(settings, ownerLogin.token, workPeerId);
  await ServerApi.deleteAddressBookPeer(settings, ownerLogin.token, labAddedId);
  await ServerApi.deleteAddressBookGroup(
    settings,
    ownerLogin.token,
    workGroup,
  );

  await ServerApi.deleteAddressBookPeer(settings, ownerLogin.token, peerId);
  await ServerApi.deleteAddressBookPeer(
    settings,
    ownerLogin.token,
    addedPeerId,
  );
  await ServerApi.deleteAddressBookPeer(
    settings,
    granteeLogin.token,
    ownPeerId,
  );
  await ServerApi.logout(settings, ownerLogin.token);
  await ServerApi.logout(settings, granteeLogin.token);
  await ServerApi.logout(settings, controlLogin.token);

  if (failed > 0) {
    stdout.writeln('$failed checks failed');
    exitCode = 1;
  } else {
    stdout.writeln('shared address book smoke checks passed');
  }
}

Future<void> _restartProbe({
  required bool prepare,
  required String base,
  required String username,
  required String password,
  required String sessionPath,
}) async {
  final ServerSettings settings = ServerSettings(url: base);
  final AccountSessionStore store = AccountSessionStore(
    file: File(sessionPath),
  );
  int failed = 0;
  void check(bool ok, String message) {
    stdout.writeln(ok ? 'ok $message' : 'FAIL $message');
    if (!ok) {
      failed++;
    }
  }

  if (prepare) {
    final AccountLoginResult login = await ServerApi.login(
      settings,
      username: username,
      password: password,
    );
    await store.save(
      AccountSession(
        server: base,
        username: login.username,
        token: login.token,
        expiresAt: DateTime.now().toUtc().add(
          Duration(milliseconds: login.expiresInMs),
        ),
      ),
    );
    await ServerApi.updateAddressBookGroup(
      settings,
      login.token,
      'persist-group',
      '持久分组',
    );
    await ServerApi.updateAddressBookPeer(
      settings,
      login.token,
      'persist-probe',
      name: '重启前',
      tags: <String>['persisted'],
      groupId: 'persist-group',
    );
    final AddressBookSnapshot snapshot = await ServerApi.addressBook(
      settings,
      login.token,
    );
    await store.save(
      AccountSession(
        server: base,
        username: login.username,
        token: login.token,
        expiresAt: DateTime.now().toUtc().add(
          Duration(milliseconds: login.expiresInMs),
        ),
        bookRevision: snapshot.revision,
      ),
    );
    check(await File(sessionPath).exists(), 'prepare saved account.json');
    stdout.writeln('restart the server, then run with check');
  } else {
    final AccountSession? session = await store.load();
    check(
      session != null && session.isActiveAt(DateTime.now()),
      'an active account session survives the restart',
    );
    check(
      session != null && session.bookRevision > 0,
      'the address-book revision survives the restart',
    );
    if (session != null) {
      final AddressBookSnapshot snapshot = await ServerApi.addressBook(
        settings,
        session.token,
      );
      check(
        snapshot.revision >= session.bookRevision &&
            snapshot.peers.any(
              (AddressBookPeer peer) =>
                  peer.id == 'persist-probe' &&
                  peer.name == '重启前' &&
                  peer.tags.join(',') == 'persisted' &&
                  peer.groupId == 'persist-group',
            ) &&
            snapshot.groups.any(
              (AddressBookGroup group) =>
                  group.id == 'persist-group' && group.name == '持久分组',
            ),
        'the token, address book, tags and group survive the restart',
      );
      await ServerApi.deleteAddressBookGroup(
        settings,
        session.token,
        'persist-group',
      );
      await ServerApi.deleteAddressBookPeer(
        settings,
        session.token,
        'persist-probe',
      );
      final AddressBookSnapshot delta = await ServerApi.addressBook(
        settings,
        session.token,
        since: session.bookRevision,
      );
      check(
        delta.tombstones.any(
              (AddressBookPeer peer) => peer.id == 'persist-probe',
            ) &&
            delta.groupChanges.any(
              (AddressBookGroup group) =>
                  group.id == 'persist-group' && group.deleted,
            ),
        'the persisted revision sees peer and group deletions after the restart',
      );
      await ServerApi.logout(settings, session.token);
      await store.clear();
      check(await store.load() == null, 'the restart probe cleaned up');
    }
  }

  if (failed > 0) {
    stdout.writeln('$failed checks failed');
    exitCode = 1;
  } else {
    stdout.writeln('restart persistence checks passed');
  }
}
