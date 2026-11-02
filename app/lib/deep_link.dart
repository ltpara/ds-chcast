/// A connection request carried by a `chast://` link or the command line.
class DeepLinkRequest {
  const DeepLinkRequest(this.target, {this.fileTransferOnly = false});

  /// Device ID or `host:port` address, handed to the same resolver as typed
  /// input on the home page.
  final String target;

  /// `chast://file-transfer/new/<id>` opens the file-only session instead of
  /// a screen session.
  final bool fileTransferOnly;
}

const String _scheme = 'chast';

/// Parses the URI forms the installer registers:
///
/// ```text
/// chast://<id>
/// chast://<host>:<port>
/// chast://connection/new/<id>
/// chast://file-transfer/new/<id>
/// ```
///
/// Query and fragment are ignored, so links copied from other tools still
/// work. Anything else, including a bare or oversized scheme, is rejected
/// rather than guessed at.
DeepLinkRequest? parseChastUri(String input) {
  final String text = input.trim();
  final String prefix = '$_scheme://';
  if (!text.toLowerCase().startsWith(prefix)) {
    return null;
  }
  // Uri lowercases the host, but host ids are case-sensitive: keep the raw
  // authority for the direct form and only use Uri to split the path forms.
  final String rest = text.substring(prefix.length);
  int end = rest.length;
  for (int i = 0; i < rest.length; ++i) {
    final String c = rest[i];
    if (c == '/' || c == '?' || c == '#') {
      end = i;
      break;
    }
  }
  final String authority = rest.substring(0, end);
  final String mode = authority.toLowerCase();
  if (mode == 'connection' || mode == 'file-transfer') {
    final Uri? uri = Uri.tryParse(text);
    if (uri == null) {
      return null;
    }
    final List<String> segments = uri.pathSegments
        .where((String part) => part.isNotEmpty)
        .toList();
    if (segments.length != 2 || segments[0] != 'new') {
      return null;
    }
    final String id = segments[1].trim();
    if (id.isEmpty) {
      return null;
    }
    return DeepLinkRequest(id, fileTransferOnly: mode == 'file-transfer');
  }
  if (authority.isEmpty) {
    return null;
  }
  return DeepLinkRequest(authority);
}

/// Reads the first connection request from the process arguments. Windows
/// hands a clicked link over as a single `chast://...` argument; `--connect`
/// stays supported for scripts and the existing acceptance runs.
DeepLinkRequest? parseLaunchArguments(List<String> args) {
  for (int i = 0; i < args.length; ++i) {
    final String arg = args[i];
    if (arg == '--connect' && i + 1 < args.length) {
      return DeepLinkRequest(args[i + 1]);
    }
    if (arg.startsWith('--connect=')) {
      return DeepLinkRequest(arg.substring('--connect='.length));
    }
    final DeepLinkRequest? link = parseChastUri(arg);
    if (link != null) {
      return link;
    }
  }
  return null;
}
