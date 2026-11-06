import 'dart:convert';
import 'dart:io';
import 'package:chast_viewer/l10n.dart';

import 'store_lock.dart';

/// How a server-assisted viewer reaches the host. Direct and relay are meant
/// for troubleshooting and for validating one path at a time; auto is the
/// product default.
enum TunnelMode { auto, direct, relay }

extension TunnelModeName on TunnelMode {
  String get name {
    switch (this) {
      case TunnelMode.auto:
        return 'auto';
      case TunnelMode.direct:
        return 'direct';
      case TunnelMode.relay:
        return 'relay';
    }
  }

  String get label {
    switch (this) {
      case TunnelMode.auto:
        return tr('自动直连 · 失败回退中继');
      case TunnelMode.direct:
        return tr('仅直连');
      case TunnelMode.relay:
        return tr('仅中继');
    }
  }

  String get shortLabel {
    switch (this) {
      case TunnelMode.auto:
        return tr('自动');
      case TunnelMode.direct:
        return tr('直连');
      case TunnelMode.relay:
        return tr('中继');
    }
  }

  static TunnelMode parse(Object? value) {
    switch (value) {
      case 'direct':
        return TunnelMode.direct;
      case 'relay':
        return TunnelMode.relay;
      default:
        return TunnelMode.auto;
    }
  }
}

/// Canonical HTTP(S) base URL, or an empty string when [input] is not one.
/// A bare loopback host gets http:// because that is how a local development
/// service usually listens; every other bare host gets https://. Plain HTTP is
/// rejected for non-loopback hosts since the API carries the account password,
/// the server key and bearer tokens. Trailing slashes are dropped because
/// every API path is appended with one.
String normalizeServerUrl(String input) {
  String text = input.trim();
  if (text.isEmpty) {
    return '';
  }
  if (!text.contains('://')) {
    final Uri? bare = Uri.tryParse('https://$text');
    text = bare != null && bare.host.isNotEmpty && _isLoopbackHost(bare.host)
        ? 'http://$text'
        : 'https://$text';
  }
  final Uri? uri = Uri.tryParse(text);
  if (uri == null ||
      (uri.scheme != 'http' && uri.scheme != 'https') ||
      uri.host.isEmpty ||
      uri.userInfo.isNotEmpty ||
      uri.hasQuery ||
      uri.hasFragment) {
    return '';
  }
  if (uri.scheme == 'http' && !_isLoopbackHost(uri.host)) {
    return '';
  }
  String path = uri.path;
  while (path.endsWith('/')) {
    path = path.substring(0, path.length - 1);
  }
  return Uri(
    scheme: uri.scheme,
    host: uri.host,
    port: uri.hasPort ? uri.port : null,
    path: path,
  ).toString();
}

bool _isLoopbackHost(String host) {
  final String name = host.toLowerCase();
  if (name == 'localhost') {
    return true;
  }
  final InternetAddress? address = InternetAddress.tryParse(name);
  return address != null && address.isLoopback;
}

/// Canonical outbound proxy URL, or an empty string when [input] is empty or
/// invalid. Accepted schemes: http, https, socks5 and socks5h, each with an
/// explicit port. Credentials may be embedded in the userinfo part.
String normalizeProxyUrl(String input) {
  final String text = input.trim();
  if (text.isEmpty || text.length > 512 || !text.contains('://')) {
    return '';
  }
  final Uri? uri = Uri.tryParse(text);
  if (uri == null) {
    return '';
  }
  final String scheme = uri.scheme.toLowerCase();
  if (scheme != 'http' &&
      scheme != 'https' &&
      scheme != 'socks5' &&
      scheme != 'socks5h') {
    return '';
  }
  if (uri.host.isEmpty || !uri.hasPort) {
    return '';
  }
  if (uri.port < 1 || uri.port > 65535) {
    return '';
  }
  if (uri.path.isNotEmpty && uri.path != '/') {
    return '';
  }
  if (uri.hasQuery || uri.hasFragment) {
    return '';
  }
  return uri.replace(scheme: scheme, path: '').toString();
}

/// The proxy that actually applies: the configured value when present,
/// otherwise `CHAST_PROXY`. An invalid value throws instead of returning
/// empty, so a misconfigured proxy fails closed rather than dialing directly.
String effectiveProxyUrl(String configured) {
  final String text = configured.trim().isNotEmpty
      ? configured.trim()
      : (Platform.environment['CHAST_PROXY']?.trim() ?? '');
  if (text.isEmpty) {
    return '';
  }
  final String normalized = normalizeProxyUrl(text);
  if (normalized.isEmpty) {
    throw FormatException(
      tr('出站代理地址无效，需要 http://、https://、socks5:// 或 socks5h:// 且包含端口'),
    );
  }
  return normalized;
}

class ServerSettings {
  const ServerSettings({
    this.url = '',
    this.mode = TunnelMode.auto,
    this.key = '',
    this.proxy = '',
    this.uploadRecordings = false,
    this.useWebSocket = false,
  });

  final String url;
  final TunnelMode mode;

  /// Bearer key for the connection service; empty when the service runs
  /// without `--server-key`. The viewer now needs it to resolve a host and to
  /// prove its punch requests, so it belongs with the service, not a device.
  final String key;

  /// Outbound http(s)/socks5 proxy for the control plane and the TCP carrier.
  /// UDP paths are not proxied; the agent forces the carrier when this is set.
  final String proxy;

  /// Uploads a finished viewer recording to the signed-in account's archive.
  /// The server decides whether an archive exists; this only records intent.
  final bool uploadRecordings;

  /// Forces the session onto the WebSocket carrier instead of trying UDP and
  /// raw TCP first. It exists for networks whose only allowed path is a
  /// WebSocket reverse proxy or CDN; the carrier still runs the end-to-end
  /// QUIC handshake inside each message, so pinning and permissions are
  /// unchanged. RustDesk exposes the same switch as `allow-websocket`.
  final bool useWebSocket;

  bool get configured => url.isNotEmpty;

  ServerSettings copyWith({
    String? url,
    TunnelMode? mode,
    String? key,
    String? proxy,
    bool? uploadRecordings,
    bool? useWebSocket,
  }) {
    return ServerSettings(
      url: url ?? this.url,
      mode: mode ?? this.mode,
      key: key ?? this.key,
      proxy: proxy ?? this.proxy,
      uploadRecordings: uploadRecordings ?? this.uploadRecordings,
      useWebSocket: useWebSocket ?? this.useWebSocket,
    );
  }

  Map<String, Object?> toJson() => <String, Object?>{
        'url': url,
        'mode': mode.name,
        'key': key,
        'proxy': proxy,
        'upload_recordings': uploadRecordings,
        'use_websocket': useWebSocket,
      };

  static ServerSettings fromJson(Object? value) {
    if (value is! Map) {
      return const ServerSettings();
    }
    final String url = normalizeServerUrl(value['url']?.toString() ?? '');
    return ServerSettings(
      url: url,
      mode: TunnelModeName.parse(value['mode']),
      key: value['key']?.toString().trim() ?? '',
      // Keep an invalid stored value instead of dropping it: it must fail
      // loudly when used, never silently turn into a direct connection.
      proxy: value['proxy']?.toString().trim() ?? '',
      uploadRecordings: value['upload_recordings'] == true,
      useWebSocket: value['use_websocket'] == true,
    );
  }
}

/// Encrypts the settings payload before it is written to disk.
typedef ServerKeyProtector = Future<String> Function(String plaintext);

/// Decrypts a payload produced by [ServerKeyProtector].
typedef ServerKeyUnprotector = Future<String> Function(String protectedBase64);

class ServerSettingsStore {
  ServerSettingsStore({
    File? file,
    ServerKeyProtector? protectSecret,
    ServerKeyUnprotector? unprotectSecret,
  })  : _injected = file,
        _protector = protectSecret,
        _unprotector = unprotectSecret;

  final File? _injected;
  final ServerKeyProtector? _protector;
  final ServerKeyUnprotector? _unprotector;

  File? _file() {
    if (_injected != null) {
      return _injected;
    }
    final String? appData = Platform.environment['APPDATA'];
    if (appData == null || appData.isEmpty) {
      return null;
    }
    return File('$appData\\chast\\server.json');
  }

  Future<ServerSettings> load() async {
    return withStoreLock(_file(), () async {
      final File? file = _file();
      if (file == null || !await file.exists()) {
        return const ServerSettings();
      }
      try {
        final Object? decoded = jsonDecode(await file.readAsString());
        if (decoded is Map && decoded['protected'] is String) {
          final ServerKeyUnprotector? unprotect = _unprotector;
          if (unprotect == null) {
            return const ServerSettings();
          }
          return ServerSettings.fromJson(
            jsonDecode(await unprotect(decoded['protected'] as String)),
          );
        }
        final ServerSettings settings = ServerSettings.fromJson(decoded);
        if ((settings.key.isNotEmpty || settings.proxy.isNotEmpty) &&
            _protector != null) {
          // Replace a legacy plaintext secret with its protected envelope.
          await save(settings);
        }
        return settings;
      } catch (_) {
        return const ServerSettings();
      }
    });
  }

  Future<void> save(ServerSettings settings) async {
    await withStoreLock(_file(), () async {
      final File? file = _file();
      if (file == null) {
        return;
      }
      String text = jsonEncode(settings.toJson());
      final ServerKeyProtector? protect = _protector;
      if (protect != null &&
          (settings.key.isNotEmpty || settings.proxy.isNotEmpty)) {
        try {
          final String blob = await protect(text);
          text =
              jsonEncode(<String, Object?>{'version': 1, 'protected': blob});
        } catch (_) {
          // Never fall back to writing secrets in the clear: keep the
          // non-secret fields on disk and leave them for this run only.
          final bool proxyHasCredentials = settings.proxy.contains('@');
          text = jsonEncode(
            settings
                .copyWith(
                  key: '',
                  proxy: proxyHasCredentials ? '' : settings.proxy,
                )
                .toJson(),
          );
        }
      }
      try {
        await file.parent.create(recursive: true);
        await file.writeAsString(text);
      } catch (_) {
        // The address book still works against per-device servers; a lost
        // default only costs retyping the URL next time.
      }
    });
  }
}
