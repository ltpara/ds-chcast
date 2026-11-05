import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/search_match.dart';

void main() {
  test('an empty or blank query matches everything', () {
    expect(SearchMatch.matches('', <String>['张三']), isTrue);
    expect(SearchMatch.matches('   ', <String>['张三']), isTrue);
    expect(SearchMatch.matches('', const <String>[]), isTrue);
  });

  test('Han names match by initials, full pinyin and substring', () {
    expect(SearchMatch.matches('zs', <String>['张三']), isTrue);
    expect(SearchMatch.matches('ZS', <String>['张三']), isTrue);
    expect(SearchMatch.matches('zhangsan', <String>['张三']), isTrue);
    expect(SearchMatch.matches('zhang', <String>['张三']), isTrue);
    expect(SearchMatch.matches('san', <String>['张三']), isTrue);
    expect(SearchMatch.matches('张', <String>['张三']), isTrue);
    expect(SearchMatch.matches('lisi', <String>['张三']), isFalse);
  });

  test('any field can satisfy the query', () {
    expect(
      SearchMatch.matches('office', <String>['10.0.0.9:4433', 'Office Mac']),
      isTrue,
    );
    expect(SearchMatch.matches('4433', <String>['10.0.0.9:4433']), isTrue);
    expect(SearchMatch.matches('office', <String>['lab']), isFalse);
  });

  test('whitespace-separated tokens must all match', () {
    expect(SearchMatch.matches('office mac', <String>['Office Mac']), isTrue);
    expect(SearchMatch.matches('office lab', <String>['Office Mac']), isFalse);
    // Each token may hit a different form of the same field.
    expect(SearchMatch.matches('zhang san', <String>['张三']), isTrue);
    // Or a different field entirely.
    expect(
      SearchMatch.matches('office 4433', <String>['Office Mac', '10.0.0.9:4433']),
      isTrue,
    );
    expect(
      SearchMatch.matches('zhang lab', <String>['张三', 'lab']),
      isTrue,
    );
  });

  test('pure ASCII fields keep plain substring matching', () {
    expect(SearchMatch.matches('box', <String>['labbox']), isTrue);
    expect(SearchMatch.matches('LABBOX', <String>['labbox']), isTrue);
    expect(SearchMatch.matches('zs', <String>['labbox']), isFalse);
  });
}
