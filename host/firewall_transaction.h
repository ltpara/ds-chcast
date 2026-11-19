#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace chast::host::detail {

struct FirewallRuleIdentity {
    std::wstring name;
    std::vector<std::wstring> permitted_applications;
};

struct FirewallRuleSpec {
    std::wstring name;
    std::wstring application;
    std::wstring protocol;
    uint16_t local_port = 0;
};

struct FirewallRuleSnapshot {
    bool exists = false;
    std::shared_ptr<void> native;
};

class FirewallRuleBackend {
public:
    virtual ~FirewallRuleBackend() = default;
    virtual int inspect(const FirewallRuleIdentity& identity,
                        FirewallRuleSnapshot& snapshot) = 0;
    virtual int remove(const FirewallRuleIdentity& identity) = 0;
    virtual int add(const FirewallRuleSpec& rule) = 0;
    virtual int restore(const FirewallRuleIdentity& identity,
                        const FirewallRuleSnapshot& snapshot) = 0;
};

struct FirewallTransactionResult {
    int error = 0;
    int rollback_error = 0;
};

class FirewallRuleTransaction {
public:
    explicit FirewallRuleTransaction(FirewallRuleBackend& backend) : backend_(backend) {}
    FirewallRuleTransaction(const FirewallRuleTransaction&) = delete;
    FirewallRuleTransaction& operator=(const FirewallRuleTransaction&) = delete;
    ~FirewallRuleTransaction();

    FirewallTransactionResult apply(
        const std::vector<FirewallRuleIdentity>& managed,
        const std::vector<FirewallRuleSpec>& desired);
    int rollback();
    void commit();

private:
    FirewallRuleBackend& backend_;
    std::vector<FirewallRuleIdentity> managed_;
    std::vector<FirewallRuleSnapshot> snapshots_;
    bool active_ = false;
};

FirewallTransactionResult replace_firewall_rules(
    FirewallRuleBackend& backend,
    const std::vector<FirewallRuleIdentity>& managed,
    const std::vector<FirewallRuleSpec>& desired);

}  // namespace chast::host::detail
