import 'dart:convert';
import 'dart:io';

import 'package:chast_viewer/server_api.dart';
import 'package:chast_viewer/server_settings.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test(
    'password login attaches an installation proof to the request',
    () async {
      final HttpServer server = await HttpServer.bind(
        InternetAddress.loopbackIPv4,
        0,
      );
      addTearDown(() => server.close(force: true));
      addTearDown(() => ServerApi.loginDeviceProofProviderForTesting = null);
      final ServerSettings settings = ServerSettings(
        url: 'http://127.0.0.1:${server.port}',
      );
      const LoginDeviceProof proof = LoginDeviceProof(
        challengeId: 'challenge-id',
        publicKey: 'public-key',
        signature: 'signature',
      );
      ServerApi.loginDeviceProofProviderForTesting =
          (ServerSettings _, Duration _) async => proof;

      Map<String, Object?>? receivedProof;
      server.listen((HttpRequest request) async {
        final String bodyText = await utf8.decoder.bind(request).join();
        expect(request.uri.path, '/v1/account/login');
        final Map<String, Object?> body =
            jsonDecode(bodyText) as Map<String, Object?>;
        receivedProof = body['device_proof'] as Map<String, Object?>?;
        request.response.headers.contentType = ContentType.json;
        request.response.write(
          jsonEncode(<String, Object?>{
            'token': 'access-token',
            'expires_in_ms': 60000,
            'user': <String, String>{'username': 'alice'},
          }),
        );
        await request.response.close();
      });

      final AccountLoginResult result = await ServerApi.login(
        settings,
        username: 'alice',
        password: 'correct horse',
        deviceName: 'desktop',
      );
      expect(result.token, 'access-token');
      expect(receivedProof, proof.toJson());
    },
  );
}
