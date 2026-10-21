import 'server_settings.dart';

class ConnectTarget {
  const ConnectTarget(
    this.host,
    this.port, {
    this.token = '',
    this.server = '',
    this.hostId = '',
    this.displayName = '',
    this.mode = TunnelMode.auto,
  });

  final String host;
  final int port;

  /// The server's access token for this connection; empty when the host runs
  /// without one. Not part of [label]: a one-time token is not an address and
  /// does not belong in the recent list or the trust store.
  final String token;

  /// Non-empty when this connection is resolved by a chast-server host id
  /// through the tunnel agent instead of dialing [host]:[port] directly.
  final String server;
  final String hostId;
  final String displayName;
  final TunnelMode mode;

  bool get viaServer => server.isNotEmpty;

  String get label {
    if (viaServer) {
      return displayName.isEmpty || displayName == hostId
          ? hostId
          : '$displayName（$hostId）';
    }
    return host.contains(':') ? '[$host]:$port' : '$host:$port';
  }

  /// Stable identity for certificate pinning. Through a tunnel the QUIC
  /// endpoint is a fresh 127.0.0.1 port each session, so pinning by the local
  /// address would forget the host every time; the logical server + id is the
  /// identity the user actually trusts.
  String get trustKey => viaServer ? 'server:$server/$hostId' : label;
}

/// Host ids share the server's registry rule, which also keeps a mistyped
/// address from being sent to the lookup API.
bool isValidHostId(String text) {
  return RegExp(r'^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$').hasMatch(text);
}

/// Resolves the home page's single input box. A value containing ':' or
/// starting with '[' is always a direct address; a bare identifier is a host
/// id when a connection service is configured, and keeps the old
/// hostname:4433 behavior when it is not.
ConnectTarget? parseConnectInput(
  String input, {
  required String token,
  required ServerSettings server,
}) {
  final String text = input.trim();
  if (text.isEmpty) {
    return null;
  }
  final String key = token.trim();
  if (text.startsWith('[') || text.contains(':')) {
    return parseConnectTarget(text, token: key);
  }
  if (server.configured && isValidHostId(text)) {
    return ConnectTarget(
      '',
      0,
      token: key,
      server: server.url,
      hostId: text,
      mode: server.mode,
    );
  }
  return parseConnectTarget(text, token: key);
}

/// Parses "host", "host:port" or "[ipv6]:port". A missing port uses the chast
/// default, 4433.
ConnectTarget? parseConnectTarget(String input, {String token = ''}) {
  final text = input.trim();
  if (text.isEmpty) {
    return null;
  }
  var host = text;
  var port = 4433;
  if (text.startsWith('[')) {
    final end = text.indexOf(']');
    if (end < 0) {
      return null;
    }
    host = text.substring(1, end);
    final rest = text.substring(end + 1);
    if (rest.isNotEmpty) {
      if (!rest.startsWith(':')) {
        return null;
      }
      port = int.tryParse(rest.substring(1)) ?? 0;
    }
  } else {
    final first = text.indexOf(':');
    final last = text.lastIndexOf(':');
    if (first > 0 && first == last) {
      host = text.substring(0, first);
      port = int.tryParse(text.substring(first + 1)) ?? 0;
    }
  }
  if (host.isEmpty || port <= 0 || port > 65535) {
    return null;
  }
  return ConnectTarget(host, port, token: token.trim());
}
