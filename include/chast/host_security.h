#pragma once

// The host's authentication and authorization policy, independent of the
// transport and of the UI.
//
// QuicSession hands the policy a parsed Hello; the host thread either accepts
// the Result as-is or shows the operator an approval dialog and then commits
// it with note_connected(). Decisions are made in this order:
//
//   1. the IPv4 allow list, before any secret is examined;
//   2. per-address lockout after repeated failures;
//   3. an explicitly enabled access token, for CLI and automation;
//   4. the auth mode: a supplied password is checked, an empty one asks the
//      operator in Both mode, while Click always asks;
//   5. TOTP, unless a valid trusted-device signature proves this device
//      already passed it.
//
// A refusal never changes security state beyond the failure counters.
// note_connected() is the commit point: it rotates a spent temporary
// password, refreshes the trust list and clears this address's failures.
//
// The class is thread-safe: the tray reads the temporary password and the
// trust list while a transport thread is evaluating a Hello. The password hash
// and TOTP secret are passed in already hashed/protected by the caller; the
// policy only ever holds the decoded forms in memory.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "chast/host_settings.h"
#include "chast/protocol.h"
#include "chast/security.h"

namespace chast {

inline constexpr size_t kMinPermanentPasswordLength = 8;
inline constexpr int64_t kTrustedDeviceLifetimeSeconds = 90ll * 24 * 60 * 60;
// A signed Hello with a timestamp outside this window is stale. The viewer's
// clock is not assumed to be exact, so two minutes in both directions.
inline constexpr int64_t kHelloClockSkewSeconds = 120;
// Signed nonces are remembered this long, which must cover twice the clock
// window: a replay can only pass the timestamp check for that long.
inline constexpr int64_t kHelloNonceLifetimeSeconds = 300;
inline constexpr size_t kMaxRememberedNonces = 4096;

class HostSecurity {
public:
    enum class Decision {
        Allow,           // welcome; Result::permissions apply
        RequireApproval, // interactive mode: ask the operator
        Deny,            // send Result::denial and AccessDenial::reason
    };

    struct Config {
        HostAuthMode mode = HostAuthMode::Both;
        // PBKDF2-encoded permanent password; empty means none.
        std::string password_hash;
        // Base32 TOTP secret; empty turns 2FA off. Invalid base32 is a
        // configuration error that refuses password logins rather than
        // silently dropping the second factor.
        std::string totp_secret;
        // What the viewer may use before the operator narrows it further.
        uint16_t default_permissions = kPermissionAll;
        bool allow_permission_change = true;
        // Comma-separated IPv4 CIDRs. Empty means no restriction. Any
        // unparsable entry fails closed: every address is refused.
        std::string ip_whitelist;
        std::vector<TrustedDevice> trusted_devices;
        // Service mode has no interactive operator: no approval, no temporary
        // password, and no connection at all without a permanent password.
        bool service_mode = false;
        // Legacy access token, only for CLI/automation.
        bool token_auth = false;
        std::string access_token;
        // Canonical SHA-256 of the certificate this host presents. Signed
        // Hellos bind to it, so a proof cannot be replayed against another
        // host.
        std::string host_fingerprint;
    };

    struct Result {
        Decision decision = Decision::Deny;
        // Valid when decision is Deny; RequireApproval leaves it at Declined.
        AccessDenial denial = AccessDenial::Declined;
        std::string reason;
        // Config defaults intersected with the Hello request. The operator may
        // narrow this further before allowing.
        uint16_t permissions = 0;
        bool used_temporary_password = false;
        bool via_token = false;
        bool via_trusted_device = false;
        // The Hello carried a valid device signature and asked to be trusted.
        // note_connected() adds it, so a denial must not commit the Result.
        bool request_trust = false;
        std::string peer_address;  // "ip:port" as reported by the transport
        std::string peer_ip;       // for allow-list and lockout accounting
        std::string device_name;
        uint8_t device_platform = 0;
        std::vector<uint8_t> device_key;
    };

    HostSecurity();
    explicit HostSecurity(Config config);

    // Replaces the policy. The temporary password, failure counters and nonce
    // cache survive, so an operator edit cannot be used to clear a lockout.
    void set_config(Config config);
    Config config() const;

    // Decides one parsed Hello. `peer_address` is the transport's "ip:port";
    // an unparsable value only disables allow-list matching and uses the whole
    // string as the lockout key.
    Result evaluate(const ClientHello& hello, const std::string& peer_address,
                    int64_t now_unix);

    // Commits an accepted Result. Must be called exactly once for a connection
    // that proceeds, including one the operator approved; never for a denial.
    void note_connected(const Result& result, int64_t now_unix);

    // ----- password -----

    // Hashes and stores a permanent password. Changing it clears the trust
    // list: every trusted device was trusted under the old secret. 2FA stays
    // as it was.
    bool set_password(const std::string& password, std::string* error);
    void clear_password();
    bool password_configured() const;
    std::string password_hash() const;

    // ----- 2FA -----

    bool set_totp_secret(const std::string& secret_base32, std::string* error);
    void clear_totp_secret();
    bool two_factor_enabled() const;
    std::string totp_secret() const;

    // ----- temporary password -----

    // Interactive password/both modes keep one; click and service modes do
    // not. Empty when the CSPRNG failed, in which case a permanent password is
    // the only way in.
    bool has_temporary_password() const;
    std::string temporary_password() const;
    // Generates a new one and clears the consecutive-failure counter. Returns
    // the new value (empty on CSPRNG failure).
    std::string refresh_temporary_password();

    // ----- trust list -----

    // Unexpired entries, most recently used first.
    std::vector<TrustedDevice> trusted_devices(int64_t now_unix) const;
    bool revoke_trusted_device(const std::vector<uint8_t>& public_key);
    void clear_trusted_devices();

    // ----- lockout -----

    // True while `peer_address` is inside a backoff window. retry_after_seconds
    // receives the remaining time when it is not null.
    bool locked_out(const std::string& peer_address, int64_t now_unix,
                    uint32_t* retry_after_seconds = nullptr) const;

private:
    struct AddressFailures {
        std::vector<int64_t> failures;  // ascending, pruned to ten minutes
        int64_t blocked_until = 0;
        uint32_t block_count = 0;  // drives the backoff ladder
    };

    static std::string address_key(const std::string& peer_address, std::string& ip);
    static bool password_hash_usable(const std::string& hash);

    void fail_locked(const std::string& key, int64_t now_unix);
    void prune_nonces_locked(int64_t now_unix);

    mutable std::mutex mutex_;
    Config config_;
    std::string temporary_password_;
    uint32_t temporary_failures_ = 0;
    std::vector<Ipv4Range> whitelist_;
    bool whitelist_configured_ = false;
    bool whitelist_valid_ = false;
    bool totp_valid_ = false;
    std::map<std::string, AddressFailures> failures_;
    std::vector<std::pair<std::array<uint8_t, kHelloNonceBytes>, int64_t>> nonces_;
};

// Parses one comma-separated IPv4 CIDR list. Empty or blank text is a valid
// empty list. Returns false on the first bad entry and leaves the entries read
// so far in `ranges`; callers that enforce a list must fail closed on false.
bool parse_ip_whitelist(const std::string& text, std::vector<Ipv4Range>& ranges,
                        std::string* error);

}  // namespace chast
