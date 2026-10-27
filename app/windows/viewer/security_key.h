#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace chast_viewer {

// Windows Hello ceremonies through webauthn.dll. The helpers build the client
// data JSON themselves and report every binary value as unpadded base64url, so
// the Flutter side only moves JSON fields around. They block while Windows
// shows its verification UI and must run off the platform thread.

struct SecurityKeyOptions {
    std::string rp_id;
    // Empty means https://<rp_id>, which is the origin dashboards expect.
    std::string origin;
    // Echoed verbatim into client data; the server compares it byte for byte.
    std::string challenge;
    // base64url credential ids, used as the exclude or allow list.
    std::vector<std::string> credential_ids;
    int timeout_ms = 120000;
};

struct SecurityKeyCredential {
    std::string credential_id;
    std::string client_data_json;
    std::string attestation_object;
};

struct SecurityKeyAssertion {
    std::string credential_id;
    std::string client_data_json;
    std::string authenticator_data;
    std::string signature;
};

struct SecurityKeyAvailability {
    bool available = false;
    std::string error;
};

SecurityKeyAvailability QuerySecurityKeyAvailability();

// error_code is one of not_supported, bad_args, user_cancelled, timeout,
// credential_exists, not_found, failed.
SecurityKeyCredential MakeSecurityKeyCredential(HWND window,
                                                const SecurityKeyOptions& options,
                                                const std::string& user_id_base64,
                                                const std::string& username,
                                                const std::string& display_name,
                                                std::string* error,
                                                std::string* error_code);

SecurityKeyAssertion GetSecurityKeyAssertion(HWND window, const SecurityKeyOptions& options,
                                             std::string* error, std::string* error_code);

}  // namespace chast_viewer
