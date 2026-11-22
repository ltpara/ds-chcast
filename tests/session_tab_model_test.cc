#include <cstdio>

#include "session_tab_model.h"

namespace {
int failures = 0;

void Check(bool condition, const char* name) {
  if (condition) return;
  std::fprintf(stderr, "FAIL %s\n", name);
  ++failures;
}
}  // namespace

int main() {
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
