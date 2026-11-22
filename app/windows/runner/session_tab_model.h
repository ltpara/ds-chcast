#ifndef RUNNER_SESSION_TAB_MODEL_H_
#define RUNNER_SESSION_TAB_MODEL_H_

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

class SessionTabModel {
 public:
  using Id = uint64_t;

  struct Tab {
    Id id;
    bool closing = false;
  };

  std::optional<Id> Add() {
    if (next_id_ == 0) return std::nullopt;
    const Id id = next_id_++;
    tabs_.push_back(Tab{id, false});
    selected_ = id;
    return id;
  }

  bool Select(Id id) {
    const auto it = Find(id);
    if (it == tabs_.end() || it->closing) return false;
    selected_ = id;
    return true;
  }

  bool BeginClose(Id id) {
    const auto it = Find(id);
    if (it == tabs_.end() || it->closing) return false;
    it->closing = true;
    if (selected_ == id) SelectNeighbor(it);
    return true;
  }

  bool RollbackFailedClose(Id id, std::optional<Id> selected_before,
                           bool window_alive) {
    if (!window_alive) return false;
    const auto it = Find(id);
    if (it == tabs_.end() || !it->closing) return false;
    it->closing = false;
    if (selected_before) {
      const auto previous = Find(*selected_before);
      if (previous != tabs_.end() && !previous->closing) {
        selected_ = *selected_before;
        return true;
      }
    }
    if (!selected_) SelectNeighbor(it);
    return true;
  }

  bool Remove(Id id) {
    const auto it = Find(id);
    if (it == tabs_.end()) return false;
    const bool was_selected = selected_ == id;
    const auto next = tabs_.erase(it);
    if (was_selected) SelectNeighbor(next);
    return true;
  }

  bool IsSelected(Id id) const { return selected_ == id; }
  std::optional<Id> selected() const { return selected_; }
  const std::vector<Tab>& tabs() const { return tabs_; }

 private:
  using Iterator = std::vector<Tab>::iterator;

  Iterator Find(Id id) {
    return std::find_if(tabs_.begin(), tabs_.end(),
                        [id](const Tab& tab) { return tab.id == id; });
  }

  std::vector<Tab>::const_iterator Find(Id id) const {
    return std::find_if(tabs_.begin(), tabs_.end(),
                        [id](const Tab& tab) { return tab.id == id; });
  }

  void SelectNeighbor(Iterator position) {
    const auto index = static_cast<size_t>(position - tabs_.begin());
    for (size_t i = index; i < tabs_.size(); ++i) {
      if (!tabs_[i].closing) {
        selected_ = tabs_[i].id;
        return;
      }
    }
    for (size_t i = index; i > 0; --i) {
      if (!tabs_[i - 1].closing) {
        selected_ = tabs_[i - 1].id;
        return;
      }
    }
    selected_.reset();
  }

  std::vector<Tab> tabs_;
  std::optional<Id> selected_;
  Id next_id_ = 1;
};

#endif  // RUNNER_SESSION_TAB_MODEL_H_
