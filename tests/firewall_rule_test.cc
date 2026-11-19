#include <netfw.h>
#include <oleauto.h>

#include <cstdio>
#include <string>

#include "firewall_com_rule.h"
#include "platform.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}

void test_com_rule_configuration() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        check(false, "Firewall COM test apartment initializes");
        return;
    }

    const chast::host::detail::FirewallRuleSpec specs[] = {
        {L"chast-com-smoke-udp", L"C:\\chast-host.exe", L"UDP", 12345},
        {L"chast-com-smoke-tcp", L"C:\\chast-host.exe", L"TCP", 12346},
    };
    for (const auto& spec : specs) {
        INetFwRule* rule = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(INetFwRule), reinterpret_cast<void**>(&rule));
        check(SUCCEEDED(hr) && rule != nullptr, "an in-memory Firewall COM rule is created");
        if (FAILED(hr) || rule == nullptr) continue;

        hr = chast::host::detail::configure_firewall_com_rule(rule, spec);
        check(SUCCEEDED(hr), "COM accepts the production rule property sequence");
        if (SUCCEEDED(hr)) {
            long protocol = 0;
            BSTR ports = nullptr;
            const HRESULT protocol_result = rule->get_Protocol(&protocol);
            const HRESULT ports_result = rule->get_LocalPorts(&ports);
            const long expected_protocol = spec.protocol == L"UDP"
                                               ? NET_FW_IP_PROTOCOL_UDP
                                               : NET_FW_IP_PROTOCOL_TCP;
            check(SUCCEEDED(protocol_result) && protocol == expected_protocol,
                  "the configured protocol is retained");
            const std::wstring expected_port = std::to_wstring(spec.local_port);
            check(SUCCEEDED(ports_result) && ports != nullptr &&
                      std::wstring(ports, SysStringLen(ports)) == expected_port,
                  "the configured local port is retained");
            SysFreeString(ports);
        }
        rule->Release();
    }

    if (uninitialize) CoUninitialize();
}

}  // namespace

int main() {
    test_com_rule_configuration();
    check(chast::host::firewall_rule_application_matches(
              L"C:\\Program Files\\chast\\chast-service.exe",
              L"c:\\program files\\CHAST\\CHAST-SERVICE.EXE"),
          "Windows path case differences still identify the same executable");
    check(chast::host::firewall_rule_application_matches(
              L"C:\\Program Files\\chast\\chast-host.exe",
              L"C:\\Program Files\\chast\\chast-service.exe",
              L"C:\\Program Files\\chast\\chast-host.exe"),
          "service and tray binaries are both recognized as product host rules");
    check(!chast::host::firewall_rule_application_matches(
              L"C:\\Program Files\\other\\service.exe",
              L"C:\\Program Files\\chast\\chast-service.exe"),
          "a fixed-name rule owned by another executable is not removable");
    check(!chast::host::firewall_rule_application_matches(L"", L"C:\\chast.exe"),
          "an unrestricted rule does not count as product-owned");
    return failures == 0 ? 0 : 1;
}
