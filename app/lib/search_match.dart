import 'pinyin_initials.dart';

final Map<int, String> _initialByRune = <int, String>{
  for (final MapEntry<String, String> group in pinyinInitialGroups.entries)
    for (final int rune in group.value.runes) rune: group.key,
};

bool _isAsciiAlphanumeric(int rune) {
  return (rune >= 0x30 && rune <= 0x39) ||
      (rune >= 0x41 && rune <= 0x5A) ||
      (rune >= 0x61 && rune <= 0x7A);
}

/// Pinyin initials of [text]. Hanzi use the primary reading recorded by the
/// GB2312 level-1 order (so 重 is "z" and 长 is "c"); ASCII letters and
/// digits are kept lowercased and spaces or punctuation are dropped, making
/// both "我的电脑" and "我的 电脑" yield "wddn". Other scripts are left out
/// of the initials form and still match literally.
String pinyinInitials(String text) {
  final StringBuffer buffer = StringBuffer();
  for (final int rune in text.runes) {
    final String? initial = _initialByRune[rune];
    if (initial != null) {
      buffer.write(initial);
      continue;
    }
    if (_isAsciiAlphanumeric(rune)) {
      buffer.write(String.fromCharCode(rune).toLowerCase());
    }
  }
  return buffer.toString();
}

/// Case-insensitive substring match against any field, extended with the
/// pinyin-initial form so "wddn" finds "我的电脑". A blank query matches
/// everything. Rare characters outside GB2312 level-1 only match literally.
bool matchesSearch(Iterable<String> fields, String query) {
  final String needle = query.trim().toLowerCase();
  if (needle.isEmpty) {
    return true;
  }
  for (final String field in fields) {
    if (field.toLowerCase().contains(needle)) {
      return true;
    }
    if (pinyinInitials(field).contains(needle)) {
      return true;
    }
  }
  return false;
}
