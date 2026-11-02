import 'package:flutter_test/flutter_test.dart';

import 'package:chast_viewer/pinyin_initials.dart';
import 'package:chast_viewer/search_match.dart';

void main() {
  test('pinyin initials cover the common hanzi and drop separators', () {
    expect(pinyinInitials('我的电脑'), 'wddn');
    expect(pinyinInitials('我的 电脑'), 'wddn');
    expect(pinyinInitials('办公室-PC!'), 'bgspc');
    expect(pinyinInitials('Office Mac'), 'officemac');
    expect(pinyinInitials('重庆'), 'zq');
    expect(pinyinInitials(''), '');
  });

  test('the generated table keeps the GB2312 level-1 size', () {
    final int count = pinyinInitialGroups.values.fold<int>(
      0,
      (int sum, String group) => sum + group.runes.length,
    );
    expect(pinyinInitialGroups.length, 23);
    expect(count, 3755);
  });

  test('search matches literal substrings and pinyin initials', () {
    expect(matchesSearch(<String>['实验室'], 'sys'), isTrue);
    expect(matchesSearch(<String>['实验室'], 'SYS'), isTrue);
    expect(matchesSearch(<String>['实验室'], '实验'), isTrue);
    expect(matchesSearch(<String>['实验室'], 'lab'), isFalse);
    expect(matchesSearch(<String>['Office Mac'], 'mac'), isTrue);
    expect(matchesSearch(<String>['工作室 PC'], 'gzspc'), isTrue);
    expect(matchesSearch(<String>['工作室 PC'], 'pc'), isTrue);
  });

  test('any field can hit, and a blank query matches', () {
    final List<String> fields = <String>['10.0.0.9:4433', '工作室'];
    expect(matchesSearch(fields, '10.0.0.9'), isTrue);
    expect(matchesSearch(fields, 'gzs'), isTrue);
    expect(matchesSearch(fields, 'nope'), isFalse);
    expect(matchesSearch(fields, '   '), isTrue);
  });

  test('characters outside GB2312 level-1 only match literally', () {
    expect(pinyinInitials('𠀀'), '');
    expect(matchesSearch(<String>['𠀀'], '𠀀'), isTrue);
    expect(matchesSearch(<String>['𠀀'], 'yk'), isFalse);
  });
}
