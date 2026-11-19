#pragma once

#include <functional>
#include <utility>
#include <vector>

namespace chast::host::detail {

class RollbackStack {
public:
    using Action = std::function<int()>;

    RollbackStack() = default;
    RollbackStack(const RollbackStack&) = delete;
    RollbackStack& operator=(const RollbackStack&) = delete;
    ~RollbackStack() {
        if (!finished_) (void)rollback();
    }

    void push(Action action) {
        if (!finished_ && action) actions_.push_back(std::move(action));
    }

    int rollback() {
        if (finished_) return 0;
        int first_error = 0;
        while (!actions_.empty()) {
            const int error = actions_.back()();
            if (first_error == 0 && error != 0) first_error = error;
            actions_.pop_back();
        }
        finished_ = true;
        return first_error;
    }

    void commit() {
        actions_.clear();
        finished_ = true;
    }

private:
    std::vector<Action> actions_;
    bool finished_ = false;
};

}  // namespace chast::host::detail
