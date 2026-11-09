#pragma once

// Access-ticket v1 verification for managed hosts.
//
// The rendezvous server signs a short-lived ticket with a dedicated Ed25519
// issuer key; hosts pin that public key out of band. This module mirrors
// server/internal/acl (ticket.go and replay.go) byte for byte: strict envelope
// decoding, the signature over the raw envelope, then issuer, audience, host
// binding and the time window. A verifier also consumes the ticket id exactly
// once, so a replayed ticket cannot authorize a second session.
//
// The format is frozen in docs/access-control-design-2026-11-09.md section 6.
// Re-encoding the envelope before verification would accept a different byte
// string than the one signed; verification always uses the received bytes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace chast {

inline constexpr size_t kTicketClientKeySize = 32;
inline constexpr size_t kTicketSignatureSize = 64;
inline constexpr size_t kTicketMaxEnvelope = 1024;
inline constexpr size_t kTicketMaxSigned = kTicketMaxEnvelope + kTicketSignatureSize;
inline constexpr int64_t kTicketMaxLifetimeSeconds = 60;
inline constexpr int64_t kTicketClockSkewSeconds = 5;

enum class TicketError {
    None,
    // The envelope is truncated, too large, or has trailing bytes.
    Malformed,
    // Unknown version, algorithm, or reserved flags.
    Unsupported,
    // A field is empty, over-long, a time is negative, or the structure of
    // the time window is invalid.
    Field,
    // The ticket names a key id the host has not pinned.
    UnknownKey,
    // The Ed25519 signature over the raw envelope was not accepted.
    Signature,
    Issuer,
    Audience,
    Host,
    Fingerprint,
    NotYetValid,
    Expired,
    // Expiry minus issue time exceeds sixty seconds.
    Lifetime,
    // This verifier already admitted the ticket id.
    Replayed,
    // The replay cache is full of live entries; refusing is safer than
    // evicting a still-valid id.
    ReplayFull,
};

// Stable text for logs and test output.
const char* ticket_error_text(TicketError error);

struct AccessTicket {
    std::string issuer;
    std::string audience;
    std::string key_id;
    std::string host_id;
    std::string host_public_key_fingerprint;
    std::string subject;
    std::array<uint8_t, kTicketClientKeySize> client_public_key{};
    uint32_t permissions = 0;
    uint32_t policy_version = 0;
    int64_t issued_at = 0;
    int64_t not_before = 0;
    int64_t expires_at = 0;
    std::string id;
};

// Parses one canonical envelope without its signature. Does not judge the
// current time: that belongs to a verifier, which has the pinned key and the
// deployment expectations.
TicketError decode_access_ticket(const std::vector<uint8_t>& payload, AccessTicket* out);

// Remembers consumed ticket ids until they expire plus the allowed clock skew.
// It never evicts a live entry to make room: when the bounded cache is full,
// new admissions are refused instead of silently allowing a replay.
class TicketReplayGuard {
public:
    enum class Consume {
        Accepted,
        Replayed,
        Full,
    };

    explicit TicketReplayGuard(size_t capacity = 4096);

    // Records id and reports whether it was unseen. The entry lives until the
    // ticket expiry plus the skew, so a replay after expiry is rejected by
    // ticket validation rather than by eviction.
    Consume consume(const std::string& id, int64_t expires_at, int64_t now_unix);

    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, int64_t> entries_;
    size_t capacity_;
};

class AccessTicketVerifier {
public:
    // The issuer and audience are exact-match expectations; both must be set.
    explicit AccessTicketVerifier(std::string issuer, std::string audience);

    // Installs a trusted issuer key for a key id. The key is base64url without
    // padding, exactly as the server prints it. Rotating keys stay available
    // until the administrator removes the old pin.
    bool pin_key(const std::string& kid, const std::string& public_key_base64url,
                 std::string* error);

    // Verifies one envelope||signature ticket and consumes its id. Both host
    // expectations are required: a ticket is only valid for the device it was
    // issued to, not for any host that trusts the issuer.
    TicketError verify(const std::vector<uint8_t>& signed_ticket,
                       const std::string& expected_host_id,
                       const std::string& expected_host_fingerprint, int64_t now_unix,
                       AccessTicket* out);

    // Same validation without consuming the id. A caller that must still check
    // something only known after decoding, such as the possession proof, uses
    // this so a failed check does not burn a ticket the peer can retry.
    TicketError inspect(const std::vector<uint8_t>& signed_ticket,
                        const std::string& expected_host_id,
                        const std::string& expected_host_fingerprint, int64_t now_unix,
                        AccessTicket* out);

    // Consumes the id of a ticket that passed inspect() and every other gate.
    // Returns Replayed or ReplayFull when the id cannot be used.
    TicketError commit(const AccessTicket& ticket, int64_t now_unix);

    size_t replay_size() const;

private:
    std::string issuer_;
    std::string audience_;
    std::unordered_map<std::string, std::array<uint8_t, kTicketClientKeySize>> keys_;
    TicketReplayGuard replay_;
};

// Unpadded base64url (RFC 4648 section 5), the encoding the server uses for
// ticket envelopes, public keys and signed tickets.
std::string encode_ticket_base64url(const std::vector<uint8_t>& data);

// Generates a fresh Ed25519 keypair for one viewer installation. The seed is
// the raw 32-byte private key OpenSSL accepts; the public key is what the
// viewer names in lookup so the server can bind a ticket to it. Both outputs
// are kTicketClientKeySize bytes.
bool generate_access_ticket_key(std::vector<uint8_t>* seed,
                                std::vector<uint8_t>* public_key);

// Derives the public key of an existing seed. Fails when the seed is not
// exactly kTicketClientKeySize bytes or OpenSSL rejects it.
bool derive_access_ticket_public_key(const std::vector<uint8_t>& seed,
                                     std::vector<uint8_t>* public_key);

// Strict base64url (RFC 4648 section 5, no padding) decoding. Exposed for the
// settings loader so pin text is rejected consistently.
bool decode_ticket_base64url(const std::string& text, std::vector<uint8_t>* out);

inline constexpr size_t kTicketProofHostNonceSize = 32;
inline constexpr size_t kTicketProofSize = 64;

// Bytes the viewer signs with the Ed25519 key its ticket names. The proof binds
// the ticket to one connection: the host id and certificate digest, the host's
// per-connection challenge, the client's Hello nonce and a SHA-256 digest of
// the signed ticket. A captured proof cannot be replayed on another connection
// or with another ticket.
std::vector<uint8_t> access_ticket_proof_message(
    const std::string& host_id, const std::string& host_fingerprint,
    const std::array<uint8_t, kTicketProofHostNonceSize>& host_nonce,
    const std::vector<uint8_t>& client_nonce,
    const std::vector<uint8_t>& signed_ticket);

// Verifies a possession proof against the client key carried by an already
// decoded ticket. The caller must have checked the ticket signature first or
// use the returned key only for this comparison.
bool verify_access_ticket_proof(const AccessTicket& ticket,
                                const std::vector<uint8_t>& message,
                                const std::vector<uint8_t>& proof);

// Signs a possession-proof message with the viewer's Ed25519 seed (exactly
// kTicketClientKeySize bytes). Fails when the seed is malformed or OpenSSL
// cannot derive a key from it.
bool sign_access_ticket_proof(const std::vector<uint8_t>& key_seed,
                              const std::vector<uint8_t>& message,
                              std::vector<uint8_t>* proof);

}  // namespace chast
