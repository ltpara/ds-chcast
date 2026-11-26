#pragma once

#include <string>

namespace chast {

// Creates or migrates a machine-state tree to protected SYSTEM/Administrators
// DACLs. Migration requires an exclusive no-follow handle and a trusted owner;
// it replaces inherited/explicit ACLs without following or deleting objects.
bool harden_machine_state_directory(const std::wstring& path, std::string* error);

// Checks the machine-state directory without changing it.
bool machine_state_directory_trusted(const std::wstring& path, std::string* error);

bool machine_state_file_trusted(const std::wstring& path, std::string* error);

// Validates an already-open no-follow file handle. Callers must also validate
// the containing directory; this avoids reopening a file by name when a shared
// handle would otherwise cause a sharing conflict.
bool machine_state_file_handle_trusted(void* file_handle, std::string* error);

// Secures one file beneath a trusted machine-state directory. Missing files are
// accepted so callers can prepare a path before its first atomic write.
bool harden_machine_state_file(const std::wstring& path, std::string* error);

bool process_token_is_elevated();
bool process_is_elevated_administrator();
bool process_can_harden_machine_state();

}  // namespace chast
