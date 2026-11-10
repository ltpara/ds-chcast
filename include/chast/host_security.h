#pragma once

// The host's authentication and authorization policy, independent of the
// transport and of the UI.
//
// QuicSession hands the policy a parsed Hello; the host thread either accepts
// the Result as-is or shows the operator an approval dialog and then commits
// it with note_connected(). Decisions are made in this order:
//
//   1. the IP allow list, before any secret is examined;
//   2. per-address lockout after repeated failures; an IPv6 peer is accounted
//      by its /64, /56 and /48, so rotating the interface address inside its
//      own block does not reset the counter;
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
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "chast/access_ticket.h"
#include "chast/host_settings.h"
#include "chast/protocol.h"
#include "chast/security.h"

namespace chast {

inline constexpr size_t kMinPermanentPasswordLength = 8;
// The default lifetime of a remembered device, in seconds. A host may override
// it with trusted_device_days; 0 there means "never expires".
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
        PermissionMask default_permissions = kPermissionAll;
        bool allow_permission_change = true;
        // Comma-separated IPv4 and/or IPv6 CIDRs. Empty means no restriction.
        // Any unparsable entry fails closed: every address is refused.
        std::string ip_whitelist;
        std::vector<TrustedDevice> trusted_devices;
        // How long a remembered device stays usable, in seconds. 0 disables
        // expiry. The default mirrors kTrustedDeviceLifetimeSeconds.
        int64_t trusted_device_lifetime_seconds = kTrustedDeviceLifetimeSeconds;
        // Service mode never creates temporary passwords. Without a local
        // approval channel it requires permanent-password authentication.
        bool service_mode = false;
        // An authenticated local service manager can answer Click/Both.
        // Temporary passwords remain unavailable in service mode.
        bool service_approval = false;
        // Legacy access token, only for CLI/automation.
        bool token_auth = false;
        std::string access_token;
        // Canonical SHA-256 of the certificate this host presents. Signed
        // Hellos bind to it, so a proof cannot be replayed against another
        // host.
        std::string host_fingerprint;
        // Machine policy state written by chast-tunnel (<stem>.policy.json).
        // Empty keeps the host unmanaged. A managed statement refuses every
        // new session until a verified access ticket arrives; an unreadable
        // state file also refuses, so a managed host cannot fall back to
        // local-only authentication by corrupting its marker.
        std::string policy_state;
        // The device id this host answers to. Tickets carry audience
        // "device:<id>", so a ticket issued for another device is refused.
        std::string device_id;
        // Access-ticket issuer pins installed by the administrator. The
        // primary triple is required unless access_keys supplies pins on its
        // own; an incomplete primary or an invalid extra pin leaves a managed
        // host fail-closed. Extra pins exist so two issuer keys overlap while
        // the server rotates to a new kid.
        std::string access_issuer;
        std::string access_kid;
        std::string access_public_key;
        struct AccessKeyPin {
            std::string kid;
            std::string public_key;
        };
        std::vector<AccessKeyPin> access_keys;
    };

    struct Result {
        Decision decision = Decision::Deny;
        // Valid when decision is Deny; RequireApproval leaves it at Declined.
        AccessDenial denial = AccessDenial::Declined;
        std::string reason;
        // Config defaults intersected with the Hello request. The operator may
        // narrow this further before allowing.
        PermissionMask permissions = 0;
        bool used_temporary_password = false;
        bool via_token = false;
        bool via_trusted_device = false;
        // The Hello carried a valid device signature and asked to be trusted.
        // note_connected() adds it, so a denial must not commit the Result.
        bool request_trust = false;
        // Set when a managed session was admitted through a verified access
        // ticket. The ticket's id, subject and deadline belong in the audit
        // record and bound how long this session may stay connected.
        bool via_access_ticket = false;
        std::string ticket_subject;
        std::string ticket_id;
        // Issuer key that signed the admitted ticket. An online revocation
        // names this kid, so only the sessions admitted under that key end.
        std::string ticket_key_id;
        int64_t ticket_expires_at = 0;
        uint32_t ticket_policy_version = 0;
        // The key the admitted ticket names. A mid-session renewal must
        // present a ticket for this same key; another key belongs to another
        // viewer and cannot extend this session.
        std::array<uint8_t, kTicketClientKeySize> ticket_client_key{};
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
    // Updates only capability defaults; preserves credentials and replay state.
    void set_default_permissions(PermissionMask permissions);
    Config config() const;

    // What a managed Hello brought besides its credentials. The client nonce is
    // not repeated here: the proof is built from the Hello's own nonce, so a
    // caller cannot pair a proof with a different Hello.
    struct TicketRequest {
        // The v62 envelope plus signature, exactly as the viewer sent it.
        std::vector<uint8_t> signed_ticket;
        // The per-connection challenge the host issued before the Hello.
        std::array<uint8_t, kTicketProofHostNonceSize> host_nonce{};
        // Ed25519 signature by the ticket's client key over
        // access_ticket_proof_message().
        std::vector<uint8_t> proof;
    };

    // Decides one parsed Hello. `peer_address` is the transport's "ip:port";
    // an unparsable value only disables allow-list matching and uses the whole
    // string as the lockout key. `ticket` is required on a managed host: the
    // ticket must verify, carry the current policy version and prove possession
    // of the client key on this connection. Local credentials and approval
    // still apply after the ticket is accepted.
    Result evaluate(const ClientHello& hello, const std::string& peer_address,
                    int64_t now_unix, const TicketRequest* ticket = nullptr);

    // One mid-session renewal: a freshly issued ticket for the same device
    // plus a possession proof over this session's Hello nonce and the host
    // challenge. The caller passes what it admitted at connect time; a
    // renewal cannot switch subject or client key. `permissions` is the
    // ticket's ceiling intersected with the local defaults only; the caller
    // intersects it with the mask already in force so a renewal can narrow
    // but never widen what the operator approved.
    struct TicketRenewal {
        std::vector<uint8_t> signed_ticket;
        std::vector<uint8_t> proof;
    };

    struct RenewalResult {
        bool accepted = false;
        std::string reason;
        PermissionMask permissions = 0;
        int64_t expires_at = 0;
        std::string subject;
        std::string ticket_id;
        uint32_t policy_version = 0;
        std::string key_id;
    };

    RenewalResult renew_ticket(const TicketRenewal& renewal, const std::string& peer_address,
                               const std::array<uint8_t, kTicketProofHostNonceSize>& host_nonce,
                               const std::vector<uint8_t>& client_nonce,
                               const std::array<uint8_t, kTicketClientKeySize>& session_key,
                               const std::string& session_subject, int64_t now_unix);

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
    // Highest RFC 6238 step accepted so far. Every later login must use a
    // strictly newer step, so a captured code cannot be replayed inside its
    // +/- window.
    uint64_t last_totp_step() const;

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

    // Lockout accounting keys for one address: the address itself, or the
    // /64, /56 and /48 networks a peer may rotate inside.
    static std::vector<std::string> lockout_keys(const std::string& ip);
    static bool password_hash_usable(const std::string& hash);

    void fail_locked(const std::vector<std::string>& keys, int64_t now_unix);
    void prune_failures_locked(int64_t now_unix);
    void evict_failure_locked();
    void prune_nonces_locked(int64_t now_unix);

    mutable std::mutex mutex_;
    Config config_;
    std::string temporary_password_;
    uint32_t temporary_failures_ = 0;
    std::vector<IpRange> whitelist_;
    bool whitelist_configured_ = false;
    bool whitelist_valid_ = false;
    bool totp_valid_ = false;
    // Monotonic within the lifetime of this policy object, including across
    // set_config() calls; replacing the secret does not rewind it.
    uint64_t last_totp_step_ = 0;
    std::map<std::string, AddressFailures> failures_;
    std::vector<std::pair<std::array<uint8_t, kHelloNonceBytes>, int64_t>> nonces_;
    // Built from the administrator pins. Null until all three values decode;
    // a managed host without it refuses every session.
    std::unique_ptr<AccessTicketVerifier> tickets_;
};

// Parses one comma-separated CIDR list of either family. Empty or blank text
// is a valid empty list. Returns false on the first bad entry and leaves the
// entries read so far in `ranges`; callers that enforce a list must fail closed
// on false.
bool parse_ip_whitelist(const std::string& text, std::vector<IpRange>& ranges,
                        std::string* error);

// The bare address of the transport's "ip:port": brackets and the port are
// removed. A value that is already an address is returned unchanged.
std::string peer_ip_text(const std::string& peer_address);

}  // namespace chast
