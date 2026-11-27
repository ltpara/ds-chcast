#pragma once

#define NOMINMAX
#include <windows.h>

#include <chast/capture.h>
#include <chast/input.h>

#include "portable_helper_authorization.h"
#include "portable_helper_input_journal.h"
#include "portable_helper_lifecycle.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace chast::host {

// The service supplies the actual token/session process-creation policy. This
// layer owns the security-critical ordering around it: create the per-session
// pipe first, launch one process, bind its live identity, then exchange the
// capability and accept WorkerReady before returning a usable channel.
using PortableHelperSessionWorkerLauncher = std::function<bool(
    const PortableHelperServiceSpec& spec, const PortableHelperGrant& grant,
    HANDLE* process, std::string* error)>;

struct PortableHelperSessionWorkerLaunchArguments final {
  PortableHelperInstanceId instance_id{};
  PortableHelperConnectionId connection_id{};
  uint32_t broker_process_id = 0;
  uint64_t broker_creation_time = 0;
};

struct PortableHelperInputCleanupLaunchArguments final {
  PortableHelperInstanceId instance_id{};
  PortableHelperConnectionId connection_id{};
  uint64_t generation = 0;
  uint32_t broker_process_id = 0;
  uint64_t broker_creation_time = 0;
};

// Parses the exact, ordered command line emitted by the suspended launcher.
// No optional switches or trailing arguments are accepted.
bool parse_portable_helper_session_worker_command_line(
    const std::vector<std::wstring>& arguments,
    PortableHelperSessionWorkerLaunchArguments* parsed,
    std::string* error);

// Verifies that this is the signed, protected image for the exact helper
// instance and that the live Session 0 SYSTEM broker process still has the
// PID/creation time emitted by the launcher. This is a read-only preflight.
bool validate_portable_helper_session_worker_process_identity(
    const PortableHelperSessionWorkerLaunchArguments& arguments,
    PortableHelperProcessIdentity* worker_identity,
    PortableHelperProcessIdentity* broker_identity,
    std::string* error);

bool connect_portable_helper_session_worker_pipe(
    const PortableHelperSessionWorkerLaunchArguments& arguments,
    const PortableHelperProcessIdentity& broker_identity,
    uint32_t timeout_ms, std::unique_ptr<PortableHelperPipe>* channel,
    std::string* error);

bool parse_portable_helper_input_cleanup_command_line(
    const std::vector<std::wstring>& arguments,
    PortableHelperInputCleanupLaunchArguments* parsed, std::string* error);
bool validate_portable_helper_input_cleanup_process_identity(
    const PortableHelperInputCleanupLaunchArguments& arguments,
    PortableHelperServiceSpec* recorded_spec,
    PortableHelperProcessIdentity* worker_identity,
    PortableHelperProcessIdentity* broker_identity, std::string* error);
bool connect_portable_helper_input_cleanup_pipe(
    const PortableHelperInputCleanupLaunchArguments& arguments,
    const PortableHelperProcessIdentity& broker_identity,
    uint32_t timeout_ms, std::unique_ptr<PortableHelperPipe>* channel,
    std::string* error);

bool make_portable_helper_input_cleanup_request(
    const PortableHelperInputJournalScope& scope, uint64_t request_id,
    PortableHelperMessage* request, std::string* error);
bool validate_portable_helper_input_cleanup_request(
    const PortableHelperMessage& request,
    const PortableHelperInputJournalScope& expected_scope,
    std::string* error);
bool run_portable_helper_input_cleanup_worker(
    PortableHelperPipe& channel,
    const PortableHelperInputCleanupLaunchArguments& arguments,
    const PortableHelperProcessIdentity& expected_broker,
    const PortableHelperProcessIdentity& expected_worker,
    std::string* error);

// Imports the grant only after checking the live worker identity again and
// matching the authenticated pipe peer to the exact broker process.
bool accept_portable_helper_session_worker_grant(
    PortableHelperPipe& channel,
    const PortableHelperSessionWorkerLaunchArguments& arguments,
    const PortableHelperProcessIdentity& expected_broker,
    const PortableHelperProcessIdentity& expected_worker,
    PortableHelperAuthorization& authorization,
    PortableHelperGrant* grant, uint32_t timeout_ms, std::string* error);

// Opens a primary SYSTEM token from winlogon.exe in the exact interactive
// session bound to spec.parent. The helper service must be the staged image in
// Session 0 as LocalSystem; the target WTS user SID/logon SID and winlogon
// token/session are revalidated before the token is returned. This never
// rewrites TokenSessionId and has no user-token fallback. The caller owns the
// returned token handle and must revalidate the session after process creation.
bool open_portable_helper_session_worker_token(
    const PortableHelperServiceSpec& spec, HANDLE* primary_token,
    std::string* error);

// Creates the dedicated worker mode in the authorized session using the
// selector above. The process starts suspended while its image, token, session,
// parent and WTS user are rechecked; it is resumed only after those checks
// succeed. On failure, *worker_process is normally empty. If bounded cleanup
// cannot confirm child exit, ownership of the live process handle is returned
// through *worker_process so the caller can retry termination.
bool launch_portable_helper_session_worker_process(
    const PortableHelperServiceSpec& spec, const PortableHelperGrant& grant,
    HANDLE* worker_process, std::string* error);

// Launches the one-shot cleanup mode in the exact session SYSTEM token. The
// generation is an argument only for routing and scope checks; journal state
// is loaded by the child from the protected store after peer authentication.
bool launch_portable_helper_input_cleanup_process(
    const PortableHelperServiceSpec& spec,
    const PortableHelperInputJournalScope& scope,
    HANDLE* worker_process, std::string* error);

// Runs a bounded authenticated cleanup transaction. A lost ACK is failure to
// the caller and leaves the journal retryable; the released tombstone makes a
// later retry idempotent if the child completed just before its ACK was lost.
bool execute_portable_helper_authenticated_input_cleanup(
    const PortableHelperServiceSpec& spec,
    const PortableHelperInputJournalScope& scope, std::string* error);

// Applies one received typed input request while retaining the worker-side
// operation lease until InputInjector has attempted the SendInput operation.
// The injector must be owned by the authorized worker session.
bool apply_portable_helper_worker_input_request(
    PortableHelperAuthorization& authorization,
    chast::InputInjector& injector, const PortableHelperMessage& message,
    std::string* error);

struct PortableHelperCapturedFrame final {
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t frame_number = 0;
  std::vector<uint8_t> png;
};

inline constexpr size_t kPortableHelperMaxCapturedFrameBytes =
    64u * 1024u * 1024u;
inline constexpr size_t kPortableHelperFrameChunkMetadataBytes = 56;

// Splits a captured PNG into CaptureFrameChunk messages. Each message remains
// within the helper IPC payload limit and is bound to the exact grant and
// capture request ID.
bool make_portable_helper_capture_frame_chunks(
    const PortableHelperGrant& grant, uint64_t request_id,
    const PortableHelperCapturedFrame& frame,
    std::vector<PortableHelperMessage>* chunks, std::string* error);

// Reassembles an ordered set of chunks, enforcing exact scope/metadata,
// dimensions, total-size, per-chunk length, and PNG-signature checks.
bool reassemble_portable_helper_capture_frame_chunks(
    const PortableHelperGrant& grant, uint64_t request_id,
    const std::vector<PortableHelperMessage>& chunks,
    PortableHelperCapturedFrame* frame, std::string* error);

bool send_portable_helper_captured_frame(
    PortableHelperPipe& pipe, const PortableHelperGrant& grant,
    uint64_t request_id, const PortableHelperCapturedFrame& frame,
    uint32_t timeout_ms, std::string* error);
bool receive_portable_helper_captured_frame(
    PortableHelperPipe& pipe, const PortableHelperGrant& grant,
    uint64_t request_id, PortableHelperCapturedFrame* frame,
    uint32_t timeout_ms, std::string* error);

// Captures and encodes one immutable BGRA frame while holding the worker-side
// capture lease. The caller must create a CPU-readable capturer (for example
// CaptureBackend::GdiCpu) and frame transport must enforce its own bounded
// fragmentation limit.
bool capture_portable_helper_worker_frame(
    PortableHelperAuthorization& authorization,
    chast::IDxgiCapturer& capturer, const PortableHelperMessage& message,
    PortableHelperCapturedFrame* frame, std::string* error);

// Runs the strictly typed worker request loop. Adapters must be fully
// initialized before entry; WorkerReady is sent only after their presence is
// matched to the exact grant operation mask. Input is synchronously released
// when Stop is received or the authenticated pipe fails.
bool run_portable_helper_session_worker_request_loop(
    PortableHelperPipe& pipe, PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, chast::IDxgiCapturer* capturer,
    chast::InputInjector* injector, std::string* error);

// Starts and authenticates one session worker over its connection-scoped pipe.
// On success, ownership of *channel and *worker_process transfers to the
// caller. If startup fails and the child cannot be terminated, its process
// handle is still returned so the caller can retry cleanup; the channel is
// always closed on failure. This function does not itself install a service or
// choose which Windows token the launcher uses.
bool start_portable_helper_session_worker_channel(
    const PortableHelperServiceSpec& spec,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant,
    const PortableHelperSessionWorkerLauncher& launch_worker,
    uint32_t timeout_ms,
    std::unique_ptr<PortableHelperPipe>* channel,
    HANDLE* worker_process,
    PortableHelperProcessIdentity* worker_identity,
    std::string* error,
    PortableHelperSecret* worker_ready_proof = nullptr);

// Product launcher overload. It uses the fail-closed Win32 token selector and
// CreateProcessAsUser implementation above; authorization must already be in
// AwaitingWorkerReady, which callers may enter only after explicit local
// authorization.
bool start_portable_helper_session_worker_channel(
    const PortableHelperServiceSpec& spec,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint32_t timeout_ms,
    std::unique_ptr<PortableHelperPipe>* channel, HANDLE* worker_process,
    PortableHelperProcessIdentity* worker_identity, std::string* error,
    PortableHelperSecret* worker_ready_proof = nullptr);

}  // namespace chast::host
