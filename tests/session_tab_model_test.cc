#include <cstdio>

#include "session_tab_labels.h"
#include "session_tab_model.h"

namespace {
int failures = 0;

void Check(bool condition, const char* name) {
  if (condition) return;
  std::fprintf(stderr, "FAIL %s\n", name);
  ++failures;
}

void TestLabels() {
  using namespace session_tab_labels;
  const auto clipped = BaseLabel(std::wstring(31, L'A') + L"xx", 9);
  Check(clipped == BaseLabel(std::wstring(31, L'A') + L"yy", 10),
        "collision detection uses the capped base label");
  Check(BaseLabel(L"A\tB\n", 7) == L"AB", "label drops controls");
  const auto long_label = std::wstring(96, L'N');
  Check(Sanitize(long_label) == long_label &&
            BaseLabel(long_label, 7).size() == 32,
        "full accessible caption is retained while the collision key is capped");
  Check(AccessibleCaption(long_label, L"#7") ==
            long_label + L" · #7",
        "accessible captions retain the full long name and disambiguator");
  Check(BaseLabel(L"\n", 7) == L"\u4f1a\u8bdd 7",
        "empty label falls back to stable id");
  Check(PublicDeviceHint("D_42-9") == L"D_42-9" &&
            PublicDeviceHint("abcdefg").empty() &&
            PublicDeviceHint("pw: 12").empty(),
        "only bounded public-id characters are accepted");

  std::vector<Tab> tabs{{1, L"Office", L"12AB"},
                        {2, L"Office", L"34CD"},
                        {4, L"Office", L""},
                        {5, L"Lab", L"12AB"}};
  Check(Disambiguator(tabs[0], tabs) == L"12AB" &&
            Disambiguator(tabs[1], tabs) == L"34CD",
        "same named devices get their distinct device hints");
  Check(Disambiguator(tabs[2], tabs) == L"#4" &&
            Disambiguator(tabs[3], tabs).empty(),
        "missing hint uses a local number; unique names stay unchanged");
  tabs.push_back({7, L"Office", L"12AB"});
  Check(Disambiguator(tabs[0], tabs) == L"#1" &&
            Disambiguator(tabs.back(), tabs) == L"#7",
        "duplicate hints use distinct stable numbers");
  tabs.erase(tabs.begin() + 1);
  Check(Disambiguator(tabs[1], tabs) == L"#4",
        "remaining tab number is stable when a neighbor closes");
  tabs = {{1, clipped, L""}, {2, clipped, L""}};
  Check(Disambiguator(tabs[0], tabs) == L"#1" &&
            Disambiguator(tabs[1], tabs) == L"#2",
        "long aliases sharing the capped prefix also get a suffix");
  tabs.pop_back();
  Check(Disambiguator(tabs.front(), tabs).empty(),
        "suffix is removed when no collision remains");
}
}  // namespace

int main() {
  TestLabels();
  SessionTabModel model;
  const auto first = model.Add();
  const auto second = model.Add();
  const auto third = model.Add();
  Check(first && second && third, "new tabs receive stable ids");
  if (!first || !second || !third) return 1;
  Check(model.selected() == third, "newly opened tab becomes active");
  Check(model.Select(*first), "select existing tab");
  Check(model.Select(*third), "switch to another tab");
  Check(model.BeginClose(*third), "start closing selected tab");
  Check(model.selected() == second,
        "closing the final tab selects the nearest previous live tab");
  Check(!model.Select(*third), "closing tab cannot be reselected");
  Check(!model.BeginClose(*third), "duplicate close is ignored");
  Check(!model.RollbackFailedClose(*third, second, false),
        "a dead child window stays closing for pruning");
  Check(!model.Select(*third), "dead child is not restored as a tab");
  Check(model.RollbackFailedClose(*third, second, true),
        "failed close can be rolled back while child is alive");
  Check(model.IsSelected(*second), "rollback preserves the prior selection");
  Check(model.BeginClose(*third), "rolled back tab can be closed again");
  Check(model.Remove(*third), "remove the closed tab by stable id");
  Check(!model.Remove(*third), "duplicate removal is ignored");
  Check(model.IsSelected(*second), "removing a background tab preserves selection");
  Check(model.BeginClose(*second), "close middle tab");
  Check(model.selected() == first, "closing last live neighbor selects previous tab");
  Check(model.BeginClose(*first), "close final live tab");
  Check(!model.selected(), "no tab remains selected while all tabs close");
  Check(model.RollbackFailedClose(*first, std::nullopt, true),
        "failed final close restores a selectable tab");
  Check(model.IsSelected(*first), "final close rollback selects restored tab");
  Check(model.BeginClose(*first), "restored tab can be closed again");
  Check(model.Remove(*second), "remove the middle tab");
  Check(model.Remove(*first), "remove the final tab");
  Check(!model.selected() && model.tabs().empty(), "empty model is stable");
  const auto replacement = model.Add();
  Check(replacement && replacement != first, "new tab does not reuse a closed id");
  return failures ? 1 : 0;
}
