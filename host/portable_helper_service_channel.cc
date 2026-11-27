#define NOMINMAX
#include "portable_helper_service_channel.h"

#include <winsvc.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "chast/machine_state_security.h"
#include "chast/security.h"
#include "portable_helper_worker_session.h"
#include "portable_helper_staging.h"

namespace chast::host {
namespace {

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
  return false;
}

bool same_identity(const PortableHelperProcessIdentity& actual,
                   const PortableHelperProcessIdentity& expected) {
  return actual.process_id == expected.process_id &&
      actual.session_id == expected.session_id &&
      actual.creation_time == expected.creation_time &&
      !expected.image_path.empty() &&
      _wcsicmp(actual.image_path.c_str(), expected.image_path.c_str()) == 0 &&
      actual.user_sid == expected.user_sid &&
      actual.logon_sid == expected.logon_sid;
}

class ScHandle final {
 public:
  explicit ScHandle(SC_HANDLE value = nullptr) : value_(value) {}
  ~ScHandle() { if (value_ != nullptr) CloseServiceHandle(value_); }
  ScHandle(const ScHandle&) = delete;
  ScHandle& operator=(const ScHandle&) = delete;
  SC_HANDLE get() const { return value_; }

 private:
  SC_HANDLE value_ = nullptr;
};

bool query_service_configuration(SC_HANDLE service,
                                const PortableHelperServiceSpec& spec,
                                std::string* error) {
  DWORD needed = 0;
  QueryServiceConfigW(service, nullptr, 0, &needed);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0)
    return fail(error, "cannot size portable helper service configuration");
  std::vector<uint8_t> storage(needed);
  auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data());
  if (!QueryServiceConfigW(service, config, needed, &needed))
    return fail(error, "cannot read portable helper service configuration");

  const std::wstring expected_command = L"\"" + spec.executable_path +
      L"\" --service-name " + spec.service_name;
  const bool system_account = config->lpServiceStartName != nullptr &&
      (_wcsicmp(config->lpServiceStartName, L"LocalSystem") == 0 ||
       _wcsicmp(config->lpServiceStartName, L"NT AUTHORITY\\SYSTEM") == 0);
  if (config->dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
      config->dwStartType != SERVICE_DEMAND_START || !system_account ||
      config->lpBinaryPathName == nullptr ||
      _wcsicmp(config->lpBinaryPathName, expected_command.c_str()) != 0)
    return fail(error, "SCM helper configuration does not match its instance spec");
  return true;
}

void save_thread_error(std::mutex& mutex, std::string& destination,
                       const std::string& value) {
  std::lock_guard lock(mutex);
  if (destination.empty()) destination = value;
}

}  // namespace

PortableHelperServiceConnection::~PortableHelperServiceConnection() {
  Reset();
}

PortableHelperServiceConnection::PortableHelperServiceConnection(
    PortableHelperServiceConnection&& other) noexcept
    : channel(std::move(other.channel)),
      service_process(std::exchange(other.service_process, nullptr)),
      service_identity(std::move(other.service_identity)),
      instance_id(std::exchange(other.instance_id, {})) {}

PortableHelperServiceConnection& PortableHelperServiceConnection::operator=(
    PortableHelperServiceConnection&& other) noexcept {
  if (this == &other) return *this;
  Reset();
  channel = std::move(other.channel);
  service_process = std::exchange(other.service_process, nullptr);
  service_identity = std::move(other.service_identity);
  instance_id = std::exchange(other.instance_id, {});
  return *this;
}

void PortableHelperServiceConnection::Reset() noexcept {
  channel.reset();
  if (service_process != nullptr) {
    CloseHandle(service_process);
    service_process = nullptr;
  }
  service_identity = {};
  instance_id = {};
}

bool connect_portable_helper_service_channel(
    const PortableHelperServiceSpec& spec, uint32_t timeout_ms,
    PortableHelperServiceConnection* connection, std::string* error) {
  if (error != nullptr) error->clear();
  if (connection == nullptr)
    return fail(error, "missing portable helper connection output");

  PortableHelperServiceSpec canonical;
  if (!make_portable_helper_service_spec(spec.instance_id,
                                        spec.executable_path, spec.parent,
                                        &canonical, error) ||
      canonical.service_name != spec.service_name ||
      _wcsicmp(canonical.staging_directory.c_str(),
               spec.staging_directory.c_str()) != 0 ||
      _wcsicmp(canonical.executable_path.c_str(),
               spec.executable_path.c_str()) != 0)
    return fail(error, "portable helper service spec is not canonical");

  PortableHelperProcessIdentity current_parent;
  std::string parent_error;
  if (!current_portable_helper_process_identity(&current_parent,
                                                &parent_error) ||
      !same_identity(current_parent, spec.parent))
    return fail(error, parent_error.empty()
                           ? "current process does not match the authorized helper parent"
                           : parent_error);

  std::string trust_error;
  if (!chast::machine_state_file_trusted(spec.executable_path, &trust_error) ||
      !verify_configured_portable_helper_publisher_signature(
          spec.executable_path, &trust_error))
    return fail(error, "portable helper service image is not trusted: " +
                           trust_error);

  ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (manager.get() == nullptr)
    return fail(error, "cannot open SCM to query the portable helper");
  ScHandle service(OpenServiceW(manager.get(), spec.service_name.c_str(),
                                SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
  if (service.get() == nullptr)
    return fail(error, "cannot open the instance-derived portable helper service");
  if (!query_service_configuration(service.get(), spec, error)) return false;

  SERVICE_STATUS_PROCESS status{};
  DWORD bytes = 0;
  if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                            reinterpret_cast<BYTE*>(&status), sizeof(status),
                            &bytes) ||
      status.dwCurrentState != SERVICE_RUNNING || status.dwProcessId == 0)
    return fail(error, "portable helper SCM service is not running with a process");

  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE |
                                   PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, status.dwProcessId);
  if (process == nullptr)
    return fail(error, "cannot open the SCM-reported portable helper process");
  PortableHelperProcessIdentity identity;
  std::string identity_error;
  bool identity_ok = GetProcessId(process) == status.dwProcessId &&
      query_portable_helper_process_identity(status.dwProcessId, &identity,
                                             &identity_error);
  FILETIME created{}, exited{}, kernel{}, user{};
  identity_ok = identity_ok && GetProcessTimes(process, &created, &exited,
                                                &kernel, &user) != FALSE &&
      identity.creation_time ==
          ((static_cast<uint64_t>(created.dwHighDateTime) << 32) |
           created.dwLowDateTime) &&
      WaitForSingleObject(process, 0) == WAIT_TIMEOUT &&
      identity.session_id == 0 && identity.user_sid == L"S-1-5-18" &&
      _wcsicmp(identity.image_path.c_str(), spec.executable_path.c_str()) == 0;
  if (!identity_ok) {
    CloseHandle(process);
    return fail(error, identity_error.empty()
                           ? "SCM process identity does not match the helper image"
                           : identity_error);
  }

  auto channel = PortableHelperPipe::ConnectClient(
      spec.instance_id, identity, timeout_ms, error);
  if (!channel) {
    CloseHandle(process);
    return false;
  }
  PortableHelperProcessIdentity authenticated_peer;
  if (!channel->AuthenticatedPeer(&authenticated_peer, error) ||
      authenticated_peer.process_id != identity.process_id ||
      authenticated_peer.creation_time != identity.creation_time ||
      WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
    channel.reset();
    CloseHandle(process);
    return fail(error, "connected named-pipe server is not the live SCM process");
  }

  connection->Reset();
  connection->channel = std::move(channel);
  connection->service_process = process;
  connection->service_identity = std::move(identity);
  connection->instance_id = spec.instance_id;
  return true;
}

bool request_portable_helper_service_stop(
    PortableHelperServiceConnection& connection, uint32_t timeout_ms,
    std::string* error) {
  std::lock_guard rpc_lock(connection.rpc_mutex);
  if (error != nullptr) error->clear();
  if (connection.service_process == nullptr)
    return fail(error, "portable helper connection has no SCM process handle");
  const DWORD current = WaitForSingleObject(connection.service_process, 0);
  if (current == WAIT_OBJECT_0) {
    connection.channel.reset();
    return true;
  }
  if (current != WAIT_TIMEOUT)
    return fail(error, "cannot query portable helper process before stop");

  std::string control_error;
  bool acknowledged = false;
  if (connection.channel) {
    PortableHelperMessage stop;
    stop.type = PortableHelperMessageType::Stop;
    do {
      if (!secure_random(reinterpret_cast<uint8_t*>(&stop.request_id),
                         sizeof(stop.request_id))) {
        control_error = "cannot generate a helper stop request ID";
        break;
      }
    } while (stop.request_id == 0);
    const uint64_t request_id = stop.request_id;
    if (control_error.empty() &&
        connection.channel->Send(std::move(stop), timeout_ms, &control_error)) {
      PortableHelperMessage response;
      if (connection.channel->Receive(&response, timeout_ms, &control_error) &&
          response.type == PortableHelperMessageType::Acknowledgement &&
          response.request_id == request_id && response.payload ==
              std::vector<uint8_t>{1}) {
        acknowledged = true;
      } else if (control_error.empty()) {
        control_error = "helper returned an invalid stop acknowledgement";
      }
    }
  } else {
    control_error = "portable helper control channel is already closed";
  }

  const DWORD waited = WaitForSingleObject(connection.service_process,
                                            timeout_ms);
  if (waited != WAIT_OBJECT_0) {
    if (error != nullptr) {
      *error = waited == WAIT_TIMEOUT
          ? "portable helper did not exit before the stop deadline"
          : "cannot wait for portable helper process exit";
      if (!control_error.empty()) *error += "; " + control_error;
    }
    return false;
  }
  connection.channel.reset();
  if (!acknowledged)
    return fail(error, control_error.empty()
                           ? "portable helper exited without a stop acknowledgement"
                           : control_error);
  return true;
}

namespace {

bool new_control_request_id(uint64_t* request_id, std::string* error) {
  if (request_id == nullptr)
    return fail(error, "missing portable helper request ID output");
  *request_id = 0;
  do {
    if (!secure_random(reinterpret_cast<uint8_t*>(request_id),
                       sizeof(*request_id)))
      return fail(error, "cannot generate a portable helper request ID");
  } while (*request_id == 0);
  return true;
}

bool expect_acknowledgement(PortableHelperPipe& channel, uint64_t request_id,
                            uint32_t timeout_ms, std::string* error) {
  PortableHelperMessage response;
  if (!channel.Receive(&response, timeout_ms, error)) return false;
  if (response.type != PortableHelperMessageType::Acknowledgement ||
      response.request_id != request_id || response.payload.size() != 1 ||
      response.payload[0] != 1)
    return fail(error, "portable helper returned a rejected or uncorrelated acknowledgement");
  return true;
}

void revoke_parent_forwarding_gate(
    PortableHelperServiceConnection& connection,
    PortableHelperAuthorization& authorization, const std::string& reason,
    std::string* error) {
  std::string revoke_error;
  // The parent authorization gates messages sent to the helper; it does not
  // inject input itself. The worker/service owns the actual desktop release.
  (void)authorization.Revoke([] { return true; }, [] { return true; },
                             &revoke_error);
  connection.channel.reset();
  if (error != nullptr) {
    *error = reason;
    if (!revoke_error.empty()) *error += "; local authorization revoked: " + revoke_error;
  }
}

}  // namespace

bool start_portable_helper_session_worker(
    PortableHelperServiceConnection& connection,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint32_t timeout_ms,
    std::string* error, PortableHelperSecret* worker_ready_proof) {
  std::lock_guard rpc_lock(connection.rpc_mutex);
  if (error != nullptr) error->clear();
  if (worker_ready_proof != nullptr)
    SecureZeroMemory(worker_ready_proof->data(), worker_ready_proof->size());
  if (authorization.state() !=
      PortableHelperAuthorizationState::AwaitingWorkerReady) {
    if (error != nullptr)
      *error = "portable helper start requires a locally authorized pending grant";
    return false;
  }
  if (!connection.channel ||
      connection.instance_id == PortableHelperInstanceId{} ||
      connection.instance_id != grant.instance_id) {
    revoke_parent_forwarding_gate(connection, authorization,
        "portable helper start connection does not match the local grant",
        error);
    return false;
  }
  if (connection.service_process != nullptr &&
      WaitForSingleObject(connection.service_process, 0) != WAIT_TIMEOUT) {
    revoke_parent_forwarding_gate(connection, authorization,
        "portable helper service process is no longer running", error);
    return false;
  }

  uint64_t request_id = 0;
  PortableHelperMessage request;
  std::string rpc_error;
  if (!new_control_request_id(&request_id, &rpc_error) ||
      !make_portable_helper_session_worker_start_request(
          grant, request_id, &request, &rpc_error) ||
      !connection.channel->Send(std::move(request), timeout_ms, &rpc_error) ||
      !expect_acknowledgement(*connection.channel, request_id, timeout_ms,
                              &rpc_error)) {
    revoke_parent_forwarding_gate(connection, authorization,
        rpc_error.empty() ? "portable helper worker start failed" : rpc_error,
        error);
    return false;
  }

  PortableHelperMessage ready;
  if (!connection.channel->Receive(&ready, timeout_ms, &rpc_error)) {
    revoke_parent_forwarding_gate(connection, authorization,
        rpc_error.empty() ? "portable helper WorkerReady proof was not received"
                          : rpc_error,
        error);
    return false;
  }
  if (ready.type != PortableHelperMessageType::WorkerReady ||
      ready.instance_id != grant.instance_id ||
      ready.request_id != grant.generation ||
      ready.payload.size() != PortableHelperSecret{}.size()) {
    if (!ready.payload.empty())
      SecureZeroMemory(ready.payload.data(), ready.payload.size());
    revoke_parent_forwarding_gate(connection, authorization,
        "portable helper WorkerReady proof has an invalid scope or size", error);
    return false;
  }
  PortableHelperSecret proof{};
  std::copy(ready.payload.begin(), ready.payload.end(), proof.begin());
  SecureZeroMemory(ready.payload.data(), ready.payload.size());
  const bool confirmed = authorization.ConfirmBrokerManagedWorkerReady(
      grant, proof, &rpc_error);
  if (!confirmed) {
    SecureZeroMemory(proof.data(), proof.size());
    revoke_parent_forwarding_gate(connection, authorization,
        rpc_error.empty() ? "portable helper WorkerReady proof was rejected"
                          : rpc_error,
        error);
    return false;
  }
  if (worker_ready_proof != nullptr) *worker_ready_proof = proof;
  SecureZeroMemory(proof.data(), proof.size());
  return true;
}

bool stop_portable_helper_session_worker(
    PortableHelperServiceConnection& connection,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint32_t timeout_ms,
    std::string* error) {
  std::lock_guard rpc_lock(connection.rpc_mutex);
  if (error != nullptr) error->clear();
  if (authorization.state() != PortableHelperAuthorizationState::Ready) {
    if (error != nullptr)
      *error = "portable helper stop requires a ready session grant";
    return false;
  }
  if (!connection.channel || connection.instance_id != grant.instance_id) {
    revoke_parent_forwarding_gate(connection, authorization,
        "portable helper stop connection does not match the active grant",
        error);
    return false;
  }
  if (connection.service_process != nullptr &&
      WaitForSingleObject(connection.service_process, 0) != WAIT_TIMEOUT) {
    revoke_parent_forwarding_gate(connection, authorization,
        "portable helper service process exited before worker stop", error);
    return false;
  }

  uint64_t request_id = 0;
  std::string rpc_error;
  PortableHelperMessage stop;
  if (!new_control_request_id(&request_id, &rpc_error)) {
    revoke_parent_forwarding_gate(connection, authorization, rpc_error, error);
    return false;
  }
  stop.type = PortableHelperMessageType::SessionWorkerStop;
  stop.request_id = request_id;
  stop.payload.insert(stop.payload.end(), grant.connection_id.begin(),
                      grant.connection_id.end());
  for (unsigned shift = 0; shift < 64; shift += 8)
    stop.payload.push_back(static_cast<uint8_t>(grant.generation >> shift));
  if (!connection.channel->Send(std::move(stop), timeout_ms, &rpc_error) ||
      !expect_acknowledgement(*connection.channel, request_id, timeout_ms,
                              &rpc_error)) {
    revoke_parent_forwarding_gate(connection, authorization,
        rpc_error.empty() ? "portable helper worker stop was not confirmed"
                          : rpc_error,
        error);
    return false;
  }

  if (!authorization.Revoke([] { return true; }, [] { return true; },
                            &rpc_error)) {
    connection.channel.reset();
    if (error != nullptr)
      *error = rpc_error.empty()
          ? "portable helper stopped but the parent authorization gate could not be revoked"
          : rpc_error;
    return false;
  }
  return true;
}

bool request_portable_helper_capture_frame(
    PortableHelperServiceConnection& connection,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t request_id,
    uint32_t timeout_ms, PortableHelperCapturedFrame* frame,
    std::string* error) {
  std::lock_guard rpc_lock(connection.rpc_mutex);
  if (error != nullptr) error->clear();
  if (frame == nullptr || !connection.channel ||
      connection.instance_id != grant.instance_id ||
      authorization.state() != PortableHelperAuthorizationState::Ready)
    return fail(error, "portable helper capture request requires a ready matching connection");
  *frame = {};
  PortableHelperMessage request;
  std::string rpc_error;
  if (!make_portable_helper_capture_request(authorization, grant, request_id,
                                             &request, &rpc_error)) {
    if (error != nullptr) *error = rpc_error;
    return false;
  }
  if (!connection.channel->Send(std::move(request), timeout_ms, &rpc_error) ||
      !receive_portable_helper_captured_frame(
          *connection.channel, grant, request_id, frame, timeout_ms,
          &rpc_error)) {
    if (!frame->png.empty()) {
      SecureZeroMemory(frame->png.data(), frame->png.size());
      frame->png.clear();
    }
    revoke_parent_forwarding_gate(connection, authorization,
        rpc_error.empty() ? "portable helper capture RPC failed" : rpc_error,
        error);
    return false;
  }
  return true;
}

bool request_portable_helper_input_event(
    PortableHelperServiceConnection& connection,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t request_id,
    const chast::InputEvent& event, uint32_t timeout_ms,
    std::string* error) {
  std::lock_guard rpc_lock(connection.rpc_mutex);
  if (error != nullptr) error->clear();
  if (!connection.channel || connection.instance_id != grant.instance_id ||
      authorization.state() != PortableHelperAuthorizationState::Ready)
    return fail(error, "portable helper input request requires a ready matching connection");
  PortableHelperMessage request;
  std::string rpc_error;
  if (!make_portable_helper_input_request(authorization, grant, request_id,
                                          event, &request, &rpc_error)) {
    if (error != nullptr) *error = rpc_error;
    return false;
  }
  if (!connection.channel->Send(std::move(request), timeout_ms, &rpc_error) ||
      !expect_acknowledgement(*connection.channel, request_id, timeout_ms,
                              &rpc_error)) {
    revoke_parent_forwarding_gate(connection, authorization,
        rpc_error.empty() ? "portable helper input RPC failed" : rpc_error,
        error);
    return false;
  }
  return true;
}

bool run_portable_helper_service_control_channel(
    std::unique_ptr<PortableHelperPipe> server_channel,
    const PortableHelperProcessIdentity& expected_parent,
    HANDLE parent_process, HANDLE service_stop_event,
    const std::function<void()>& request_service_stop,
    std::string* error,
    PortableHelperServiceControlRequestHandler request_handler) {
  if (error != nullptr) error->clear();
  if (!server_channel || parent_process == nullptr ||
      parent_process == INVALID_HANDLE_VALUE || service_stop_event == nullptr ||
      !request_service_stop)
    return fail(error, "portable helper control channel inputs are incomplete");
  if (WaitForSingleObject(service_stop_event, 0) == WAIT_OBJECT_0 ||
      WaitForSingleObject(parent_process, 0) == WAIT_OBJECT_0)
    return true;

  std::string accept_error;
  if (!server_channel->Accept(expected_parent, 15000, &accept_error))
    return fail(error, "portable helper parent pipe authentication failed: " +
                           accept_error);

  HANDLE reader_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (reader_done == nullptr)
    return fail(error, "cannot create portable helper pipe-reader event");
  std::atomic<bool> orderly_stop{false};
  std::mutex error_mutex;
  std::string reader_error;
  PortableHelperPipe* const pipe = server_channel.get();
  std::thread reader([&, pipe] {
    auto done = [&] { SetEvent(reader_done); };
    for (;;) {
      PortableHelperMessage request;
      std::string read_error;
      if (!pipe->Receive(&request, INFINITE, &read_error)) {
        save_thread_error(error_mutex, reader_error,
                          read_error.empty() ? "parent control pipe closed"
                                             : read_error);
        done();
        return;
      }

      if (request_handler) {
        bool stop_service = false;
        std::string handler_error;
        bool handled = false;
        try {
          handled = request_handler(*pipe, request, &stop_service,
                                    &handler_error);
        } catch (...) {
          handler_error = "helper control handler raised an exception";
        }
        if (!handled) {
          save_thread_error(error_mutex, reader_error,
                            handler_error.empty()
                                ? "helper control handler rejected a request"
                                : handler_error);
          done();
          return;
        }
        if (stop_service && request.type != PortableHelperMessageType::Stop) {
          save_thread_error(error_mutex, reader_error,
                            "helper control handler requested shutdown for a non-Stop message");
          done();
          return;
        }
        if (stop_service) {
          orderly_stop.store(true, std::memory_order_release);
          try {
            request_service_stop();
          } catch (...) {
            save_thread_error(error_mutex, reader_error,
                              "service stop callback raised an exception");
          }
          done();
          return;
        }
        continue;
      }

      if (request.type != PortableHelperMessageType::Status &&
          request.type != PortableHelperMessageType::Stop) {
        save_thread_error(error_mutex, reader_error,
                          "parent sent an unsupported helper control message");
        done();
        return;
      }
      PortableHelperMessage response;
      response.type = PortableHelperMessageType::Acknowledgement;
      response.request_id = request.request_id;
      response.payload = {1};
      std::string send_error;
      if (!pipe->Send(std::move(response), 1000, &send_error)) {
        save_thread_error(error_mutex, reader_error,
                          send_error.empty() ? "cannot acknowledge helper control"
                                             : send_error);
        done();
        return;
      }
      if (request.type == PortableHelperMessageType::Stop) {
        orderly_stop.store(true, std::memory_order_release);
        try {
          request_service_stop();
        } catch (...) {
          save_thread_error(error_mutex, reader_error,
                            "service stop callback raised an exception");
        }
        done();
        return;
      }
    }
  });

  HANDLE waits[] = {service_stop_event, parent_process, reader_done};
  const DWORD waited = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
  const DWORD wait_error = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
  // Destroying the pipe cancels a pending overlapped Receive; then joining the
  // reader ensures no callback can outlive the service channel.
  server_channel.reset();
  if (reader.joinable()) reader.join();
  CloseHandle(reader_done);

  if (waited == WAIT_FAILED)
    return fail(error, "portable helper control wait failed (Win32 error " +
                           std::to_string(wait_error) + ")");
  if (waited == WAIT_OBJECT_0 || waited == WAIT_OBJECT_0 + 1 ||
      (waited == WAIT_OBJECT_0 + 2 &&
       orderly_stop.load(std::memory_order_acquire)))
    return true;

  std::lock_guard lock(error_mutex);
  return fail(error, reader_error.empty()
                         ? "portable helper control channel ended unexpectedly"
                         : reader_error);
}

bool Win32PortableHelperServiceBackend::Connect(
    const PortableHelperServiceSpec& spec, uint32_t timeout_ms,
    std::unique_ptr<PortableHelperServiceConnection>* connection,
    std::string* error) {
  if (connection == nullptr)
    return fail(error, "missing portable helper connection output");
  auto connected = std::make_unique<PortableHelperServiceConnection>();
  if (!connect_portable_helper_service_channel(spec, timeout_ms,
                                               connected.get(), error))
    return false;
  *connection = std::move(connected);
  return true;
}

}  // namespace chast::host
