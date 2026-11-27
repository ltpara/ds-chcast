#pragma once

#include "portable_helper_lifecycle.h"

#include <functional>
#include <vector>

namespace chast::host {

// A versioned, non-secret record in the protected instance directory. Its
// directory/file DACLs are the authorization boundary; the codec is strict so
// truncated or mismatched state cannot name another service or image.
bool encode_portable_helper_recovery_record(
    const PortableHelperServiceSpec& spec, std::vector<uint8_t>* encoded,
    std::string* error);
bool decode_portable_helper_recovery_record(
    const std::vector<uint8_t>& encoded, PortableHelperServiceSpec* spec,
    std::string* error);

bool portable_helper_recovery_record_path(
    const PortableHelperServiceSpec& spec, std::wstring* path,
    std::string* error);
bool write_portable_helper_recovery_record(
    const PortableHelperServiceSpec& spec, std::string* error);
bool read_portable_helper_recovery_record(
    const PortableHelperServiceSpec& expected,
    PortableHelperServiceSpec* actual, std::string* error);
bool read_portable_helper_recovery_record_for_instance(
    const PortableHelperInstanceId& instance_id,
    PortableHelperServiceSpec* actual, std::string* error);
bool remove_portable_helper_recovery_record(
    const PortableHelperServiceSpec& expected, std::string* error);

namespace detail {

using PortableHelperCleanupAction =
    std::function<bool(std::string* error)>;
using PortableHelperDirectoryAbsentCheck = std::function<bool(
    const std::wstring& directory, bool* absent, std::string* error)>;

// Removes the durable recovery record only for the final directory removal.
// If directory removal fails, the record is restored so a later lifecycle
// retry can still identify and clean this instance.
bool remove_recovery_record_then_directory(
    const std::wstring& directory,
    const PortableHelperCleanupAction& remove_record,
    const PortableHelperCleanupAction& remove_directory,
    const PortableHelperDirectoryAbsentCheck& directory_is_absent,
    const PortableHelperCleanupAction& restore_record,
    std::string* error);

}  // namespace detail

struct PortableHelperRecoveryReport {
  size_t inspected = 0;
  size_t active = 0;
  size_t cleaned = 0;
  size_t pending = 0;
  size_t invalid = 0;
  bool interrupted = false;
  std::vector<std::string> details;
};

using PortableHelperParentLivenessCheck = std::function<bool(
    const PortableHelperProcessIdentity& expected, bool* alive,
    std::string* error)>;
using PortableHelperRecoveryContinueCheck = std::function<bool()>;

// Processes already validated records. An exact, live parent keeps its
// service; an exited/replaced parent triggers verified stop/delete/image
// cleanup. Individual failures are recorded and do not abort later instances.
bool recover_portable_helper_records(
    const std::vector<PortableHelperServiceSpec>& records,
    const PortableHelperParentLivenessCheck& parent_is_alive,
    PortableHelperServiceBackend& backend,
    PortableHelperRecoveryReport* report, std::string* error,
    const PortableHelperRecoveryContinueCheck& should_continue = {});

// Requires an elevated administrator or SYSTEM token. Scans only the
// protected ProgramData\Chast\PortableHelper instance root.
bool scan_and_recover_portable_helper_instances(
    PortableHelperServiceBackend& backend,
    PortableHelperRecoveryReport* report, std::string* error,
    const PortableHelperRecoveryContinueCheck& should_continue = {});

}  // namespace chast::host
