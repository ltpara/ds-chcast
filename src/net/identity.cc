#include "chast/identity.h"

#include <cctype>

namespace chast {

namespace {

constexpr size_t kFingerprintDigits = 64;

char hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c;
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<char>(c - 'a' + 'A');
    }
    if (c >= 'A' && c <= 'F') {
        return c;
    }
    return 0;
}

}  // namespace

std::string normalize_fingerprint(std::string_view text) {
    std::string digits;
    digits.reserve(kFingerprintDigits);
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ':') {
            continue;
        }
        const char hex = hex_digit(c);
        if (hex == 0) {
            return {};
        }
        digits.push_back(hex);
    }
    if (digits.size() != kFingerprintDigits) {
        return {};
    }
    return digits;
}

std::string format_fingerprint(std::string_view hex) {
    const std::string canonical = normalize_fingerprint(hex);
    if (canonical.empty()) {
        return std::string(hex);
    }
    std::string formatted;
    formatted.reserve(canonical.size() + canonical.size() / 2 - 1);
    for (size_t i = 0; i < canonical.size(); ++i) {
        if (i > 0 && i % 2 == 0) {
            formatted.push_back(':');
        }
        formatted.push_back(canonical[i]);
    }
    return formatted;
}

bool fingerprint_matches(std::string_view pinned, std::string_view actual) {
    const std::string expected = normalize_fingerprint(pinned);
    if (expected.empty()) {
        return false;
    }
    return expected == normalize_fingerprint(actual);
}

}  // namespace chast
