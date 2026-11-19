#pragma once

#include <netfw.h>

#include "firewall_transaction.h"

namespace chast::host::detail {

HRESULT configure_firewall_com_rule(INetFwRule* native_rule,
                                    const FirewallRuleSpec& rule);

}  // namespace chast::host::detail
