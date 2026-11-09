#include "chast/host_security.h"

#include <algorithm>
#include <string_view>

#include "chast/host_settings.h"
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
// Upper bound on the address lockout table. A blocked peer retries, but an
// attacker rotating across a large address space must not grow the map without
// limit; idle entries are pruned and the oldest active one gives way.
constexpr size_t kMaxFailureEntries = 4096;

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

// A control-only session may request nothing beyond the one permission its
// mode serves: the host announces no picture, input or other control for it.
PermissionMask mode_permissions(SessionMode mode) {
    switch (mode) {
        case SessionMode::FileTransfer:
            return kPermissionFileTransfer;
        case SessionMode::Terminal:
            return kPermissionTerminal;
        case SessionMode::Camera:
            return kPermissionCamera;
        case SessionMode::PortForward:
            return kPermissionPortForward;
        case SessionMode::Desktop:
            break;
    }
    return kPermissionAll;
}

std::string mode_disabled_reason(SessionMode mode) {
    switch (mode) {
        case SessionMode::FileTransfer:
            return "file transfer is disabled on this host";
        case SessionMode::Terminal:
            return "remote terminal is disabled on this host";
        case SessionMode::Camera:
            return "camera streaming is disabled on this host";
        case SessionMode::PortForward:
            return "port forwarding is disabled on this host";
        case SessionMode::Desktop:
            break;
    }
    return {};
}

bool device_expired(const TrustedDevice& device, int64_t now_unix,
                    int64_t lifetime_seconds) {
    const int64_t last = device.last_seen_unix_seconds != 0 ? device.last_seen_unix_seconds
                                                            : device.added_unix_seconds;
    if (last <= 0) {
        return true;
    }
    if (lifetime_seconds <= 0) {
        return false;
    }
    if (now_unix <= last) {
        return false;
    }
    return now_unix - last > lifetime_seconds;
}

}  // namespace

bool parse_ip_whitelist(const std::string& text, std::vector<IpRange>& ranges,
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
            IpRange range;
            if (!parse_ip_cidr(entry, range)) {
                if (error != nullptr) {
                    *error = "'" + entry + "' is not an IP address or CIDR";
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

void HostSecurity::set_default_permissions(PermissionMask permissions) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.default_permissions = permissions & kPermissionAll;
}

void HostSecurity::set_config(Config config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (config.service_mode && !config.service_approval) {
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
    tickets_.reset();
    const bool primary_complete =
        !config_.access_kid.empty() && !config_.access_public_key.empty();
    const bool primary_partial =
        config_.access_kid.empty() != config_.access_public_key.empty();
    if (!config_.access_issuer.empty() &&
        (!config_.access_kid.empty() || !config_.access_public_key.empty() ||
         !config_.access_keys.empty())) {
        // A partial primary pin is a configuration error; leaving the verifier
        // unbuilt keeps managed hosts fail-closed instead of guessing. Extra
        // pins may stand alone, which is how a rotation starts before the
        // primary kid is switched.
        if (!config_.device_id.empty() && !config_.access_issuer.empty() &&
            !primary_partial && (primary_complete || !config_.access_keys.empty())) {
            auto verifier = std::make_unique<AccessTicketVerifier>(
                config_.access_issuer, "device:" + config_.device_id);
            std::string error;
            bool pinned = true;
            if (primary_complete) {
                pinned = verifier->pin_key(config_.access_kid,
                                           config_.access_public_key, &error);
            }
            for (const Config::AccessKeyPin& pin : config_.access_keys) {
                if (pinned) {
                    pinned = verifier->pin_key(pin.kid, pin.public_key, &error);
                }
            }
            if (pinned) {
                tickets_ = std::move(verifier);
            }
        }
    }
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

std::string peer_ip_text(const std::string& peer_address) {
    if (peer_address.empty()) {
        return {};
    }
    std::string host = peer_address;
    if (host.front() == '[') {
        const size_t close = host.find(']');
        if (close != std::string::npos) {
            return host.substr(1, close - 1);
        }
    }
    // A bare IPv6 literal has several colons and no port to strip.
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon) {
        return host.substr(0, colon);
    }
    return host;
}

std::vector<std::string> HostSecurity::lockout_keys(const std::string& ip) {
    IpAddress address;
    if (ip.empty() || !parse_ip_address(ip, address)) {
        return {ip.empty() ? "unknown" : ip};
    }
    if (!address.ipv6) {
        // A mapped IPv4 form folds here too, so both spellings share a bucket.
        return {format_ipv4((static_cast<uint32_t>(address.bytes[0]) << 24) |
                            (static_cast<uint32_t>(address.bytes[1]) << 16) |
                            (static_cast<uint32_t>(address.bytes[2]) << 8) |
                            static_cast<uint32_t>(address.bytes[3]))};
    }
    // A client picks its interface address inside the /64 its provider
    // delegates, so per-address accounting alone would never lock it out.
    return {format_ipv6_prefix(address.bytes, 64), format_ipv6_prefix(address.bytes, 56),
            format_ipv6_prefix(address.bytes, 48)};
}

bool HostSecurity::password_hash_usable(const std::string& hash) {
    return !hash.empty() && hash.size() <= 512 && has_prefix(hash, "pbkdf2-sha256$");
}

void HostSecurity::fail_locked(const std::vector<std::string>& keys, int64_t now_unix) {
    for (const std::string& key : keys) {
        if (failures_.find(key) == failures_.end() && failures_.size() >= kMaxFailureEntries) {
            prune_failures_locked(now_unix);
            while (failures_.size() >= kMaxFailureEntries) {
                evict_failure_locked();
            }
        }
        AddressFailures& state = failures_[key];
        const int64_t long_cutoff = now_unix - static_cast<int64_t>(kLongWindowSeconds);
        state.failures.erase(
            std::remove_if(state.failures.begin(), state.failures.end(),
                           [long_cutoff](int64_t stamp) { return stamp < long_cutoff; }),
            state.failures.end());
        state.failures.push_back(now_unix);

        const int64_t short_cutoff = now_unix - static_cast<int64_t>(kShortWindowSeconds);
        size_t recent = 0;
        for (auto it = state.failures.rbegin();
             it != state.failures.rend() && *it >= short_cutoff; ++it) {
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
}

void HostSecurity::prune_failures_locked(int64_t now_unix) {
    const int64_t cutoff = now_unix - static_cast<int64_t>(kLongWindowSeconds);
    for (auto it = failures_.begin(); it != failures_.end();) {
        AddressFailures& state = it->second;
        state.failures.erase(
            std::remove_if(state.failures.begin(), state.failures.end(),
                           [cutoff](int64_t stamp) { return stamp < cutoff; }),
            state.failures.end());
        if (state.failures.empty() && state.blocked_until <= now_unix) {
            it = failures_.erase(it);
        } else {
            ++it;
        }
    }
}

void HostSecurity::evict_failure_locked() {
    auto last_stamp = [](const AddressFailures& state) {
        return state.failures.empty() ? int64_t{0} : state.failures.back();
    };
    auto oldest = failures_.end();
    for (auto it = failures_.begin(); it != failures_.end(); ++it) {
        if (oldest == failures_.end() ||
            last_stamp(it->second) < last_stamp(oldest->second)) {
            oldest = it;
        }
    }
    if (oldest != failures_.end()) {
        failures_.erase(oldest);
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
                                            int64_t now_unix,
                                            const TicketRequest* ticket_request) {
    std::lock_guard<std::mutex> lock(mutex_);
    Result result;
    result.peer_address = peer_address;
    result.peer_ip = peer_ip_text(peer_address);
    const std::string& ip = result.peer_ip;
    const std::vector<std::string> keys = lockout_keys(ip);
    result.device_name = hello.device_name;
    result.device_platform = hello.device_platform;
    result.device_key = hello.device_key;
    // A control-only session may request nothing but the permission its mode
    // serves: the host will announce no picture, input or other control for
    // it. Clamping the request here keeps every later decision and the
    // approval window on the same mask.
    PermissionMask requested = hello.requested_permissions & mode_permissions(hello.mode);

    // A managed device only admits a verified access ticket, and the ticket is
    // an upper bound in addition to the local policy: permissions are the
    // intersection of what the ticket grants, what the device policy allows
    // and what this host and operator permit. The gate runs before every
    // credential check so a refusal neither confirms a password nor consumes a
    // failure slot.
    PermissionMask managed_mask = kPermissionAll;
    {
        const ManagedPolicyState marker = read_managed_policy_state(config_.policy_state);
        if (!marker.readable) {
            result = deny(AccessDenial::Declined,
                          "the managed policy state cannot be read; "
                          "refusing new sessions");
            result.peer_address = peer_address;
            result.peer_ip = ip;
            return result;
        }
        if (marker.managed) {
            if (!tickets_) {
                result = deny(AccessDenial::Declined,
                              "this device is centrally managed; no trusted "
                              "access-ticket key is configured");
                result.peer_address = peer_address;
                result.peer_ip = ip;
                return result;
            }
            if (ticket_request == nullptr) {
                result = deny(AccessDenial::Declined,
                              "this device is centrally managed; an access "
                              "ticket is required");
                result.peer_address = peer_address;
                result.peer_ip = ip;
                return result;
            }
            AccessTicket ticket;
            TicketError error =
                tickets_->inspect(ticket_request->signed_ticket, config_.device_id,
                                  config_.host_fingerprint, now_unix, &ticket);
            if (error != TicketError::None) {
                result = deny(AccessDenial::Declined,
                              std::string("this device is centrally managed; "
                                          "the access ticket was refused: ") +
                                  ticket_error_text(error));
                result.peer_address = peer_address;
                result.peer_ip = ip;
                return result;
            }
            // A ticket issued for an older policy may still carry a wider
            // ceiling. Renewal is a new ticket; an old one cannot ride in.
            if (ticket.policy_version != marker.policy_version) {
                result = deny(AccessDenial::Declined,
                              "the access ticket does not match the current "
                              "device policy version");
                result.peer_address = peer_address;
                result.peer_ip = ip;
                return result;
            }
            bool challenged = false;
            for (uint8_t byte : ticket_request->host_nonce) {
                if (byte != 0) {
                    challenged = true;
                    break;
                }
            }
            if (!challenged) {
                result = deny(AccessDenial::Declined,
                              "this device is centrally managed; the connection "
                              "carried no host challenge");
                result.peer_address = peer_address;
                result.peer_ip = ip;
                return result;
            }
            // Possession is checked before the id is consumed, so a broken or
            // replayed proof cannot burn a ticket the peer may still use.
            const std::vector<uint8_t> client_nonce(hello.nonce.begin(), hello.nonce.end());
            const std::vector<uint8_t> proof_message = access_ticket_proof_message(
                config_.device_id, config_.host_fingerprint, ticket_request->host_nonce,
                client_nonce, ticket_request->signed_ticket);
            if (!verify_access_ticket_proof(ticket, proof_message, ticket_request->proof)) {
                result = deny(AccessDenial::Declined,
                              "this device is centrally managed; the access-ticket "
                              "possession proof was refused");
                result.peer_address = peer_address;
                result.peer_ip = ip;
                return result;
            }
            error = tickets_->commit(ticket, now_unix);
            if (error != TicketError::None) {
                result = deny(AccessDenial::Declined,
                              std::string("this device is centrally managed; "
                                          "the access ticket was refused: ") +
                                  ticket_error_text(error));
                result.peer_address = peer_address;
                result.peer_ip = ip;
                return result;
            }
            managed_mask = ticket.permissions & marker.permission_ceiling;
            result.via_access_ticket = true;
            result.ticket_subject = ticket.subject;
            result.ticket_id = ticket.id;
            result.ticket_expires_at = ticket.expires_at;
            result.ticket_policy_version = ticket.policy_version;
            result.ticket_client_key = ticket.client_public_key;
        }
    }
    requested &= managed_mask;
    result.permissions = config_.default_permissions & requested;

    if (whitelist_configured_) {
        IpAddress address;
        if (!whitelist_valid_ || !parse_ip_address(ip, address) ||
            !ip_in_ranges(address, whitelist_)) {
            result = deny(AccessDenial::NotWhitelisted);
            result.peer_address = peer_address;
            result.peer_ip = ip;
            return result;
        }
    }

    // Unattended/password-only services require a permanent password, even
    // with a legacy token. Click/Both can instead ask the local operator.
    if (config_.service_mode && config_.password_hash.empty() &&
        (!config_.service_approval || config_.mode == HostAuthMode::Password)) {
        result = deny(AccessDenial::PasswordRequired,
                      "the service has no permanent password; set one before connecting");
        result.peer_address = peer_address;
        result.peer_ip = ip;
        return result;
    }

    {
        int64_t blocked_until = 0;
        for (const std::string& lock_key : keys) {
            const auto it = failures_.find(lock_key);
            if (it != failures_.end()) {
                blocked_until = (std::max)(blocked_until, it->second.blocked_until);
            }
        }
        if (blocked_until > now_unix) {
            const int64_t remaining = blocked_until - now_unix;
            result = deny(AccessDenial::LockedOut,
                          "too many failed attempts; try again in " +
                              std::to_string(remaining) + " s");
            result.peer_address = peer_address;
            result.peer_ip = ip;
            return result;
        }
    }

    // A control-only session has nothing to serve when the host does not
    // allow its one feature; refusing here is clearer than an empty session
    // that refuses every request.
    if (hello.mode != SessionMode::Desktop &&
        (config_.default_permissions & mode_permissions(hello.mode)) == 0) {
        result = deny(AccessDenial::Declined, mode_disabled_reason(hello.mode));
        result.peer_address = peer_address;
        result.peer_ip = ip;
        return result;
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
        fail_locked(keys, now_unix);
        result = deny(AccessDenial::InvalidToken);
        result.peer_address = peer_address;
        result.peer_ip = ip;
        result.permissions =
            config_.default_permissions & requested;
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
        !device_expired(config_.trusted_devices[trusted_index], now_unix,
                        config_.trusted_device_lifetime_seconds);
    if (signature_flag && !signature_valid && (trusted_active || trust_request)) {
        result = deny(AccessDenial::Declined,
                      trusted_active ? "the trusted device proof is not valid"
                                     : "the device proof could not be verified");
        result.peer_address = peer_address;
        result.peer_ip = ip;
        result.permissions =
            config_.default_permissions & requested;
        result.device_key = hello.device_key;
        return result;
    }
    result.request_trust = trust_request && signature_valid;

    if (config_.mode == HostAuthMode::Click) {
        result.decision = Decision::RequireApproval;
        result.permissions =
            config_.default_permissions & requested;
        return result;
    }

    if (hello.password.empty()) {
        if (config_.mode == HostAuthMode::Password) {
            result = deny(AccessDenial::PasswordRequired);
            result.peer_address = peer_address;
            result.peer_ip = ip;
            result.permissions =
                config_.default_permissions & requested;
            return result;
        }
        result.decision = Decision::RequireApproval;
        result.permissions =
            config_.default_permissions & requested;
        return result;
    }

    const bool hash_present = !config_.password_hash.empty();
    const bool hash_usable = password_hash_usable(config_.password_hash);
    bool permanent_ok = false;
    if (hash_present && hash_usable) {
        permanent_ok = verify_password(hello.password, config_.password_hash);
        // The plaintext is only available here, so this is the one moment an
        // older, cheaper hash can be upgraded. HostServer persists the new
        // encoding after the login commits.
        if (permanent_ok && password_hash_needs_rehash(config_.password_hash)) {
            std::string rehashed = hash_password(hello.password);
            if (!rehashed.empty()) {
                config_.password_hash = std::move(rehashed);
            }
        }
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
        result.peer_ip = ip;
        result.permissions =
            config_.default_permissions & requested;
        if (hash_present && !hash_usable) {
            result = deny(AccessDenial::Declined,
                          "the host's password configuration is invalid");
            result.peer_address = peer_address;
            result.peer_ip = ip;
            result.permissions =
                config_.default_permissions & requested;
            return result;
        }
        fail_locked(keys, now_unix);
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
            result.peer_ip = ip;
            result.permissions =
                config_.default_permissions & requested;
            return result;
        }
        const bool skip = trusted_active && signature_valid;
        if (skip) {
            result.via_trusted_device = true;
        } else if (hello.totp.empty()) {
            result = deny(AccessDenial::TwoFactorRequired);
            result.peer_address = peer_address;
            result.peer_ip = ip;
            result.permissions =
                config_.default_permissions & requested;
            return result;
        } else {
            uint64_t step = 0;
            // A well-formed code from an earlier step is a replay: RFC 6238
            // requires each time step to be accepted at most once.
            const bool fresh =
                verify_totp(config_.totp_secret, hello.totp, now_unix, 1, &step) &&
                step > last_totp_step_;
            if (!fresh) {
                result = deny(AccessDenial::WrongTwoFactor);
                result.peer_address = peer_address;
                result.peer_ip = ip;
                result.permissions =
                    config_.default_permissions & requested;
                fail_locked(keys, now_unix);
                if (!config_.service_mode && !temporary_password_.empty()) {
                    ++temporary_failures_;
                    if (temporary_failures_ >= kTempPasswordFailureLimit) {
                        temporary_password_ = random_digits(6);
                        temporary_failures_ = 0;
                    }
                }
                return result;
            }
            last_totp_step_ = step;
        }
    } else if (trusted_active && signature_valid) {
        result.via_trusted_device = true;
    }

    result.decision = Decision::Allow;
    return result;
}

HostSecurity::RenewalResult HostSecurity::renew_ticket(
    const TicketRenewal& renewal, const std::string& peer_address,
    const std::array<uint8_t, kTicketProofHostNonceSize>& host_nonce,
    const std::vector<uint8_t>& client_nonce,
    const std::array<uint8_t, kTicketClientKeySize>& session_key,
    const std::string& session_subject, int64_t now_unix) {
    std::lock_guard<std::mutex> lock(mutex_);
    RenewalResult result;
    auto refuse = [&result](std::string reason) {
        result.accepted = false;
        result.reason = std::move(reason);
        return result;
    };
    const ManagedPolicyState marker = read_managed_policy_state(config_.policy_state);
    if (!marker.readable) {
        return refuse("the managed policy state cannot be read; refusing the renewal");
    }
    if (!marker.managed) {
        return refuse("this device is not centrally managed; no renewal is expected");
    }
    if (!tickets_) {
        return refuse("no trusted access-ticket key is configured");
    }
    AccessTicket ticket;
    TicketError error = tickets_->inspect(renewal.signed_ticket, config_.device_id,
                                          config_.host_fingerprint, now_unix, &ticket);
    if (error != TicketError::None) {
        return refuse(std::string("the access ticket was refused: ") +
                      ticket_error_text(error));
    }
    if (ticket.policy_version != marker.policy_version) {
        return refuse("the access ticket does not match the current device policy version");
    }
    if (ticket.subject != session_subject) {
        return refuse("the access ticket names a different subject");
    }
    bool challenged = false;
    for (uint8_t byte : host_nonce) {
        if (byte != 0) {
            challenged = true;
            break;
        }
    }
    if (!challenged) {
        return refuse("the renewal carried no host challenge");
    }
    // The renewal extends this connection only: a ticket for another client
    // key belongs to a different viewer and cannot inherit this session.
    if (!std::equal(ticket.client_public_key.begin(), ticket.client_public_key.end(),
                    session_key.begin())) {
        return refuse("the access ticket names a different client key");
    }
    const std::vector<uint8_t> proof_message = access_ticket_proof_message(
        config_.device_id, config_.host_fingerprint, host_nonce, client_nonce,
        renewal.signed_ticket);
    if (!verify_access_ticket_proof(ticket, proof_message, renewal.proof)) {
        return refuse("the access-ticket possession proof was refused");
    }
    if (whitelist_configured_) {
        const std::string ip = peer_ip_text(peer_address);
        IpAddress address;
        if (!whitelist_valid_ || !parse_ip_address(ip, address) ||
            !ip_in_ranges(address, whitelist_)) {
            return refuse("this address is not on the host's allow list");
        }
    }
    // The id is consumed only after every check, exactly like admission: a
    // refused renewal must not burn a ticket the viewer may still use.
    error = tickets_->commit(ticket, now_unix);
    if (error != TicketError::None) {
        return refuse(std::string("the access ticket was refused: ") +
                      ticket_error_text(error));
    }
    result.accepted = true;
    result.permissions =
        ticket.permissions & marker.permission_ceiling & config_.default_permissions;
    result.expires_at = ticket.expires_at;
    result.subject = ticket.subject;
    result.ticket_id = ticket.id;
    return result;
}

void HostSecurity::note_connected(const Result& result, int64_t now_unix) {
    if (result.decision == Decision::Deny) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::string& key : lockout_keys(peer_ip_text(result.peer_address))) {
        failures_.erase(key);
    }
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

uint64_t HostSecurity::last_totp_step() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_totp_step_;
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
        if (!device_expired(device, now_unix, config_.trusted_device_lifetime_seconds)) {
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
    int64_t blocked_until = 0;
    for (const std::string& key : lockout_keys(peer_ip_text(peer_address))) {
        const auto it = failures_.find(key);
        if (it != failures_.end()) {
            blocked_until = (std::max)(blocked_until, it->second.blocked_until);
        }
    }
    if (blocked_until <= now_unix) {
        return false;
    }
    if (retry_after_seconds != nullptr) {
        *retry_after_seconds = static_cast<uint32_t>(blocked_until - now_unix);
    }
    return true;
}

}  // namespace chast
