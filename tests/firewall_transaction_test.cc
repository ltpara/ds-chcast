#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "firewall_transaction.h"

namespace {

using chast::host::detail::FirewallRuleBackend;
using chast::host::detail::FirewallRuleIdentity;
using chast::host::detail::FirewallRuleSnapshot;
using chast::host::detail::FirewallRuleSpec;
using chast::host::detail::FirewallTransactionResult;

struct RuleValue {
    std::wstring application;
    std::wstring protocol;
    uint16_t port = 0;
};

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}

bool same_path(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

class FakeBackend final : public FirewallRuleBackend {
public:
    std::map<std::wstring, RuleValue> rules;
    int fail_add_at = 0;
    int fail_remove_at = 0;
    int fail_restore_at = 0;
    int add_count = 0;
    int remove_count = 0;
    int restore_count = 0;

    int inspect(const FirewallRuleIdentity& identity,
                FirewallRuleSnapshot& snapshot) override {
        const auto it = rules.find(identity.name);
        if (it == rules.end()) return 0;
        const bool owned = std::any_of(identity.permitted_applications.begin(),
                                       identity.permitted_applications.end(),
                                       [&it](const std::wstring& path) {
                                           return same_path(it->second.application, path);
                                       });
        if (!owned) return ERROR_ALREADY_EXISTS;
        snapshot.exists = true;
        snapshot.native = std::make_shared<RuleValue>(it->second);
        return 0;
    }

    int remove(const FirewallRuleIdentity& identity) override {
        if (++remove_count == fail_remove_at) return ERROR_ACCESS_DENIED;
        rules.erase(identity.name);
        return 0;
    }

    int add(const FirewallRuleSpec& rule) override {
        if (++add_count == fail_add_at) return ERROR_GEN_FAILURE;
        rules[rule.name] = {rule.application, rule.protocol, rule.local_port};
        return 0;
    }

    int restore(const FirewallRuleIdentity& identity,
                const FirewallRuleSnapshot& snapshot) override {
        if (++restore_count == fail_restore_at) return ERROR_ACCESS_DENIED;
        if (!snapshot.exists) {
            rules.erase(identity.name);
            return 0;
        }
        const auto previous = std::static_pointer_cast<RuleValue>(snapshot.native);
        rules[identity.name] = *previous;
        return 0;
    }
};

std::vector<FirewallRuleIdentity> managed_rules() {
    return {{L"host", {L"C:\\chast\\chast-service.exe", L"C:\\chast\\chast-host.exe"}},
            {L"tunnel", {L"C:\\chast\\chast-tunnel.exe"}}};
}

std::vector<FirewallRuleSpec> desired_rules() {
    return {{L"host", L"C:\\chast\\chast-service.exe", L"UDP", 21110},
            {L"tunnel", L"C:\\chast\\chast-tunnel.exe", L"UDP", 21120}};
}

bool original_state(const FakeBackend& backend) {
    return backend.rules.size() == 1 && backend.rules.contains(L"host") &&
           backend.rules.at(L"host").port == 21100 &&
           same_path(backend.rules.at(L"host").application,
                     L"C:\\chast\\chast-host.exe");
}

void test_successful_replacement() {
    FakeBackend backend;
    backend.rules[L"host"] = {L"C:\\chast\\chast-host.exe", L"UDP", 21100};
    backend.rules[L"stale"] = {L"C:\\chast\\chast-tunnel.exe", L"UDP", 21130};
    const auto result = chast::host::detail::replace_firewall_rules(
        backend, managed_rules(), desired_rules());
    check(result.error == 0 && result.rollback_error == 0,
          "successful rule replacement returns cleanly");
    check(backend.rules.size() == 3 && backend.rules.contains(L"host") &&
              backend.rules.contains(L"tunnel") && backend.rules.at(L"host").port == 21110,
          "success leaves exactly the requested managed rules");
    check(backend.rules.contains(L"stale"), "unmanaged names are never touched");
}

void test_add_failure_restores_snapshot() {
    FakeBackend backend;
    backend.rules[L"host"] = {L"C:\\chast\\chast-host.exe", L"UDP", 21100};
    backend.fail_add_at = 2;
    const auto result = chast::host::detail::replace_firewall_rules(
        backend, managed_rules(), desired_rules());
    check(result.error == ERROR_GEN_FAILURE && result.rollback_error == 0,
          "add failure is returned after a successful rollback");
    check(original_state(backend), "add failure restores the exact pre-transaction state");
}

void test_remove_failure_restores_snapshot() {
    FakeBackend backend;
    backend.rules[L"host"] = {L"C:\\chast\\chast-host.exe", L"UDP", 21100};
    backend.fail_remove_at = 2;
    const auto result = chast::host::detail::replace_firewall_rules(
        backend, managed_rules(), desired_rules());
    check(result.error == ERROR_ACCESS_DENIED && result.rollback_error == 0,
          "remove failure is returned after restoring the snapshot");
    check(original_state(backend), "partial removal is rolled back to the exact prior state");
}

void test_conflicting_rule_refuses_mutation() {
    FakeBackend backend;
    backend.rules[L"host"] = {L"C:\\other\\unrelated.exe", L"UDP", 21100};
    const auto result = chast::host::detail::replace_firewall_rules(
        backend, managed_rules(), desired_rules());
    check(result.error == ERROR_ALREADY_EXISTS && result.rollback_error == 0,
          "foreign fixed-name rules fail preflight");
    check(backend.rules.size() == 1 &&
              same_path(backend.rules.at(L"host").application,
                        L"C:\\other\\unrelated.exe") &&
              backend.remove_count == 0 && backend.add_count == 0,
          "ownership conflict causes no backend mutations");
}

void test_rollback_failure_is_reported() {
    FakeBackend backend;
    backend.rules[L"host"] = {L"C:\\chast\\chast-host.exe", L"UDP", 21100};
    backend.fail_add_at = 1;
    backend.fail_restore_at = 1;
    const auto result = chast::host::detail::replace_firewall_rules(
        backend, managed_rules(), desired_rules());
    check(result.error == ERROR_GEN_FAILURE && result.rollback_error == ERROR_ACCESS_DENIED,
          "rollback failure remains distinguishable from the original operation failure");
}

void test_clear_can_be_rolled_back() {
    FakeBackend backend;
    backend.rules[L"host"] = {L"C:\\chast\\chast-host.exe", L"UDP", 21100};
    backend.rules[L"tunnel"] = {L"C:\\chast\\chast-tunnel.exe", L"UDP", 21120};

    chast::host::detail::FirewallRuleTransaction transaction(backend);
    const auto result = transaction.apply(managed_rules(), {});
    check(result.error == 0 && backend.rules.empty(),
          "clearing managed firewall rules removes the managed set");
    check(transaction.rollback() == 0 && backend.rules.size() == 2 &&
              backend.rules.at(L"host").port == 21100 &&
              backend.rules.at(L"tunnel").port == 21120,
          "a later uninstall failure can restore all cleared rules");
}

}  // namespace

int main() {
    test_successful_replacement();
    test_add_failure_restores_snapshot();
    test_remove_failure_restores_snapshot();
    test_conflicting_rule_refuses_mutation();
    test_rollback_failure_is_reported();
    test_clear_can_be_rolled_back();
    return failures == 0 ? 0 : 1;
}
