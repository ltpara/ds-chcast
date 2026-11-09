#pragma once

// Test-only ticket minter. It mirrors server/internal/acl Encode() and signs
// with a caller-chosen Ed25519 seed, so tests can mint tickets for clients
// whose private key they control and can prove the C++ encoder reproduces the
// frozen golden vector byte for byte.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include "chast/access_ticket.h"

namespace ticket_test {

using Seed = std::array<uint8_t, 32>;

inline Seed sequential_seed(uint8_t first) {
    Seed seed{};
    for (size_t i = 0; i < seed.size(); ++i) {
        seed[i] = static_cast<uint8_t>(first + i);
    }
    return seed;
}

inline bool ed25519_public_key(const Seed& seed, std::vector<uint8_t>* out) {
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(),
                                                 seed.size());
    if (key == nullptr) {
        return false;
    }
    out->resize(chast::kTicketClientKeySize);
    size_t length = out->size();
    const bool ok = EVP_PKEY_get_raw_public_key(key, out->data(), &length) == 1 &&
                    length == out->size();
    EVP_PKEY_free(key);
    return ok;
}

inline bool ed25519_sign(const Seed& seed, const std::vector<uint8_t>& message,
                         std::vector<uint8_t>* signature) {
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(),
                                                 seed.size());
    if (key == nullptr) {
        return false;
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = ctx != nullptr && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1;
    if (ok) {
        signature->resize(chast::kTicketSignatureSize);
        size_t length = signature->size();
        ok = EVP_DigestSign(ctx, signature->data(), &length, message.data(), message.size()) ==
             1;
        signature->resize(ok ? length : 0);
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

inline void append_u16(std::vector<uint8_t>* out, size_t value) {
    out->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out->push_back(static_cast<uint8_t>(value & 0xff));
}

inline void append_u32(std::vector<uint8_t>* out, uint32_t value) {
    out->push_back(static_cast<uint8_t>(value >> 24));
    out->push_back(static_cast<uint8_t>(value >> 16));
    out->push_back(static_cast<uint8_t>(value >> 8));
    out->push_back(static_cast<uint8_t>(value));
}

inline void append_u64(std::vector<uint8_t>* out, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out->push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

inline void append_field(std::vector<uint8_t>* out, const std::string& value) {
    append_u16(out, value.size());
    out->insert(out->end(), value.begin(), value.end());
}

inline std::vector<uint8_t> encode(const chast::AccessTicket& ticket) {
    std::vector<uint8_t> out{1, 1, 0, 0};
    append_field(&out, ticket.issuer);
    append_field(&out, ticket.audience);
    append_field(&out, ticket.key_id);
    append_field(&out, ticket.host_id);
    append_field(&out, ticket.host_public_key_fingerprint);
    append_field(&out, ticket.subject);
    out.insert(out.end(), ticket.client_public_key.begin(), ticket.client_public_key.end());
    append_u32(&out, ticket.permissions);
    append_u32(&out, ticket.policy_version);
    append_u64(&out, static_cast<uint64_t>(ticket.issued_at));
    append_u64(&out, static_cast<uint64_t>(ticket.not_before));
    append_u64(&out, static_cast<uint64_t>(ticket.expires_at));
    append_field(&out, ticket.id);
    return out;
}

inline std::vector<uint8_t> sign_ticket(const Seed& issuer_seed,
                                        const std::vector<uint8_t>& envelope) {
    std::vector<uint8_t> message{'c', 'h', 'a', 's', 't', '-', 'a', 'c', 'c', 'e', 's',
                                 's', '-', 't', 'i', 'c', 'k', 'e', 't', '-', 'v', '1', 0};
    message.insert(message.end(), envelope.begin(), envelope.end());
    std::vector<uint8_t> signature;
    if (!ed25519_sign(issuer_seed, message, &signature)) {
        return {};
    }
    std::vector<uint8_t> out = envelope;
    out.insert(out.end(), signature.begin(), signature.end());
    return out;
}

}  // namespace ticket_test
