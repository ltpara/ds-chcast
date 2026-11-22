import 'dart:io';

/// Canonical HTTP(S) base URL, or an empty string when [input] is not one.
/// A bare loopback host gets http:// because that is how a local development
/// service usually listens; every other bare host gets https://. Plain HTTP is
/// rejected for non-loopback hosts since the API carries account credentials.
/// Trailing slashes are dropped because API paths are appended with one.
String normalizeServerUrl(String input) {
  String text = input.trim();
  if (text.isEmpty) return '';
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
  if (uri.scheme == 'http' && !_isLoopbackHost(uri.host)) return '';
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
  if (name == 'localhost') return true;
  final InternetAddress? address = InternetAddress.tryParse(name);
  return address != null && address.isLoopback;
}
