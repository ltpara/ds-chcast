#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "chast/host_client_strategy.h"

namespace {
int failures = 0;
void check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", label);
    if (!condition) ++failures;
}

std::string replace(std::string text, const std::string& from, const std::string& to) {
    const auto pos = text.find(from);
    check(pos != std::string::npos, "mutation target exists");
    if (pos != std::string::npos) text.replace(pos, from.size(), to);
    return text;
}

std::string envelope(const chast::HostStrategyIdentity& identity, const std::string& settings,
                     int64_t revision = 8) {
    const std::string hash_input = std::string("host\0", 5) + settings;
    std::array<uint8_t, 32> hash{};
    check(chast::sha256(reinterpret_cast<const uint8_t*>(hash_input.data()), hash_input.size(), hash),
          "test snapshot digest calculated");
    return "{\"version\":1,\"scope\":\"" + identity.scope +
        "\",\"server_url\":\"" + identity.server_url + "\",\"server_public_key\":\"" +
        identity.server_public_key + "\",\"host_public_key\":\"" + identity.host_public_key +
        "\",\"snapshot\":{\"peer_id\":\"" + identity.peer_id + "\",\"target\":\"host\",\"revision\":" +
        std::to_string(revision) + ",\"settings\":" + settings + ",\"digest\":\"" +
        chast::hex_encode(hash.data(), hash.size()) + "\",\"updated_at_ms\":0}}";
}
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    const std::string golden((std::istreambuf_iterator<char>(file)), {});
    check(!golden.empty(), "Go-generated cache fixture exists");
    chast::HostSettings local;
    local.server_url = "https://strategy.example.test///";
    local.tunnel_id = "native-host";
    local.tunnel_key = "native-test-secret";
    chast::HostStrategyIdentity identity;
    std::string error;
    check(chast::host_strategy_identity(local, "", identity, &error), "native identity derives successfully");
    chast::HostClientStrategy strategy;
    check(chast::parse_host_client_strategy(golden, identity, nullptr, strategy, &error),
          "native decoder accepts actual Go scope, public key and digest");
    check(strategy.revision == 7 && strategy.settings.size() == 6 &&
          strategy.policy_id == "policy-测试<>&😀", "all settings and escaped Unicode metadata survive");
    chast::HostClientStrategy from_file;
    check(chast::read_host_client_strategy(argv[1], identity, &strategy, from_file, &error),
          "bounded file reader accepts an idempotent snapshot");

    const auto applied = chast::host_strategy_effective_settings(local, strategy);
    check(applied.lock_after_session_end && !applied.keep_awake && applied.auto_disconnect_minutes == 15 &&
          applied.max_viewers == 4 && !applied.carrier && !applied.lan_discovery,
          "all six settings apply to the runtime copy");
    check(local.keep_awake && local.max_viewers == 2 && local.carrier && local.lan_discovery &&
          local.explicit_preferences == 0, "overlay evaluation leaves local preferences intact");
    std::string serialized = "unchanged";
    check(!chast::serialize_host_settings("host.ini", applied, serialized, &error) && serialized == "unchanged",
          "runtime overlays cannot overwrite the local settings file");
    chast::mark_host_preference(local, chast::HostPreference::KeepAwake);
    local.max_viewers = 3;
    chast::mark_host_preference(local, chast::HostPreference::Carrier);
    const auto respected = chast::host_strategy_effective_settings(local, strategy);
    check(respected.keep_awake && respected.max_viewers == 3 && !respected.carrier,
          "managed defaults respect explicit local values while locked values override them");

    chast::HostClientStrategy removed;
    check(chast::parse_host_client_strategy(envelope(identity, "{}"), identity, &strategy, removed, &error),
          "a higher-version empty snapshot is an explicit removal");
    const auto restored = chast::host_strategy_effective_settings(local, removed);
    check(restored.keep_awake && restored.max_viewers == 3 && restored.carrier && restored.lan_discovery &&
          !restored.lock_after_session_end && restored.auto_disconnect_minutes == 0,
          "removal restores the complete local settings without copying old overlay values");

    const auto rejects = [&](const std::string& text, const char* label,
                             const chast::HostClientStrategy* previous = nullptr) {
        auto sentinel = strategy;
        check(!chast::parse_host_client_strategy(text, identity, previous, sentinel, &error) &&
              !error.empty() && sentinel.digest == strategy.digest && sentinel.revision == strategy.revision,
              label);
    };
    rejects(golden, "a stale snapshot cannot roll back a newer removal", &removed);
    rejects(envelope(identity, "{}", 7), "same revision with different content is refused", &strategy);
    rejects(replace(golden, "policy-", "changed-"), "same revision cannot change policy source", &strategy);
    rejects(replace(golden, "\"version\": 1", "\"version\": 2"), "unknown cache versions are refused");
    rejects(replace(golden, "\"version\": 1", "\"version\": 1, \"version\": 1"), "duplicate envelope fields are refused");
    rejects(replace(golden, "\"mode\": \"locked\"", "\"mode\": \"locked\", \"mo\\u0064e\": \"default\""),
            "escaped duplicate setting fields are refused");
    rejects(replace(golden, "\"target\": \"host\"", "\"target\": \"viewer\""), "viewer-target cache is refused");
    rejects(replace(golden, "\"peer_id\": \"native-host\"", "\"peer_id\": \"another-host\""), "different device id is refused");
    rejects(replace(golden, "\"server_public_key\": \"\"", "\"server_public_key\": \"different\""), "changed server pin is refused");
    rejects(replace(golden, "\"host_public_key\": \"", "\"host_public_key\": \"changed"), "changed device key is refused");
    rejects(replace(golden, "\"scope\": \"", "\"scope\": \"changed"), "wrong authority scope is refused");
    rejects(replace(golden, "\"server_url\": \"https://strategy.example.test\"", "\"server_url\": \"https://other.test\""),
            "wrong server URL is refused even if the old scope was copied");
    rejects(replace(golden, "\"value\": 15", "\"value\": 16"), "tampered settings do not match the digest");
    rejects(golden + "{}", "trailing JSON is refused");
    rejects(std::string(65537, ' '), "oversized files are refused");
    rejects(replace(golden, "\"revision\": 7", "\"revision\": 9007199254740992"), "inexact revisions are refused");
    rejects(replace(golden, "\"revision\": 7", "\"revision\": 07"), "leading-zero numbers are refused");
    rejects(replace(golden, "\"revision\": 7", "\"revision\": 7e0"), "exponent revisions are refused");
    rejects(replace(golden, "policy-", "\\ud800"), "unpaired Unicode surrogates are refused");
    rejects(replace(golden, "policy-", std::string(1, static_cast<char>(0xff))), "invalid UTF-8 is refused");
    rejects(envelope(identity, R"({"host.max_viewers":{"mode":"locked","value":0}})"), "invalid viewer limits are refused");
    rejects(envelope(identity, R"({"host.auto_disconnect_minutes":{"mode":"locked","value":10081}})"), "invalid idle limits are refused");
    rejects(envelope(identity, R"({"host.carrier":{"mode":"locked","value":1}})"), "numeric booleans are refused");
    rejects(envelope(identity, R"({"host.carrier":{"mode":"locked","value":null}})"), "null does not silently remove a setting");
    rejects(envelope(identity, R"({"host.carrier":{"mode":"optional","value":false}})"), "unknown modes are refused");
    rejects(envelope(identity, R"({"viewer.view_fit":{"mode":"locked","value":1}})"), "foreign target keys are refused");

    const std::string future_settings = R"({"host.future_option":{"mode":"locked","value":"\u003c\u003e\u0026\u2028😀"}})";
    chast::HostClientStrategy future;
    check(chast::parse_host_client_strategy(envelope(identity, future_settings), identity, &strategy, future, &error) &&
          future.settings.at("host.future_option").preference == chast::HostPreference::Count,
          "future scalar settings retain unsupported identity and Go-compatible string digest");
    check(chast::host_strategy_has_unsupported_settings(future),
          "future keys mark the strategy as unsupported for admission");
    const auto future_applied = chast::host_strategy_effective_settings(local, future);
    check(future_applied.carrier == local.carrier && future_applied.keep_awake == local.keep_awake,
          "unknown keys never change known local settings");
    const std::string unsupported_cache =
        std::string(argv[1]) + ".unsupported-cache";
    {
        std::ofstream cache(unsupported_cache, std::ios::binary | std::ios::trunc);
        cache << envelope(identity, future_settings);
    }
    check(!chast::host_strategy_start_allowed(local, unsupported_cache, "", &error) &&
          error.find("unsupported") != std::string::npos,
          "a real cache containing a future key blocks new viewer admission");
    std::remove(unsupported_cache.c_str());

    auto other_local = local;
    other_local.tunnel_key = "rotated-native-secret";
    chast::HostStrategyIdentity other_identity;
    check(chast::host_strategy_identity(other_local, "", other_identity, &error) &&
          other_identity.scope != identity.scope &&
          !chast::parse_host_client_strategy(golden, other_identity, nullptr, future, &error),
          "key rotation cannot inherit the previous identity cache");
    check(!chast::read_host_client_strategy(std::string(argv[1]) + ".missing", identity, &strategy, from_file, &error) &&
          from_file.digest == strategy.digest, "missing cache leaves the last accepted state untouched");
    const bool local_only_allowed =
        chast::host_strategy_start_allowed(local, std::string(argv[1]) + ".missing", "", &error);
    check(local_only_allowed, "a local-only host without a managed cache may start");
    check(!chast::host_strategy_start_allowed(local, std::string("bad\0strategy", 12), "", &error) &&
          !error.empty(), "a cache path inspection error cannot masquerade as local-only startup");

    auto receipt_local = local;
    receipt_local.policy_state = std::string(argv[1]) + ".receipt-test";
    const std::string receipt_path = chast::host_client_strategy_ack_path(receipt_local);
    std::remove(receipt_path.c_str());
    check(chast::write_host_client_strategy_ack(receipt_local, identity, strategy, true, false, &error),
          "native host writes a receipt after the listener is ready");
    std::ifstream receipt_file(receipt_path, std::ios::binary);
    std::string receipt((std::istreambuf_iterator<char>(receipt_file)), {});
    check(receipt.find("\"sequence\":1") != std::string::npos &&
          receipt.find("\"status\":\"pending_restart\"") != std::string::npos &&
          receipt.find("\"host.lan_discovery\":{\"status\":\"pending_restart\"}") != std::string::npos,
          "restart-only settings are not reported applied before the tunnel starts");
    receipt_file.close();
    check(chast::write_host_client_strategy_ack(receipt_local, identity, strategy, true, true, &error),
          "native host updates the receipt after the tunnel starts");
    receipt_file.open(receipt_path, std::ios::binary);
    receipt.assign((std::istreambuf_iterator<char>(receipt_file)), {});
    check(receipt.find("\"sequence\":2") != std::string::npos &&
          receipt.find("\"host.lan_discovery\":{\"status\":\"applied\"}") != std::string::npos &&
          receipt.find("\"host.keep_awake\":{\"status\":\"applied\"}") != std::string::npos &&
          receipt.find("\"host.lock_after_session_end\":{\"status\":\"pending_session\"}") !=
              std::string::npos,
          "the receipt covers every key and advances sequence when application state changes");
    receipt_file.close();
    auto local_only_receipt = receipt_local;
    local_only_receipt.tunnel = false;
    check(chast::write_host_client_strategy_ack(
              local_only_receipt, identity, strategy, true, false, &error),
          "a locally disabled tunnel still produces a receipt");
    receipt_file.open(receipt_path, std::ios::binary);
    receipt.assign((std::istreambuf_iterator<char>(receipt_file)), {});
    check(receipt.find("\"host.lan_discovery\":{\"status\":\"applied\"}") !=
              std::string::npos,
          "LAN discovery is not left pending when the tunnel is explicitly disabled");
    receipt_file.close();
    check(chast::write_host_client_strategy_ack(receipt_local, identity, strategy, true, true, &error),
          "an unchanged native receipt remains idempotent");
    receipt_file.open(receipt_path, std::ios::binary);
    receipt.assign((std::istreambuf_iterator<char>(receipt_file)), {});
    check(receipt.find("\"sequence\":2") != std::string::npos,
          "unchanged retries do not burn receipt sequence numbers");
    receipt_file.close();
    check(chast::write_host_client_strategy_ack(
              receipt_local, identity, strategy, true, true, &error, true, true),
          "a definitive startup failure produces a native receipt");
    receipt_file.open(receipt_path, std::ios::binary);
    receipt.assign((std::istreambuf_iterator<char>(receipt_file)), {});
    check(receipt.find("\"sequence\":3") != std::string::npos &&
          receipt.find("\"status\":\"failed\"") != std::string::npos &&
          receipt.find("\"host.keep_awake\":{\"status\":\"failed\"}") != std::string::npos &&
          receipt.find("\"host.lan_discovery\":{\"status\":\"failed\"}") != std::string::npos,
          "failed startup is more severe than pending and is visible per key");
    receipt_file.close();
    std::remove(receipt_path.c_str());

    auto unknown_local = local;
    unknown_local.policy_state = std::string(argv[1]) + ".receipt-unknown";
    const std::string unknown_path = chast::host_client_strategy_ack_path(unknown_local);
    std::remove(unknown_path.c_str());
    check(chast::write_host_client_strategy_ack(
              unknown_local, identity, future, true, true, &error),
          "an unsupported future key still receives an application result");
    std::ifstream unknown_file(unknown_path, std::ios::binary);
    const std::string unknown_receipt(
        (std::istreambuf_iterator<char>(unknown_file)), {});
    check(unknown_receipt.find("\"status\":\"unsupported\"") != std::string::npos &&
          unknown_receipt.find(
              "\"host.future_option\":{\"status\":\"unsupported\"}") !=
              std::string::npos,
          "unknown scalar keys are reported as unsupported");
    std::remove(unknown_path.c_str());

    auto revision_local = local;
    revision_local.policy_state = std::string(argv[1]) + ".receipt-revision";
    const std::string revision_path =
        chast::host_client_strategy_ack_path(revision_local);
    std::remove(revision_path.c_str());
    check(chast::write_host_client_strategy_ack(
              revision_local, identity, strategy, true, true, &error),
          "the first revision writes its first receipt");
    check(chast::write_host_client_strategy_ack(
              revision_local, identity, removed, true, true, &error),
          "an explicit empty strategy writes a receipt");
    std::ifstream revision_file(revision_path, std::ios::binary);
    std::string revision_receipt(
        (std::istreambuf_iterator<char>(revision_file)), {});
    check(revision_receipt.find("\"results\":{}") != std::string::npos &&
          revision_receipt.find("\"revision\":8") != std::string::npos &&
          revision_receipt.find("\"sequence\":1") != std::string::npos &&
          revision_receipt.find("\"status\":\"applied\"") != std::string::npos,
          "a new revision resets sequence and an empty overlay reports its removal");
    revision_file.close();
    {
      std::ofstream corrupt(revision_path, std::ios::binary | std::ios::trunc);
      corrupt << "{";
    }
    check(chast::write_host_client_strategy_ack(
              revision_local, identity, removed, true, true, &error),
          "a corrupt old acknowledgement can be rebuilt");
    revision_file.open(revision_path, std::ios::binary);
    revision_receipt.assign(
        (std::istreambuf_iterator<char>(revision_file)), {});
    check(revision_receipt.find("\"sequence\":1") != std::string::npos,
          "a corrupt acknowledgement does not fabricate a sequence base");
    std::remove(revision_path.c_str());

    std::ifstream viewer_file(argv[2], std::ios::binary);
    const std::string viewer_text((std::istreambuf_iterator<char>(viewer_file)), {});
    chast::HostClientStrategy viewer;
    check(chast::parse_host_strategy_snapshot(viewer_text, chast::HostStrategyTarget::Viewer,
              identity, nullptr, viewer, &error),
          "an authenticated viewer snapshot validates against its target digest");
    check(viewer.revision == 9 && viewer.settings.size() == 5 &&
          viewer.settings.at("viewer.view_fit").preference == chast::HostPreference::Count,
          "the host accepts future viewer-only scalars without applying host values");
    check(!chast::parse_host_strategy_snapshot(golden, chast::HostStrategyTarget::Viewer,
              identity, nullptr, viewer, &error),
          "a host-target snapshot cannot be used for a viewer session");
    check(!chast::parse_host_strategy_snapshot(viewer_text, chast::HostStrategyTarget::Host,
              identity, nullptr, viewer, &error),
          "a viewer-target snapshot cannot be used for host settings");
    std::printf("host client strategy checks: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
