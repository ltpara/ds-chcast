import 'dart:convert';
import 'dart:io';

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
        return '自动直连 · 失败回退中继';
      case TunnelMode.direct:
        return '仅直连';
      case TunnelMode.relay:
        return '仅中继';
    }
  }

  String get shortLabel {
    switch (this) {
      case TunnelMode.auto:
        return '自动';
      case TunnelMode.direct:
        return '直连';
      case TunnelMode.relay:
        return '中继';
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
/// A bare host gets http:// so typing an intranet address is enough; trailing
/// slashes are dropped because every API path is appended with one.
String normalizeServerUrl(String input) {
  String text = input.trim();
  if (text.isEmpty) {
    return '';
  }
  if (!text.contains('://')) {
    text = 'http://$text';
  }
  final Uri? uri = Uri.tryParse(text);
  if (uri == null ||
      (uri.scheme != 'http' && uri.scheme != 'https') ||
      uri.host.isEmpty ||
      uri.hasQuery ||
      uri.hasFragment) {
    return '';
  }
  String path = uri.path;
  while (path.endsWith('/')) {
    path = path.substring(0, path.length - 1);
  }
  return Uri(
    scheme: uri.scheme,
    userInfo: uri.userInfo,
    host: uri.host,
    port: uri.hasPort ? uri.port : null,
    path: path,
  ).toString();
}

class ServerSettings {
  const ServerSettings({
    this.url = '',
    this.mode = TunnelMode.auto,
    this.key = '',
  });

  final String url;
  final TunnelMode mode;

  /// Bearer key for the connection service; empty when the service runs
  /// without `--server-key`. The viewer now needs it to resolve a host and to
  /// prove its punch requests, so it belongs with the service, not a device.
  final String key;

  bool get configured => url.isNotEmpty;

  ServerSettings copyWith({String? url, TunnelMode? mode, String? key}) {
    return ServerSettings(
      url: url ?? this.url,
      mode: mode ?? this.mode,
      key: key ?? this.key,
    );
  }

  Map<String, Object?> toJson() => <String, Object?>{
        'url': url,
        'mode': mode.name,
        'key': key,
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
    );
  }
}

class ServerSettingsStore {
  ServerSettingsStore({File? file}) : _injected = file;

  final File? _injected;

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
      return ServerSettings.fromJson(jsonDecode(await file.readAsString()));
    } catch (_) {
      return const ServerSettings();
    }
  }

  Future<void> save(ServerSettings settings) async {
    final File? file = _file();
    if (file == null) {
      return;
    }
    try {
      await file.parent.create(recursive: true);
      await file.writeAsString(jsonEncode(settings.toJson()));
    } catch (_) {
      // The address book still works against per-device servers; a lost
      // default only costs retyping the URL next time.
    }
  }
}
