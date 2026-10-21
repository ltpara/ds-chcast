import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'server_settings.dart';

/// One host as reported by `GET /v1/hosts/{id}`.
class HostInfo {
  const HostInfo({
    required this.id,
    required this.online,
    this.candidate = '',
    this.relay = '',
    this.punch = '',
    this.fingerprint = '',
    this.lastSeenMs = 0,
    this.expiresInMs = 0,
  });

  final String id;
  final bool online;
  final String candidate;
  final String relay;
  final String punch;
  final String fingerprint;
  final int lastSeenMs;
  final int expiresInMs;

  bool get hasRelay => relay.isNotEmpty;
  bool get hasDirectCandidate => candidate.isNotEmpty;

  static HostInfo fromJson(Map<Object?, Object?> map, String fallbackId) {
    String text(Object? value) => value?.toString() ?? '';
    int number(Object? value) => (value as num?)?.toInt() ?? 0;
    return HostInfo(
      id: text(map['id']).isEmpty ? fallbackId : text(map['id']),
      online: map['online'] == true,
      candidate: text(map['candidate']),
      relay: text(map['relay']),
      punch: text(map['punch']),
      fingerprint: text(map['fingerprint']),
      lastSeenMs: number(map['last_seen_ms']),
      expiresInMs: number(map['expires_in_ms']),
    );
  }
}

class ServerApiException implements Exception {
  const ServerApiException(this.message, {this.statusCode});

  final String message;
  final int? statusCode;

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

/// Lookup-only client for the M4.0 HTTP service. The viewer needs no server
/// key: registration is authenticated, resolving a host id is not.
class ServerApi {
  ServerApi._();

  static Future<HostInfo> lookup(
    ServerSettings settings,
    String hostId, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw const ServerApiException('未配置连接服务');
    }
    final String id = hostId.trim();
    if (id.isEmpty) {
      throw const ServerApiException('设备 ID 为空');
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/v1/hosts/${Uri.encodeComponent(id)}');
    } on FormatException {
      throw const ServerApiException('连接服务地址无效');
    }
    final Object? decoded = await _getJson(uri, timeout);
    if (decoded is! Map) {
      throw const ServerApiException('连接服务返回了无效数据');
    }
    return HostInfo.fromJson(decoded, id);
  }

  static Future<void> health(
    ServerSettings settings, {
    Duration timeout = const Duration(seconds: 6),
  }) async {
    if (!settings.configured) {
      throw const ServerApiException('未配置连接服务');
    }
    final Uri uri;
    try {
      uri = Uri.parse('${settings.url}/healthz');
    } on FormatException {
      throw const ServerApiException('连接服务地址无效');
    }
    await _getJson(uri, timeout);
  }

  static Future<Object?> _getJson(Uri uri, Duration timeout) async {
    final HttpClient client = HttpClient()..connectionTimeout = timeout;
    try {
      final HttpClientRequest request = await client.getUrl(uri).timeout(timeout);
      request.headers.set(HttpHeaders.acceptHeader, 'application/json');
      final HttpClientResponse response = await request.close().timeout(timeout);
      final String body =
          await response.transform(utf8.decoder).join().timeout(timeout);
      if (response.statusCode != HttpStatus.ok) {
        throw ServerApiException(
          response.statusCode == HttpStatus.notFound
              ? '设备未注册或已下线'
              : '连接服务返回 HTTP ${response.statusCode}',
          statusCode: response.statusCode,
        );
      }
      try {
        return jsonDecode(body);
      } on FormatException {
        throw const ServerApiException('连接服务返回了无效 JSON');
      }
    } on ServerApiException {
      rethrow;
    } on TimeoutException {
      throw const ServerApiException('连接服务超时');
    } on SocketException catch (error) {
      final String detail = error.message.trim();
      throw ServerApiException(
        detail.isEmpty ? '无法连接连接服务' : '无法连接连接服务：$detail',
      );
    } on HttpException catch (error) {
      throw ServerApiException('连接服务请求失败：${error.message}');
    } catch (error) {
      throw ServerApiException('连接服务请求失败：$error');
    } finally {
      client.close(force: true);
    }
  }
}
