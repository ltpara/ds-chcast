#include "chast/security.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <bcrypt.h>
#include <dpapi.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <utility>

namespace chast {
namespace {

constexpr char kDeviceFileHeader[] = "chast-device-v1";
constexpr char kDeviceEntropy[] = "chast-device-key-v1";
constexpr uint32_t kMaxPasswordIterations = 10'000'000;
constexpr size_t kP256PublicBlobBytes = sizeof(BCRYPT_ECCKEY_BLOB) + 32 + 32;
constexpr size_t kP256PrivateBlobBytes = sizeof(BCRYPT_ECCKEY_BLOB) + 32 * 3;
constexpr size_t kP256SignatureBytes = 64;

void set_error(std::string* error, std::string text) {
    if (error) {
        *error = std::move(text);
    }
}

std::string win32_error(const char* what) {
    return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

std::string nt_error(const char* what, NTSTATUS status) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08lX)", what,
                  static_cast<unsigned long>(status));
    return buffer;
}

std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), length);
    return wide;
}

std::string narrow_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string narrow(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        narrow.data(), length, nullptr, nullptr);
    return narrow;
}

std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool hmac_sha1(const std::vector<uint8_t>& key, const uint8_t* data, size_t size,
               std::array<uint8_t, 20>& mac) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM,
                                                   nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0,
                                        const_cast<PUCHAR>(key.data()),
                                        static_cast<ULONG>(key.size()), 0))) {
        ok = BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(data),
                                           static_cast<ULONG>(size), 0)) &&
             BCRYPT_SUCCESS(BCryptFinishHash(hash, mac.data(),
                                             static_cast<ULONG>(mac.size()), 0));
    }
    if (hash) {
        BCryptDestroyHash(hash);
    }
    if (algorithm) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return ok;
}

bool pbkdf2_sha256(const std::string& password, const uint8_t* salt, size_t salt_size,
                   uint32_t iterations, uint8_t* out, size_t out_size) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    const NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status)) {
        return false;
    }
    const NTSTATUS derived = BCryptDeriveKeyPBKDF2(
        algorithm, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt),
        static_cast<ULONG>(salt_size), static_cast<ULONGLONG>(iterations), out,
        static_cast<ULONG>(out_size), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return BCRYPT_SUCCESS(derived);
}

int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

bool base32_decode(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t buffer = 0;
    int bits = 0;
    for (unsigned char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '-') {
            continue;
        }
        if (c == '=') {
            break;
        }
        int value = 0;
        if (c >= 'A' && c <= 'Z') {
            value = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            value = c - 'a';
        } else if (c >= '2' && c <= '7') {
            value = c - '2' + 26;
        } else {
            return false;
        }
        buffer = (buffer << 5) | static_cast<uint32_t>(value);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    if (out.empty()) {
        return false;
    }
    if (bits > 0 && (buffer & ((1u << bits) - 1)) != 0) {
        return false;
    }
    return true;
}

std::string base32_encode(const uint8_t* data, size_t size) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string out;
    out.reserve((size * 8 + 4) / 5);
    uint32_t buffer = 0;
    int bits = 0;
    for (size_t i = 0; i < size; ++i) {
        buffer = (buffer << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(kAlphabet[(buffer >> bits) & 0x1F]);
        }
    }
    if (bits > 0) {
        out.push_back(kAlphabet[(buffer << (5 - bits)) & 0x1F]);
    }
    return out;
}

std::string url_encode(const std::string& text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// The otpauth label keeps ':' as the issuer/account separator and '@' in the
// account, matching what authenticator apps show.
std::string url_encode_label(const std::string& text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
            c == ':' || c == '@') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

bool valid_public_blob(const std::vector<uint8_t>& blob) {
    if (blob.size() != kP256PublicBlobBytes) {
        return false;
    }
    BCRYPT_ECCKEY_BLOB header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    return header.dwMagic == BCRYPT_ECDSA_PUBLIC_P256_MAGIC && header.cbKey == 32;
}

bool valid_private_blob(const std::vector<uint8_t>& blob) {
    if (blob.size() != kP256PrivateBlobBytes) {
        return false;
    }
    BCRYPT_ECCKEY_BLOB header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    return header.dwMagic == BCRYPT_ECDSA_PRIVATE_P256_MAGIC && header.cbKey == 32;
}

bool export_key(BCRYPT_KEY_HANDLE key, LPCWSTR type, std::vector<uint8_t>& out,
                std::string* error) {
    ULONG length = 0;
    NTSTATUS status = BCryptExportKey(key, nullptr, type, nullptr, 0, &length, 0);
    if (!BCRYPT_SUCCESS(status) || length == 0) {
        set_error(error, nt_error("BCryptExportKey", status));
        return false;
    }
    out.resize(length);
    status = BCryptExportKey(key, nullptr, type, out.data(), length, &length, 0);
    if (!BCRYPT_SUCCESS(status)) {
        out.clear();
        set_error(error, nt_error("BCryptExportKey", status));
        return false;
    }
    out.resize(length);
    return true;
}

bool read_file(const std::string& path, std::string& contents, std::string* error) {
    const std::wstring wide = widen_utf8(path);
    if (wide.empty()) {
        set_error(error, "invalid device key path");
        return false;
    }
    std::ifstream file(std::filesystem::path(wide), std::ios::binary);
    if (!file) {
        set_error(error, "cannot open " + path);
        return false;
    }
    contents.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (!file.good() && !file.eof()) {
        set_error(error, "cannot read " + path);
        return false;
    }
    return true;
}

void wipe(void* data, size_t size) {
    if (data && size > 0) {
        SecureZeroMemory(data, size);
    }
}

bool file_exists(const std::string& path) {
    const std::wstring wide = widen_utf8(path);
    return !wide.empty() && GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string computer_name() {
    wchar_t buffer[256] = {};
    DWORD size = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, buffer, &size)) {
        return narrow_utf8(std::wstring(buffer, size));
    }
    size = 256;
    if (GetComputerNameW(buffer, &size)) {
        return narrow_utf8(std::wstring(buffer, size));
    }
    return "unknown";
}

}  // namespace

// ----- hashing -----

struct Sha256::Impl {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;

    ~Impl() {
        if (hash) {
            BCryptDestroyHash(hash);
        }
        if (algorithm) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    }
};

Sha256::Sha256() : impl_(std::make_unique<Impl>()) {
    reset();
}

Sha256::~Sha256() = default;
Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&&) noexcept = default;

void Sha256::reset() {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    if (impl_->hash) {
        BCryptDestroyHash(impl_->hash);
        impl_->hash = nullptr;
    }
    if (!impl_->algorithm &&
        !BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&impl_->algorithm,
                                                    BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        impl_->algorithm = nullptr;
        return;
    }
    if (!BCRYPT_SUCCESS(BCryptCreateHash(impl_->algorithm, &impl_->hash, nullptr, 0,
                                         nullptr, 0, 0))) {
        impl_->hash = nullptr;
    }
}

bool Sha256::ready() const {
    return impl_ && impl_->hash != nullptr;
}

bool Sha256::update(const void* data, size_t size) {
    if (!ready()) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    if (!data) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (offset < size) {
        const size_t chunk = std::min<size_t>(size - offset, 0xFFFFFFFFu);
        if (!BCRYPT_SUCCESS(BCryptHashData(impl_->hash,
                                           const_cast<PUCHAR>(bytes + offset),
                                           static_cast<ULONG>(chunk), 0))) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

bool Sha256::finish(std::array<uint8_t, 32>& digest) {
    if (!ready()) {
        return false;
    }
    const bool ok = BCRYPT_SUCCESS(BCryptFinishHash(impl_->hash, digest.data(),
                                                    static_cast<ULONG>(digest.size()), 0));
    BCryptDestroyHash(impl_->hash);
    impl_->hash = nullptr;
    return ok;
}

bool sha256(const uint8_t* data, size_t size, std::array<uint8_t, 32>& digest) {
    Sha256 hash;
    return hash.update(data, size) && hash.finish(digest);
}

// ----- random -----

bool secure_random(uint8_t* out, size_t size) {
    if (!out && size != 0) {
        return false;
    }
    size_t offset = 0;
    while (offset < size) {
        const size_t chunk = std::min<size_t>(size - offset, 1u << 20);
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out + offset,
                                           static_cast<ULONG>(chunk),
                                           BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

std::string random_token(size_t length) {
    static constexpr char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    constexpr uint32_t kAlphabetSize = sizeof(kAlphabet) - 1;
    constexpr uint32_t kLimit = 256 - (256 % kAlphabetSize);
    std::string token;
    token.reserve(length);
    uint8_t byte = 0;
    while (token.size() < length) {
        if (!secure_random(&byte, 1)) {
            return {};
        }
        if (byte >= kLimit) {
            continue;
        }
        token.push_back(kAlphabet[byte % kAlphabetSize]);
    }
    return token;
}

std::string random_digits(size_t digits) {
    constexpr uint32_t kLimit = 250;  // largest multiple of 10 below 256
    std::string text;
    text.reserve(digits);
    uint8_t byte = 0;
    while (text.size() < digits) {
        if (!secure_random(&byte, 1)) {
            return {};
        }
        if (byte >= kLimit) {
            continue;
        }
        text.push_back(static_cast<char>('0' + byte % 10));
    }
    return text;
}

// ----- encoding -----

std::string hex_encode(const uint8_t* data, size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

std::string hex_encode(const std::vector<uint8_t>& data) {
    return hex_encode(data.data(), data.size());
}

bool hex_decode(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    int high = -1;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            continue;
        }
        int value = -1;
        if (c >= '0' && c <= '9') {
            value = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            value = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            value = c - 'A' + 10;
        } else {
            return false;
        }
        if (high < 0) {
            high = value;
        } else {
            out.push_back(static_cast<uint8_t>((high << 4) | value));
            high = -1;
        }
    }
    return high < 0;
}

std::string base64_encode(const uint8_t* data, size_t size) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const uint32_t value = (static_cast<uint32_t>(data[i]) << 16) |
                               (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kAlphabet[(value >> 18) & 0x3F]);
        out.push_back(kAlphabet[(value >> 12) & 0x3F]);
        out.push_back(kAlphabet[(value >> 6) & 0x3F]);
        out.push_back(kAlphabet[value & 0x3F]);
    }
    if (i < size) {
        const uint32_t value = static_cast<uint32_t>(data[i]) << 16 |
                               (i + 1 < size ? static_cast<uint32_t>(data[i + 1]) << 8 : 0);
        out.push_back(kAlphabet[(value >> 18) & 0x3F]);
        out.push_back(kAlphabet[(value >> 12) & 0x3F]);
        out.push_back(i + 1 < size ? kAlphabet[(value >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::string base64_encode(const std::vector<uint8_t>& data) {
    return base64_encode(data.data(), data.size());
}

bool base64_decode(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t buffer = 0;
    int bits = 0;
    bool padding = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            continue;
        }
        if (c == '=') {
            padding = true;
            continue;
        }
        if (padding) {
            return false;
        }
        const int value = base64_value(c);
        if (value < 0) {
            return false;
        }
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    if (bits > 0 && (buffer & ((1u << bits) - 1)) != 0) {
        return false;
    }
    return true;
}

bool constant_time_equal(const void* a, const void* b, size_t size) {
    const uint8_t* left = static_cast<const uint8_t*>(a);
    const uint8_t* right = static_cast<const uint8_t*>(b);
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) {
        difference |= static_cast<uint8_t>(left[i] ^ right[i]);
    }
    return difference == 0;
}

// ----- password hashing -----

std::string hash_password_with_salt(const std::string& password,
                                    const std::vector<uint8_t>& salt,
                                    uint32_t iterations) {
    if (salt.empty() || iterations == 0 || iterations > kMaxPasswordIterations) {
        return {};
    }
    std::vector<uint8_t> digest(kPasswordHashBytes);
    if (!pbkdf2_sha256(password, salt.data(), salt.size(), iterations, digest.data(),
                       digest.size())) {
        return {};
    }
    return "pbkdf2-sha256$" + std::to_string(iterations) + "$" + hex_encode(salt) + "$" +
           hex_encode(digest);
}

std::string hash_password(const std::string& password) {
    std::vector<uint8_t> salt(kPasswordSaltBytes);
    if (!secure_random(salt.data(), salt.size())) {
        return {};
    }
    return hash_password_with_salt(password, salt, kPasswordHashIterations);
}

bool verify_password(const std::string& password, const std::string& encoded) {
    constexpr std::string_view kPrefix = "pbkdf2-sha256$";
    if (encoded.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    const size_t iterations_begin = kPrefix.size();
    const size_t iterations_end = encoded.find('$', iterations_begin);
    if (iterations_end == std::string::npos) {
        return false;
    }
    const size_t salt_end = encoded.find('$', iterations_end + 1);
    if (salt_end == std::string::npos) {
        return false;
    }
    if (encoded.find('$', salt_end + 1) != std::string::npos) {
        return false;
    }
    uint32_t iterations = 0;
    const auto parsed = std::from_chars(encoded.data() + iterations_begin,
                                        encoded.data() + iterations_end, iterations);
    if (parsed.ec != std::errc() || parsed.ptr != encoded.data() + iterations_end ||
        iterations == 0 || iterations > kMaxPasswordIterations) {
        return false;
    }
    std::vector<uint8_t> salt;
    std::vector<uint8_t> expected;
    if (!hex_decode(encoded.substr(iterations_end + 1, salt_end - iterations_end - 1),
                    salt) ||
        !hex_decode(encoded.substr(salt_end + 1), expected)) {
        return false;
    }
    if (salt.empty() || expected.size() < 16 || expected.size() > 64) {
        return false;
    }
    std::vector<uint8_t> actual(expected.size());
    if (!pbkdf2_sha256(password, salt.data(), salt.size(), iterations, actual.data(),
                       actual.size())) {
        return false;
    }
    const bool equal = constant_time_equal(actual.data(), expected.data(), actual.size());
    wipe(actual.data(), actual.size());
    return equal;
}

// ----- TOTP -----

std::string generate_totp_secret() {
    std::array<uint8_t, 20> key{};
    if (!secure_random(key.data(), key.size())) {
        return {};
    }
    return base32_encode(key.data(), key.size());
}

bool parse_totp_secret(const std::string& base32, std::vector<uint8_t>& key) {
    return base32_decode(base32, key);
}

uint32_t totp_code(const std::vector<uint8_t>& key, uint64_t unix_seconds,
                   uint32_t step_seconds, uint32_t digits) {
    if (key.empty() || step_seconds == 0 || digits == 0 || digits > 9) {
        return 0;
    }
    std::array<uint8_t, 8> counter{};
    uint64_t value = unix_seconds / step_seconds;
    for (size_t i = counter.size(); i > 0; --i) {
        counter[i - 1] = static_cast<uint8_t>(value & 0xFF);
        value >>= 8;
    }
    std::array<uint8_t, 20> mac{};
    if (!hmac_sha1(key, counter.data(), counter.size(), mac)) {
        return 0;
    }
    const size_t offset = mac[19] & 0x0F;
    const uint32_t binary = (static_cast<uint32_t>(mac[offset] & 0x7F) << 24) |
                            (static_cast<uint32_t>(mac[offset + 1]) << 16) |
                            (static_cast<uint32_t>(mac[offset + 2]) << 8) |
                            static_cast<uint32_t>(mac[offset + 3]);
    uint32_t modulus = 1;
    for (uint32_t i = 0; i < digits; ++i) {
        modulus *= 10;
    }
    return binary % modulus;
}

bool verify_totp(const std::string& base32_secret, const std::string& code,
                 uint64_t unix_seconds, int window) {
    std::vector<uint8_t> key;
    if (!parse_totp_secret(base32_secret, key)) {
        return false;
    }
    std::string digits;
    for (char c : code) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        digits.push_back(c);
    }
    if (digits.size() != 6) {
        return false;
    }
    const int span = std::clamp(window, 0, 10);
    constexpr int64_t kStep = 30;
    for (int offset = -span; offset <= span; ++offset) {
        const int64_t moment =
            static_cast<int64_t>(unix_seconds) + static_cast<int64_t>(offset) * kStep;
        if (moment < 0) {
            continue;
        }
        const uint32_t expected =
            totp_code(key, static_cast<uint64_t>(moment), 30, 6);
        char text[8];
        std::snprintf(text, sizeof(text), "%06u", expected);
        if (constant_time_equal(digits.data(), text, 6)) {
            return true;
        }
    }
    return false;
}

std::string totp_uri(const std::string& secret_base32, const std::string& issuer,
                     const std::string& account) {
    const std::string label =
        issuer.empty() ? account : issuer + ":" + account;
    std::string uri = "otpauth://totp/" + url_encode_label(label) + "?secret=" +
                      url_encode(secret_base32) + "&algorithm=SHA1&digits=6&period=30";
    if (!issuer.empty()) {
        uri += "&issuer=" + url_encode(issuer);
    }
    return uri;
}

// ----- DPAPI -----

bool protect_secret(const std::string& plaintext, SecretScope scope,
                    const std::string& entropy, std::string& protected_base64,
                    std::string* error) {
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());
    DATA_BLOB entropy_blob{};
    entropy_blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(entropy.data()));
    entropy_blob.cbData = static_cast<DWORD>(entropy.size());
    DATA_BLOB output{};
    DWORD flags = CRYPTPROTECT_UI_FORBIDDEN;
    if (scope == SecretScope::LocalMachine) {
        flags |= CRYPTPROTECT_LOCAL_MACHINE;
    }
    if (!CryptProtectData(&input, nullptr, entropy.empty() ? nullptr : &entropy_blob,
                          nullptr, nullptr, flags, &output)) {
        set_error(error, win32_error("CryptProtectData"));
        return false;
    }
    protected_base64 = base64_encode(output.pbData, output.cbData);
    wipe(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}

bool unprotect_secret(const std::string& protected_base64, SecretScope scope,
                      const std::string& entropy, std::string& plaintext,
                      std::string* error) {
    // The blob records the scope it was protected with, so the flag is only
    // meaningful on the protect side.
    (void)scope;
    std::vector<uint8_t> blob;
    if (!base64_decode(protected_base64, blob) || blob.empty()) {
        set_error(error, "protected secret is not valid base64");
        return false;
    }
    DATA_BLOB input{};
    input.pbData = blob.data();
    input.cbData = static_cast<DWORD>(blob.size());
    DATA_BLOB entropy_blob{};
    entropy_blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(entropy.data()));
    entropy_blob.cbData = static_cast<DWORD>(entropy.size());
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, entropy.empty() ? nullptr : &entropy_blob,
                            nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        set_error(error, win32_error("CryptUnprotectData"));
        return false;
    }
    plaintext.assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    wipe(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}

// ----- device identity -----

struct DeviceIdentity::Impl {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    std::vector<uint8_t> public_blob;
    std::vector<uint8_t> private_blob;

    ~Impl() {
        if (key) {
            BCryptDestroyKey(key);
        }
        if (algorithm) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        wipe(private_blob.data(), private_blob.size());
    }
};

DeviceIdentity::DeviceIdentity() : impl_(std::make_unique<Impl>()) {}

DeviceIdentity::~DeviceIdentity() = default;

std::unique_ptr<DeviceIdentity> DeviceIdentity::generate(std::string* error) {
    std::unique_ptr<DeviceIdentity> identity(new DeviceIdentity());
    Impl& impl = *identity->impl_;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&impl.algorithm,
                                                  BCRYPT_ECDSA_P256_ALGORITHM,
                                                  nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        set_error(error, nt_error("BCryptOpenAlgorithmProvider", status));
        return nullptr;
    }
    status = BCryptGenerateKeyPair(impl.algorithm, &impl.key, 256, 0);
    if (!BCRYPT_SUCCESS(status)) {
        set_error(error, nt_error("BCryptGenerateKeyPair", status));
        return nullptr;
    }
    status = BCryptFinalizeKeyPair(impl.key, 0);
    if (!BCRYPT_SUCCESS(status)) {
        set_error(error, nt_error("BCryptFinalizeKeyPair", status));
        return nullptr;
    }
    if (!export_key(impl.key, BCRYPT_ECCPUBLIC_BLOB, impl.public_blob, error) ||
        !export_key(impl.key, BCRYPT_ECCPRIVATE_BLOB, impl.private_blob, error)) {
        return nullptr;
    }
    if (!valid_public_blob(impl.public_blob) || !valid_private_blob(impl.private_blob)) {
        set_error(error, "the device key has an unexpected shape");
        return nullptr;
    }
    return identity;
}

std::unique_ptr<DeviceIdentity> DeviceIdentity::load(const std::string& path,
                                                     std::string* error) {
    std::string contents;
    if (!read_file(path, contents, error)) {
        return nullptr;
    }
    const size_t newline = contents.find('\n');
    if (newline == std::string::npos) {
        set_error(error, path + " is not a device key file");
        return nullptr;
    }
    std::string header = contents.substr(0, newline);
    if (!header.empty() && header.back() == '\r') {
        header.pop_back();
    }
    if (header != kDeviceFileHeader) {
        set_error(error, path + " is not a device key file");
        return nullptr;
    }
    const std::string payload = trim(contents.substr(newline + 1));
    std::string plain;
    if (!unprotect_secret(payload, SecretScope::CurrentUser, kDeviceEntropy, plain,
                          error)) {
        return nullptr;
    }
    std::vector<uint8_t> private_blob(plain.begin(), plain.end());
    wipe(plain.data(), plain.size());
    if (!valid_private_blob(private_blob)) {
        set_error(error, path + " holds an invalid device key");
        return nullptr;
    }
    std::unique_ptr<DeviceIdentity> identity(new DeviceIdentity());
    Impl& impl = *identity->impl_;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&impl.algorithm,
                                                  BCRYPT_ECDSA_P256_ALGORITHM,
                                                  nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        set_error(error, nt_error("BCryptOpenAlgorithmProvider", status));
        return nullptr;
    }
    status = BCryptImportKeyPair(impl.algorithm, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                 &impl.key, private_blob.data(),
                                 static_cast<ULONG>(private_blob.size()), 0);
    if (!BCRYPT_SUCCESS(status)) {
        set_error(error, nt_error("BCryptImportKeyPair", status));
        return nullptr;
    }
    if (!export_key(impl.key, BCRYPT_ECCPUBLIC_BLOB, impl.public_blob, error)) {
        return nullptr;
    }
    impl.private_blob = std::move(private_blob);
    return identity;
}

std::unique_ptr<DeviceIdentity> DeviceIdentity::load_or_create(const std::string& path,
                                                               std::string* error) {
    if (file_exists(path)) {
        return load(path, error);
    }
    std::unique_ptr<DeviceIdentity> identity = generate(error);
    if (!identity) {
        return nullptr;
    }
    if (!identity->save(path, error)) {
        return nullptr;
    }
    return identity;
}

bool DeviceIdentity::save(const std::string& path, std::string* error) const {
    if (!impl_ || impl_->private_blob.empty()) {
        set_error(error, "the device key is not initialized");
        return false;
    }
    const std::string private_blob(
        reinterpret_cast<const char*>(impl_->private_blob.data()),
        impl_->private_blob.size());
    std::string protected_blob;
    if (!protect_secret(private_blob, SecretScope::CurrentUser, kDeviceEntropy,
                        protected_blob, error)) {
        return false;
    }
    const std::string contents =
        std::string(kDeviceFileHeader) + "\n" + protected_blob + "\n";
    const std::wstring wide_path = widen_utf8(path);
    if (wide_path.empty()) {
        set_error(error, "invalid device key path");
        return false;
    }
    const std::wstring temporary_path = wide_path + L".tmp";
    {
        std::ofstream file(std::filesystem::path(temporary_path),
                           std::ios::binary | std::ios::trunc);
        if (!file) {
            set_error(error, "cannot open " + path + " for writing");
            return false;
        }
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        file.flush();
        if (!file) {
            DeleteFileW(temporary_path.c_str());
            set_error(error, "cannot write " + path);
            return false;
        }
    }
    if (!MoveFileExW(temporary_path.c_str(), wide_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        set_error(error, win32_error("MoveFileExW"));
        DeleteFileW(temporary_path.c_str());
        return false;
    }
    return true;
}

const std::vector<uint8_t>& DeviceIdentity::public_key() const {
    return impl_->public_blob;
}

std::vector<uint8_t> DeviceIdentity::private_key_blob() const {
    return impl_->private_blob;
}

bool DeviceIdentity::sign(const uint8_t* data, size_t size,
                          std::vector<uint8_t>& signature, std::string* error) const {
    signature.clear();
    if (!impl_ || !impl_->key) {
        set_error(error, "the device key is not initialized");
        return false;
    }
    std::array<uint8_t, 32> digest{};
    if (!sha256(data, size, digest)) {
        set_error(error, "SHA-256 failed");
        return false;
    }
    ULONG length = 0;
    NTSTATUS status = BCryptSignHash(impl_->key, nullptr, digest.data(),
                                     static_cast<ULONG>(digest.size()), nullptr, 0,
                                     &length, 0);
    if (!BCRYPT_SUCCESS(status) || length != kP256SignatureBytes) {
        set_error(error, nt_error("BCryptSignHash", status));
        return false;
    }
    signature.resize(length);
    status = BCryptSignHash(impl_->key, nullptr, digest.data(),
                            static_cast<ULONG>(digest.size()), signature.data(), length,
                            &length, 0);
    if (!BCRYPT_SUCCESS(status)) {
        signature.clear();
        set_error(error, nt_error("BCryptSignHash", status));
        return false;
    }
    signature.resize(length);
    return true;
}

bool DeviceIdentity::sign(const std::vector<uint8_t>& message,
                          std::vector<uint8_t>& signature, std::string* error) const {
    return sign(message.data(), message.size(), signature, error);
}

std::string DeviceIdentity::name() const {
    return computer_name();
}

bool DeviceIdentity::verify(const std::vector<uint8_t>& public_key, const uint8_t* data,
                            size_t size, const std::vector<uint8_t>& signature) {
    if (!valid_public_blob(public_key) || signature.size() != kP256SignatureBytes) {
        return false;
    }
    std::array<uint8_t, 32> digest{};
    if (!sha256(data, size, digest)) {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM,
                                                   nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptImportKeyPair(
            algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
            const_cast<PUCHAR>(public_key.data()),
            static_cast<ULONG>(public_key.size()), 0))) {
        const NTSTATUS status = BCryptVerifySignature(
            key, nullptr, digest.data(), static_cast<ULONG>(digest.size()),
            const_cast<PUCHAR>(signature.data()),
            static_cast<ULONG>(signature.size()), 0);
        ok = BCRYPT_SUCCESS(status);
    }
    if (key) {
        BCryptDestroyKey(key);
    }
    if (algorithm) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return ok;
}

// ----- IPv4 CIDR -----

bool parse_ipv4(const std::string& text, uint32_t& address) {
    const std::string value = trim(text);
    uint32_t result = 0;
    int parts = 0;
    size_t position = 0;
    while (position <= value.size()) {
        const size_t end = value.find('.', position);
        const size_t stop = end == std::string::npos ? value.size() : end;
        if (stop == position || stop - position > 3 || parts >= 4) {
            return false;
        }
        uint32_t octet = 0;
        for (size_t i = position; i < stop; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(value[i]))) {
                return false;
            }
            octet = octet * 10 + static_cast<uint32_t>(value[i] - '0');
        }
        if (octet > 255) {
            return false;
        }
        result = (result << 8) | octet;
        ++parts;
        if (end == std::string::npos) {
            break;
        }
        position = end + 1;
    }
    if (parts != 4) {
        return false;
    }
    address = result;
    return true;
}

bool parse_ipv4_cidr(const std::string& text, Ipv4Range& range) {
    const std::string value = trim(text);
    const size_t slash = value.find('/');
    const std::string address_text =
        slash == std::string::npos ? value : value.substr(0, slash);
    uint32_t address = 0;
    if (!parse_ipv4(address_text, address)) {
        return false;
    }
    uint8_t prefix = 32;
    if (slash != std::string::npos) {
        const std::string prefix_text = trim(value.substr(slash + 1));
        if (prefix_text.empty() || prefix_text.size() > 2) {
            return false;
        }
        uint32_t parsed = 0;
        for (char c : prefix_text) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                return false;
            }
            parsed = parsed * 10 + static_cast<uint32_t>(c - '0');
        }
        if (parsed > 32) {
            return false;
        }
        prefix = static_cast<uint8_t>(parsed);
    }
    range.address = address;
    range.prefix = prefix;
    return true;
}

bool ipv4_in_range(uint32_t address, const Ipv4Range& range) {
    if (range.prefix == 0) {
        return true;
    }
    if (range.prefix > 32) {
        return false;
    }
    const uint32_t mask = 0xFFFFFFFFu << (32 - range.prefix);
    return (address & mask) == (range.address & mask);
}

bool ipv4_in_ranges(uint32_t address, const std::vector<Ipv4Range>& ranges) {
    for (const Ipv4Range& range : ranges) {
        if (ipv4_in_range(address, range)) {
            return true;
        }
    }
    return false;
}

}  // namespace chast
