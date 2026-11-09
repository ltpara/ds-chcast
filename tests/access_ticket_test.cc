// Access-ticket v1 against the frozen golden vector and adversarial inputs.
// No GPU, no network: the signature, the byte layout and the replay cache are
// all deterministic.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "access_ticket_vector.h"
#include "chast/access_ticket.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

void check_error(chast::TicketError want, chast::TicketError got, const char* what) {
    const bool ok = want == got;
    std::printf("%s %s (got %s)\n", ok ? "ok  " : "FAIL", what,
                chast::ticket_error_text(got));
    if (!ok) {
        ++failures;
    }
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

std::vector<uint8_t> from_hex(const char* text) {
    std::vector<uint8_t> out;
    int high = -1;
    for (const char* p = text; *p != '\0'; ++p) {
        const int nibble = hex_nibble(*p);
        if (nibble < 0) {
            continue;
        }
        if (high < 0) {
            high = nibble;
        } else {
            out.push_back(static_cast<uint8_t>((high << 4) | nibble));
            high = -1;
        }
    }
    return out;
}

// Frozen in docs/access-control-design-2026-11-09.md section 6.
constexpr const char* kEnvelopeHex =
    "01010000000d63686173742e6578616d706c65000d6465766963653a646576626f78000b6b69642d323032362d"
    "31310006646576626f7800475348413235363a3031323334353637383961626364656630313233343536373839"
    "61626364656630313233343536373839616263646566303132333435363738396162636465660005616c696365"
    "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f0000000f00000002000000"
    "006af1b5c0000000006af1b5c0000000006af1b5fc00086a74692d30303031";
constexpr const char* kSignatureHex =
    "e907b6f1daaf566b88b482132e197190803bdb5e1d87b5c3100eb3bda6a993799033f6b3b09ab233dc61d3a"
    "82ccc8d70f78f2b2f6df5634794cdec5b31bad504";
// Public key derived from the issuer seed 00..1f.
constexpr const char* kPublicKeyB64 = "A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg";
constexpr const char* kIssuer = "chast.example";
constexpr const char* kAudience = "device:devbox";
constexpr const char* kKeyID = "kid-2026-11";
constexpr const char* kHostID = "devbox";
constexpr const char* kFingerprint =
    "SHA256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr int64_t kIssuedAt = 1794225600;
constexpr int64_t kExpiresAt = 1794225660;
constexpr size_t kEnvelopeSize = 209;
// Field offsets in the frozen envelope, used to mutate one field at a time.
constexpr size_t kIssuerLengthOffset = 4;
constexpr size_t kKidOffset = 36;
constexpr size_t kClientKeyOffset = 135;
constexpr size_t kIssuedAtOffset = 175;
constexpr size_t kNotBeforeOffset = 183;
constexpr size_t kExpiresAtOffset = 191;
constexpr size_t kIdLengthOffset = 199;

std::vector<uint8_t> golden_envelope() { return from_hex(kEnvelopeHex); }

std::vector<uint8_t> golden_signed() {
    std::vector<uint8_t> out = golden_envelope();
    const std::vector<uint8_t> signature = from_hex(kSignatureHex);
    out.insert(out.end(), signature.begin(), signature.end());
    return out;
}

std::unique_ptr<chast::AccessTicketVerifier> golden_verifier() {
    auto verifier =
        std::make_unique<chast::AccessTicketVerifier>(kIssuer, kAudience);
    std::string error;
    if (!verifier->pin_key(kKeyID, kPublicKeyB64, &error)) {
        std::printf("FAIL cannot pin the golden key: %s\n", error.c_str());
        ++failures;
    }
    return verifier;
}

void put_u64(std::vector<uint8_t>* data, size_t offset, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        (*data)[offset + i] = static_cast<uint8_t>(value >> ((7 - i) * 8));
    }
}

chast::TicketError verify_at(std::vector<uint8_t> signed_ticket, int64_t now_unix) {
    auto verifier = golden_verifier();
    chast::AccessTicket ticket;
    return verifier->verify(signed_ticket, kHostID, kFingerprint, now_unix, &ticket);
}

void test_golden_decode() {
    chast::AccessTicket ticket;
    const std::vector<uint8_t> envelope = golden_envelope();
    check(envelope.size() == kEnvelopeSize, "the frozen envelope is 209 bytes");
    check_error(chast::TicketError::None, chast::decode_access_ticket(envelope, &ticket),
                "the golden envelope decodes");
    check(ticket.issuer == kIssuer, "the issuer is decoded");
    check(ticket.audience == kAudience, "the audience is decoded");
    check(ticket.key_id == kKeyID, "the key id is decoded");
    check(ticket.host_id == kHostID, "the host id is decoded");
    check(ticket.host_public_key_fingerprint == kFingerprint, "the fingerprint is decoded");
    check(ticket.subject == "alice", "the subject is decoded");
    check(ticket.client_public_key.front() == 0x40 && ticket.client_public_key.back() == 0x5f,
          "the client public key is decoded");
    check(ticket.permissions == 0x0000000f, "the permission mask is decoded");
    check(ticket.policy_version == 2, "the policy version is decoded");
    check(ticket.issued_at == kIssuedAt && ticket.not_before == kIssuedAt &&
              ticket.expires_at == kExpiresAt,
          "the time window is decoded");
    check(ticket.id == "jti-0001", "the ticket id is decoded");
}

void test_golden_encode() {
    chast::AccessTicket ticket;
    check_error(chast::TicketError::None,
                chast::decode_access_ticket(golden_envelope(), &ticket),
                "the golden envelope decodes for re-encoding");
    const std::vector<uint8_t> encoded = ticket_test::encode(ticket);
    check(encoded == golden_envelope(),
          "the test encoder reproduces the frozen envelope byte for byte");
    const std::vector<uint8_t> minted =
        ticket_test::sign_ticket(ticket_test::sequential_seed(0x00), encoded);
    check(minted == golden_signed(),
          "signing the reproduced envelope reproduces the frozen signature");
}

void test_golden_verify() {
    auto verifier = golden_verifier();
    chast::AccessTicket ticket;
    check_error(chast::TicketError::None,
                verifier->verify(golden_signed(), kHostID, kFingerprint, kIssuedAt, &ticket),
                "the golden ticket verifies");
    check(ticket.id == "jti-0001", "verification returns the decoded ticket");
    check(verifier->replay_size() == 1, "a verified ticket id is consumed");
    check_error(chast::TicketError::Replayed,
                verifier->verify(golden_signed(), kHostID, kFingerprint, kIssuedAt, &ticket),
                "the same ticket cannot be used twice");
}

void test_fingerprint_spelling() {
    const std::string bare = std::string(kFingerprint).substr(7);
    std::string upper = kFingerprint;
    for (char& c : upper) {
        if (c >= 'a' && c <= 'f') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    std::string separated;
    for (size_t i = 0; i < bare.size(); i += 2) {
        if (!separated.empty()) {
            separated.push_back(':');
        }
        separated += bare.substr(i, 2);
    }
    const char* labels[] = {"a bare digest", "an uppercase digest", "a colon-separated digest"};
    const std::string values[] = {bare, upper, separated};
    for (size_t i = 0; i < 3; ++i) {
        auto verifier = golden_verifier();
        chast::AccessTicket ticket;
        check_error(chast::TicketError::None,
                    verifier->verify(golden_signed(), kHostID, values[i], kIssuedAt, &ticket),
                    labels[i]);
    }
}

// Deterministic Ed25519 key shared with the other ticket tests: no key files,
// no randomness, and the ticket entry carries the matching public key.
ticket_test::Seed proof_seed() { return ticket_test::sequential_seed(0x00); }

void test_proof() {
    std::vector<uint8_t> client_key;
    check(ticket_test::ed25519_public_key(proof_seed(), &client_key),
          "the proof key derives");
    chast::AccessTicket ticket;
    const std::vector<uint8_t> envelope = golden_envelope();
    check_error(chast::TicketError::None, chast::decode_access_ticket(envelope, &ticket),
                "the golden envelope decodes for the proof");
    std::memcpy(ticket.client_public_key.data(), client_key.data(),
                chast::kTicketClientKeySize);
    std::array<uint8_t, chast::kTicketProofHostNonceSize> host_nonce{};
    std::vector<uint8_t> client_nonce(16, 0);
    for (size_t i = 0; i < host_nonce.size(); ++i) {
        host_nonce[i] = static_cast<uint8_t>(i);
    }
    for (size_t i = 0; i < client_nonce.size(); ++i) {
        client_nonce[i] = static_cast<uint8_t>(0xa0 + i);
    }
    const std::vector<uint8_t> signed_ticket = golden_signed();
    const std::vector<uint8_t> message = chast::access_ticket_proof_message(
        kHostID, kFingerprint, host_nonce, client_nonce, signed_ticket);
    std::vector<uint8_t> proof;
    check(ticket_test::ed25519_sign(proof_seed(), message, &proof), "the proof signs");
    check(proof.size() == chast::kTicketProofSize, "the proof is 64 bytes");
    check(chast::verify_access_ticket_proof(ticket, message, proof),
          "the proof verifies against the ticket key");
    check(!chast::verify_access_ticket_proof(ticket, message, {}),
          "an empty proof is rejected");
    {
        std::vector<uint8_t> short_proof(proof.begin(), proof.end() - 1);
        check(!chast::verify_access_ticket_proof(ticket, message, short_proof),
              "a short proof is rejected");
    }
    {
        std::vector<uint8_t> broken = proof;
        broken[0] ^= 0x01;
        check(!chast::verify_access_ticket_proof(ticket, message, broken),
              "a broken proof is rejected");
    }
    {
        chast::AccessTicket other = ticket;
        other.client_public_key[0] ^= 0x01;
        check(!chast::verify_access_ticket_proof(other, message, proof),
              "a proof from another client key is rejected");
    }
    {
        std::array<uint8_t, chast::kTicketProofHostNonceSize> changed = host_nonce;
        changed[0] ^= 0x01;
        const std::vector<uint8_t> other_message = chast::access_ticket_proof_message(
            kHostID, kFingerprint, changed, client_nonce, signed_ticket);
        check(other_message != message, "the host challenge is part of the proof message");
        check(!chast::verify_access_ticket_proof(ticket, other_message, proof),
              "a proof for another host challenge is rejected");
    }
    {
        const std::vector<uint8_t> other_message = chast::access_ticket_proof_message(
            "otherbox", kFingerprint, host_nonce, client_nonce, signed_ticket);
        check(other_message != message, "the host id is part of the proof message");
    }
    {
        const std::vector<uint8_t> other_message = chast::access_ticket_proof_message(
            kHostID, "SHA256:other", host_nonce, client_nonce, signed_ticket);
        check(other_message != message, "the host fingerprint is part of the proof message");
    }
    {
        std::vector<uint8_t> other_ticket = signed_ticket;
        other_ticket[0] ^= 0x01;
        const std::vector<uint8_t> other_message = chast::access_ticket_proof_message(
            kHostID, kFingerprint, host_nonce, client_nonce, other_ticket);
        check(other_message != message, "the ticket digest is part of the proof message");
    }
    {
        const std::string bare = std::string(kFingerprint).substr(7);
        std::string upper = kFingerprint;
        for (char& c : upper) {
            if (c >= 'a' && c <= 'f') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        const std::vector<uint8_t> bare_message = chast::access_ticket_proof_message(
            kHostID, bare, host_nonce, client_nonce, signed_ticket);
        const std::vector<uint8_t> upper_message = chast::access_ticket_proof_message(
            kHostID, upper, host_nonce, client_nonce, signed_ticket);
        check(bare_message == message && upper_message == message,
              "the proof message ignores fingerprint spelling");
    }
}

void test_inspect_and_commit() {
    auto verifier = golden_verifier();
    chast::AccessTicket ticket;
    check_error(chast::TicketError::None,
                verifier->inspect(golden_signed(), kHostID, kFingerprint, kIssuedAt, &ticket),
                "inspect accepts the golden ticket");
    check(verifier->replay_size() == 0, "inspect does not consume the ticket id");
    check_error(chast::TicketError::None, verifier->commit(ticket, kIssuedAt),
                "commit accepts the id");
    check(verifier->replay_size() == 1, "commit records the id");
    check_error(chast::TicketError::Replayed, verifier->commit(ticket, kIssuedAt),
                "commit refuses a second use");
    check_error(chast::TicketError::Replayed,
                verifier->verify(golden_signed(), kHostID, kFingerprint, kIssuedAt, &ticket),
                "verify sees the id committed earlier");
}

void test_time_window() {
    check_error(chast::TicketError::NotYetValid, verify_at(golden_signed(), kIssuedAt - 6),
                "a ticket issued in the future is rejected");
    check_error(chast::TicketError::None, verify_at(golden_signed(), kIssuedAt - 5),
                "the five-second skew is allowed before not-before");
    check_error(chast::TicketError::None, verify_at(golden_signed(), kExpiresAt + 5),
                "the five-second skew is allowed after expiry");
    check_error(chast::TicketError::Expired, verify_at(golden_signed(), kExpiresAt + 6),
                "a ticket after expiry plus skew is rejected");
}

void test_binding() {
    {
        auto verifier = golden_verifier();
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Host,
                    verifier->verify(golden_signed(), "otherbox", kFingerprint, kIssuedAt,
                                     &ticket),
                    "a ticket for another host is rejected");
        check(verifier->replay_size() == 0, "a rejected ticket is not consumed");
    }
    {
        auto verifier = golden_verifier();
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Fingerprint,
                    verifier->verify(golden_signed(), kHostID, "SHA256:other", kIssuedAt,
                                     &ticket),
                    "a ticket for another certificate is rejected");
    }
    {
        auto verifier = golden_verifier();
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Field,
                    verifier->verify(golden_signed(), "", kFingerprint, kIssuedAt, &ticket),
                    "an empty host expectation fails closed");
    }
    {
        chast::AccessTicketVerifier verifier(kIssuer, kAudience);
        chast::AccessTicket ticket;
        check_error(chast::TicketError::UnknownKey,
                    verifier.verify(golden_signed(), kHostID, kFingerprint, kIssuedAt, &ticket),
                    "a verifier without pins rejects every ticket");
    }
}

void test_issuer_and_audience() {
    {
        chast::AccessTicketVerifier verifier("other.example", kAudience);
        std::string error;
        verifier.pin_key(kKeyID, kPublicKeyB64, &error);
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Issuer,
                    verifier.verify(golden_signed(), kHostID, kFingerprint, kIssuedAt, &ticket),
                    "a ticket from another issuer is rejected");
    }
    {
        chast::AccessTicketVerifier verifier(kIssuer, "device:other");
        std::string error;
        verifier.pin_key(kKeyID, kPublicKeyB64, &error);
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Audience,
                    verifier.verify(golden_signed(), kHostID, kFingerprint, kIssuedAt, &ticket),
                    "a ticket for another audience is rejected");
    }
}

void test_signature_and_key() {
    {
        std::vector<uint8_t> signed_ticket = golden_signed();
        signed_ticket.back() ^= 0x01;
        check_error(chast::TicketError::Signature,
                    verify_at(std::move(signed_ticket), kIssuedAt),
                    "a flipped signature byte is rejected");
    }
    {
        std::vector<uint8_t> envelope = golden_envelope();
        envelope[kClientKeyOffset] ^= 0x01;
        std::vector<uint8_t> signed_ticket = envelope;
        const std::vector<uint8_t> signature = from_hex(kSignatureHex);
        signed_ticket.insert(signed_ticket.end(), signature.begin(), signature.end());
        check_error(chast::TicketError::Signature,
                    verify_at(std::move(signed_ticket), kIssuedAt),
                    "a changed envelope no longer matches the signature");
    }
    {
        std::vector<uint8_t> envelope = golden_envelope();
        envelope[kKidOffset] = 'x';
        std::vector<uint8_t> signed_ticket = envelope;
        const std::vector<uint8_t> signature = from_hex(kSignatureHex);
        signed_ticket.insert(signed_ticket.end(), signature.begin(), signature.end());
        check_error(chast::TicketError::UnknownKey,
                    verify_at(std::move(signed_ticket), kIssuedAt),
                    "an unknown key id is rejected before the signature");
    }
}

void test_malformed() {
    {
        std::vector<uint8_t> envelope = golden_envelope();
        envelope.push_back(0);
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Malformed,
                    chast::decode_access_ticket(envelope, &ticket),
                    "a trailing byte is rejected");
    }
    {
        std::vector<uint8_t> envelope = golden_envelope();
        envelope.resize(kEnvelopeSize - 1);
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Malformed,
                    chast::decode_access_ticket(envelope, &ticket),
                    "a truncated envelope is rejected");
    }
    {
        std::vector<uint8_t> payload(88, 0);
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Malformed,
                    chast::decode_access_ticket(payload, &ticket),
                    "an envelope below the minimum size is rejected");
    }
    {
        std::vector<uint8_t> payload(chast::kTicketMaxEnvelope + 1, 0);
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Malformed,
                    chast::decode_access_ticket(payload, &ticket),
                    "an oversized envelope is rejected");
    }
    {
        std::vector<uint8_t> signed_ticket(chast::kTicketMaxSigned + 1, 0);
        auto verifier = golden_verifier();
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Malformed,
                    verifier->verify(signed_ticket, kHostID, kFingerprint, kIssuedAt, &ticket),
                    "an oversized signed ticket is rejected");
    }
    {
        std::vector<uint8_t> signed_ticket = golden_signed();
        signed_ticket.pop_back();
        check_error(chast::TicketError::Malformed,
                    verify_at(std::move(signed_ticket), kIssuedAt),
                    "a signed ticket without the full signature is rejected");
    }
}

void test_unsupported_and_fields() {
    {
        std::vector<uint8_t> envelope = golden_envelope();
        envelope[0] = 2;
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Unsupported,
                    chast::decode_access_ticket(envelope, &ticket),
                    "an unknown envelope version is rejected");
    }
    {
        std::vector<uint8_t> envelope = golden_envelope();
        envelope[1] = 2;
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Unsupported,
                    chast::decode_access_ticket(envelope, &ticket),
                    "an unknown algorithm is rejected");
    }
    {
        std::vector<uint8_t> envelope = golden_envelope();
        envelope[3] = 1;
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Unsupported,
                    chast::decode_access_ticket(envelope, &ticket),
                    "reserved flags are rejected");
    }
    {
        std::vector<uint8_t> envelope = golden_envelope();
        envelope[kIssuerLengthOffset] = 0;
        envelope[kIssuerLengthOffset + 1] = 0;
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Field,
                    chast::decode_access_ticket(envelope, &ticket),
                    "an empty field is rejected");
    }
    {
        std::vector<uint8_t> envelope = golden_envelope();
        envelope[kIssuerLengthOffset] = 0xff;
        envelope[kIssuerLengthOffset + 1] = 0xff;
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Field,
                    chast::decode_access_ticket(envelope, &ticket),
                    "an over-long field is rejected");
    }
    {
        std::vector<uint8_t> envelope = golden_envelope();
        for (size_t i = 0; i < chast::kTicketClientKeySize; ++i) {
            envelope[kClientKeyOffset + i] = 0;
        }
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Field,
                    chast::decode_access_ticket(envelope, &ticket),
                    "an all-zero client key is rejected");
    }
    {
        std::vector<uint8_t> envelope = golden_envelope();
        envelope[kIssuedAtOffset] = 0x80;
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Field,
                    chast::decode_access_ticket(envelope, &ticket),
                    "a negative time is rejected");
    }
    {
        std::vector<uint8_t> envelope = golden_envelope();
        put_u64(&envelope, kNotBeforeOffset, static_cast<uint64_t>(kIssuedAt - 1));
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Field,
                    chast::decode_access_ticket(envelope, &ticket),
                    "not-before preceding issued-at is rejected");
    }
    {
        std::vector<uint8_t> envelope = golden_envelope();
        put_u64(&envelope, kExpiresAtOffset, static_cast<uint64_t>(kIssuedAt));
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Field,
                    chast::decode_access_ticket(envelope, &ticket),
                    "an expiry at or before not-before is rejected");
    }
    {
        std::vector<uint8_t> envelope = golden_envelope();
        put_u64(&envelope, kExpiresAtOffset, static_cast<uint64_t>(kIssuedAt + 61));
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Lifetime,
                    chast::decode_access_ticket(envelope, &ticket),
                    "a lifetime above sixty seconds is rejected");
    }
    {
        std::vector<uint8_t> envelope = golden_envelope();
        envelope[kIdLengthOffset] = 0;
        envelope[kIdLengthOffset + 1] = 0;
        chast::AccessTicket ticket;
        check_error(chast::TicketError::Field,
                    chast::decode_access_ticket(envelope, &ticket),
                    "an empty ticket id is rejected");
    }
}

void test_replay_guard() {
    chast::TicketReplayGuard guard(2);
    check(guard.consume("a", 100, 0) == chast::TicketReplayGuard::Consume::Accepted,
          "the first id is accepted");
    check(guard.consume("a", 100, 0) == chast::TicketReplayGuard::Consume::Replayed,
          "the same id is refused");
    check(guard.consume("b", 100, 0) == chast::TicketReplayGuard::Consume::Accepted,
          "a second id fits the cache");
    check(guard.consume("c", 100, 0) == chast::TicketReplayGuard::Consume::Full,
          "a full cache refuses new ids instead of evicting live ones");
    check(guard.size() == 2, "a refused id is not recorded");
    check(guard.consume("a", 100, 105) == chast::TicketReplayGuard::Consume::Replayed,
          "an id survives the last accepted second");
    check(guard.consume("a", 100, 106) == chast::TicketReplayGuard::Consume::Accepted,
          "an expired id is pruned and may be reused");
    check(guard.consume("d", 100, 106) == chast::TicketReplayGuard::Consume::Accepted,
          "pruning makes room again");
    check(guard.consume("e", 100, 106) == chast::TicketReplayGuard::Consume::Full,
          "the capacity still bounds the cache");
}

void test_client_key_helpers() {
    std::vector<uint8_t> raw;
    check(chast::decode_ticket_base64url(kPublicKeyB64, &raw), "the golden key decodes");
    check(chast::encode_ticket_base64url(raw) == kPublicKeyB64,
          "base64url encoding reproduces the golden public key");
    check(chast::encode_ticket_base64url({}) == "", "an empty value encodes to nothing");

    std::vector<uint8_t> seed_first;
    std::vector<uint8_t> public_first;
    check(chast::generate_access_ticket_key(&seed_first, &public_first),
          "a client keypair is generated");
    check(seed_first.size() == chast::kTicketClientKeySize &&
              public_first.size() == chast::kTicketClientKeySize,
          "the generated keypair is two 32-byte values");
    std::vector<uint8_t> seed_second;
    std::vector<uint8_t> public_second;
    check(chast::generate_access_ticket_key(&seed_second, &public_second),
          "a second client keypair is generated");
    check(seed_first != seed_second && public_first != public_second,
          "generated keypairs differ");
    std::vector<uint8_t> derived;
    check(chast::derive_access_ticket_public_key(seed_first, &derived) &&
              derived == public_first,
          "the public key derives from the seed");
    seed_first.pop_back();
    check(!chast::derive_access_ticket_public_key(seed_first, &derived),
          "a short seed is rejected");

    std::vector<uint8_t> proof;
    const std::vector<uint8_t> message = {'r', 'e', 'n', 'e', 'w'};
    check(chast::sign_access_ticket_proof(seed_second, message, &proof) &&
              proof.size() == chast::kTicketProofSize,
          "the generated seed signs a possession proof");
    chast::AccessTicket ticket;
    std::memcpy(ticket.client_public_key.data(), public_second.data(),
                chast::kTicketClientKeySize);
    check(chast::verify_access_ticket_proof(ticket, message, proof),
          "the derived public key verifies the proof");
    proof[0] ^= 0x01;
    check(!chast::verify_access_ticket_proof(ticket, message, proof),
          "a modified proof is rejected");
}

void test_base64url_and_pins() {
    std::vector<uint8_t> raw;
    check(chast::decode_ticket_base64url(kPublicKeyB64, &raw), "the golden pin decodes");
    check(raw.size() == chast::kTicketClientKeySize, "the golden pin is 32 bytes");
    check(!chast::decode_ticket_base64url("A", &raw),
          "a single base64url character is not canonical");
    check(!chast::decode_ticket_base64url("AB", &raw),
          "non-zero trailing bits are rejected");
    check(!chast::decode_ticket_base64url(
              std::string(kPublicKeyB64) + "=", &raw),
          "base64 padding is rejected");
    check(!chast::decode_ticket_base64url(
              "A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMb!", &raw),
          "a character outside the URL alphabet is rejected");

    chast::AccessTicketVerifier verifier(kIssuer, kAudience);
    std::string error;
    check(!verifier.pin_key("", kPublicKeyB64, &error), "an empty key id is rejected");
    check(!verifier.pin_key(std::string(65, 'k'), kPublicKeyB64, &error),
          "an over-long key id is rejected");
    check(!verifier.pin_key(kKeyID, "AAAA", &error), "a short public key is rejected");
    check(verifier.pin_key(" " + std::string(kKeyID) + " ",
                           std::string(" ") + kPublicKeyB64 + "\n", &error),
          "surrounding whitespace is trimmed");
    chast::AccessTicket ticket;
    check_error(chast::TicketError::None,
                verifier.verify(golden_signed(), kHostID, kFingerprint, kIssuedAt, &ticket),
                "a trimmed pin admits the golden ticket");
}

}  // namespace

int main() {
    test_golden_decode();
    test_golden_encode();
    test_golden_verify();
    test_fingerprint_spelling();
    test_proof();
    test_inspect_and_commit();
    test_time_window();
    test_binding();
    test_issuer_and_audience();
    test_signature_and_key();
    test_malformed();
    test_unsupported_and_fields();
    test_replay_guard();
    test_client_key_helpers();
    test_base64url_and_pins();
    if (failures != 0) {
        std::printf("%d access-ticket checks failed\n", failures);
        return 1;
    }
    std::printf("access-ticket checks passed\n");
    return 0;
}
