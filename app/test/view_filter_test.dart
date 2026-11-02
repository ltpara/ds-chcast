import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/view_filter.dart';

void main() {
  late Directory dir;
  late String path;

  setUp(() {
    dir = Directory.systemTemp.createTempSync('chast_view_filter_');
    path = '${dir.path}${Platform.pathSeparator}ui-filter.json';
    ViewFilter.configPathOverride = path;
  });

  tearDown(() {
    ViewFilter.configPathOverride = null;
    dir.deleteSync(recursive: true);
  });

  test('tags round-trip through the filter file', () {
    ViewFilter.save(<String>{'office', 'lab'});
    expect(jsonDecode(File(path).readAsStringSync()), <String, Object?>{
      'tags': <String>['lab', 'office'],
    });
    expect(ViewFilter.load(), <String>{'lab', 'office'});
  });

  test('blank tags are dropped and duplicates collapse', () {
    ViewFilter.save(<String>[' lab ', '', '  ', 'lab', 'office', 'office']);
    expect(ViewFilter.load(), <String>{'lab', 'office'});
  });

  test('a missing or corrupt file reads as no filter', () {
    expect(ViewFilter.load(), isEmpty);
    File(path).writeAsStringSync('{not json');
    expect(ViewFilter.load(), isEmpty);
    File(path).writeAsStringSync('{"tags": "lab"}');
    expect(ViewFilter.load(), isEmpty);
  });

  test('clearing the filter writes an empty list', () {
    ViewFilter.save(<String>{'lab'});
    ViewFilter.save(const <String>{});
    expect(File(path).readAsStringSync(), '{"tags":[]}');
    expect(ViewFilter.load(), isEmpty);
  });

  test('search text round-trips per view without clobbering', () {
    ViewFilter.saveDeviceSearch(' 实验 ');
    ViewFilter.saveSharedSearch('sys');
    expect(jsonDecode(File(path).readAsStringSync()), <String, Object?>{
      'deviceSearch': '实验',
      'sharedSearch': 'sys',
    });
    expect(ViewFilter.loadDeviceSearch(), '实验');
    expect(ViewFilter.loadSharedSearch(), 'sys');

    ViewFilter.save(<String>{'lab'});
    ViewFilter.saveCollapsed(<String>{'g1'});
    expect(ViewFilter.loadSharedSearch(), 'sys');
    expect(ViewFilter.loadDeviceSearch(), '实验');

    ViewFilter.saveSharedSearch('office');
    expect(ViewFilter.loadDeviceSearch(), '实验');
    expect(ViewFilter.loadSharedSearch(), 'office');
  });

  test('clearing a search removes only its own key', () {
    ViewFilter.saveDeviceSearch('实验');
    ViewFilter.saveSharedSearch('sys');
    ViewFilter.saveDeviceSearch('   ');
    final Map<String, Object?> payload =
        jsonDecode(File(path).readAsStringSync()) as Map<String, Object?>;
    expect(payload.containsKey('deviceSearch'), isFalse);
    expect(payload['sharedSearch'], 'sys');
    expect(ViewFilter.loadDeviceSearch(), isEmpty);

    ViewFilter.saveSharedSearch('');
    expect(jsonDecode(File(path).readAsStringSync()), <String, Object?>{});
    expect(ViewFilter.loadSharedSearch(), isEmpty);
  });

  test('collapsed groups and tags share the file without clobbering', () {
    ViewFilter.save(<String>{'lab'});
    ViewFilter.saveCollapsed(<String>{'', 'g1'});
    expect(ViewFilter.load(), <String>{'lab'});
    expect(ViewFilter.loadCollapsed(), <String>{'', 'g1'});

    ViewFilter.save(<String>{'office'});
    expect(ViewFilter.loadCollapsed(), <String>{'', 'g1'});

    ViewFilter.saveCollapsed(const <String>{});
    expect(ViewFilter.load(), <String>{'office'});
    expect(ViewFilter.loadCollapsed(), isEmpty);
  });

  test('the favorites-only filter round-trips and clears its key', () {
    expect(ViewFilter.loadFavoritesOnly(), isFalse);
    ViewFilter.saveFavoritesOnly(true);
    expect(ViewFilter.loadFavoritesOnly(), isTrue);

    ViewFilter.save(<String>{'lab'});
    expect(ViewFilter.loadFavoritesOnly(), isTrue);

    ViewFilter.saveFavoritesOnly(false);
    expect(ViewFilter.loadFavoritesOnly(), isFalse);
    expect(jsonDecode(File(path).readAsStringSync()), <String, Object?>{
      'tags': <String>['lab'],
    });
  });
}
