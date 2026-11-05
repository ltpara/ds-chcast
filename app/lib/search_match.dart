import 'package:pinyin/pinyin.dart';

/// Address-book matching shared by "我的设备" and the shared-books dialog.
///
/// A query is split on whitespace and every token must match at least one of
/// the supplied fields. Besides a plain case-insensitive substring, a field
/// that contains Han characters is also matched by its full pinyin and by the
/// pinyin initials, so "zs" and "zhangsan" both find "张三". Fields that are
/// pure ASCII keep the original substring behavior.
class SearchMatch {
  SearchMatch._();

  static final Map<String, _Forms> _cache = <String, _Forms>{};
  static const int _cacheLimit = 4096;
  static final RegExp _whitespace = RegExp(r'\s+');
  static final RegExp _han = RegExp(r'[\u3400-\u9fff\uf900-\ufaff]');

  /// True when [query] is empty or every token matches one of [fields].
  static bool matches(String query, Iterable<String> fields) {
    final List<String> tokens = query
        .trim()
        .toLowerCase()
        .split(_whitespace)
        .where((String token) => token.isNotEmpty)
        .toList();
    if (tokens.isEmpty) {
      return true;
    }
    final List<String> list = fields.toList();
    for (final String token in tokens) {
      if (!list.any((String field) => _matchesToken(field, token))) {
        return false;
      }
    }
    return true;
  }

  static bool _matchesToken(String field, String token) {
    if (field.isEmpty) {
      return false;
    }
    final _Forms forms = _formsFor(field);
    return forms.lower.contains(token) ||
        forms.pinyin.contains(token) ||
        forms.initials.contains(token);
  }

  /// Pinyin conversion is cached because a keystroke re-checks every device
  /// and the dictionary lookup is much more expensive than the comparison.
  static _Forms _formsFor(String field) {
    final _Forms? cached = _cache[field];
    if (cached != null) {
      return cached;
    }
    final String lower = field.toLowerCase();
    String pinyin = lower;
    String initials = lower;
    if (_han.hasMatch(field)) {
      pinyin = PinyinHelper.getPinyinE(
        field,
        separator: '',
        defPinyin: '',
      ).toLowerCase();
      initials = PinyinHelper.getShortPinyin(
        field,
      ).replaceAll(' ', '').toLowerCase();
    }
    if (_cache.length >= _cacheLimit) {
      _cache.clear();
    }
    final _Forms forms = _Forms(lower, pinyin, initials);
    _cache[field] = forms;
    return forms;
  }
}

class _Forms {
  const _Forms(this.lower, this.pinyin, this.initials);

  final String lower;
  final String pinyin;
  final String initials;
}
