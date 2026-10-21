class ConnectTarget {
  const ConnectTarget(this.host, this.port);

  final String host;
  final int port;

  String get label => host.contains(':') ? '[$host]:$port' : '$host:$port';
}

/// Parses "host", "host:port" or "[ipv6]:port". A missing port uses the chast
/// default, 4433.
ConnectTarget? parseConnectTarget(String input) {
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
  return ConnectTarget(host, port);
}
