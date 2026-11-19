#include <cstdio>

#include "chast/reference_counted_lease.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}

void test_shared_lifetime() {
    int acquired = 0;
    int released = 0;
    chast::ReferenceCountedLease lease(
        [&] { ++acquired; return true; }, [&] { ++released; return true; });
    check(lease.acquire() && lease.acquire(), "two sessions acquire a lease");
    check(acquired == 1 && lease.count() == 2,
          "the underlying request is acquired once");
    check(lease.release() && released == 0 && lease.count() == 1,
          "closing one session keeps the request active");
    check(lease.release() && released == 1 && lease.count() == 0,
          "the final session releases the request");
    check(!lease.release() && released == 1,
          "an unmatched release does not underflow or release twice");
}

void test_failed_acquire_is_retryable() {
    int attempts = 0;
    int released = 0;
    chast::ReferenceCountedLease lease(
        [&] { return ++attempts > 1; }, [&] { ++released; return true; });
    check(!lease.acquire() && lease.count() == 0,
          "a rejected underlying request creates no lease");
    check(lease.acquire() && lease.count() == 1,
          "a later acquire retries after failure");
    check(lease.release() && released == 1,
          "a successful retry is released once");
}

void test_final_release_reports_failure() {
    chast::ReferenceCountedLease lease([] { return true; }, [] { return false; });
    check(lease.acquire(), "lease is acquired before release failure");
    check(!lease.release() && lease.count() == 0,
          "final cleanup failure is reported after dropping the lease");
}

}  // namespace

int main() {
    test_shared_lifetime();
    test_failed_acquire_is_retryable();
    test_final_release_reports_failure();
    return failures == 0 ? 0 : 1;
}
