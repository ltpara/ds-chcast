#include "security_key.h"

#include <webauthn.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace chast_viewer {

namespace {

constexpr const char* kBase64UrlAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

constexpr DWORD kMinTimeoutMs = 15000;
constexpr DWORD kMaxTimeoutMs = 300000;

std::wstring wide_from_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string utf8_from_wide(PCWSTR text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, out.data(), size - 1, nullptr,
                        nullptr);
    return out;
}

int base64_url_value(char raw) {
    const auto value = static_cast<unsigned char>(raw);
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '-') {
        return 62;
    }
    if (value == '_') {
        return 63;
    }
    return -1;
}

std::vector<uint8_t> base64_url_decode(const std::string& text) {
    std::vector<uint8_t> out;
    if (text.empty()) {
        return out;
    }
    uint32_t buffer = 0;
    int bits = 0;
    for (const char raw : text) {
        if (raw == '=') {
            continue;
        }
        const int value = base64_url_value(raw);
        if (value < 0) {
            return {};
        }
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFFu));
        }
    }
    return out;
}

std::string base64_url_encode(const BYTE* data, DWORD size) {
    if (data == nullptr || size == 0) {
        return {};
    }
    std::string out;
    out.reserve((static_cast<size_t>(size) + 2) / 3 * 4);
    size_t index = 0;
    const size_t length = static_cast<size_t>(size);
    while (index + 3 <= length) {
        const uint32_t block = (static_cast<uint32_t>(data[index]) << 16) |
                               (static_cast<uint32_t>(data[index + 1]) << 8) |
                               static_cast<uint32_t>(data[index + 2]);
        out.push_back(kBase64UrlAlphabet[(block >> 18) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(block >> 12) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(block >> 6) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[block & 0x3F]);
        index += 3;
    }
    const size_t rest = length - index;
    if (rest == 1) {
        const uint32_t block = static_cast<uint32_t>(data[index]) << 16;
        out.push_back(kBase64UrlAlphabet[(block >> 18) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(block >> 12) & 0x3F]);
    } else if (rest == 2) {
        const uint32_t block = (static_cast<uint32_t>(data[index]) << 16) |
                               (static_cast<uint32_t>(data[index + 1]) << 8);
        out.push_back(kBase64UrlAlphabet[(block >> 18) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(block >> 12) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(block >> 6) & 0x3F]);
    }
    return out;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char raw : text) {
        const auto value = static_cast<unsigned char>(raw);
        switch (value) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (value < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", value);
                    out += buffer;
                } else {
                    out.push_back(raw);
                }
                break;
        }
    }
    return out;
}

std::string client_data_json(const char* type, const std::string& challenge,
                             const std::string& origin) {
    return std::string("{\"type\":\"") + type + "\",\"challenge\":\"" + json_escape(challenge) +
           "\",\"origin\":\"" + json_escape(origin) + "\",\"crossOrigin\":false}";
}

std::string error_name(HRESULT result) {
    return utf8_from_wide(WebAuthNGetErrorName(result));
}

std::string failed_code(HRESULT result, const char* action, std::string* error) {
    const std::string name = error_name(result);
    std::string code = "failed";
    if (name == "NotAllowedError") {
        code = "user_cancelled";
    } else if (name == "InvalidStateError") {
        code = "credential_exists";
    } else if (name == "NotSupportedError") {
        code = "not_supported";
    } else if (name == "AbortError") {
        code = "timeout";
    }
    char detail[16];
    std::snprintf(detail, sizeof(detail), "0x%08lx", static_cast<unsigned long>(result));
    *error = std::string(action) + " failed";
    if (!name.empty()) {
        *error += " (" + name + ")";
    }
    *error += ", HRESULT ";
    *error += detail;
    return code;
}

DWORD clamp_timeout(int timeout_ms) {
    return static_cast<DWORD>(std::clamp(timeout_ms, static_cast<int>(kMinTimeoutMs),
                                         static_cast<int>(kMaxTimeoutMs)));
}

// Builds the legacy WEBAUTHN_CREDENTIALS list together with the byte storage
// its pbId pointers point into.
bool build_credential_list(const std::vector<std::string>& ids, std::vector<std::vector<uint8_t>>* storage,
                           std::vector<WEBAUTHN_CREDENTIAL>* credentials,
                           std::string* error) {
    storage->reserve(ids.size());
    credentials->reserve(ids.size());
    for (const std::string& encoded : ids) {
        std::vector<uint8_t> bytes = base64_url_decode(encoded);
        if (bytes.empty()) {
            *error = "credential ids must be base64url";
            return false;
        }
        storage->push_back(std::move(bytes));
        WEBAUTHN_CREDENTIAL credential{};
        credential.dwVersion = WEBAUTHN_CREDENTIAL_CURRENT_VERSION;
        credential.cbId = static_cast<DWORD>(storage->back().size());
        credential.pbId = storage->back().data();
        credential.pwszCredentialType = WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY;
        credentials->push_back(credential);
    }
    return true;
}

bool make_client_data(const std::string& json, WEBAUTHN_CLIENT_DATA* out) {
    out->dwVersion = WEBAUTHN_CLIENT_DATA_CURRENT_VERSION;
    out->cbClientDataJSON = static_cast<DWORD>(json.size());
    out->pbClientDataJSON = reinterpret_cast<PBYTE>(const_cast<char*>(json.data()));
    out->pwszHashAlgId = WEBAUTHN_HASH_ALGORITHM_SHA_256;
    return out->cbClientDataJSON > 0;
}

}  // namespace

SecurityKeyAvailability QuerySecurityKeyAvailability() {
    SecurityKeyAvailability status;
    if (WebAuthNGetApiVersionNumber() < WEBAUTHN_API_VERSION_1) {
        status.error = "Windows Hello is not available";
        return status;
    }
    BOOL available = FALSE;
    const HRESULT result = WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable(&available);
    if (FAILED(result)) {
        std::string error;
        failed_code(result, "Windows Hello availability check", &error);
        status.error = error;
        return status;
    }
    status.available = available != FALSE;
    if (!status.available) {
        status.error = "no Windows Hello platform authenticator is configured";
    }
    return status;
}

SecurityKeyCredential MakeSecurityKeyCredential(HWND window, const SecurityKeyOptions& options,
                                                const std::string& user_id_base64,
                                                const std::string& username,
                                                const std::string& display_name,
                                                std::string* error, std::string* error_code) {
    SecurityKeyCredential result;
    if (WebAuthNGetApiVersionNumber() < WEBAUTHN_API_VERSION_1) {
        *error_code = "not_supported";
        *error = "Windows Hello is not available";
        return result;
    }
    const std::vector<uint8_t> user_id = base64_url_decode(user_id_base64);
    if (user_id.empty() || user_id.size() > WEBAUTHN_MAX_USER_ID_LENGTH) {
        *error_code = "bad_args";
        *error = "user id must be 1..64 bytes of base64url";
        return result;
    }
    const std::wstring rp_id = wide_from_utf8(options.rp_id);
    const std::wstring username_wide = wide_from_utf8(username);
    const std::wstring display_wide = wide_from_utf8(display_name.empty() ? username : display_name);
    if (rp_id.empty() || username_wide.empty() || display_wide.empty()) {
        *error_code = "bad_args";
        *error = "relying party id and user name must be valid UTF-8";
        return result;
    }

    const std::string origin =
        options.origin.empty() ? "https://" + options.rp_id : options.origin;
    const std::string client_json = client_data_json("webauthn.create", options.challenge, origin);
    WEBAUTHN_CLIENT_DATA client{};
    make_client_data(client_json, &client);

    WEBAUTHN_RP_ENTITY_INFORMATION rp{};
    rp.dwVersion = WEBAUTHN_RP_ENTITY_INFORMATION_CURRENT_VERSION;
    rp.pwszId = rp_id.c_str();
    rp.pwszName = rp_id.c_str();

    WEBAUTHN_USER_ENTITY_INFORMATION user{};
    user.dwVersion = WEBAUTHN_USER_ENTITY_INFORMATION_CURRENT_VERSION;
    user.cbId = static_cast<DWORD>(user_id.size());
    user.pbId = const_cast<PBYTE>(user_id.data());
    user.pwszName = username_wide.c_str();
    user.pwszDisplayName = display_wide.c_str();

    WEBAUTHN_COSE_CREDENTIAL_PARAMETER parameters[2]{};
    parameters[0].dwVersion = WEBAUTHN_COSE_CREDENTIAL_PARAMETER_CURRENT_VERSION;
    parameters[0].pwszCredentialType = WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY;
    parameters[0].lAlg = WEBAUTHN_COSE_ALGORITHM_ECDSA_P256_WITH_SHA256;
    parameters[1] = parameters[0];
    parameters[1].lAlg = WEBAUTHN_COSE_ALGORITHM_RSASSA_PKCS1_V1_5_WITH_SHA256;
    WEBAUTHN_COSE_CREDENTIAL_PARAMETERS credential_parameters{2, parameters};

    std::vector<std::vector<uint8_t>> storage;
    std::vector<WEBAUTHN_CREDENTIAL> excluded;
    std::string list_error;
    if (!build_credential_list(options.credential_ids, &storage, &excluded, &list_error)) {
        *error_code = "bad_args";
        *error = list_error;
        return result;
    }
    WEBAUTHN_CREDENTIALS exclude_list{};
    if (!excluded.empty()) {
        exclude_list.cCredentials = static_cast<DWORD>(excluded.size());
        exclude_list.pCredentials = excluded.data();
    }

    WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS credential_options{};
    credential_options.dwVersion = WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS_VERSION_1;
    credential_options.dwTimeoutMilliseconds = clamp_timeout(options.timeout_ms);
    credential_options.CredentialList = exclude_list;
    credential_options.dwAuthenticatorAttachment = WEBAUTHN_AUTHENTICATOR_ATTACHMENT_PLATFORM;
    credential_options.bRequireResidentKey = FALSE;
    credential_options.dwUserVerificationRequirement =
        WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED;
    credential_options.dwAttestationConveyancePreference =
        WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_NONE;

    PWEBAUTHN_CREDENTIAL_ATTESTATION attestation = nullptr;
    const HRESULT make_result = WebAuthNAuthenticatorMakeCredential(
        window, &rp, &user, &credential_parameters, &client, &credential_options, &attestation);
    if (FAILED(make_result) || attestation == nullptr) {
        *error_code = failed_code(make_result, "Windows Hello registration", error);
        if (attestation != nullptr) {
            WebAuthNFreeCredentialAttestation(attestation);
        }
        return result;
    }
    result.client_data_json = client_json;
    result.credential_id = base64_url_encode(attestation->pbCredentialId,
                                             attestation->cbCredentialId);
    result.attestation_object = base64_url_encode(attestation->pbAttestationObject,
                                                  attestation->cbAttestationObject);
    WebAuthNFreeCredentialAttestation(attestation);
    if (result.credential_id.empty() || result.attestation_object.empty()) {
        *error_code = "failed";
        *error = "Windows Hello returned an incomplete credential";
    }
    return result;
}

SecurityKeyAssertion GetSecurityKeyAssertion(HWND window, const SecurityKeyOptions& options,
                                             std::string* error, std::string* error_code) {
    SecurityKeyAssertion result;
    if (WebAuthNGetApiVersionNumber() < WEBAUTHN_API_VERSION_1) {
        *error_code = "not_supported";
        *error = "Windows Hello is not available";
        return result;
    }
    if (options.credential_ids.empty()) {
        *error_code = "bad_args";
        *error = "at least one credential id is required";
        return result;
    }
    const std::wstring rp_id = wide_from_utf8(options.rp_id);
    if (rp_id.empty()) {
        *error_code = "bad_args";
        *error = "relying party id must be a valid host name";
        return result;
    }

    const std::string origin =
        options.origin.empty() ? "https://" + options.rp_id : options.origin;
    const std::string client_json = client_data_json("webauthn.get", options.challenge, origin);
    WEBAUTHN_CLIENT_DATA client{};
    make_client_data(client_json, &client);

    std::vector<std::vector<uint8_t>> storage;
    std::vector<WEBAUTHN_CREDENTIAL> allowed;
    std::string list_error;
    if (!build_credential_list(options.credential_ids, &storage, &allowed, &list_error)) {
        *error_code = "bad_args";
        *error = list_error;
        return result;
    }
    WEBAUTHN_CREDENTIALS allow_list{};
    allow_list.cCredentials = static_cast<DWORD>(allowed.size());
    allow_list.pCredentials = allowed.data();

    WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS assertion_options{};
    assertion_options.dwVersion = WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_1;
    assertion_options.dwTimeoutMilliseconds = clamp_timeout(options.timeout_ms);
    assertion_options.CredentialList = allow_list;
    assertion_options.dwAuthenticatorAttachment = WEBAUTHN_AUTHENTICATOR_ATTACHMENT_PLATFORM;
    assertion_options.dwUserVerificationRequirement =
        WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED;

    PWEBAUTHN_ASSERTION assertion = nullptr;
    const HRESULT get_result = WebAuthNAuthenticatorGetAssertion(
        window, rp_id.c_str(), &client, &assertion_options, &assertion);
    if (FAILED(get_result) || assertion == nullptr) {
        *error_code = failed_code(get_result, "Windows Hello verification", error);
        if (assertion != nullptr) {
            WebAuthNFreeAssertion(assertion);
        }
        return result;
    }
    result.client_data_json = client_json;
    result.credential_id = base64_url_encode(assertion->Credential.pbId,
                                             assertion->Credential.cbId);
    result.authenticator_data = base64_url_encode(assertion->pbAuthenticatorData,
                                                  assertion->cbAuthenticatorData);
    result.signature = base64_url_encode(assertion->pbSignature, assertion->cbSignature);
    WebAuthNFreeAssertion(assertion);
    if (result.credential_id.empty() || result.authenticator_data.empty() ||
        result.signature.empty()) {
        *error_code = "failed";
        *error = "Windows Hello returned an incomplete assertion";
    }
    return result;
}

}  // namespace chast_viewer
