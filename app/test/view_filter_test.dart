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

  test('searches round-trip per view without clobbering each other', () {
    ViewFilter.saveSearch(SearchView.devices, ' lab ');
    ViewFilter.saveSearch(SearchView.shared, 'zs');
    expect(ViewFilter.loadSearch(SearchView.devices), 'lab');
    expect(ViewFilter.loadSearch(SearchView.shared), 'zs');
    expect(ViewFilter.load(), isEmpty);

    ViewFilter.save(<String>{'office'});
    ViewFilter.saveCollapsed(<String>{'g1'});
    expect(ViewFilter.loadSearch(SearchView.devices), 'lab');
    expect(ViewFilter.loadSearch(SearchView.shared), 'zs');

    ViewFilter.saveSearch(SearchView.devices, '');
    expect(ViewFilter.loadSearch(SearchView.devices), isEmpty);
    expect(ViewFilter.loadSearch(SearchView.shared), 'zs');
    expect(ViewFilter.load(), <String>{'office'});
    expect(ViewFilter.loadCollapsed(), <String>{'g1'});
    final Map<String, Object?> saved =
        jsonDecode(File(path).readAsStringSync()) as Map<String, Object?>;
    expect(saved.containsKey('search_devices'), isFalse);
    expect(saved['search_shared'], 'zs');
  });

  test('a whitespace-only search clears the saved query', () {
    ViewFilter.saveSearch(SearchView.shared, 'zs');
    ViewFilter.saveSearch(SearchView.shared, '   ');
    final Map<String, Object?> saved =
        jsonDecode(File(path).readAsStringSync()) as Map<String, Object?>;
    expect(saved.containsKey('search_shared'), isFalse);
    expect(ViewFilter.loadSearch(SearchView.shared), isEmpty);
  });
}
