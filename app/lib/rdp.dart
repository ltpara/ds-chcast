import 'dart:io';

/// Arguments for the Windows RDP client. [localPort] is the viewer-side
/// listener bound by the port-forward rule, not the host's RDP port.
List<String> rdpClientArguments(int localPort) =>
    <String>['/v:127.0.0.1:$localPort'];

/// Opens the system RDP client against [localPort]. Credentials are typed
/// into mstsc's own prompt: the viewer stores none and puts none on a command
/// line, unlike RustDesk's cmdkey flow.
Future<void> launchRdpClient(int localPort) async {
  if (!Platform.isWindows) {
    throw ProcessException(
      'mstsc',
      rdpClientArguments(localPort),
      'the RDP client is only available on Windows',
    );
  }
  await Process.start(
    'mstsc',
    rdpClientArguments(localPort),
    mode: ProcessStartMode.detached,
  );
}
