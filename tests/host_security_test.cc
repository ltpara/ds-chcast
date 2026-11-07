// Pure-logic checks for the host authentication policy: no GPU, no network,
// no certificate store. The device identity test generates a throwaway key in
// memory and never writes it.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "chast/host_security.h"
#include "chast/security.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

const std::string& test_password() {
    static const std::string password = "correct-horse-battery";
    return password;
}

const std::string& test_password_hash() {
    static const std::string hash = chast::hash_password(test_password());
    return hash;
}

chast::PermissionMask permissions(bool input, bool clipboard, bool audio) {
    return (input ? chast::kPermissionInput : 0) |
           (clipboard ? chast::kPermissionClipboard : 0) |
           (audio ? chast::kPermissionAudio : 0);
}

chast::ClientHello hello_with(const std::string& password) {
    chast::ClientHello hello;
    hello.password = password;
    hello.requested_permissions = chast::kPermissionAll;
    return hello;
}

std::string totp_now(const std::string& secret, uint64_t now) {
    std::vector<uint8_t> key;
    chast::parse_totp_secret(secret, key);
    char text[16]{};
    std::snprintf(text, sizeof(text), "%06u", chast::totp_code(key, now));
    return text;
}

using Decision = chast::HostSecurity::Decision;
using AccessDenial = chast::AccessDenial;

void test_modes() {
    chast::HostSecurity::Config config;
    config.mode = chast::HostAuthMode::Both;
    config.password_hash = test_password_hash();
    config.default_permissions = permissions(true, true, false);
    chast::HostSecurity security(config);

    chast::ClientHello hello = hello_with("");
    auto result = security.evaluate(hello, "192.168.1.10:5000", 1000);
    check(result.decision == Decision::RequireApproval,
          "both with an empty password asks the operator");
    check(result.permissions == permissions(true, true, false),
          "the default permissions are intersected with the request");

    hello.requested_permissions = chast::kPermissionInput;
    result = security.evaluate(hello, "192.168.1.10:5000", 1001);
    check(result.permissions == chast::kPermissionInput,
          "a viewer that asks for less gets less");

    hello.password = "wrong";
    result = security.evaluate(hello, "192.168.1.10:5000", 1002);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::WrongPassword,
          "a wrong password is refused instead of falling back to approval");

    hello.password = test_password();
    result = security.evaluate(hello, "192.168.1.10:5000", 1003);
    check(result.decision == Decision::Allow && !result.used_temporary_password,
          "the permanent password allows the connection");

    config.mode = chast::HostAuthMode::Password;
    security.set_config(config);
    hello.password.clear();
    result = security.evaluate(hello, "192.168.1.10:5000", 1004);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::PasswordRequired,
          "password mode refuses an empty password");

    config.mode = chast::HostAuthMode::Click;
    security.set_config(config);
    hello.password = "ignored";
    result = security.evaluate(hello, "192.168.1.10:5000", 1005);
    check(result.decision == Decision::RequireApproval,
          "click mode always asks the operator");
    check(!security.has_temporary_password(),
          "click mode does not publish a temporary password");

    config.mode = chast::HostAuthMode::Both;
    security.set_config(config);
    check(security.has_temporary_password(), "both mode publishes a temporary password");
}

void test_temporary_password() {
    chast::HostSecurity::Config config;
    config.mode = chast::HostAuthMode::Password;
    chast::HostSecurity security(config);
    const std::string first = security.temporary_password();
    check(first.size() == 6, "the temporary password is six digits");
    bool digits = !first.empty();
    for (char c : first) {
        digits = digits && c >= '0' && c <= '9';
    }
    check(digits, "the temporary password is decimal");

    chast::ClientHello hello = hello_with(first);
    auto result = security.evaluate(hello, "127.0.0.1:1000", 2000);
    check(result.decision == Decision::Allow && result.used_temporary_password,
          "the temporary password allows the connection");
    security.note_connected(result, 2000);
    check(security.temporary_password() != first,
          "a spent temporary password is rotated on connect");

    const std::string before = security.temporary_password();
    for (int i = 0; i < 10; ++i) {
        chast::ClientHello wrong = hello_with("nope");
        const std::string peer = "10.0.0." + std::to_string(i + 1) + ":4000";
        security.evaluate(wrong, peer, 3000 + i);
    }
    check(security.temporary_password() != before,
          "ten consecutive failures rotate the temporary password");

    const std::string refreshed = security.refresh_temporary_password();
    bool refreshed_digits = refreshed.size() == 6;
    for (char c : refreshed) {
        refreshed_digits = refreshed_digits && c >= '0' && c <= '9';
    }
    check(refreshed_digits, "a manual refresh returns a new six-digit password");
    check(refreshed == security.temporary_password(), "the refreshed value is published");
}

void test_token_auth() {
    chast::HostSecurity::Config config;
    config.mode = chast::HostAuthMode::Both;
    config.token_auth = true;
    config.access_token = "CLI23456";
    chast::HostSecurity security(config);

    chast::ClientHello hello = hello_with("");
    hello.access_token = "CLI23456";
    auto result = security.evaluate(hello, "127.0.0.1:2000", 1000);
    check(result.decision == Decision::Allow && result.via_token,
          "an enabled token allows a CLI client without a password");
    check(result.permissions == chast::kPermissionAll,
          "a token client gets the default permissions");

    hello.access_token = "BADTOKEN";
    result = security.evaluate(hello, "127.0.0.2:2000", 1001);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::InvalidToken,
          "a wrong token is refused");

    hello.access_token.clear();
    result = security.evaluate(hello, "127.0.0.3:2000", 1002);
    check(result.decision == Decision::RequireApproval,
          "a client without a token falls through to the password flow");

    config.token_auth = false;
    security.set_config(config);
    hello.access_token = "CLI23456";
    result = security.evaluate(hello, "127.0.0.4:2000", 1003);
    check(result.decision == Decision::RequireApproval,
          "the token is ignored while token authentication is off");
}

void test_two_factor() {
    chast::HostSecurity::Config config;
    config.mode = chast::HostAuthMode::Password;
    config.password_hash = test_password_hash();
    config.totp_secret = chast::generate_totp_secret();
    chast::HostSecurity security(config);
    const uint64_t now = 1700000000;

    chast::ClientHello hello = hello_with(test_password());
    auto result = security.evaluate(hello, "127.0.0.1:3000", now);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::TwoFactorRequired,
          "2FA asks the viewer to reconnect with a code");

    hello.totp = "000000";
    result = security.evaluate(hello, "127.0.0.1:3000", now);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::WrongTwoFactor,
          "a wrong TOTP code is refused");

    hello.totp = totp_now(config.totp_secret, now);
    result = security.evaluate(hello, "127.0.0.1:3000", now);
    check(result.decision == Decision::Allow, "the current TOTP code allows the connection");

    // RFC 6238: a time step authenticates at most once, so a captured code
    // cannot be replayed inside its +/- window.
    result = security.evaluate(hello, "127.0.0.1:3000", now);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::WrongTwoFactor,
          "a consumed TOTP step cannot be replayed");
    check(security.last_totp_step() == static_cast<uint64_t>(now) / 30,
          "the accepted TOTP step is remembered");

    hello.totp = totp_now(config.totp_secret, now + 30);
    result = security.evaluate(hello, "127.0.0.1:3000", now + 30);
    check(result.decision == Decision::Allow, "the next TOTP step is still accepted");

    hello.totp = totp_now(config.totp_secret, now + 300);
    result = security.evaluate(hello, "127.0.0.1:3000", now);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::WrongTwoFactor,
          "a code outside the window is refused");

    config.totp_secret = "not base32!";
    security.set_config(config);
    hello.totp = "123456";
    result = security.evaluate(hello, "127.0.0.1:3000", now);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::Declined,
          "a broken 2FA configuration fails closed");

    std::string error;
    check(!security.set_totp_secret("not base32!", &error), "an invalid TOTP secret is rejected");
    check(security.set_totp_secret(chast::generate_totp_secret(), &error),
          "a generated TOTP secret is accepted");
    security.clear_totp_secret();
    check(!security.two_factor_enabled(), "2FA can be turned off");
}

struct SignedDevice {
    std::unique_ptr<chast::DeviceIdentity> identity;
    std::string fingerprint;
    std::string error;
    uint8_t nonce_seed = 1;

    SignedDevice() {
        identity = chast::DeviceIdentity::generate(&error);
        fingerprint.assign(64, 'A');
    }

    void sign(chast::ClientHello& hello, int64_t now, uint8_t flags) {
        hello.device_key = identity->public_key();
        hello.device_platform = static_cast<uint8_t>(chast::DevicePlatform::Windows);
        hello.device_name = "viewer-pc";
        hello.nonce.fill(0);
        hello.nonce[0] = nonce_seed++;
        hello.unix_seconds = static_cast<uint64_t>(now);
        hello.auth_flags = flags;
        hello.signature.clear();
        const std::vector<uint8_t> message =
            chast::hello_signature_message(fingerprint, hello);
        identity->sign(message, hello.signature, &error);
    }
};

void test_trusted_devices() {
    SignedDevice device;
    check(device.identity != nullptr, "a device identity is generated");
    if (!device.identity) {
        return;
    }
    const uint64_t now = 1700000000;
    chast::HostSecurity::Config config;
    config.mode = chast::HostAuthMode::Password;
    config.password_hash = test_password_hash();
    config.totp_secret = chast::generate_totp_secret();
    config.host_fingerprint = device.fingerprint;
    chast::TrustedDevice trusted;
    trusted.public_key = device.identity->public_key();
    trusted.name = "viewer-pc";
    trusted.platform = 1;
    trusted.added_unix_seconds = static_cast<int64_t>(now) - 10000;
    trusted.last_seen_unix_seconds = static_cast<int64_t>(now) - 5000;
    config.trusted_devices = {trusted};
    chast::HostSecurity security(config);

    chast::ClientHello hello = hello_with(test_password());
    device.sign(hello, static_cast<int64_t>(now), chast::kAuthFlagSigned);
    auto result = security.evaluate(hello, "127.0.0.1:4000", now);
    check(result.decision == Decision::Allow && result.via_trusted_device,
          "a trusted device skips 2FA with a valid signature");
    security.note_connected(result, static_cast<int64_t>(now));
    const auto devices = security.trusted_devices(now);
    check(devices.size() == 1 && devices[0].last_seen_unix_seconds == static_cast<int64_t>(now),
          "a successful trusted-device login refreshes its last-seen time");

    result = security.evaluate(hello, "127.0.0.1:4000", now);
    check(result.decision == Decision::Deny,
          "replaying the same signed Hello is refused");

    chast::ClientHello fresh = hello_with(test_password());
    device.sign(fresh, static_cast<int64_t>(now), chast::kAuthFlagSigned);
    fresh.signature[0] ^= 0x01;
    result = security.evaluate(fresh, "127.0.0.1:4000", now);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::Declined,
          "a broken signature from a trusted key is refused");

    chast::ClientHello stale = hello_with(test_password());
    device.sign(stale, static_cast<int64_t>(now), chast::kAuthFlagSigned);
    stale.unix_seconds = now - chast::kHelloClockSkewSeconds - 10;
    result = security.evaluate(stale, "127.0.0.1:4000", now);
    check(result.decision == Decision::Deny, "a signed Hello outside the clock window is refused");

    config.trusted_devices = {trusted};
    config.trusted_devices[0].last_seen_unix_seconds =
        static_cast<int64_t>(now) - chast::kTrustedDeviceLifetimeSeconds - 1;
    security.set_config(config);
    chast::ClientHello expired = hello_with(test_password());
    device.sign(expired, static_cast<int64_t>(now), chast::kAuthFlagSigned);
    result = security.evaluate(expired, "127.0.0.1:4000", now);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::TwoFactorRequired,
          "an expired trusted device has to pass 2FA again");
    check(security.trusted_devices(now).empty(),
          "the expired device is not listed for the operator");

    // The lifetime comes from the host settings, so a short one expires a
    // device that the default 90 days would still accept.
    chast::TrustedDevice recent_device = trusted;
    recent_device.last_seen_unix_seconds = static_cast<int64_t>(now) - 10;
    config.trusted_device_lifetime_seconds = 60;
    config.trusted_devices = {recent_device};
    security.set_config(config);
    chast::ClientHello recent = hello_with(test_password());
    device.sign(recent, static_cast<int64_t>(now), chast::kAuthFlagSigned);
    result = security.evaluate(recent, "127.0.0.1:4000", now);
    check(result.decision == Decision::Allow && result.via_trusted_device,
          "a device inside a shortened lifetime still skips 2FA");
    chast::ClientHello after = hello_with(test_password());
    device.sign(after, static_cast<int64_t>(now + 61), chast::kAuthFlagSigned);
    result = security.evaluate(after, "127.0.0.1:4000", now + 61);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::TwoFactorRequired,
          "a device older than the configured lifetime has to pass 2FA again");

    // Zero disables expiry entirely: even a device last seen years ago stays.
    config.trusted_device_lifetime_seconds = 0;
    config.trusted_devices = {recent_device};
    security.set_config(config);
    const uint64_t much_later = now + static_cast<uint64_t>(10 * 365 * 24 * 60 * 60);
    chast::ClientHello ancient = hello_with(test_password());
    device.sign(ancient, static_cast<int64_t>(much_later), chast::kAuthFlagSigned);
    result = security.evaluate(ancient, "127.0.0.1:4000", much_later);
    check(result.decision == Decision::Allow && result.via_trusted_device,
          "a zero lifetime never expires a trusted device");
}

void test_trust_request() {
    SignedDevice device;
    check(device.identity != nullptr, "the trust-request identity is generated");
    if (!device.identity) {
        return;
    }
    const uint64_t now = 1700000000;
    chast::HostSecurity::Config config;
    config.mode = chast::HostAuthMode::Password;
    config.password_hash = test_password_hash();
    config.totp_secret = chast::generate_totp_secret();
    config.host_fingerprint = device.fingerprint;
    chast::HostSecurity security(config);

    chast::ClientHello hello = hello_with(test_password());
    hello.totp = totp_now(config.totp_secret, now);
    device.sign(hello, static_cast<int64_t>(now),
                chast::kAuthFlagSigned | chast::kAuthFlagTrustDevice);
    auto result = security.evaluate(hello, "127.0.0.1:5000", now);
    check(result.decision == Decision::Allow && result.request_trust,
          "a signed 2FA login may ask to be trusted");
    check(security.trusted_devices(now).empty(), "nothing is trusted before the commit");
    security.note_connected(result, static_cast<int64_t>(now));
    check(security.trusted_devices(now).size() == 1,
          "note_connected commits the trusted device");

    chast::ClientHello later = hello_with(test_password());
    device.sign(later, static_cast<int64_t>(now + 100), chast::kAuthFlagSigned);
    result = security.evaluate(later, "127.0.0.1:5000", now + 100);
    check(result.decision == Decision::Allow && result.via_trusted_device,
          "the newly trusted device skips 2FA next time");

    config.trusted_devices = security.trusted_devices(now);
    config.trusted_devices[0].name = "renamed";
    security.set_config(config);
    check(security.revoke_trusted_device(device.identity->public_key()),
          "a trusted device can be revoked");
    check(security.trusted_devices(now).empty(), "the revoked device is gone");
}

void test_allow_list() {
    chast::HostSecurity::Config config;
    config.mode = chast::HostAuthMode::Both;
    config.ip_whitelist = "10.0.0.0/8, 192.168.5.5";
    chast::HostSecurity security(config);

    chast::ClientHello hello = hello_with("");
    auto result = security.evaluate(hello, "192.168.1.2:6000", 1000);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::NotWhitelisted,
          "an address outside the list is refused before authentication");

    result = security.evaluate(hello, "10.3.4.5:6000", 1001);
    check(result.decision == Decision::RequireApproval, "an address inside the list proceeds");

    result = security.evaluate(hello, "192.168.5.5:6000", 1002);
    check(result.decision == Decision::RequireApproval, "a bare address is treated as /32");

    result = security.evaluate(hello, "[::1]:6000", 1003);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::NotWhitelisted,
          "an IPv6 address cannot match an IPv4 list");

    config.ip_whitelist = "10.0.0.0/8, 192.168.5.5, 2001:db8:1::/48";
    security.set_config(config);
    result = security.evaluate(hello, "[2001:db8:1:7::5]:6000", 1004);
    check(result.decision == Decision::RequireApproval, "an IPv6 entry admits its subnet");
    result = security.evaluate(hello, "[2001:db8:2::5]:6000", 1005);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::NotWhitelisted,
          "an IPv6 address outside every entry is refused");
    result = security.evaluate(hello, "10.3.4.5:6000", 1006);
    check(result.decision == Decision::RequireApproval,
          "a mixed list still admits its IPv4 entries");

    // A dual-stack listener may report an IPv4 peer in its mapped form.
    result = security.evaluate(hello, "[::ffff:10.1.2.3]:6000", 1007);
    check(result.decision == Decision::RequireApproval,
          "a mapped IPv4 peer matches the IPv4 entry");

    config.ip_whitelist = "10.0.0.0/33";
    security.set_config(config);
    result = security.evaluate(hello, "10.1.2.3:6000", 1008);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::NotWhitelisted,
          "a broken allow list fails closed");
    config.ip_whitelist = "2001:db8::/129";
    security.set_config(config);
    result = security.evaluate(hello, "[2001:db8::1]:6000", 1009);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::NotWhitelisted,
          "an IPv6 prefix past 128 fails closed");

    config.ip_whitelist = "10.0.0.0/8, 2001:db8::1:";
    security.set_config(config);
    result = security.evaluate(hello, "[2001:db8::1]:6000", 1009);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::NotWhitelisted,
          "a trailing-colon IPv6 whitelist entry fails closed");
    result = security.evaluate(hello, "10.1.2.3:6000", 1009);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::NotWhitelisted,
          "a malformed IPv6 entry also refuses otherwise allowed IPv4 peers");

    config.ip_whitelist = "10.0.0.0/8, not-an-address";
    security.set_config(config);
    result = security.evaluate(hello, "10.1.2.3:6000", 1010);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::NotWhitelisted,
          "one bad entry refuses even the addresses it does cover");

    config.ip_whitelist.clear();
    security.set_config(config);
    result = security.evaluate(hello, "[::1]:6000", 1011);
    check(result.decision == Decision::RequireApproval,
          "an empty allow list accepts any address");

    std::vector<chast::IpRange> ranges;
    std::string error;
    const bool parsed =
        chast::parse_ip_whitelist("10.0.0.0/8, 2001:db8::/32, ::ffff:192.168.0.0/112",
                                  ranges, &error);
    check(parsed && ranges.size() == 3, "a mixed list parses three entries");
    chast::IpAddress mapped;
    chast::parse_ip_address("192.168.4.4", mapped);
    check(parsed && chast::ip_in_ranges(mapped, ranges),
          "a mapped IPv6 entry is stored as the IPv4 range it names");
    check(chast::parse_ip_whitelist("", ranges, &error) && ranges.empty(),
          "a blank list parses as empty");
}

void test_lockout() {
    chast::HostSecurity::Config config;
    config.mode = chast::HostAuthMode::Password;
    chast::HostSecurity security(config);
    chast::ClientHello wrong = hello_with("not-the-password");

    for (int i = 0; i < 6; ++i) {
        const auto result = security.evaluate(wrong, "127.0.0.1:7000", 1000 + i);
        check(result.denial == AccessDenial::WrongPassword,
              "each of the first six failures is a password refusal");
    }
    uint32_t retry = 0;
    check(security.locked_out("127.0.0.1:7001", 1006, &retry) && retry == 29,
          "the sixth failure in a minute starts a 30 s backoff");
    auto result = security.evaluate(wrong, "127.0.0.1:7000", 1006);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::LockedOut,
          "a locked address is refused even before the password is checked");
    check(security.evaluate(wrong, "127.0.0.2:7000", 1006).denial == AccessDenial::WrongPassword,
          "another address is not affected");
    check(!security.locked_out("127.0.0.1:7000", 1066),
          "the backoff ends after 30 seconds");

    // Wait until the first burst has left the one-minute window, then make a
    // fresh burst: the ladder remembers that this address was blocked before.
    for (int i = 0; i < 6; ++i) {
        security.evaluate(wrong, "127.0.0.1:7000", 1066 + i);
    }
    check(security.locked_out("127.0.0.1:7000", 1072, &retry) && retry == 59,
          "the next burst doubles the backoff");
    check(!security.locked_out("127.0.0.1:7000", 1132),
          "the doubled backoff ends after 60 seconds");

    for (int i = 0; i < 30; ++i) {
        security.evaluate(wrong, "10.9.8.7:7000", 5000 + i * 20);
    }
    check(security.locked_out("10.9.8.7:7000", 5581),
          "thirty failures in ten minutes trigger the long-window lockout");

    chast::HostSecurity::Result accepted;
    accepted.decision = Decision::Allow;
    accepted.peer_address = "10.9.8.7:7000";
    security.note_connected(accepted, 6000);
    check(!security.locked_out("10.9.8.7:7000", 6000),
          "a successful connection clears the lockout");
}

void test_ipv6_lockout() {
    chast::HostSecurity::Config config;
    config.mode = chast::HostAuthMode::Password;
    chast::HostSecurity security(config);
    chast::ClientHello wrong = hello_with("not-the-password");

    // A client picks any interface address in its own /64, so six attempts
    // spread over six addresses must still start the backoff.
    for (int i = 0; i < 6; ++i) {
        const std::string peer = "[2001:db8:1:2::" + std::to_string(i + 1) + "]:7000";
        const auto result = security.evaluate(wrong, peer, 1000 + i);
        check(result.denial == AccessDenial::WrongPassword,
              "each early IPv6 failure is still a password refusal");
    }
    uint32_t retry = 0;
    check(security.locked_out("[2001:db8:1:2::99]:7001", 1006, &retry),
          "rotating the address inside the same /64 does not escape the lockout");
    check(security.evaluate(wrong, "[2001:db8:1:2:ffff::1]:7000", 1006).denial ==
              AccessDenial::LockedOut,
          "a fresh address in the locked /64 is refused");
    check(security.locked_out("[2001:db8:1:9::1]:7000", 1006),
          "a neighbouring /64 in the same /48 is locked as well");
    check(!security.locked_out("[2001:db8:2::1]:7000", 1006),
          "another /48 keeps its own counter");

    // An IPv4-mapped peer shares the IPv4 bucket with its plain spelling.
    for (int i = 0; i < 6; ++i) {
        security.evaluate(wrong, "203.0.113.9:8000", 3000 + i);
    }
    check(security.locked_out("[::ffff:203.0.113.9]:8001", 3006),
          "the mapped form of a blocked IPv4 address is blocked too");

    chast::HostSecurity::Result accepted;
    accepted.decision = Decision::Allow;
    accepted.peer_address = "[2001:db8:1:2::7]:7000";
    security.note_connected(accepted, 4000);
    check(!security.locked_out("[2001:db8:1:2::99]:7000", 4000),
          "a successful connection clears every prefix bucket");
}

void test_service_mode() {
    chast::HostSecurity::Config config;
    config.mode = chast::HostAuthMode::Click;
    config.service_mode = true;
    config.token_auth = true;
    config.access_token = "CLI23456";
    chast::HostSecurity security(config);
    check(security.config().mode == chast::HostAuthMode::Password,
          "service mode forces password authentication");
    check(!security.has_temporary_password(), "service mode has no temporary password");

    chast::ClientHello hello = hello_with("");
    hello.access_token = "CLI23456";
    auto result = security.evaluate(hello, "127.0.0.1:8000", 1000);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::PasswordRequired,
          "a service without a permanent password refuses everyone, even with a token");

    config.password_hash = test_password_hash();
    security.set_config(config);
    hello.access_token.clear();
    result = security.evaluate(hello, "127.0.0.1:8000", 1001);
    check(result.denial == AccessDenial::PasswordRequired,
          "service mode still requires the password");

    hello.password = test_password();
    result = security.evaluate(hello, "127.0.0.1:8000", 1002);
    check(result.decision == Decision::Allow, "the service accepts its permanent password");

    hello.totp.clear();
    hello.password = test_password();
    hello.access_token = "CLI23456";
    result = security.evaluate(hello, "127.0.0.1:8000", 1003);
    check(result.decision == Decision::Allow && result.via_token,
          "the service accepts an explicitly enabled token");
}

void test_password_and_permissions() {
    chast::HostSecurity security;
    std::string error;
    check(!security.set_password("short", &error), "a short permanent password is rejected");
    check(security.set_password("long-enough", &error), "a long permanent password is accepted");
    check(security.password_configured(), "the password is marked configured");
    check(security.password_hash().rfind("pbkdf2-sha256$", 0) == 0,
          "only the PBKDF2 encoding is stored");

    chast::HostSecurity::Config config = security.config();
    chast::TrustedDevice device;
    device.public_key = {1, 2, 3};
    device.added_unix_seconds = 1000;
    device.last_seen_unix_seconds = 1000;
    config.trusted_devices = {device};
    security.set_config(config);
    check(security.trusted_devices(2000).size() == 1, "the trust list is loaded");
    check(security.set_password("another-long-password", &error),
          "the permanent password can be changed");
    check(security.trusted_devices(2000).empty(),
          "changing the permanent password clears the trust list");

    config = security.config();
    config.default_permissions = chast::kPermissionClipboard;
    security.set_config(config);
    chast::ClientHello hello = hello_with("");
    hello.requested_permissions =
        chast::kPermissionInput | chast::kPermissionClipboard | chast::kPermissionAudio;
    config.mode = chast::HostAuthMode::Click;
    security.set_config(config);
    auto result = security.evaluate(hello, "127.0.0.1:9000", 1000);
    check(result.permissions == chast::kPermissionClipboard,
          "permissions are the default mask intersected with the request");

    hello.requested_permissions = 0;
    result = security.evaluate(hello, "127.0.0.1:9000", 1001);
    check(result.permissions == 0, "a viewer may ask to watch without any permission");

    hello.requested_permissions = 0xFFFF;
    result = security.evaluate(hello, "127.0.0.1:9000", 1002);
    check(result.permissions == chast::kPermissionClipboard,
          "unknown permission bits are ignored");

    check(chast::HostSecurity::Decision::Allow != Decision::Deny,
          "the decision enum is usable");
}

void test_password_rehash() {
    chast::HostSecurity::Config config;
    config.mode = chast::HostAuthMode::Password;
    const std::vector<uint8_t> salt(16, 0x42);
    config.password_hash = chast::hash_password_with_salt(test_password(), salt, 1);
    check(chast::password_hash_needs_rehash(config.password_hash),
          "the fixture starts on an old iteration count");
    chast::HostSecurity security(config);
    chast::ClientHello hello = hello_with(test_password());
    auto result = security.evaluate(hello, "127.0.0.1:9100", 1000);
    check(result.decision == Decision::Allow, "the old hash still verifies");
    check(security.password_hash().rfind("pbkdf2-sha256$600000$", 0) == 0,
          "a successful login upgrades the hash to the current cost");
    check(chast::verify_password(test_password(), security.password_hash()),
          "the upgraded hash still verifies the password");

    chast::ClientHello wrong = hello_with("not-the-password");
    result = security.evaluate(wrong, "127.0.0.1:9100", 1001);
    check(result.decision == Decision::Deny,
          "the upgraded hash rejects a wrong password");
}

void test_file_transfer_mode() {
    chast::HostSecurity::Config config;
    config.mode = chast::HostAuthMode::Click;
    config.default_permissions = chast::kPermissionAll;
    chast::HostSecurity security(config);

    chast::ClientHello hello = hello_with("");
    hello.mode = chast::SessionMode::FileTransfer;
    auto result = security.evaluate(hello, "192.168.1.20:6000", 1000);
    check(result.decision == Decision::RequireApproval,
          "a files-only Hello still goes through the approval policy");
    check(result.permissions == chast::kPermissionFileTransfer,
          "a files-only Hello is clamped to the file-transfer permission");

    hello.requested_permissions = chast::kPermissionInput;
    result = security.evaluate(hello, "192.168.1.20:6001", 1001);
    check(result.permissions == 0,
          "a files-only Hello asking for input is granted nothing");

    config = security.config();
    config.default_permissions = chast::kPermissionInput | chast::kPermissionClipboard;
    security.set_config(config);
    hello.requested_permissions = chast::kPermissionAll;
    result = security.evaluate(hello, "192.168.1.20:6002", 1002);
    check(result.decision == Decision::Deny && result.denial == AccessDenial::Declined,
          "a files-only Hello is refused when file transfer is disabled");
    check(result.reason.find("file transfer") != std::string::npos,
          "the refusal names file transfer");

    hello.mode = chast::SessionMode::Desktop;
    result = security.evaluate(hello, "192.168.1.20:6003", 1003);
    check(result.decision == Decision::RequireApproval,
          "a desktop Hello is unaffected by the file-transfer gates");
    check(result.permissions == (chast::kPermissionInput | chast::kPermissionClipboard),
          "a desktop Hello keeps the default permissions");

    config = security.config();
    config.mode = chast::HostAuthMode::Both;
    config.default_permissions = chast::kPermissionAll;
    config.token_auth = true;
    config.access_token = "FILEONLY123";
    security.set_config(config);
    hello = hello_with("");
    hello.mode = chast::SessionMode::FileTransfer;
    hello.access_token = "FILEONLY123";
    result = security.evaluate(hello, "192.168.1.20:6004", 1004);
    check(result.decision == Decision::Allow && result.via_token,
          "automation may open a files-only session with the token");
    check(result.permissions == chast::kPermissionFileTransfer,
          "the token's files-only session carries only file transfer");
}

void test_signature_message() {
    chast::ClientHello hello;
    hello.nonce = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    hello.unix_seconds = 0x0102030405060708ull;
    const std::string fingerprint(64, 'A');
    const std::vector<uint8_t> message = chast::hello_signature_message(fingerprint, hello);
    check(message.size() == 64 + 16 + 8, "the signed message has the documented size");
    bool layout = true;
    for (size_t i = 0; i < fingerprint.size(); ++i) {
        layout = layout && message[i] == static_cast<uint8_t>('A');
    }
    for (size_t i = 0; i < hello.nonce.size(); ++i) {
        layout = layout && message[64 + i] == hello.nonce[i];
    }
    for (size_t i = 0; i < 8; ++i) {
        layout = layout && message[80 + i] == static_cast<uint8_t>(hello.unix_seconds >> (8 * i));
    }
    check(layout, "the signed message is fingerprint || nonce || little-endian time");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_modes();
    test_temporary_password();
    test_token_auth();
    test_two_factor();
    test_trusted_devices();
    test_trust_request();
    test_allow_list();
    test_lockout();
    test_ipv6_lockout();
    test_service_mode();
    test_password_and_permissions();
    test_password_rehash();
    test_file_transfer_mode();
    test_signature_message();
    if (failures != 0) {
        std::printf("host security checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("host security checks passed\n");
    return 0;
}
