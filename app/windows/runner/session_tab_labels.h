#ifndef RUNNER_SESSION_TAB_LABELS_H_
#define RUNNER_SESSION_TAB_LABELS_H_

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace session_tab_labels {

inline std::wstring Sanitize(std::wstring label) {
  label.erase(std::remove_if(label.begin(), label.end(),
                            [](wchar_t c) { return c < 0x20 || c == 0x7f; }),
              label.end());
  return label;
}

inline std::wstring BaseLabel(std::wstring label, uint64_t id) {
  label = Sanitize(std::move(label));
  if (label.empty()) return L"\u4f1a\u8bdd " + std::to_wstring(id);
  constexpr size_t kMaxLength = 32;
  if (label.size() > kMaxLength) {
    label.resize(kMaxLength - 1);
    // On Windows wchar_t is a UTF-16 code unit.
    if (label.back() >= 0xd800 && label.back() <= 0xdbff) label.pop_back();
    label.push_back(L'\u2026');
  }
  return label;
}

inline std::wstring PublicDeviceHint(std::string_view hint) {
  if (hint.size() > 6 ||
      !std::all_of(hint.begin(), hint.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
               (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.';
      })) {
    return {};
  }
  return std::wstring(hint.begin(), hint.end());
}

inline std::wstring AccessibleCaption(std::wstring_view full_label,
                                     std::wstring_view suffix) {
  std::wstring caption(full_label);
  if (!suffix.empty()) caption += L" · " + std::wstring(suffix);
  return caption;
}

struct Tab {
  uint64_t id;
  std::wstring_view label;
  std::wstring_view device_hint;
};

inline std::wstring Disambiguator(const Tab& tab,
                                  const std::vector<Tab>& tabs) {
  bool duplicate_name = false;
  bool duplicate_hint = false;
  for (const auto& other : tabs) {
    if (other.id == tab.id || other.label != tab.label) continue;
    duplicate_name = true;
    duplicate_hint |= other.device_hint == tab.device_hint;
  }
  if (!duplicate_name) return {};
  if (!duplicate_hint && !tab.device_hint.empty())
    return std::wstring(tab.device_hint);
  return L"#" + std::to_wstring(tab.id);
}

}  // namespace session_tab_labels

#endif  // RUNNER_SESSION_TAB_LABELS_H_
