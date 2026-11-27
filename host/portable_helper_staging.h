#pragma once

#include "portable_helper_lifecycle.h"

#include <string_view>

namespace chast::host {

// Copies the currently running, elevated host image into this instance's
// protected ProgramData directory and returns the matching service spec.
// This performs filesystem mutations and must only run after local elevation
// approval; it does not create or start an SCM service.
bool stage_portable_helper_service_image(
    PortableHelperServiceSpec* spec, std::string* error);

// Stages the same trusted image under a caller-selected, already-authorized
// instance ID. This binds the installed service name and recovery record to
// the instance scope captured by a local approval gate. The ID is still
// validated and the instance directory is created exclusively.
bool stage_portable_helper_service_image_for_instance(
    const PortableHelperInstanceId& instance_id,
    PortableHelperServiceSpec* spec, std::string* error);

// Requires a valid Authenticode chain and an exact SHA-256 signer-certificate
// thumbprint match. The thumbprint is public policy, normally set at configure
// time with CHAST_PORTABLE_HELPER_PUBLISHER_CERT_SHA256.
bool verify_portable_helper_publisher_signature(
    const std::wstring& image_path, std::string_view expected_thumbprint,
    std::string* error);
bool verify_configured_portable_helper_publisher_signature(
    const std::wstring& image_path, std::string* error);

namespace detail {

// Internal copy primitive exposed for isolated temp-directory regression
// tests. Production callers must use stage_portable_helper_service_image.
bool copy_portable_helper_image_file(const std::wstring& source_path,
                                    const std::wstring& target_path,
                                    std::string* error);

}  // namespace detail
}  // namespace chast::host
