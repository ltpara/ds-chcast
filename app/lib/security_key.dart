import 'package:flutter/services.dart';

import 'server_api.dart';
import 'server_settings.dart';

/// A failed Windows Hello ceremony or a platform that cannot run one.
class SecurityKeyException implements Exception {
  const SecurityKeyException(this.message, {this.code = ''});

  final String message;

  /// Machine-readable reason, such as `not_supported` or `user_cancelled`.
  final String code;

  @override
  String toString() => message;
}

/// One freshly created credential. All binary fields are unpadded base64url,
/// exactly as the server expects them.
class SecurityKeyMakeResult {
  const SecurityKeyMakeResult({
    required this.credentialId,
    required this.clientDataJSON,
    required this.attestationObject,
  });

  final String credentialId;
  final String clientDataJSON;
  final String attestationObject;
}

/// One assertion signed by Windows Hello for a pending login challenge.
class SecurityKeyAssertion {
  const SecurityKeyAssertion({
    required this.credentialId,
    required this.clientDataJSON,
    required this.authenticatorData,
    required this.signature,
  });

  final String credentialId;
  final String clientDataJSON;
  final String authenticatorData;
  final String signature;
}

/// Talks to the native `chast/viewer` bridge, which calls Windows Hello
/// through webauthn.dll. Dart only moves JSON fields and base64url around.
class SecurityKey {
  SecurityKey({this.channel = const MethodChannel('chast/viewer')});

  final MethodChannel channel;

  /// Whether Windows Hello has a configured platform authenticator (a PIN or
  /// biometrics). False on platforms without the bridge.
  Future<bool> available() async {
    try {
      return await channel.invokeMethod<bool>('securityKeyAvailable') ?? false;
    } on MissingPluginException {
      throw const SecurityKeyException(
        '当前平台不支持 Windows Hello',
        code: 'not_supported',
      );
    } on PlatformException catch (error) {
      throw SecurityKeyException(_message(error.code, error.message), code: error.code);
    }
  }

  /// Registers a credential for [rpId]. The origin is baked into client data;
  /// it defaults to `https://<rpId>`, which is what the server allowlist
  /// usually carries.
  Future<SecurityKeyMakeResult> makeCredential({
    required String rpId,
    required String userId,
    required String username,
    String displayName = '',
    required String challenge,
    List<String> excludeCredentials = const <String>[],
    int timeoutMs = 120000,
    String? origin,
  }) async {
    final Map<Object?, Object?> reply = await _invoke('securityKeyMakeCredential', <String, Object?>{
      'rpId': rpId,
      'origin': origin ?? 'https://$rpId',
      'userId': userId,
      'username': username,
      'displayName': displayName.isEmpty ? username : displayName,
      'challenge': challenge,
      'credentials': excludeCredentials,
      'timeoutMs': timeoutMs,
    });
    final SecurityKeyMakeResult result = SecurityKeyMakeResult(
      credentialId: (reply['credentialId'] ?? '').toString(),
      clientDataJSON: (reply['clientDataJSON'] ?? '').toString(),
      attestationObject: (reply['attestationObject'] ?? '').toString(),
    );
    if (result.credentialId.isEmpty ||
        result.clientDataJSON.isEmpty ||
        result.attestationObject.isEmpty) {
      throw const SecurityKeyException('Windows Hello 未返回完整的凭据', code: 'failed');
    }
    return result;
  }

  /// Asks Windows Hello to sign [challenge] with one of [allowCredentials].
  Future<SecurityKeyAssertion> getAssertion({
    required String rpId,
    required String challenge,
    required List<String> allowCredentials,
    int timeoutMs = 120000,
    String? origin,
  }) async {
    final Map<Object?, Object?> reply = await _invoke('securityKeyGetAssertion', <String, Object?>{
      'rpId': rpId,
      'origin': origin ?? 'https://$rpId',
      'challenge': challenge,
      'credentials': allowCredentials,
      'timeoutMs': timeoutMs,
    });
    final SecurityKeyAssertion result = SecurityKeyAssertion(
      credentialId: (reply['credentialId'] ?? '').toString(),
      clientDataJSON: (reply['clientDataJSON'] ?? '').toString(),
      authenticatorData: (reply['authenticatorData'] ?? '').toString(),
      signature: (reply['signature'] ?? '').toString(),
    );
    if (result.credentialId.isEmpty ||
        result.clientDataJSON.isEmpty ||
        result.authenticatorData.isEmpty ||
        result.signature.isEmpty) {
      throw const SecurityKeyException('Windows Hello 未返回完整的断言', code: 'failed');
    }
    return result;
  }

  Future<Map<Object?, Object?>> _invoke(
    String method,
    Map<String, Object?> arguments,
  ) async {
    try {
      final Object? reply = await channel.invokeMethod<Object>(method, arguments);
      if (reply is Map) {
        return reply.cast<Object?, Object?>();
      }
      throw const SecurityKeyException('Windows Hello 返回了无效数据', code: 'failed');
    } on MissingPluginException {
      throw const SecurityKeyException(
        '当前平台不支持 Windows Hello',
        code: 'not_supported',
      );
    } on PlatformException catch (error) {
      throw SecurityKeyException(_message(error.code, error.message), code: error.code);
    }
  }
}

/// Completes a password-checked security-key login: start the challenge,
/// sign it with Windows Hello, then exchange the assertion for a session.
Future<AccountLoginResult> loginWithSecurityKey(
  ServerSettings server, {
  required String username,
  required String password,
  SecurityKey? securityKey,
  String deviceName = '',
}) async {
  final SecurityKey key = securityKey ?? SecurityKey();
  final WebAuthnLoginOptions? options = await ServerApi.loginWebAuthnBegin(
    server,
    username: username,
    password: password,
  );
  if (options == null) {
    throw const ServerApiException('该账号未注册安全密钥');
  }
  if (!await key.available()) {
    throw const SecurityKeyException('此设备未配置 Windows Hello', code: 'not_supported');
  }
  final SecurityKeyAssertion assertion = await key.getAssertion(
    rpId: options.rpId,
    challenge: options.challenge,
    allowCredentials: <String>[
      for (final WebAuthnCredential credential in options.credentials) credential.id,
    ],
    timeoutMs: options.timeoutMs,
  );
  return ServerApi.loginWebAuthnFinish(
    server,
    username: username,
    credentialId: assertion.credentialId,
    clientDataJSON: assertion.clientDataJSON,
    authenticatorData: assertion.authenticatorData,
    signature: assertion.signature,
    deviceName: deviceName,
  );
}

String _message(String code, String? detail) {
  switch (code) {
    case 'not_supported':
      return '当前设备未配置 Windows Hello';
    case 'user_cancelled':
      return '已取消 Windows Hello 验证';
    case 'timeout':
      return 'Windows Hello 验证超时';
    case 'credential_exists':
      return '该安全密钥已经注册';
    case 'not_found':
      return '未找到匹配的安全密钥';
    case 'busy':
      return '另一个安全密钥操作正在进行';
    case 'bad_args':
      return '安全密钥参数无效';
  }
  final String text = detail?.trim() ?? '';
  return text.isEmpty ? 'Windows Hello 操作失败' : 'Windows Hello 操作失败：$text';
}
