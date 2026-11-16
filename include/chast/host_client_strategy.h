#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "chast/host_settings.h"

namespace chast {

struct HostStrategyIdentity {
    std::string server_url;
    std::string server_public_key;
    std::string peer_id;
    std::string host_public_key;
    std::string scope;
};

enum class HostStrategyTarget { Host, Viewer };

struct HostStrategySetting {
    HostPreference preference = HostPreference::Count; // Unknown to this build.
    bool locked = false;
    uint32_t value = 0; // Validated boolean or integer for supported keys.
    std::string canonical_value; // Retained for unknown scalar keys and digest verification.
};

struct HostClientStrategy {
    std::string scope;
    int64_t revision = 0;
    std::string digest;
    std::string policy_id;
    std::map<std::string, HostStrategySetting> settings;
};

// Derives the same public device key and authority scope as chast-tunnel. The
// secret never appears in the cache or diagnostics. The server pin must match
// the pin actually passed to the agent (currently empty for the tray launcher).
bool host_strategy_identity(const HostSettings& local, const std::string& server_pin,
                            HostStrategyIdentity& identity, std::string* error);

// Strict bounded cache decoding. Failure leaves output/previous untouched;
// callers retain the last accepted overlay on missing/corrupt files or outages.
bool parse_host_client_strategy(const std::string& text, const HostStrategyIdentity& identity,
                                const HostClientStrategy* previous, HostClientStrategy& output,
                                std::string* error);
bool read_host_client_strategy(const std::string& path, const HostStrategyIdentity& identity,
                               const HostClientStrategy* previous, HostClientStrategy& output,
                               std::string* error);

// Decodes an already authenticated snapshot body. Authority authentication and
// pinning happen before this function; it still validates every bound field.
bool parse_host_strategy_snapshot(const std::string& text, HostStrategyTarget target,
                                  const HostStrategyIdentity& identity,
                                  const HostClientStrategy* previous, HostClientStrategy& output,
                                  std::string* error);

// Pure full-overlay evaluation. Always begin with LOCAL settings, never the
// previous effective copy. An empty overlay restores local values. The return
// value is runtime-only and must not be passed to save_host_settings(). Unknown
// keys remain in the snapshot so application receipts can report unsupported.
HostSettings host_strategy_effective_settings(const HostSettings& local,
                                              const HostClientStrategy& strategy);

// True when the snapshot contains a well-formed key this build cannot apply.
// Callers may keep the listener alive for diagnostics, but must refuse new
// viewer admissions and report the key as unsupported.
bool host_strategy_has_unsupported_settings(const HostClientStrategy& strategy);

// Missing cache keeps a local-only host usable. A present but unreadable cache
// disables this build in strict mode; the caller sets TUNNEL_POLICY_STRICT=1.
bool host_strategy_start_allowed(const HostSettings& local, const std::string& cache_path,
                                 const std::string& server_pin, std::string* error);

// The strategy cache and its native application receipt live beside the
// tunnel's managed access-policy marker. They are deliberately separate from
// access-policy state so one receipt can never be mistaken for the other.
std::string host_client_strategy_path(const HostSettings& local);
std::string host_client_strategy_ack_path(const HostSettings& local);

// Writes the receipt consumed by chast-tunnel after the native consumer has
// reached the supplied startup state. A restart-only key is applied only once
// its owning subsystem is ready; session-only keys remain pending_session.
// Sequence is persisted and increases when the outcome for one revision
// changes, so delayed results cannot replace a newer application state.
bool write_host_client_strategy_ack(const HostSettings& local,
                                    const HostStrategyIdentity& identity,
                                    const HostClientStrategy& strategy,
                                    bool server_ready, bool tunnel_ready,
                                    std::string* error, bool server_failed = false,
                                    bool tunnel_failed = false);

} // namespace chast
