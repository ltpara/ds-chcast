// Crypto primitives against published vectors, plus DPAPI and device-key
// round trips inside the current user's profile.

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "chast/security.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

std::vector<uint8_t> bytes_of(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::string temp_path(const char* suffix) {
    char directory[MAX_PATH] = {};
    const DWORD length = GetTempPathA(MAX_PATH, directory);
    std::string path = length > 0 ? std::string(directory, length) : std::string(".\\");
    path += "chast-security-test-";
    path += std::to_string(GetCurrentProcessId());
    path += "-";
    path += suffix;
    return path;
}

void test_encoding() {
    const std::vector<uint8_t> data = {0x00, 0x01, 0xFE, 0xFF, 0x42};
    check(chast::hex_encode(data) == "0001feff42", "hex encodes lower case");
    std::vector<uint8_t> decoded;
    check(chast::hex_decode("0001FEFF42", decoded) && decoded == data,
          "hex decodes mixed case");
    check(!chast::hex_decode("0g", decoded), "hex rejects a non-digit");
    check(!chast::hex_decode("abc", decoded), "hex rejects an odd length");

    check(chast::base64_encode(data) == "AAH+/0I=", "base64 encodes padding");
    check(chast::base64_decode("AAH+/0I=", decoded) && decoded == data,
          "base64 decodes padding");
    check(chast::base64_decode("AAH+\n/0I=", decoded) && decoded == data,
          "base64 tolerates line breaks");
    check(chast::base64_decode("AAH+/0I", decoded) && decoded == data,
          "base64 accepts an unpadded tail");
    check(chast::base64_decode("QUI=", decoded) && bytes_of("AB") == decoded,
          "base64 stops at padding");

    const uint8_t left[] = {1, 2, 3, 4};
    const uint8_t right[] = {1, 2, 3, 4};
    const uint8_t other[] = {1, 2, 3, 5};
    check(chast::constant_time_equal(left, right, sizeof(left)),
          "constant-time equal accepts identical buffers");
    check(!chast::constant_time_equal(left, other, sizeof(left)),
          "constant-time equal rejects a difference");
    check(chast::constant_time_equal(left, other, 0),
          "constant-time equal with zero size is true");
}

void test_random() {
    const std::string token = chast::random_token(16);
    check(token.size() == 16, "random token has the requested length");
    bool alphabet_ok = !token.empty();
    for (char c : token) {
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '9'))) {
            alphabet_ok = false;
        }
    }
    check(alphabet_ok, "random token stays in the look-alike-free alphabet");
    check(chast::random_token(16) != token, "two random tokens differ");

    const std::string digits = chast::random_digits(6);
    bool digits_ok = digits.size() == 6;
    for (char c : digits) {
        if (c < '0' || c > '9') {
            digits_ok = false;
        }
    }
    check(digits_ok, "random digits are six decimal characters");

    uint8_t buffer[32] = {};
    check(chast::secure_random(buffer, sizeof(buffer)), "secure_random fills a buffer");
    bool any = false;
    for (uint8_t value : buffer) {
        any = any || value != 0;
    }
    check(any, "secure_random does not return zeros");
}

void test_password_hashing() {
    const std::vector<uint8_t> salt = bytes_of("salt");
    const std::string one =
        chast::hash_password_with_salt("password", salt, 1);
    check(one ==
              "pbkdf2-sha256$1$73616c74$"
              "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b",
          "PBKDF2-HMAC-SHA256 matches the published vector");
    check(chast::verify_password("password", one), "verify accepts the vector hash");
    check(!chast::verify_password("Password", one),
          "verify rejects a different password");
    check(!chast::verify_password("password", ""), "verify rejects an empty encoding");
    check(!chast::verify_password("password", "pbkdf2-sha256$0$73616c74$00"),
          "verify rejects zero iterations");
    check(!chast::verify_password("password", "pbkdf2-sha256$1$73616c74$00"),
          "verify rejects a truncated hash");
    check(!chast::verify_password("password", "bcrypt$1$73616c74$00"),
          "verify rejects an unknown scheme");
    check(chast::hash_password_with_salt("password", salt, 0).empty(),
          "hashing rejects zero iterations");

    const std::string first = chast::hash_password("correct horse");
    const std::string second = chast::hash_password("correct horse");
    check(!first.empty() &&
              first.rfind("pbkdf2-sha256$600000$", 0) == 0,
          "hash_password uses the default iteration count");
    check(first != second, "hash_password salts each hash");
    check(chast::verify_password("correct horse", first),
          "hash_password round-trips");
    check(!chast::verify_password("wrong horse", first),
          "hash_password rejects the wrong password");
    check(chast::password_hash_needs_rehash(one),
          "an older iteration count asks for a rehash");
    check(!chast::password_hash_needs_rehash(first),
          "a current hash needs no rehash");
    check(!chast::password_hash_needs_rehash(""),
          "an empty encoding needs no rehash");
    check(!chast::password_hash_needs_rehash("bcrypt$1$73616c74$00"),
          "an unknown scheme needs no rehash");
}

void test_totp() {
    // RFC 6238 SHA-1 seed: the ASCII string "12345678901234567890".
    const std::string secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
    std::vector<uint8_t> key;
    check(chast::parse_totp_secret(secret, key) && key.size() == 20,
          "TOTP secret parses to 20 bytes");
    check(chast::totp_code(key, 59) == 287082, "TOTP vector at 59 seconds");
    check(chast::totp_code(key, 1111111109) == 81804, "TOTP vector at 1111111109");
    check(chast::totp_code(key, 1111111111) == 50471, "TOTP vector at 1111111111");
    check(chast::totp_code(key, 1234567890) == 5924, "TOTP vector at 1234567890");
    check(chast::totp_code(key, 2000000000) == 279037, "TOTP vector at 2000000000");
    check(chast::totp_code(key, 20000000000ull) == 353130,
          "TOTP vector at 20000000000");

    uint64_t matched_step = 0;
    check(chast::verify_totp(secret, "287082", 59, 0, &matched_step) && matched_step == 1,
          "verify reports the matching RFC 6238 time step");
    check(chast::verify_totp(secret, " 287 082 ", 59),
          "verify tolerates spaces in the code");
    check(!chast::verify_totp(secret, "287083", 59), "verify rejects a wrong code");
    check(!chast::verify_totp(secret, "28708", 59), "verify rejects five digits");
    check(!chast::verify_totp("not a secret!", "000000", 59),
          "verify rejects an invalid secret");

    const uint32_t previous = chast::totp_code(key, 29);
    char code[8];
    std::snprintf(code, sizeof(code), "%06u", previous);
    check(!chast::verify_totp(secret, code, 59, 0),
          "window 0 rejects the previous step");
    check(chast::verify_totp(secret, code, 59, 1),
          "window 1 accepts the previous step");

    const std::string generated = chast::generate_totp_secret();
    std::vector<uint8_t> generated_key;
    check(!generated.empty() &&
              chast::parse_totp_secret(generated, generated_key) &&
              generated_key.size() == 20,
          "generated TOTP secret is 20 bytes of base32");
    const std::string uri =
        chast::totp_uri(generated, "chast host", "viewer@example");
    check(uri.find("otpauth://totp/chast%20host:viewer@example?secret=") == 0 &&
              uri.find(generated) != std::string::npos &&
              uri.find("issuer=chast%20host") != std::string::npos,
          "otpauth URI carries the secret and issuer");
}

void test_dpapi() {
    std::string error;
    std::string protected_blob;
    check(chast::protect_secret("hello secret", chast::SecretScope::CurrentUser,
                                "chast-test", protected_blob, &error),
          "DPAPI protects a secret");
    check(!protected_blob.empty() && protected_blob != "hello secret",
          "the protected blob does not contain the plaintext");

    std::string plaintext;
    check(chast::unprotect_secret(protected_blob, chast::SecretScope::CurrentUser,
                                  "chast-test", plaintext, &error) &&
              plaintext == "hello secret",
          "DPAPI round-trips the secret");
    check(!chast::unprotect_secret(protected_blob, chast::SecretScope::CurrentUser,
                                   "wrong-entropy", plaintext, &error),
          "DPAPI rejects a different entropy");
    check(!chast::unprotect_secret("not base64 !!", chast::SecretScope::CurrentUser,
                                   "chast-test", plaintext, &error),
          "DPAPI rejects malformed input");

    std::vector<uint8_t> blob;
    check(chast::base64_decode(protected_blob, blob) && !blob.empty(),
          "the protected blob decodes");
    blob[blob.size() / 2] ^= 0x01;
    check(!chast::unprotect_secret(chast::base64_encode(blob),
                                   chast::SecretScope::CurrentUser, "chast-test",
                                   plaintext, &error),
          "DPAPI rejects a tampered blob");
}

void test_device_identity() {
    const std::string path = temp_path("device.bin");
    const std::string missing = temp_path("missing.bin");
    const std::string unwritable = temp_path("no-such-dir\\device.bin");
    DeleteFileA(path.c_str());
    DeleteFileA(missing.c_str());

    std::string error;
    check(chast::DeviceIdentity::load(missing, &error) == nullptr,
          "loading a missing device key fails");
    check(chast::DeviceIdentity::load_or_create(unwritable, &error) == nullptr,
          "load_or_create fails when the path cannot be written");

    auto identity = chast::DeviceIdentity::load_or_create(path, &error);
    if (!identity) {
        std::printf("     device error: %s\n", error.c_str());
    }
    check(identity != nullptr, "load_or_create generates a device key");
    if (!identity) {
        DeleteFileA(path.c_str());
        return;
    }
    check(identity->public_key().size() == 72, "P-256 public key blob is 72 bytes");
    check(!identity->name().empty(), "the device name is populated");

    const std::vector<uint8_t> message = bytes_of("chast device proof");
    std::vector<uint8_t> signature;
    check(identity->sign(message, signature, &error), "the device key signs");
    check(signature.size() == 64, "P-256 signatures are 64 bytes");
    check(chast::DeviceIdentity::verify(identity->public_key(), message.data(),
                                        message.size(), signature),
          "the signature verifies against the public key");

    std::vector<uint8_t> altered = message;
    altered[0] ^= 0x01;
    check(!chast::DeviceIdentity::verify(identity->public_key(), altered.data(),
                                         altered.size(), signature),
          "verification fails for altered data");
    std::vector<uint8_t> bad_signature = signature;
    bad_signature[0] ^= 0x01;
    check(!chast::DeviceIdentity::verify(identity->public_key(), message.data(),
                                         message.size(), bad_signature),
          "verification fails for an altered signature");

    auto reloaded = chast::DeviceIdentity::load(path, &error);
    if (!reloaded) {
        std::printf("     device reload error: %s\n", error.c_str());
    }
    check(reloaded != nullptr, "the saved device key loads again");
    check(reloaded && reloaded->public_key() == identity->public_key(),
          "the reloaded key keeps the same public key");
    if (reloaded) {
        std::vector<uint8_t> second_signature;
        check(reloaded->sign(message, second_signature, &error) &&
                  chast::DeviceIdentity::verify(identity->public_key(),
                                                message.data(), message.size(),
                                                second_signature),
              "the reloaded private key still signs");
    }

    const std::string corrupt_path = temp_path("corrupt.bin");
    {
        std::ofstream file(corrupt_path, std::ios::binary | std::ios::trunc);
        file << "chast-device-v1\nAAAA\n";
    }
    check(chast::DeviceIdentity::load(corrupt_path, &error) == nullptr,
          "a corrupt device key is rejected");

    DeleteFileA(path.c_str());
    DeleteFileA(corrupt_path.c_str());
    DeleteFileA((path + ".tmp").c_str());
}

void test_ipv4() {
    uint32_t address = 0;
    check(chast::parse_ipv4("192.168.1.10", address) &&
              address == (192u << 24 | 168u << 16 | 1u << 8 | 10u),
          "IPv4 parses a dotted quad");
    check(chast::parse_ipv4(" 255.255.255.255 ", address) &&
              address == 0xFFFFFFFFu,
          "IPv4 trims surrounding spaces");
    check(!chast::parse_ipv4("256.0.0.1", address), "IPv4 rejects an octet over 255");
    check(!chast::parse_ipv4("1.2.3", address), "IPv4 rejects three octets");
    check(!chast::parse_ipv4("1.2.3.4.5", address), "IPv4 rejects five octets");
    check(!chast::parse_ipv4("1..3.4", address), "IPv4 rejects an empty octet");
    check(!chast::parse_ipv4("", address), "IPv4 rejects an empty string");

    chast::Ipv4Range range;
    check(chast::parse_ipv4_cidr("10.0.0.0/8", range) && range.prefix == 8,
          "CIDR parses a prefix");
    chast::parse_ipv4("10.255.255.255", address);
    check(chast::ipv4_in_range(address, range), "CIDR contains its last address");
    chast::parse_ipv4("11.0.0.0", address);
    check(!chast::ipv4_in_range(address, range), "CIDR excludes the next block");

    check(chast::parse_ipv4_cidr("0.0.0.0/0", range) && range.prefix == 0,
          "CIDR accepts /0");
    chast::parse_ipv4("203.0.113.9", address);
    check(chast::ipv4_in_range(address, range), "CIDR /0 matches everything");

    check(chast::parse_ipv4_cidr("192.168.1.5/32", range) && range.prefix == 32,
          "CIDR accepts /32");
    chast::parse_ipv4("192.168.1.5", address);
    check(chast::ipv4_in_range(address, range), "CIDR /32 matches exactly");
    chast::parse_ipv4("192.168.1.6", address);
    check(!chast::ipv4_in_range(address, range), "CIDR /32 rejects a neighbour");

    check(chast::parse_ipv4_cidr("192.168.1.0/24", range), "CIDR accepts /24");
    chast::parse_ipv4("192.168.1.255", address);
    check(chast::ipv4_in_range(address, range), "CIDR /24 includes the broadcast");
    chast::parse_ipv4("192.168.2.1", address);
    check(!chast::ipv4_in_range(address, range), "CIDR /24 excludes the next subnet");

    check(!chast::parse_ipv4_cidr("10.0.0.0/33", range), "CIDR rejects /33");
    check(!chast::parse_ipv4_cidr("10.0.0.0/", range), "CIDR rejects an empty prefix");
    check(!chast::parse_ipv4_cidr("10.0.0.0/-1", range), "CIDR rejects a negative prefix");

    chast::Ipv4Range first;
    chast::Ipv4Range second;
    chast::parse_ipv4_cidr("10.0.0.0/8", first);
    chast::parse_ipv4_cidr("192.168.0.0/16", second);
    const std::vector<chast::Ipv4Range> ranges = {first, second};
    chast::parse_ipv4("192.168.4.4", address);
    check(chast::ipv4_in_ranges(address, ranges), "a list matches its second range");
    chast::parse_ipv4("172.16.0.1", address);
    check(!chast::ipv4_in_ranges(address, ranges), "a list rejects an outsider");
    check(!chast::ipv4_in_ranges(address, {}), "an empty list matches nothing");
}

void test_ipv6() {
    std::array<uint8_t, 16> address{};
    check(chast::parse_ipv6("2001:db8::1", address) && address[0] == 0x20 &&
              address[1] == 0x01 && address[2] == 0x0d && address[3] == 0xb8 &&
              address[14] == 0 && address[15] == 1,
          "IPv6 parses a compressed address");
    check(chast::parse_ipv6("::1", address) && address[15] == 1, "IPv6 parses the loopback");
    check(chast::parse_ipv6("::", address) && address == std::array<uint8_t, 16>{},
          "IPv6 parses the unspecified address");
    check(chast::parse_ipv6("2001:DB8:0:0:0:0:0:1", address) && address[15] == 1,
          "IPv6 accepts uppercase hex and full groups");
    check(chast::parse_ipv6("fe80::1%eth0", address) && address[15] == 1,
          "IPv6 ignores a zone suffix");
    check(chast::parse_ipv6("::ffff:192.168.1.10", address) && address[12] == 192 &&
              address[15] == 10,
          "IPv6 accepts a dotted-quad tail");

    check(!chast::parse_ipv6("1:2:3:4:5:6:7", address), "IPv6 rejects seven groups");
    check(!chast::parse_ipv6("1:2:3:4:5:6:7:8:9", address), "IPv6 rejects nine groups");
    check(!chast::parse_ipv6("1::2::3", address), "IPv6 rejects two gaps");
    check(!chast::parse_ipv6("2001:db8::1:", address),
          "IPv6 rejects a single trailing colon after compression");
    check(!chast::parse_ipv6("1:2:3:4:5:6:7:8:", address),
          "IPv6 rejects a single trailing colon after eight groups");
    check(chast::parse_ipv6("2001:db8::", address),
          "IPv6 still accepts trailing zero compression");
    check(!chast::parse_ipv6("12345::", address), "IPv6 rejects a five-digit group");
    check(!chast::parse_ipv6("::1.2.3.4:5", address), "IPv6 rejects a tail that is not last");
    check(!chast::parse_ipv6("", address), "IPv6 rejects an empty string");
    check(!chast::parse_ipv6("2001:db8::zz", address), "IPv6 rejects a non-hex group");

    chast::Ipv6Range range;
    chast::parse_ipv6_cidr("2001:db8::/32", range);
    chast::parse_ipv6("2001:db8:ffff::1", address);
    check(range.prefix == 32 && chast::ipv6_in_range(address, range),
          "an IPv6 CIDR contains a later subnet");
    chast::parse_ipv6("2001:db9::1", address);
    check(!chast::ipv6_in_range(address, range), "an IPv6 CIDR excludes a neighbour");

    check(chast::parse_ipv6_cidr("::/0", range) && range.prefix == 0, "IPv6 accepts /0");
    chast::parse_ipv6("2001:4860::8888", address);
    check(chast::ipv6_in_range(address, range), "IPv6 /0 matches everything");

    check(chast::parse_ipv6_cidr("2001:db8::5/128", range) && range.prefix == 128,
          "IPv6 accepts /128");
    chast::parse_ipv6("2001:db8::5", address);
    check(chast::ipv6_in_range(address, range), "IPv6 /128 matches exactly");
    chast::parse_ipv6("2001:db8::6", address);
    check(!chast::ipv6_in_range(address, range), "IPv6 /128 rejects a neighbour");
    check(!chast::parse_ipv6_cidr("2001:db8::/129", range), "IPv6 rejects /129");

    chast::parse_ipv6("2001:db8:1:2:3:4:5:6", address);
    check(chast::format_ipv6_prefix(address, 64) == "2001:db8:1:2::/64",
          "an IPv6 prefix formats with the compressed zero run");
    chast::parse_ipv6("2001:0db8:0000:0000:0000:0000:0000:0001", address);
    check(chast::format_ipv6_prefix(address, 48) == "2001:db8::/48",
          "prefix formatting is canonical");
    chast::parse_ipv6("::", address);
    check(chast::format_ipv6_prefix(address, 0) == "::/0", "the all-zero prefix formats");

    chast::IpAddress single;
    check(chast::parse_ip_address("192.168.1.1", single) && !single.ipv6 &&
              single.bytes[0] == 192,
          "a plain address parses as IPv4");
    check(chast::parse_ip_address("[2001:db8::1]", single) && single.ipv6,
          "a bracketed literal parses as IPv6");
    check(chast::parse_ip_address("::ffff:10.0.0.1", single) && !single.ipv6 &&
              single.bytes[0] == 10 && single.bytes[3] == 1,
          "a mapped literal folds to IPv4");
    check(!chast::parse_ip_address("not-an-address", single),
          "a non-address does not parse");

    chast::IpRange list_entry;
    check(chast::parse_ip_cidr("::ffff:192.168.0.0/112", list_entry) && !list_entry.ipv6 &&
              list_entry.prefix == 16,
          "a mapped CIDR becomes the equivalent IPv4 range");
    check(!chast::parse_ip_cidr("::ffff:192.168.0.0/64", list_entry),
          "a mapped CIDR wider than the IPv4 block is rejected");

    chast::IpRange v4_entry;
    chast::IpRange v6_entry;
    chast::parse_ip_cidr("10.0.0.0/8", v4_entry);
    chast::parse_ip_cidr("2001:db8::/32", v6_entry);
    const std::vector<chast::IpRange> mixed = {v4_entry, v6_entry};
    chast::parse_ip_address("10.1.2.3", single);
    check(chast::ip_in_ranges(single, mixed), "a mixed list matches its IPv4 entry");
    chast::parse_ip_address("2001:db8::9", single);
    check(chast::ip_in_ranges(single, mixed), "a mixed list matches its IPv6 entry");
    chast::parse_ip_address("172.16.0.1", single);
    check(!chast::ip_in_ranges(single, mixed), "a mixed list rejects an outsider");
    chast::parse_ip_address("2001:db9::9", single);
    check(!chast::ip_in_ranges(single, mixed), "family and prefix both have to match");
}

}  // namespace

int main() {
    test_encoding();
    test_random();
    test_password_hashing();
    test_totp();
    test_dpapi();
    test_device_identity();
    test_ipv4();
    test_ipv6();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all security checks passed\n");
    return 0;
}
