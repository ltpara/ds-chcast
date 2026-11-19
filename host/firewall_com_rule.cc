#include "firewall_com_rule.h"

#include <oleauto.h>

#include <string>

namespace chast::host::detail {
namespace {

template <typename Setter>
HRESULT set_text(INetFwRule* rule, const std::wstring& value, Setter setter) {
    BSTR text = SysAllocString(value.c_str());
    if (text == nullptr) return E_OUTOFMEMORY;
    const HRESULT result = (rule->*setter)(text);
    SysFreeString(text);
    return result;
}

}  // namespace

HRESULT configure_firewall_com_rule(INetFwRule* native_rule,
                                    const FirewallRuleSpec& rule) {
    if (native_rule == nullptr) return E_POINTER;

    HRESULT hr = set_text(native_rule, rule.name, &INetFwRule::put_Name);
    if (SUCCEEDED(hr)) {
        hr = set_text(native_rule, rule.application, &INetFwRule::put_ApplicationName);
    }
    if (SUCCEEDED(hr)) {
        const long protocol = rule.protocol == L"UDP" ? NET_FW_IP_PROTOCOL_UDP
                                                       : NET_FW_IP_PROTOCOL_TCP;
        hr = native_rule->put_Protocol(protocol);
    }
    // Firewall COM validates LocalPorts against the protocol already assigned.
    if (SUCCEEDED(hr)) {
        hr = set_text(native_rule, std::to_wstring(rule.local_port),
                      &INetFwRule::put_LocalPorts);
    }
    if (SUCCEEDED(hr)) hr = native_rule->put_Direction(NET_FW_RULE_DIR_IN);
    if (SUCCEEDED(hr)) hr = native_rule->put_Action(NET_FW_ACTION_ALLOW);
    if (SUCCEEDED(hr)) hr = native_rule->put_Profiles(NET_FW_PROFILE2_ALL);
    if (SUCCEEDED(hr)) hr = native_rule->put_Enabled(VARIANT_TRUE);
    return hr;
}

}  // namespace chast::host::detail
