#pragma once

#include "portable_helper_protocol.h"

#include <mutex>
#include <memory>
#include <string>
#include <vector>

namespace chast::host {

struct PortableHelperServiceConnection;

struct PortableHelperServiceSpec {
  PortableHelperInstanceId instance_id{};
  std::wstring service_name;
  std::wstring staging_directory;
  std::wstring executable_path;
  PortableHelperProcessIdentity parent;
};

bool generate_portable_helper_instance_id(PortableHelperInstanceId* instance_id,
                                          std::string* error);

bool portable_helper_staging_paths(
    const PortableHelperInstanceId& instance_id,
    std::wstring* staging_directory, std::wstring* executable_path,
    std::string* error);

bool make_portable_helper_service_spec(
    const PortableHelperInstanceId& instance_id,
    const std::wstring& executable_path,
    const PortableHelperProcessIdentity& parent,
    PortableHelperServiceSpec* spec, std::string* error);

// Parses the exact non-secret StartService argument set and binds it to the
// instance-derived service name and this service image path.
bool parse_portable_helper_service_start_arguments(
    const std::wstring& service_name,
    const std::vector<std::wstring>& arguments,
    const std::wstring& executable_path,
    PortableHelperServiceSpec* spec, std::string* error);

// A unique, instance-derived SCM name. It can never equal the installed
// "chast-host" service name.
std::wstring portable_helper_service_name(
    const PortableHelperInstanceId& instance_id);

// CreateExclusive must fail without changing a pre-existing service. Stop must
// succeed when this instance is already stopped. DeleteIfOwned must verify the
// exact instance-derived service name and configured image path before deletion.
class PortableHelperServiceBackend {
 public:
  virtual ~PortableHelperServiceBackend() = default;
  virtual bool CreateExclusive(const PortableHelperServiceSpec& spec,
                               std::string* error) = 0;
  virtual bool Start(const PortableHelperServiceSpec& spec,
                     std::string* error) = 0;
  virtual bool Connect(
      const PortableHelperServiceSpec& spec, uint32_t timeout_ms,
      std::unique_ptr<PortableHelperServiceConnection>* connection,
      std::string* error) = 0;
  virtual bool Stop(const PortableHelperServiceSpec& spec,
                    std::string* error) = 0;
  virtual bool DeleteIfOwned(const PortableHelperServiceSpec& spec,
                             std::string* error) = 0;
  // Removes only this instance's staged image and empty directory, and only
  // after the instance-derived service is fully absent from SCM.
  virtual bool RemoveStagedImage(const PortableHelperServiceSpec& spec,
                                 std::string* error) = 0;
};

// Windows SCM adapter. It has no constructor-side effects; mutations happen
// only when called by PortableHelperServiceLifecycle. StartService receives
// the instance ID and parent process identity as non-secret startup arguments.
class Win32PortableHelperServiceBackend final
    : public PortableHelperServiceBackend {
 public:
  bool CreateExclusive(const PortableHelperServiceSpec& spec,
                       std::string* error) override;
  bool Start(const PortableHelperServiceSpec& spec,
             std::string* error) override;
  bool Connect(
      const PortableHelperServiceSpec& spec, uint32_t timeout_ms,
      std::unique_ptr<PortableHelperServiceConnection>* connection,
      std::string* error) override;
  bool Stop(const PortableHelperServiceSpec& spec,
            std::string* error) override;
  bool DeleteIfOwned(const PortableHelperServiceSpec& spec,
                     std::string* error) override;
  bool RemoveStagedImage(const PortableHelperServiceSpec& spec,
                         std::string* error) override;
};

enum class PortableHelperServiceState {
  NotCreated,
  Created,
  Running,
  CleanupPending,
  Removed,
};

// Transaction coordinator for one temporary helper service. Cleanup is
// explicit and repeatable; a failed stop prevents deletion, and failed removal
// remains visible as CleanupPending for a later retry. Running means the SCM
// reports SERVICE_RUNNING and an authenticated parent control channel; it is
// not the authenticated desktop worker-ready gate.
class PortableHelperServiceLifecycle final {
 public:
  PortableHelperServiceLifecycle(PortableHelperServiceSpec spec,
                                 PortableHelperServiceBackend& backend);
  ~PortableHelperServiceLifecycle();
  PortableHelperServiceLifecycle(const PortableHelperServiceLifecycle&) = delete;
  PortableHelperServiceLifecycle& operator=(
      const PortableHelperServiceLifecycle&) = delete;

  bool CreateAndStart(std::string* error);
  bool Stop(std::string* error);
  bool Cleanup(std::string* error);
  PortableHelperServiceState state() const;
  std::string last_error() const;
  // Valid only while active. Callers serialize access with Stop/Cleanup and
  // revoke desktop authorization before stopping the service.
  PortableHelperServiceConnection* connection() const;

 private:
  bool stop_locked(std::string* error);
  bool cleanup_locked(std::string* error);
  void set_error_locked(const std::string& error, std::string* output);

  PortableHelperServiceSpec spec_;
  PortableHelperServiceBackend& backend_;
  std::unique_ptr<PortableHelperServiceConnection> connection_;
  mutable std::mutex mutex_;
  PortableHelperServiceState state_ = PortableHelperServiceState::NotCreated;
  bool created_ = false;
  bool running_ = false;
  bool staged_ = true;
  std::string last_error_;
};

}  // namespace chast::host
