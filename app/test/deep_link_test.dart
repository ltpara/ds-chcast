import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/deep_link.dart';

void main() {
  group('parseChastUri', () {
    test('accepts a bare device id', () {
      final DeepLinkRequest? link = parseChastUri('chast://123456789');
      expect(link, isNotNull);
      expect(link!.target, '123456789');
      expect(link.fileTransferOnly, isFalse);
    });

    test('keeps host ids case-sensitive', () {
      expect(parseChastUri('CHAST://MyHost')!.target, 'MyHost');
      expect(
        parseChastUri('chast://connection/new/MyHost')!.target,
        'MyHost',
      );
    });

    test('accepts host and port', () {
      expect(
        parseChastUri('chast://192.168.1.5:4433')!.target,
        '192.168.1.5:4433',
      );
      expect(parseChastUri('chast://[::1]:4433')!.target, '[::1]:4433');
    });

    test('accepts the connection and file-transfer forms', () {
      final DeepLinkRequest link =
          parseChastUri('chast://connection/new/42')!;
      expect(link.target, '42');
      expect(link.fileTransferOnly, isFalse);
      final DeepLinkRequest file =
          parseChastUri('chast://file-transfer/new/42')!;
      expect(file.target, '42');
      expect(file.fileTransferOnly, isTrue);
    });

    test('matches the mode case-insensitively and tolerates a trailing slash',
        () {
      expect(parseChastUri('chast://CONNECTION/new/42')!.target, '42');
      expect(parseChastUri('chast://connection/new/42/')!.target, '42');
      expect(parseChastUri('chast://connection/new/a%2Db')!.target, 'a-b');
    });

    test('ignores query and fragment', () {
      expect(
        parseChastUri('chast://connection/new/42?password=x#top')!.target,
        '42',
      );
      expect(parseChastUri('chast://42?foo=1')!.target, '42');
    });

    test('trims surrounding whitespace', () {
      expect(parseChastUri('  chast://42  ')!.target, '42');
    });

    test('rejects malformed links', () {
      expect(parseChastUri(''), isNull);
      expect(parseChastUri('chast://'), isNull);
      expect(parseChastUri('chast:///'), isNull);
      expect(parseChastUri('https://example.com'), isNull);
      expect(parseChastUri('123456789'), isNull);
      expect(parseChastUri('chast://connection'), isNull);
      expect(parseChastUri('chast://connection/42'), isNull);
      expect(parseChastUri('chast://connection/session/42'), isNull);
      expect(parseChastUri('chast://connection/new/'), isNull);
      expect(parseChastUri('chast://connection/new/a/b'), isNull);
      expect(parseChastUri('chast://file-transfer/new/'), isNull);
    });
  });

  group('parseLaunchArguments', () {
    test('reads --connect with a separate value', () {
      final DeepLinkRequest link = parseLaunchArguments(
        <String>['--connect', '127.0.0.1:45443'],
      )!;
      expect(link.target, '127.0.0.1:45443');
      expect(link.fileTransferOnly, isFalse);
    });

    test('reads --connect=value', () {
      expect(
        parseLaunchArguments(<String>['--connect=127.0.0.1:45443'])!.target,
        '127.0.0.1:45443',
      );
    });

    test('reads a clicked link among other arguments', () {
      final DeepLinkRequest link = parseLaunchArguments(<String>[
        '--foo',
        'chast://connection/new/42',
        '--bar',
      ])!;
      expect(link.target, '42');
    });

    test('marks a file-transfer link', () {
      expect(
        parseLaunchArguments(<String>[
          'chast://file-transfer/new/42',
        ])!.fileTransferOnly,
        isTrue,
      );
    });

    test('uses the first request when several are present', () {
      expect(
        parseLaunchArguments(<String>['chast://42', '--connect', '43'])!.target,
        '42',
      );
      expect(
        parseLaunchArguments(<String>['--connect=43', 'chast://42'])!.target,
        '43',
      );
    });

    test('returns null without a request', () {
      expect(parseLaunchArguments(const <String>[]), isNull);
      expect(parseLaunchArguments(<String>['--connect']), isNull);
      expect(parseLaunchArguments(<String>['--foo', 'bar']), isNull);
    });
  });
}
