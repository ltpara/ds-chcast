#include <windows.h>

#include <cstdio>
#include <vector>

#include "rollback_stack.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}

void test_reverse_order_and_first_error() {
    std::vector<int> order;
    chast::host::detail::RollbackStack stack;
    stack.push([&] { order.push_back(1); return ERROR_ACCESS_DENIED; });
    stack.push([&] { order.push_back(2); return 0; });
    stack.push([&] { order.push_back(3); return ERROR_GEN_FAILURE; });
    const int error = stack.rollback();
    check(order == std::vector<int>{3, 2, 1}, "rollback actions run in reverse registration order");
    check(error == ERROR_GEN_FAILURE, "rollback preserves the first cleanup error");
    check(stack.rollback() == 0, "rollback is idempotent");
}

void test_commit_discards_actions() {
    int calls = 0;
    chast::host::detail::RollbackStack stack;
    stack.push([&] { ++calls; return 0; });
    stack.commit();
    check(stack.rollback() == 0 && calls == 0, "commit discards rollback actions");
}

}  // namespace

int main() {
    test_reverse_order_and_first_error();
    test_commit_discards_actions();
    return failures == 0 ? 0 : 1;
}
