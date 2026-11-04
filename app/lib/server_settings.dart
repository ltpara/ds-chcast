import 'dart:convert';
import 'dart:io';
import 'package:chast_viewer/l10n.dart';

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

class ServerSettings {
  const ServerSettings({
    this.url = '',
    this.mode = TunnelMode.auto,
    this.key = '',
    this.uploadRecordings = false,
  });

  final String url;
  final TunnelMode mode;

  /// Bearer key for the connection service; empty when the service runs
  /// without `--server-key`. The viewer now needs it to resolve a host and to
  /// prove its punch requests, so it belongs with the service, not a device.
  final String key;

  /// Uploads a finished viewer recording to the signed-in account's archive.
  /// The server decides whether an archive exists; this only records intent.
  final bool uploadRecordings;

  bool get configured => url.isNotEmpty;

  ServerSettings copyWith({
    String? url,
    TunnelMode? mode,
    String? key,
    bool? uploadRecordings,
  }) {
    return ServerSettings(
      url: url ?? this.url,
      mode: mode ?? this.mode,
      key: key ?? this.key,
      uploadRecordings: uploadRecordings ?? this.uploadRecordings,
    );
  }

  Map<String, Object?> toJson() => <String, Object?>{
        'url': url,
        'mode': mode.name,
        'key': key,
        'upload_recordings': uploadRecordings,
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
      uploadRecordings: value['upload_recordings'] == true,
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
      if (settings.key.isNotEmpty && _protector != null) {
        // Replace a legacy plaintext key with its protected envelope.
        await save(settings);
      }
      return settings;
    } catch (_) {
      return const ServerSettings();
    }
  }

  Future<void> save(ServerSettings settings) async {
    final File? file = _file();
    if (file == null) {
      return;
    }
    String text = jsonEncode(settings.toJson());
    final ServerKeyProtector? protect = _protector;
    if (protect != null && settings.key.isNotEmpty) {
      try {
        final String blob = await protect(text);
        text = jsonEncode(<String, Object?>{'version': 1, 'protected': blob});
      } catch (_) {
        // Never fall back to writing the key in the clear: keep the
        // non-secret fields on disk and leave the key for this run only.
        text = jsonEncode(settings.copyWith(key: '').toJson());
      }
    }
    try {
      await file.parent.create(recursive: true);
      await file.writeAsString(text);
    } catch (_) {
      // The address book still works against per-device servers; a lost
      // default only costs retyping the URL next time.
    }
  }
}
