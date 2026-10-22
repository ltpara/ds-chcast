#include "chast/host_security.h"

#include <algorithm>
#include <string_view>

#include "chast/identity.h"

namespace chast {

namespace {

constexpr size_t kShortWindowSeconds = 60;
constexpr size_t kShortWindowLimit = 6;
constexpr size_t kLongWindowSeconds = 600;
constexpr size_t kLongWindowLimit = 30;
constexpr int64_t kBackoffBaseSeconds = 30;
constexpr int64_t kBackoffMaxSeconds = 300;
constexpr uint32_t kTempPasswordFailureLimit = 10;

std::string trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (begin < end && space(text[begin])) {
        ++begin;
    }
    while (end > begin && space(text[end - 1])) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string denial_message(AccessDenial denial) {
    switch (denial) {
        case AccessDenial::InvalidToken:
            return "the access token is not valid";
        case AccessDenial::TokenUsed:
            return "the access token was already used";
        case AccessDenial::Declined:
            return "the host declined the connection";
        case AccessDenial::WrongPassword:
            return "incorrect password";
        case AccessDenial::PasswordRequired:
            return "a password is required";
        case AccessDenial::TwoFactorRequired:
            return "two-factor authentication is required; reconnect with a code";
        case AccessDenial::WrongTwoFactor:
            return "incorrect two-factor code";
        case AccessDenial::LockedOut:
            return "too many failed attempts";
        case AccessDenial::NotWhitelisted:
            return "this address is not on the host's allow list";
        case AccessDenial::AuthTimeout:
            return "authentication timed out";
    }
    return "connection refused";
}

HostSecurity::Result deny(AccessDenial code, std::string reason = {}) {
    HostSecurity::Result result;
    result.decision = HostSecurity::Decision::Deny;
    result.denial = code;
    result.reason = reason.empty() ? denial_message(code) : std::move(reason);
    return result;
}

bool has_prefix(const std::string& text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool device_expired(const TrustedDevice& device, int64_t now_unix) {
    const int64_t last = device.last_seen_unix_seconds != 0 ? device.last_seen_unix_seconds
                                                            : device.added_unix_seconds;
    if (last <= 0) {
        return true;
    }
    if (now_unix <= last) {
        return false;
    }
    return now_unix - last > kTrustedDeviceLifetimeSeconds;
}

}  // namespace

bool parse_ip_whitelist(const std::string& text, std::vector<Ipv4Range>& ranges,
                        std::string* error) {
    ranges.clear();
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t comma = text.find(',', begin);
        const std::string entry =
            trim(std::string_view(text).substr(begin, comma == std::string::npos
                                                         ? std::string::npos
                                                         : comma - begin));
        if (!entry.empty()) {
            Ipv4Range range;
            if (!parse_ipv4_cidr(entry, range)) {
                if (error != nullptr) {
                    *error = "'" + entry + "' is not an IPv4 address or CIDR";
                }
                return false;
            }
            ranges.push_back(range);
        }
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return true;
}

HostSecurity::HostSecurity() { set_config(Config{}); }

HostSecurity::HostSecurity(Config config) { set_config(std::move(config)); }

void HostSecurity::set_config(Config config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (config.service_mode) {
        config.mode = HostAuthMode::Password;
    }
    config.default_permissions &= kPermissionAll;
    if (!config.host_fingerprint.empty()) {
        config.host_fingerprint = normalize_fingerprint(config.host_fingerprint);
    }
    if (config.trusted_devices.size() > kMaxTrustedDevices) {
        config.trusted_devices.resize(kMaxTrustedDevices);
    }

    whitelist_.clear();
    whitelist_configured_ = !trim(config.ip_whitelist).empty();
    whitelist_valid_ = true;
    if (whitelist_configured_) {
        std::string error;
        if (!parse_ip_whitelist(config.ip_whitelist, whitelist_, &error)) {
            whitelist_valid_ = false;
            whitelist_.clear();
        }
    }

    totp_valid_ = true;
    if (!config.totp_secret.empty()) {
        std::vector<uint8_t> key;
        totp_valid_ = parse_totp_secret(config.totp_secret, key);
    }

    config_ = std::move(config);
    const bool wants_temporary =
        !config_.service_mode && config_.mode != HostAuthMode::Click;
    if (!wants_temporary) {
        temporary_password_.clear();
        temporary_failures_ = 0;
    } else if (temporary_password_.empty()) {
        temporary_password_ = random_digits(6);
    }
}

HostSecurity::Config HostSecurity::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

std::string HostSecurity::address_key(const std::string& peer_address, std::string& ip) {
    ip.clear();
    if (peer_address.empty()) {
        return "unknown";
    }
    std::string host = peer_address;
    if (host.front() == '[') {
        const size_t close = host.find(']');
        if (close != std::string::npos) {
            host = host.substr(1, close - 1);
        }
    } else {
        const size_t colon = host.rfind(':');
        if (colon != std::string::npos && host.find(':') == colon) {
            host = host.substr(0, colon);
        }
    }
    if (host.empty()) {
        return "unknown";
    }
    ip = host;
    return host;
}

bool HostSecurity::password_hash_usable(const std::string& hash) {
    return !hash.empty() && hash.size() <= 512 && has_prefix(hash, "pbkdf2-sha256$");
}

void HostSecurity::fail_locked(const std::string& key, int64_t now_unix) {
    AddressFailures& state = failures_[key];
    const int64_t long_cutoff = now_unix - static_cast<int64_t>(kLongWindowSeconds);
    state.failures.erase(
        std::remove_if(state.failures.begin(), state.failures.end(),
                       [long_cutoff](int64_t stamp) { return stamp < long_cutoff; }),
        state.failures.end());
    state.failures.push_back(now_unix);

    const int64_t short_cutoff = now_unix - static_cast<int64_t>(kShortWindowSeconds);
    size_t recent = 0;
    for (auto it = state.failures.rbegin(); it != state.failures.rend() && *it >= short_cutoff;
         ++it) {
        ++recent;
    }
    if (recent >= kShortWindowLimit || state.failures.size() >= kLongWindowLimit) {
        const uint32_t shift = std::min<uint32_t>(state.block_count, 4);
        int64_t backoff = kBackoffBaseSeconds << shift;
        backoff = (std::min)(backoff, kBackoffMaxSeconds);
        state.blocked_until = now_unix + backoff;
        ++state.block_count;
    }
}

void HostSecurity::prune_nonces_locked(int64_t now_unix) {
    const int64_t cutoff = now_unix - kHelloNonceLifetimeSeconds;
    nonces_.erase(std::remove_if(nonces_.begin(), nonces_.end(),
                                 [cutoff](const auto& entry) { return entry.second < cutoff; }),
                  nonces_.end());
}

HostSecurity::Result HostSecurity::evaluate(const ClientHello& hello,
                                            const std::string& peer_address,
                                            int64_t now_unix) {
    std::lock_guard<std::mutex> lock(mutex_);
    Result result;
    result.peer_address = peer_address;
    const std::string key = address_key(peer_address, result.peer_ip);
    result.device_name = hello.device_name;
    result.device_platform = hello.device_platform;
    result.device_key = hello.device_key;
    result.permissions =
        config_.default_permissions & (hello.requested_permissions & kPermissionAll);

    if (whitelist_configured_) {
        uint32_t address = 0;
        if (!whitelist_valid_ ||
            !parse_ipv4(result.peer_ip, address) ||
            !ipv4_in_ranges(address, whitelist_)) {
            result = deny(AccessDenial::NotWhitelisted);
            result.peer_address = peer_address;
            result.peer_ip = address_key(peer_address, result.peer_ip);
            return result;
        }
    }

    // Service mode has nobody to ask and no temporary password: without a
    // permanent password it cannot let anyone in, not even with a token.
    if (config_.service_mode && config_.password_hash.empty()) {
        result = deny(AccessDenial::PasswordRequired,
                      "the service has no permanent password; set one before connecting");
        result.peer_address = peer_address;
        result.peer_ip = address_key(peer_address, result.peer_ip);
        return result;
    }

    {
        const auto it = failures_.find(key);
        if (it != failures_.end() && it->second.blocked_until > now_unix) {
            const int64_t remaining = it->second.blocked_until - now_unix;
            result = deny(AccessDenial::LockedOut,
                          "too many failed attempts; try again in " +
                              std::to_string(remaining) + " s");
            result.peer_address = peer_address;
            result.peer_ip = address_key(peer_address, result.peer_ip);
            return result;
        }
    }

    // The access token is an alternative credential for CLI/automation. A
    // client that does not carry one continues with the password flow.
    if (config_.token_auth && !hello.access_token.empty()) {
        const bool match =
            !config_.access_token.empty() &&
            config_.access_token.size() == hello.access_token.size() &&
            constant_time_equal(config_.access_token.data(), hello.access_token.data(),
                                config_.access_token.size());
        if (match) {
            result.decision = Decision::Allow;
            result.via_token = true;
            return result;
        }
        fail_locked(key, now_unix);
        result = deny(AccessDenial::InvalidToken);
        result.peer_address = peer_address;
        result.peer_ip = address_key(peer_address, result.peer_ip);
        result.permissions =
            config_.default_permissions & (hello.requested_permissions & kPermissionAll);
        return result;
    }

    // A signed Hello proves possession of the device key. It is checked
    // whenever the key is already trusted (to refuse stale or replayed proofs)
    // and whenever the client asks to be trusted. An unsigned or invalid
    // proof from an unknown device simply does not grant anything.
    const bool signature_flag = (hello.auth_flags & kAuthFlagSigned) != 0;
    const bool trust_request = (hello.auth_flags & kAuthFlagTrustDevice) != 0;
    bool signature_valid = false;
    if (signature_flag) {
        signature_valid = !hello.device_key.empty() && !hello.signature.empty() &&
                          !config_.host_fingerprint.empty() && hello.unix_seconds != 0;
        const int64_t skew = static_cast<int64_t>(hello.unix_seconds) - now_unix;
        if (skew > kHelloClockSkewSeconds || skew < -kHelloClockSkewSeconds) {
            signature_valid = false;
        }
        if (signature_valid) {
            prune_nonces_locked(now_unix);
            for (const auto& entry : nonces_) {
                if (entry.first == hello.nonce) {
                    signature_valid = false;
                    break;
                }
            }
        }
        if (signature_valid && nonces_.size() >= kMaxRememberedNonces) {
            signature_valid = false;
        }
        if (signature_valid) {
            const std::vector<uint8_t> message =
                hello_signature_message(config_.host_fingerprint, hello);
            signature_valid = DeviceIdentity::verify(hello.device_key, message.data(),
                                                     message.size(), hello.signature);
        }
        if (signature_valid) {
            nonces_.emplace_back(hello.nonce, now_unix);
        }
    }

    size_t trusted_index = config_.trusted_devices.size();
    if (!hello.device_key.empty()) {
        for (size_t i = 0; i < config_.trusted_devices.size(); ++i) {
            if (config_.trusted_devices[i].public_key == hello.device_key) {
                trusted_index = i;
                break;
            }
        }
    }
    const bool trusted_active =
        trusted_index < config_.trusted_devices.size() &&
        !device_expired(config_.trusted_devices[trusted_index], now_unix);
    if (signature_flag && !signature_valid && (trusted_active || trust_request)) {
        result = deny(AccessDenial::Declined,
                      trusted_active ? "the trusted device proof is not valid"
                                     : "the device proof could not be verified");
        result.peer_address = peer_address;
        result.peer_ip = address_key(peer_address, result.peer_ip);
        result.permissions =
            config_.default_permissions & (hello.requested_permissions & kPermissionAll);
        result.device_key = hello.device_key;
        return result;
    }
    result.request_trust = trust_request && signature_valid;

    if (config_.mode == HostAuthMode::Click) {
        result.decision = Decision::RequireApproval;
        result.permissions =
            config_.default_permissions & (hello.requested_permissions & kPermissionAll);
        return result;
    }

    if (hello.password.empty()) {
        if (config_.mode == HostAuthMode::Password) {
            result = deny(AccessDenial::PasswordRequired);
            result.peer_address = peer_address;
            result.peer_ip = address_key(peer_address, result.peer_ip);
            result.permissions =
                config_.default_permissions & (hello.requested_permissions & kPermissionAll);
            return result;
        }
        result.decision = Decision::RequireApproval;
        result.permissions =
            config_.default_permissions & (hello.requested_permissions & kPermissionAll);
        return result;
    }

    const bool hash_present = !config_.password_hash.empty();
    const bool hash_usable = password_hash_usable(config_.password_hash);
    bool permanent_ok = false;
    if (hash_present && hash_usable) {
        permanent_ok = verify_password(hello.password, config_.password_hash);
    }
    bool temporary_ok = false;
    if (!temporary_password_.empty() &&
        hello.password.size() == temporary_password_.size()) {
        temporary_ok = constant_time_equal(hello.password.data(), temporary_password_.data(),
                                           temporary_password_.size());
    }
    if (!permanent_ok && !temporary_ok) {
        result = deny(AccessDenial::WrongPassword);
        result.peer_address = peer_address;
        result.peer_ip = address_key(peer_address, result.peer_ip);
        result.permissions =
            config_.default_permissions & (hello.requested_permissions & kPermissionAll);
        if (hash_present && !hash_usable) {
            result = deny(AccessDenial::Declined,
                          "the host's password configuration is invalid");
            result.peer_address = peer_address;
            result.peer_ip = address_key(peer_address, result.peer_ip);
            result.permissions =
                config_.default_permissions & (hello.requested_permissions & kPermissionAll);
            return result;
        }
        fail_locked(key, now_unix);
        if (!config_.service_mode && !temporary_password_.empty()) {
            ++temporary_failures_;
            if (temporary_failures_ >= kTempPasswordFailureLimit) {
                temporary_password_ = random_digits(6);
                temporary_failures_ = 0;
            }
        }
        return result;
    }
    result.used_temporary_password = temporary_ok && !permanent_ok;

    if (!config_.totp_secret.empty()) {
        if (!totp_valid_) {
            result = deny(AccessDenial::Declined,
                          "the host's two-factor configuration is invalid");
            result.peer_address = peer_address;
            result.peer_ip = address_key(peer_address, result.peer_ip);
            result.permissions =
                config_.default_permissions & (hello.requested_permissions & kPermissionAll);
            return result;
        }
        const bool skip = trusted_active && signature_valid;
        if (skip) {
            result.via_trusted_device = true;
        } else if (hello.totp.empty()) {
            result = deny(AccessDenial::TwoFactorRequired);
            result.peer_address = peer_address;
            result.peer_ip = address_key(peer_address, result.peer_ip);
            result.permissions =
                config_.default_permissions & (hello.requested_permissions & kPermissionAll);
            return result;
        } else if (!verify_totp(config_.totp_secret, hello.totp, now_unix, 1)) {
            result = deny(AccessDenial::WrongTwoFactor);
            result.peer_address = peer_address;
            result.peer_ip = address_key(peer_address, result.peer_ip);
            result.permissions =
                config_.default_permissions & (hello.requested_permissions & kPermissionAll);
            fail_locked(key, now_unix);
            if (!config_.service_mode && !temporary_password_.empty()) {
                ++temporary_failures_;
                if (temporary_failures_ >= kTempPasswordFailureLimit) {
                    temporary_password_ = random_digits(6);
                    temporary_failures_ = 0;
                }
            }
            return result;
        }
    } else if (trusted_active && signature_valid) {
        result.via_trusted_device = true;
    }

    result.decision = Decision::Allow;
    return result;
}

void HostSecurity::note_connected(const Result& result, int64_t now_unix) {
    if (result.decision == Decision::Deny) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::string ip;
    failures_.erase(address_key(result.peer_address, ip));
    temporary_failures_ = 0;
    if (result.used_temporary_password) {
        temporary_password_ = random_digits(6);
    }

    if (result.device_key.empty()) {
        return;
    }
    TrustedDevice* existing = nullptr;
    for (TrustedDevice& device : config_.trusted_devices) {
        if (device.public_key == result.device_key) {
            existing = &device;
            break;
        }
    }
    if (existing != nullptr) {
        existing->last_seen_unix_seconds = now_unix;
        if (!result.device_name.empty()) {
            existing->name = result.device_name;
        }
        if (result.device_platform != 0) {
            existing->platform = result.device_platform;
        }
        return;
    }
    if (!result.request_trust) {
        return;
    }
    if (config_.trusted_devices.size() >= kMaxTrustedDevices) {
        auto oldest = config_.trusted_devices.begin();
        for (auto it = config_.trusted_devices.begin(); it != config_.trusted_devices.end();
             ++it) {
            if (it->last_seen_unix_seconds < oldest->last_seen_unix_seconds) {
                oldest = it;
            }
        }
        config_.trusted_devices.erase(oldest);
    }
    TrustedDevice device;
    device.public_key = result.device_key;
    device.name = result.device_name.empty() ? "unknown device" : result.device_name;
    device.platform = result.device_platform;
    device.added_unix_seconds = now_unix;
    device.last_seen_unix_seconds = now_unix;
    config_.trusted_devices.push_back(std::move(device));
}

bool HostSecurity::set_password(const std::string& password, std::string* error) {
    if (password.size() < kMinPermanentPasswordLength) {
        if (error != nullptr) {
            *error = "the password must be at least " +
                     std::to_string(kMinPermanentPasswordLength) + " characters";
        }
        return false;
    }
    if (password.size() > kMaxPasswordLength) {
        if (error != nullptr) {
            *error = "the password is longer than " +
                     std::to_string(kMaxPasswordLength) + " bytes";
        }
        return false;
    }
    const std::string encoded = hash_password(password);
    if (encoded.empty()) {
        if (error != nullptr) {
            *error = "the password could not be hashed";
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    config_.password_hash = encoded;
    config_.trusted_devices.clear();
    return true;
}

void HostSecurity::clear_password() {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.password_hash.clear();
}

bool HostSecurity::password_configured() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !config_.password_hash.empty();
}

std::string HostSecurity::password_hash() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.password_hash;
}

bool HostSecurity::set_totp_secret(const std::string& secret_base32, std::string* error) {
    if (secret_base32.empty()) {
        clear_totp_secret();
        return true;
    }
    std::vector<uint8_t> key;
    if (!parse_totp_secret(secret_base32, key)) {
        if (error != nullptr) {
            *error = "the TOTP secret is not valid base32";
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    config_.totp_secret = secret_base32;
    totp_valid_ = true;
    return true;
}

void HostSecurity::clear_totp_secret() {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.totp_secret.clear();
    totp_valid_ = true;
}

bool HostSecurity::two_factor_enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !config_.totp_secret.empty();
}

std::string HostSecurity::totp_secret() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.totp_secret;
}

bool HostSecurity::has_temporary_password() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !temporary_password_.empty();
}

std::string HostSecurity::temporary_password() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return temporary_password_;
}

std::string HostSecurity::refresh_temporary_password() {
    std::lock_guard<std::mutex> lock(mutex_);
    temporary_failures_ = 0;
    if (config_.service_mode || config_.mode == HostAuthMode::Click) {
        temporary_password_.clear();
        return {};
    }
    temporary_password_ = random_digits(6);
    return temporary_password_;
}

std::vector<TrustedDevice> HostSecurity::trusted_devices(int64_t now_unix) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TrustedDevice> devices;
    for (const TrustedDevice& device : config_.trusted_devices) {
        if (!device_expired(device, now_unix)) {
            devices.push_back(device);
        }
    }
    std::sort(devices.begin(), devices.end(), [](const TrustedDevice& a, const TrustedDevice& b) {
        return a.last_seen_unix_seconds > b.last_seen_unix_seconds;
    });
    return devices;
}

bool HostSecurity::revoke_trusted_device(const std::vector<uint8_t>& public_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find_if(config_.trusted_devices.begin(), config_.trusted_devices.end(),
                                 [&public_key](const TrustedDevice& device) {
                                     return device.public_key == public_key;
                                 });
    if (it == config_.trusted_devices.end()) {
        return false;
    }
    config_.trusted_devices.erase(it);
    return true;
}

void HostSecurity::clear_trusted_devices() {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.trusted_devices.clear();
}

bool HostSecurity::locked_out(const std::string& peer_address, int64_t now_unix,
                              uint32_t* retry_after_seconds) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string ip;
    const std::string key = address_key(peer_address, ip);
    const auto it = failures_.find(key);
    if (it == failures_.end() || it->second.blocked_until <= now_unix) {
        return false;
    }
    if (retry_after_seconds != nullptr) {
        *retry_after_seconds =
            static_cast<uint32_t>(it->second.blocked_until - now_unix);
    }
    return true;
}

}  // namespace chast
