#include "firewall_transaction.h"

#include <windows.h>

#include <algorithm>
#include <utility>

namespace chast::host::detail {
namespace {

bool same_name(const std::wstring& left, const std::wstring& right) {
    return !left.empty() && !right.empty() &&
           CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool permitted_application(const FirewallRuleIdentity& identity,
                           const std::wstring& application) {
    return std::any_of(identity.permitted_applications.begin(),
                       identity.permitted_applications.end(),
                       [&application](const std::wstring& permitted) {
                           return !application.empty() && !permitted.empty() &&
                                  CompareStringOrdinal(application.c_str(), -1,
                                                       permitted.c_str(), -1, TRUE) == CSTR_EQUAL;
                       });
}

}  // namespace

FirewallRuleTransaction::~FirewallRuleTransaction() {
    if (active_) (void)rollback();
}

FirewallTransactionResult FirewallRuleTransaction::apply(
    const std::vector<FirewallRuleIdentity>& managed,
    const std::vector<FirewallRuleSpec>& desired) {
    FirewallTransactionResult result;
    if (active_) {
        result.error = ERROR_INVALID_STATE;
        return result;
    }
    for (size_t index = 0; index < managed.size(); ++index) {
        if (managed[index].name.empty() || managed[index].permitted_applications.empty()) {
            result.error = ERROR_INVALID_PARAMETER;
            return result;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (same_name(managed[index].name, managed[previous].name)) {
                result.error = ERROR_INVALID_PARAMETER;
                return result;
            }
        }
    }
    for (size_t index = 0; index < desired.size(); ++index) {
        const FirewallRuleSpec& rule = desired[index];
        const auto identity = std::find_if(managed.begin(), managed.end(),
                                           [&rule](const FirewallRuleIdentity& item) {
                                               return same_name(item.name, rule.name);
                                           });
        const bool valid_protocol = rule.protocol == L"UDP" || rule.protocol == L"TCP";
        if (identity == managed.end() || rule.application.empty() || rule.local_port == 0 ||
            !valid_protocol || !permitted_application(*identity, rule.application)) {
            result.error = ERROR_INVALID_PARAMETER;
            return result;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (same_name(rule.name, desired[previous].name)) {
                result.error = ERROR_INVALID_PARAMETER;
                return result;
            }
        }
    }

    std::vector<FirewallRuleSnapshot> snapshots(managed.size());
    for (size_t index = 0; index < managed.size(); ++index) {
        result.error = backend_.inspect(managed[index], snapshots[index]);
        if (result.error != 0) return result;
    }

    managed_ = managed;
    snapshots_ = std::move(snapshots);
    active_ = true;

    for (const FirewallRuleIdentity& identity : managed) {
        result.error = backend_.remove(identity);
        if (result.error != 0) {
            result.rollback_error = rollback();
            return result;
        }
    }
    for (const FirewallRuleSpec& rule : desired) {
        result.error = backend_.add(rule);
        if (result.error != 0) {
            result.rollback_error = rollback();
            return result;
        }
    }
    return result;
}

int FirewallRuleTransaction::rollback() {
    if (!active_) return 0;
    int first_error = 0;
    for (size_t index = managed_.size(); index > 0; --index) {
        const int error = backend_.restore(managed_[index - 1], snapshots_[index - 1]);
        if (first_error == 0 && error != 0) first_error = error;
    }
    snapshots_.clear();
    managed_.clear();
    active_ = false;
    return first_error;
}

void FirewallRuleTransaction::commit() {
    snapshots_.clear();
    managed_.clear();
    active_ = false;
}

FirewallTransactionResult replace_firewall_rules(
    FirewallRuleBackend& backend,
    const std::vector<FirewallRuleIdentity>& managed,
    const std::vector<FirewallRuleSpec>& desired) {
    FirewallRuleTransaction transaction(backend);
    FirewallTransactionResult result = transaction.apply(managed, desired);
    if (result.error == 0) transaction.commit();
    return result;
}

}  // namespace chast::host::detail
