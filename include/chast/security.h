#pragma once

// Security primitives shared by the host, the viewer and the tools.
//
// Everything Windows-specific (CNG, DPAPI) lives in security.cc so callers
// deal in inert strings and byte vectors: a password hash, a TOTP secret,
// a protected blob and an ECDSA device key.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace chast {

// ----- random -----

// Fills `out` from the system CSPRNG. False only when the OS call fails.
bool secure_random(uint8_t* out, size_t size);

// `length` characters from an alphabet without look-alikes (no 0/O/1/I).
std::string random_token(size_t length = 8);

// Exactly `digits` decimal digits, uniform, leading zeros allowed.
std::string random_digits(size_t digits = 6);

// ----- encoding -----

std::string hex_encode(const uint8_t* data, size_t size);
std::string hex_encode(const std::vector<uint8_t>& data);
bool hex_decode(const std::string& text, std::vector<uint8_t>& out);

std::string base64_encode(const uint8_t* data, size_t size);
std::string base64_encode(const std::vector<uint8_t>& data);
bool base64_decode(const std::string& text, std::vector<uint8_t>& out);

// Constant-time equality for secrets. Length is allowed to leak.
bool constant_time_equal(const void* a, const void* b, size_t size);

// ----- password hashing -----

inline constexpr uint32_t kPasswordHashIterations = 210000;
inline constexpr size_t kPasswordSaltBytes = 16;
inline constexpr size_t kPasswordHashBytes = 32;

// Encoded as pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>.
std::string hash_password(const std::string& password);
std::string hash_password_with_salt(const std::string& password,
                                    const std::vector<uint8_t>& salt,
                                    uint32_t iterations = kPasswordHashIterations);
bool verify_password(const std::string& password, const std::string& encoded);

// ----- TOTP -----

// A fresh 20-byte secret, base32 encoded without padding.
std::string generate_totp_secret();
bool parse_totp_secret(const std::string& base32, std::vector<uint8_t>& key);

// RFC 6238, SHA-1, six digits by default.
uint32_t totp_code(const std::vector<uint8_t>& key, uint64_t unix_seconds,
                   uint32_t step_seconds = 30, uint32_t digits = 6);
// Accepts the code at `unix_seconds` and +/- `window` steps. The comparison
// is constant time.
bool verify_totp(const std::string& base32_secret, const std::string& code,
                 uint64_t unix_seconds, int window = 1);
std::string totp_uri(const std::string& secret_base32, const std::string& issuer,
                     const std::string& account);

// ----- DPAPI -----

enum class SecretScope {
    CurrentUser,
    LocalMachine,
};

bool protect_secret(const std::string& plaintext, SecretScope scope,
                    const std::string& entropy, std::string& protected_base64,
                    std::string* error);
bool unprotect_secret(const std::string& protected_base64, SecretScope scope,
                      const std::string& entropy, std::string& plaintext,
                      std::string* error);

// ----- device identity -----

// An ECDSA P-256 keypair. The public key is a BCRYPT_ECCPUBLIC_BLOB (72 bytes
// for P-256); signatures are the raw 64-byte r||s pair over SHA-256(message).
class DeviceIdentity {
public:
    ~DeviceIdentity();
    DeviceIdentity(const DeviceIdentity&) = delete;
    DeviceIdentity& operator=(const DeviceIdentity&) = delete;

    static std::unique_ptr<DeviceIdentity> generate(std::string* error);
    // Loads a DPAPI-protected private key. Returns nullptr when the file is
    // missing or cannot be decrypted; load_or_create handles the first case.
    static std::unique_ptr<DeviceIdentity> load(const std::string& path,
                                                std::string* error);
    static std::unique_ptr<DeviceIdentity> load_or_create(const std::string& path,
                                                          std::string* error);

    bool save(const std::string& path, std::string* error) const;
    const std::vector<uint8_t>& public_key() const;
    std::vector<uint8_t> private_key_blob() const;

    bool sign(const uint8_t* data, size_t size, std::vector<uint8_t>& signature,
              std::string* error) const;
    bool sign(const std::vector<uint8_t>& message, std::vector<uint8_t>& signature,
              std::string* error) const;

    // The local computer name, UTF-8; a display hint for the trust list.
    std::string name() const;

    static bool verify(const std::vector<uint8_t>& public_key, const uint8_t* data,
                       size_t size, const std::vector<uint8_t>& signature);

private:
    DeviceIdentity();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ----- IPv4 CIDR -----

struct Ipv4Range {
    uint32_t address = 0;  // host byte order
    uint8_t prefix = 0;
};

bool parse_ipv4(const std::string& text, uint32_t& address);
bool parse_ipv4_cidr(const std::string& text, Ipv4Range& range);
bool ipv4_in_range(uint32_t address, const Ipv4Range& range);
bool ipv4_in_ranges(uint32_t address, const std::vector<Ipv4Range>& ranges);

}  // namespace chast
