#include "chast/access_ticket.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <utility>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace chast {
namespace {

constexpr size_t kHeaderSize = 4;
constexpr uint16_t kMaxIssuer = 128;
constexpr uint16_t kMaxAudience = 128;
constexpr uint16_t kMaxKeyID = 64;
constexpr uint16_t kMaxHostID = 64;
constexpr uint16_t kMaxFingerprint = 128;
constexpr uint16_t kMaxSubject = 128;
constexpr uint16_t kMaxTicketID = 64;
// Header, seven fields with a two-byte length and at least one content byte,
// the client key, two u32 and three i64 values.
constexpr size_t kMinPayload = kHeaderSize + 7 * 3 + kTicketClientKeySize + 2 * 4 + 3 * 8;
constexpr char kDomainPrefix[] = "chast-access-ticket-v1";
constexpr char kProofDomainPrefix[] = "chast-access-proof-v1";

uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t read_u64(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | p[i];
    }
    return value;
}

class Reader {
public:
    Reader(const std::vector<uint8_t>& data, size_t offset) : data_(data), offset_(offset) {}

    bool take(size_t count, const uint8_t** out) {
        if (offset_ + count > data_.size()) {
            return false;
        }
        *out = data_.data() + offset_;
        offset_ += count;
        return true;
    }

    size_t offset() const { return offset_; }

private:
    const std::vector<uint8_t>& data_;
    size_t offset_;
};

std::string trim_ascii(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t' ||
                           value[begin] == '\r' || value[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\r' || value[end - 1] == '\n')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool base64url_value(char c, uint32_t* out) {
    if (c >= 'A' && c <= 'Z') {
        *out = static_cast<uint32_t>(c - 'A');
        return true;
    }
    if (c >= 'a' && c <= 'z') {
        *out = static_cast<uint32_t>(c - 'a' + 26);
        return true;
    }
    if (c >= '0' && c <= '9') {
        *out = static_cast<uint32_t>(c - '0' + 52);
        return true;
    }
    if (c == '-') {
        *out = 62;
        return true;
    }
    if (c == '_') {
        *out = 63;
        return true;
    }
    return false;
}

bool verify_ed25519(const std::vector<uint8_t>& message, const uint8_t* signature,
                    const uint8_t* public_key) {
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, public_key,
                                                kTicketClientKeySize);
    if (key == nullptr) {
        return false;
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool accepted = false;
    if (ctx != nullptr && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestVerify(ctx, signature, kTicketSignatureSize, message.data(),
                         message.size()) == 1) {
        accepted = true;
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return accepted;
}

std::array<uint8_t, 32> sha256(const std::vector<uint8_t>& data) {
    std::array<uint8_t, 32> digest{};
    unsigned int length = 0;
    if (EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha256(), nullptr) !=
            1 ||
        length != digest.size()) {
        digest.fill(0);
    }
    return digest;
}

void append_u16(std::vector<uint8_t>& out, size_t value) {
    const uint16_t clamped = value > 0xffff ? 0xffff : static_cast<uint16_t>(value);
    out.push_back(static_cast<uint8_t>(clamped >> 8));
    out.push_back(static_cast<uint8_t>(clamped & 0xff));
}

TicketError read_field(Reader& reader, uint16_t max, std::string* out) {
    const uint8_t* raw = nullptr;
    if (!reader.take(2, &raw)) {
        return TicketError::Malformed;
    }
    const uint16_t length = read_u16(raw);
    if (length == 0 || length > max) {
        return TicketError::Field;
    }
    if (!reader.take(length, &raw)) {
        return TicketError::Malformed;
    }
    out->assign(reinterpret_cast<const char*>(raw), length);
    return TicketError::None;
}

TicketError read_u32_field(Reader& reader, uint32_t* out) {
    const uint8_t* raw = nullptr;
    if (!reader.take(4, &raw)) {
        return TicketError::Malformed;
    }
    *out = read_u32(raw);
    return TicketError::None;
}

TicketError read_time(Reader& reader, int64_t* out) {
    const uint8_t* raw = nullptr;
    if (!reader.take(8, &raw)) {
        return TicketError::Malformed;
    }
    const uint64_t value = read_u64(raw);
    if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return TicketError::Field;
    }
    *out = static_cast<int64_t>(value);
    return TicketError::None;
}

TicketError validate_fields(const AccessTicket& ticket) {
    const std::pair<const std::string*, uint16_t> strings[] = {
        {&ticket.issuer, kMaxIssuer},
        {&ticket.audience, kMaxAudience},
        {&ticket.key_id, kMaxKeyID},
        {&ticket.host_id, kMaxHostID},
        {&ticket.host_public_key_fingerprint, kMaxFingerprint},
        {&ticket.subject, kMaxSubject},
        {&ticket.id, kMaxTicketID},
    };
    for (const auto& [value, max] : strings) {
        if (value->empty() || value->size() > max) {
            return TicketError::Field;
        }
    }
    bool zero_key = true;
    for (uint8_t byte : ticket.client_public_key) {
        if (byte != 0) {
            zero_key = false;
            break;
        }
    }
    if (zero_key) {
        return TicketError::Field;
    }
    if (ticket.issued_at <= 0 || ticket.not_before <= 0 || ticket.expires_at <= 0) {
        return TicketError::Field;
    }
    if (ticket.not_before < ticket.issued_at) {
        return TicketError::Field;
    }
    if (ticket.expires_at <= ticket.not_before) {
        return TicketError::Field;
    }
    // Both times are positive and expiry follows issue time, so the unsigned
    // difference is exact and cannot overflow.
    if (static_cast<uint64_t>(ticket.expires_at) - static_cast<uint64_t>(ticket.issued_at) >
        static_cast<uint64_t>(kTicketMaxLifetimeSeconds)) {
        return TicketError::Lifetime;
    }
    return TicketError::None;
}

}  // namespace

const char* ticket_error_text(TicketError error) {
    switch (error) {
        case TicketError::None:
            return "ok";
        case TicketError::Malformed:
            return "malformed envelope";
        case TicketError::Unsupported:
            return "unsupported ticket version or algorithm";
        case TicketError::Field:
            return "invalid ticket field";
        case TicketError::UnknownKey:
            return "ticket key id is not pinned";
        case TicketError::Signature:
            return "ticket signature was not accepted";
        case TicketError::Issuer:
            return "ticket issuer does not match";
        case TicketError::Audience:
            return "ticket audience does not match";
        case TicketError::Host:
            return "ticket is bound to another host";
        case TicketError::Fingerprint:
            return "ticket is bound to another host certificate";
        case TicketError::NotYetValid:
            return "ticket is not yet valid";
        case TicketError::Expired:
            return "ticket has expired";
        case TicketError::Lifetime:
            return "ticket lifetime exceeds the limit";
        case TicketError::Replayed:
            return "ticket id was already used";
        case TicketError::ReplayFull:
            return "ticket replay cache is full";
    }
    return "unknown ticket error";
}

TicketError decode_access_ticket(const std::vector<uint8_t>& payload, AccessTicket* out) {
    if (out == nullptr) {
        return TicketError::Malformed;
    }
    *out = AccessTicket{};
    if (payload.size() < kMinPayload || payload.size() > kTicketMaxEnvelope) {
        return TicketError::Malformed;
    }
    if (payload[0] != 1 || payload[1] != 1) {
        return TicketError::Unsupported;
    }
    if (payload[2] != 0 || payload[3] != 0) {
        return TicketError::Unsupported;
    }
    Reader reader(payload, kHeaderSize);
    AccessTicket ticket;
    TicketError error = TicketError::None;
    if ((error = read_field(reader, kMaxIssuer, &ticket.issuer)) != TicketError::None) {
        return error;
    }
    if ((error = read_field(reader, kMaxAudience, &ticket.audience)) != TicketError::None) {
        return error;
    }
    if ((error = read_field(reader, kMaxKeyID, &ticket.key_id)) != TicketError::None) {
        return error;
    }
    if ((error = read_field(reader, kMaxHostID, &ticket.host_id)) != TicketError::None) {
        return error;
    }
    if ((error = read_field(reader, kMaxFingerprint, &ticket.host_public_key_fingerprint)) !=
        TicketError::None) {
        return error;
    }
    if ((error = read_field(reader, kMaxSubject, &ticket.subject)) != TicketError::None) {
        return error;
    }
    const uint8_t* client_key = nullptr;
    if (!reader.take(kTicketClientKeySize, &client_key)) {
        return TicketError::Malformed;
    }
    std::memcpy(ticket.client_public_key.data(), client_key, kTicketClientKeySize);
    if ((error = read_u32_field(reader, &ticket.permissions)) != TicketError::None) {
        return error;
    }
    if ((error = read_u32_field(reader, &ticket.policy_version)) != TicketError::None) {
        return error;
    }
    if ((error = read_time(reader, &ticket.issued_at)) != TicketError::None) {
        return error;
    }
    if ((error = read_time(reader, &ticket.not_before)) != TicketError::None) {
        return error;
    }
    if ((error = read_time(reader, &ticket.expires_at)) != TicketError::None) {
        return error;
    }
    if ((error = read_field(reader, kMaxTicketID, &ticket.id)) != TicketError::None) {
        return error;
    }
    if (reader.offset() != payload.size()) {
        return TicketError::Malformed;
    }
    if ((error = validate_fields(ticket)) != TicketError::None) {
        return error;
    }
    *out = std::move(ticket);
    return TicketError::None;
}

TicketReplayGuard::TicketReplayGuard(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

TicketReplayGuard::Consume TicketReplayGuard::consume(const std::string& id, int64_t expires_at,
                                                      int64_t now_unix) {
    std::lock_guard<std::mutex> lock(mutex_);
    // A ticket is still accepted at expires_at + skew (the window check only
    // rejects strictly later), so the entry must survive that exact second.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second < now_unix) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    if (entries_.find(id) != entries_.end()) {
        return Consume::Replayed;
    }
    if (entries_.size() >= capacity_) {
        return Consume::Full;
    }
    int64_t expiry = expires_at + kTicketClockSkewSeconds;
    if (expiry <= now_unix) {
        expiry = now_unix + 1;
    }
    entries_.emplace(id, expiry);
    return Consume::Accepted;
}

size_t TicketReplayGuard::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

bool decode_ticket_base64url(const std::string& text, std::vector<uint8_t>* out) {
    if (out == nullptr) {
        return false;
    }
    out->clear();
    uint32_t accumulator = 0;
    int bits = 0;
    for (char c : text) {
        uint32_t value = 0;
        if (!base64url_value(c, &value)) {
            return false;
        }
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
        }
    }
    // A single dangling sextet cannot be produced by the URL-safe encoding,
    // and leftover bits must be zero or the text is not canonical.
    if (bits >= 6) {
        return false;
    }
    if (bits > 0 && (accumulator & ((1u << bits) - 1)) != 0) {
        return false;
    }
    return true;
}

AccessTicketVerifier::AccessTicketVerifier(std::string issuer, std::string audience)
    : issuer_(trim_ascii(issuer)), audience_(trim_ascii(audience)), replay_(4096) {}

bool AccessTicketVerifier::pin_key(const std::string& kid, const std::string& public_key_base64url,
                                   std::string* error) {
    const std::string trimmed_kid = trim_ascii(kid);
    if (trimmed_kid.empty() || trimmed_kid.size() > kMaxKeyID) {
        if (error != nullptr) {
            *error = "key id must be 1-64 bytes";
        }
        return false;
    }
    std::vector<uint8_t> raw;
    if (!decode_ticket_base64url(trim_ascii(public_key_base64url), &raw) ||
        raw.size() != kTicketClientKeySize) {
        if (error != nullptr) {
            *error = "public key must be 32 base64url bytes";
        }
        return false;
    }
    std::array<uint8_t, kTicketClientKeySize> key{};
    std::memcpy(key.data(), raw.data(), kTicketClientKeySize);
    keys_[trimmed_kid] = key;
    return true;
}

// Certificate digests are written with or without the "SHA256:" prefix, with
// or without ':' separators, and in either hex case depending on who produced
// them. The digest is the identity, so the comparison ignores the spelling;
// values that are not 64-digit digests fall back to an exact match.
std::string fingerprint_digits(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    std::string_view body = text.substr(begin, end - begin);
    if (body.size() > 7 && (body.substr(0, 7) == "SHA256:" || body.substr(0, 7) == "sha256:")) {
        body.remove_prefix(7);
    }
    std::string digits;
    digits.reserve(64);
    for (char c : body) {
        if (c == ':') {
            continue;
        }
        if (c >= 'a' && c <= 'f') {
            c = static_cast<char>(c - 'a' + 'A');
        } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return {};
        }
        digits.push_back(c);
    }
    return digits.size() == 64 ? digits : std::string();
}

bool fingerprint_equal(const std::string& ticket_fingerprint, const std::string& expected) {
    const std::string left = fingerprint_digits(ticket_fingerprint);
    const std::string right = fingerprint_digits(expected);
    if (!left.empty() && !right.empty()) {
        return left == right;
    }
    return ticket_fingerprint == expected;
}

TicketError AccessTicketVerifier::inspect(const std::vector<uint8_t>& signed_ticket,
                                          const std::string& expected_host_id,
                                          const std::string& expected_host_fingerprint,
                                          int64_t now_unix, AccessTicket* out) {
    if (out == nullptr) {
        return TicketError::Malformed;
    }
    *out = AccessTicket{};
    if (issuer_.empty() || audience_.empty() || expected_host_id.empty() ||
        expected_host_fingerprint.empty()) {
        return TicketError::Field;
    }
    if (signed_ticket.size() < kMinPayload + kTicketSignatureSize ||
        signed_ticket.size() > kTicketMaxSigned) {
        return TicketError::Malformed;
    }
    const size_t payload_size = signed_ticket.size() - kTicketSignatureSize;
    std::vector<uint8_t> payload(signed_ticket.begin(), signed_ticket.begin() + payload_size);
    AccessTicket ticket;
    TicketError error = decode_access_ticket(payload, &ticket);
    if (error != TicketError::None) {
        return error;
    }
    const auto pinned = keys_.find(ticket.key_id);
    if (pinned == keys_.end()) {
        return TicketError::UnknownKey;
    }
    std::vector<uint8_t> message;
    message.reserve(std::strlen(kDomainPrefix) + 1 + payload.size());
    message.insert(message.end(), kDomainPrefix, kDomainPrefix + std::strlen(kDomainPrefix) + 1);
    message.insert(message.end(), payload.begin(), payload.end());
    const uint8_t* signature = signed_ticket.data() + payload_size;
    if (!verify_ed25519(message, signature, pinned->second.data())) {
        return TicketError::Signature;
    }
    if (ticket.issuer != issuer_) {
        return TicketError::Issuer;
    }
    if (ticket.audience != audience_) {
        return TicketError::Audience;
    }
    if (ticket.host_id != expected_host_id) {
        return TicketError::Host;
    }
    if (!fingerprint_equal(ticket.host_public_key_fingerprint, expected_host_fingerprint)) {
        return TicketError::Fingerprint;
    }
    if (now_unix < ticket.not_before - kTicketClockSkewSeconds) {
        return TicketError::NotYetValid;
    }
    if (ticket.expires_at <= std::numeric_limits<int64_t>::max() - kTicketClockSkewSeconds &&
        now_unix > ticket.expires_at + kTicketClockSkewSeconds) {
        return TicketError::Expired;
    }
    *out = std::move(ticket);
    return TicketError::None;
}

TicketError AccessTicketVerifier::commit(const AccessTicket& ticket, int64_t now_unix) {
    switch (replay_.consume(ticket.id, ticket.expires_at, now_unix)) {
        case TicketReplayGuard::Consume::Accepted:
            return TicketError::None;
        case TicketReplayGuard::Consume::Replayed:
            return TicketError::Replayed;
        case TicketReplayGuard::Consume::Full:
            return TicketError::ReplayFull;
    }
    return TicketError::ReplayFull;
}

TicketError AccessTicketVerifier::verify(const std::vector<uint8_t>& signed_ticket,
                                         const std::string& expected_host_id,
                                         const std::string& expected_host_fingerprint,
                                         int64_t now_unix, AccessTicket* out) {
    AccessTicket ticket;
    const TicketError error =
        inspect(signed_ticket, expected_host_id, expected_host_fingerprint, now_unix, &ticket);
    if (error != TicketError::None) {
        return error;
    }
    const TicketError consumed = commit(ticket, now_unix);
    if (consumed != TicketError::None) {
        return consumed;
    }
    *out = std::move(ticket);
    return TicketError::None;
}

size_t AccessTicketVerifier::replay_size() const { return replay_.size(); }

std::vector<uint8_t> access_ticket_proof_message(
    const std::string& host_id, const std::string& host_fingerprint,
    const std::array<uint8_t, kTicketProofHostNonceSize>& host_nonce,
    const std::vector<uint8_t>& client_nonce,
    const std::vector<uint8_t>& signed_ticket) {
    std::vector<uint8_t> message;
    message.reserve(22 + 2 + host_id.size() + 2 + host_fingerprint.size() + 32 + 2 +
                    client_nonce.size() + 32);
    message.insert(message.end(), kProofDomainPrefix,
                   kProofDomainPrefix + std::strlen(kProofDomainPrefix) + 1);
    append_u16(message, host_id.size());
    message.insert(message.end(), host_id.begin(), host_id.end());
    // Both sides must derive the same bytes, and hosts and viewers spell the
    // certificate digest differently, so the digest form is canonical here.
    const std::string digits = fingerprint_digits(host_fingerprint);
    const std::string& fingerprint = digits.empty() ? host_fingerprint : digits;
    append_u16(message, fingerprint.size());
    message.insert(message.end(), fingerprint.begin(), fingerprint.end());
    message.insert(message.end(), host_nonce.begin(), host_nonce.end());
    append_u16(message, client_nonce.size());
    message.insert(message.end(), client_nonce.begin(), client_nonce.end());
    const std::array<uint8_t, 32> ticket_digest = sha256(signed_ticket);
    message.insert(message.end(), ticket_digest.begin(), ticket_digest.end());
    return message;
}

bool verify_access_ticket_proof(const AccessTicket& ticket,
                                const std::vector<uint8_t>& message,
                                const std::vector<uint8_t>& proof) {
    if (message.empty() || proof.size() != kTicketProofSize) {
        return false;
    }
    return verify_ed25519(message, proof.data(), ticket.client_public_key.data());
}

std::string encode_ticket_base64url(const std::vector<uint8_t>& data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((data.size() * 4 + 2) / 3);
    size_t offset = 0;
    while (offset + 3 <= data.size()) {
        const uint32_t value = (static_cast<uint32_t>(data[offset]) << 16) |
                               (static_cast<uint32_t>(data[offset + 1]) << 8) |
                               static_cast<uint32_t>(data[offset + 2]);
        out.push_back(kAlphabet[(value >> 18) & 0x3f]);
        out.push_back(kAlphabet[(value >> 12) & 0x3f]);
        out.push_back(kAlphabet[(value >> 6) & 0x3f]);
        out.push_back(kAlphabet[value & 0x3f]);
        offset += 3;
    }
    const size_t remaining = data.size() - offset;
    if (remaining == 1) {
        const uint32_t value = static_cast<uint32_t>(data[offset]) << 16;
        out.push_back(kAlphabet[(value >> 18) & 0x3f]);
        out.push_back(kAlphabet[(value >> 12) & 0x3f]);
    } else if (remaining == 2) {
        const uint32_t value = (static_cast<uint32_t>(data[offset]) << 16) |
                               (static_cast<uint32_t>(data[offset + 1]) << 8);
        out.push_back(kAlphabet[(value >> 18) & 0x3f]);
        out.push_back(kAlphabet[(value >> 12) & 0x3f]);
        out.push_back(kAlphabet[(value >> 6) & 0x3f]);
    }
    return out;
}

bool derive_access_ticket_public_key(const std::vector<uint8_t>& seed,
                                     std::vector<uint8_t>* public_key) {
    if (public_key == nullptr || seed.size() != kTicketClientKeySize) {
        return false;
    }
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(),
                                                 seed.size());
    if (key == nullptr) {
        return false;
    }
    size_t size = kTicketClientKeySize;
    std::vector<uint8_t> derived(size);
    const bool ok = EVP_PKEY_get_raw_public_key(key, derived.data(), &size) == 1 &&
                    size == kTicketClientKeySize;
    EVP_PKEY_free(key);
    if (!ok) {
        return false;
    }
    *public_key = std::move(derived);
    return true;
}

bool generate_access_ticket_key(std::vector<uint8_t>* seed,
                                std::vector<uint8_t>* public_key) {
    if (seed == nullptr || public_key == nullptr) {
        return false;
    }
    std::vector<uint8_t> generated(kTicketClientKeySize);
    if (RAND_bytes(generated.data(), static_cast<int>(generated.size())) != 1) {
        return false;
    }
    if (!derive_access_ticket_public_key(generated, public_key)) {
        return false;
    }
    *seed = std::move(generated);
    return true;
}

bool sign_access_ticket_proof(const std::vector<uint8_t>& key_seed,
                              const std::vector<uint8_t>& message,
                              std::vector<uint8_t>* proof) {
    if (proof == nullptr || message.empty() || key_seed.size() != kTicketClientKeySize) {
        return false;
    }
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, key_seed.data(),
                                                 key_seed.size());
    if (key == nullptr) {
        return false;
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    std::vector<uint8_t> signature(kTicketProofSize);
    size_t signature_size = signature.size();
    bool signed_ok = false;
    if (ctx != nullptr && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestSign(ctx, signature.data(), &signature_size, message.data(),
                       message.size()) == 1 &&
        signature_size == kTicketProofSize) {
        signed_ok = true;
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    if (!signed_ok) {
        return false;
    }
    *proof = std::move(signature);
    return true;
}

}  // namespace chast
