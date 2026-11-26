#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <future>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "chast/security.h"
#include "chast/decoder.h"
#include "clipboard_stub.h"
#include "host_media_stub.h"
#include "service_control.h"
#include "elevation_handoff.h"
#include "portable_helper_protocol.h"
#include "portable_helper_authorization.h"
#include "portable_helper_input_journal.h"
#include "portable_helper_input_journal_store.h"
#include "portable_helper_supervisor.h"
#include "portable_helper_win32_supervisor.h"
#include "portable_helper_service_channel.h"
#include "portable_helper_service_worker.h"
#include "portable_helper_worker_session.h"
#include "platform.h"
#include "chast/user_context.h"
#include "chast/png.h"
#include "chast/machine_state_security.h"

namespace {
using namespace chast;
using namespace std::chrono_literals;
int failures = 0;

void require(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) throw std::runtime_error(message);
}

bool begin_local_approval_test_grant(
    chast::host::PortableHelperAuthorization& authorization,
    const chast::host::PortableHelperInstanceId& instance_id,
    const chast::host::PortableHelperConnectionId& connection_id,
    uint32_t windows_session_id, uint32_t operations,
    chast::host::PortableHelperGrant* grant, std::string* error) {
    chast::host::PortableHelperLocalApprovalGate gate;
    chast::host::PortableHelperPendingApproval pending;
    chast::host::PortableHelperLocalApprovalReceipt receipt;
    if (!gate.QueueRemoteRequest(instance_id, connection_id, windows_session_id,
                                 operations, &pending, error)) {
        return false;
    }
    const bool approved = gate.Approve(
        std::move(pending), operations,
        [](const chast::host::PortableHelperApprovalRequestView& request,
           uint32_t approved_operations, std::string* revalidation_error) {
            if (request.windows_session_id == 0 ||
                request.requested_operations != approved_operations) {
                if (revalidation_error != nullptr)
                    *revalidation_error = "test local consent context changed";
                return false;
            }
            return true;
        },
        &receipt, error);
    return approved &&
        authorization.BeginGrant(std::move(receipt), grant, error);
}

template<class Predicate> bool until(Predicate predicate, std::chrono::milliseconds timeout = 3s) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
uint16_t free_port() {
    const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int bound = bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    int size = sizeof(addr);
    const int read = getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &size);
    closesocket(sock);
    require(bound == 0 && read == 0, "allocate loopback port");
    return ntohs(addr.sin_port);
}

struct Fixture {
    HostServerConfig config;
    std::shared_ptr<host_test::MediaState> media;
    std::unique_ptr<HostServer> host;
    std::filesystem::path root;
    uint16_t port = free_port();
    Fixture() {
        root = std::filesystem::temp_directory_path() /
            ("chast-parity-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(port));
        std::filesystem::create_directories(root);
        config.settings.bind_address = "127.0.0.1";
        config.settings.port = port;
        config.settings.carrier = config.settings.tunnel = false;
        config.settings.max_viewers = 8;
        config.settings.auth_mode = HostAuthMode::Password;
        config.settings.password_hash = hash_password("parity-test-password");
        config.settings.file_receive_directory = root.string();
        config.settings.audit_spool = (root / "audit.jsonl").string();
        config.settings.allow_input = config.settings.allow_audio = true;
        config.settings.allow_file_transfer = config.settings.allow_clipboard = true;
        config.settings.allow_remote_config = config.settings.allow_terminal = true;
        config.settings.allow_session_select = config.settings.allow_os_login = true;
        config.service_mode = true;
        config.power_request_factory = [](bool, std::string*) {
            return std::make_unique<ISessionPowerRequest>();
        };
        config.first_client_timeout_ms = 0;
        config.log = [](const std::string& s) { std::printf("  host: %s\n", s.c_str()); };
        config.list_sessions = [](std::vector<RemoteSession>& sessions, std::string*) {
            sessions = {{1, "test-user", true, true}, {2, "other-user", true, false}};
            return true;
        };
        config.select_session = [](uint32_t, std::string* error) {
            *error = "test backend refusal";
            return false;
        };
        config.os_login_state = [] { return OsLoginStatus{OsLoginState::Locked, {}}; };
        config.os_login = [](const std::string&, std::string*) { return true; };
        config.host_control = [](HostAction, bool, std::string*) { return true; };
        config.send_sas = [](std::string*) { return true; };
        use_empty_clipboard(config);
        media = host_test::use_media(config);
    }
    ~Fixture() {
        if (host) host->stop();
        host.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void start() {
        std::string error;
        host = HostServer::create(config, &error);
        if (!host) throw std::runtime_error("host create: " + error);
        host->start();
        require(until([&] { return host->status().state == HostState::Listening; }), "host listening");
    }
    std::unique_ptr<QuicSession> connect(SessionMode mode = SessionMode::Desktop,
            PermissionMask mask = kPermissionAll, std::string ref = {},
            uint16_t display = kDisplayAuto, ServerInfo* description = nullptr,
            VideoCodecMask decoders = kVideoCodecAll,
            std::optional<StreamCodecPreference> preference = {},
            VideoCodecMask decoders_444 = 0,
            VideoChroma prefer_chroma = VideoChroma::Yuv420) {
        QuicClientConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = port;
        cfg.password = "parity-test-password";
        cfg.pinned_fingerprint = host->status().fingerprint;
        cfg.requested_permissions = mask;
        cfg.session_mode = mode;
        cfg.decoding_codecs = decoders;
        cfg.decoding_444 = decoders_444;
        cfg.prefer_chroma = prefer_chroma;
        cfg.codec_preference = preference.value_or(config.settings.codec == VideoCodec::Hevc ?
            StreamCodecPreference::Hevc : config.settings.codec == VideoCodec::Av1 ?
            StreamCodecPreference::Av1 : StreamCodecPreference::H264);
        cfg.audit_ref = std::move(ref);
        std::string error;
        auto viewer = QuicSession::create_client(cfg, &error);
        if (!viewer) throw std::runtime_error("client create: " + error);
        Error result = viewer->connect(6s);
        if (!result.ok()) throw std::runtime_error(result.to_string());
        ServerInfo info;
        result = viewer->handshake(info, 8s, display);
        if (!result.ok()) throw std::runtime_error("handshake: " + result.to_string());
        if (description) *description = info;
        return viewer;
    }
};

struct FakeElevation : IElevationTask {
    std::atomic<ElevationCode> code{ElevationCode::Pending};
    std::atomic<bool> committed{false}, cancelled{false};
    ElevationState poll() override { return {0, code.load(), {}}; }
    void commit() override { committed = true; }
    void cancel() override { if (!committed) cancelled = true; }
};

ElevationState elevation_answer(QuicSession& viewer, uint32_t id, ElevationCode code) {
    ElevationState result;
    require(until([&] {
        ElevationState item;
        while (viewer.take_elevation_state(item)) {
            if (item.request_id == id && item.code == code) { result = item; return true; }
        }
        return false;
    }), "receive correlated elevation state");
    return result;
}

void elevation_permissions() {
    Fixture f;
    auto task = std::make_shared<FakeElevation>();
    std::atomic<unsigned> launches{0};
    f.config.elevation_state = [] { return ElevationState{0, ElevationCode::Available, {}}; };
    f.config.request_elevation = [&] { ++launches; return task; };
    f.start();
    auto viewer = f.connect();
    elevation_answer(*viewer, 0, ElevationCode::Available);
    auto follower = f.connect();
    const auto sent = follower->request_elevation(8);
    if (sent.ok()) elevation_answer(*follower, 8, ElevationCode::Denied);
    require(launches == 0, "view-only follower cannot launch UAC");
    auto files = f.connect(SessionMode::FileTransfer);
    elevation_answer(*files, 0, ElevationCode::Unsupported);
    require(viewer->request_elevation(1).ok(), "controller requests local UAC");
    elevation_answer(*viewer, 1, ElevationCode::Pending);
    require(viewer->request_elevation(2).ok(), "duplicate request reaches host");
    elevation_answer(*viewer, 2, ElevationCode::Busy);
    require(launches == 1 && !task->committed, "one pending launch; no premature takeover");
    for (const auto& connection : f.host->viewers()) {
        if (connection.controls_input) f.host->set_permissions(connection.id, kPermissionAll & ~kPermissionInput);
    }
    elevation_answer(*viewer, 1, ElevationCode::Denied);
    require(task->cancelled && !task->committed, "input revocation cancels pending elevation");
    files.reset(); follower.reset(); viewer.reset();
    f.host->stop(); f.host.reset();

    task = std::make_shared<FakeElevation>(); f.start(); viewer = f.connect();
    require(viewer->request_elevation(3).ok(), "request before disconnect");
    elevation_answer(*viewer, 3, ElevationCode::Pending);
    viewer.reset();
    require(until([&] { return task->cancelled.load(); }), "disconnect cancels uncommitted UAC");

    task = std::make_shared<FakeElevation>(); viewer = f.connect();
    require(viewer->request_elevation(4).ok(), "new controller can retry");
    elevation_answer(*viewer, 4, ElevationCode::Pending);
    task->code = ElevationCode::Restarting;
    elevation_answer(*viewer, 4, ElevationCode::Restarting);
    require(until([&] { return task->committed.load(); }), "host commits after queuing restarting response");
    viewer.reset(); f.host->stop();
    require(!task->cancelled, "ordinary teardown does not cancel committed handoff");
}

int elevation_child(int argc, char** argv) {
    if (argc != 5) return 2;
    std::string error;
    auto child = chast::host::ElevatedChild::receive(chast::host::widen(argv[2]),
        static_cast<DWORD>(std::stoul(argv[3])), std::stoull(argv[4]), &error, false);
    if (!child) { std::fprintf(stderr, "test child: %s\n", error.c_str()); return 3; }
    HostServerConfig config;
    config.settings = child->snapshot.settings;
    config.identity_user_token = child->identity_user_token;
    config.expected_identity_fingerprint = child->snapshot.fingerprint;
    config.serve_once = true;
    config.first_client_timeout_ms = 6000;
    use_empty_clipboard(config);
    auto media = host_test::use_media(config);
    auto server = HostServer::create(config, &error);
    if (!server) { std::fprintf(stderr, "test child create: %s\n", error.c_str()); return 4; }
    server->start();
    if (!child->announce_ready(server->status().fingerprint, &error)) {
        std::fprintf(stderr, "test child ready: %s\n", error.c_str()); return 5;
    }
    server->wait();
    return 0;
}

void portable_helper_protocol() {
    using namespace chast::host;
    PortableHelperMessage message;
    message.type = PortableHelperMessageType::HostSnapshot;
    for (size_t i = 0; i < message.instance_id.size(); ++i)
        message.instance_id[i] = static_cast<uint8_t>(i + 1);
    message.sequence = 1;
    message.request_id = 42;
    message.payload = {0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff};

    std::vector<uint8_t> encoded;
    std::string error;
    require(encode_portable_helper_message(message, &encoded, &error),
            "encode a bounded, versioned helper message");
    require(encoded.size() == kPortableHelperHeaderBytes + message.payload.size(),
            "helper frame has a fixed header and exact payload length");
    PortableHelperMessage decoded;
    require(decode_portable_helper_message(encoded, &decoded, &error) &&
                decoded.type == message.type &&
                decoded.instance_id == message.instance_id &&
                decoded.sequence == message.sequence &&
                decoded.request_id == message.request_id &&
                decoded.payload == message.payload,
            "helper message round-trips without changing its instance or body");

    PortableHelperMessage typed_request = message;
    typed_request.type = PortableHelperMessageType::InputEventRequest;
    typed_request.sequence = 1;
    require(encode_portable_helper_message(typed_request, &encoded, &error) &&
                decode_portable_helper_message(encoded, &decoded, &error) &&
                decoded.type == PortableHelperMessageType::InputEventRequest,
            "versioned IPC framing recognizes the new typed desktop request");

    typed_request.type = PortableHelperMessageType::CaptureFrameChunk;
    typed_request.sequence = 1;
    require(encode_portable_helper_message(typed_request, &encoded, &error) &&
                decode_portable_helper_message(encoded, &decoded, &error) &&
                decoded.type == PortableHelperMessageType::CaptureFrameChunk,
            "versioned IPC framing recognizes bounded capture-frame chunks");

    typed_request.type = PortableHelperMessageType::SessionWorkerStart;
    typed_request.sequence = 1;
    require(encode_portable_helper_message(typed_request, &encoded, &error) &&
                decode_portable_helper_message(encoded, &decoded, &error) &&
                decoded.type == PortableHelperMessageType::SessionWorkerStart,
            "versioned IPC framing recognizes the session-worker start control");
    typed_request.type = PortableHelperMessageType::SessionWorkerStop;
    typed_request.sequence = 1;
    require(encode_portable_helper_message(typed_request, &encoded, &error) &&
                decode_portable_helper_message(encoded, &decoded, &error) &&
                decoded.type == PortableHelperMessageType::SessionWorkerStop,
            "versioned IPC framing recognizes the session-worker stop control");

    PortableHelperSequence sequence(message.instance_id);
    require(sequence.Accept(decoded, &error) && sequence.last_sequence() == 1,
            "helper receive sequence accepts the first frame");
    require(!sequence.Accept(decoded, &error) && sequence.last_sequence() == 1,
            "helper receive sequence rejects a replay without advancing");
    decoded.sequence = 3;
    require(!sequence.Accept(decoded, &error) && sequence.last_sequence() == 1,
            "helper receive sequence rejects a gap without advancing");
    decoded.sequence = 2;
    decoded.instance_id[0] ^= 0xff;
    require(!sequence.Accept(decoded, &error),
            "helper receive sequence rejects a cross-instance frame");

    const auto rejected = [&](std::vector<uint8_t> bytes, const char* reason) {
        PortableHelperMessage output;
        error.clear();
        require(!decode_portable_helper_message(bytes, &output, &error) &&
                    !error.empty(), reason);
    };
    auto bad = encoded;
    bad[0] ^= 0xff;
    rejected(bad, "helper parser rejects an invalid magic value");
    bad = encoded;
    bad[8] = 2;
    rejected(bad, "helper parser rejects an unsupported protocol version");
    bad = encoded;
    bad[12] = 0xff;
    bad[13] = bad[14] = bad[15] = 0;
    rejected(bad, "helper parser rejects an unknown message type");
    bad = encoded;
    bad[12] = 14;
    bad[13] = bad[14] = bad[15] = 0;
    bad[12] = static_cast<uint8_t>(
        static_cast<uint32_t>(PortableHelperMessageType::SessionWorkerStop) + 1);
    rejected(bad, "helper parser rejects message types beyond the session-worker control table");
    bad = encoded;
    bad[52] = 1;
    rejected(bad, "helper parser rejects unrecognized flags");
    bad = encoded;
    bad.push_back(0);
    rejected(bad, "helper parser rejects trailing bytes");
    bad = encoded;
    bad[48] += 1;
    rejected(bad, "helper parser rejects a payload-length mismatch");
    bad = encoded;
    bad.resize(kPortableHelperHeaderBytes - 1);
    rejected(bad, "helper parser rejects a truncated header");

    message.payload.resize(kPortableHelperMaxPayloadBytes + 1);
    error.clear();
    require(!encode_portable_helper_message(message, &encoded, &error) &&
                encoded.empty() && !error.empty(),
            "helper encoder refuses an over-limit payload");
}

void portable_helper_worker_command_line() {
    using namespace chast::host;
    const std::vector<std::wstring> canonical = {
        L"C:\\ProgramData\\ChastPortableHelper\\chast-host.exe",
        L"--portable-helper-worker",
        L"--portable-helper-instance",
        L"00112233445566778899aabbccddeeff",
        L"--portable-helper-connection",
        L"ffeeddccbbaa99887766554433221100",
        L"--portable-helper-broker-pid",
        L"1234",
        L"--portable-helper-broker-created",
        L"987654321",
    };
    PortableHelperSessionWorkerLaunchArguments parsed;
    std::string error;
    require(parse_portable_helper_session_worker_command_line(
                canonical, &parsed, &error) &&
                parsed.instance_id[0] == 0x00 &&
                parsed.instance_id[1] == 0x11 &&
                parsed.instance_id[15] == 0xff &&
                parsed.connection_id[0] == 0xff &&
                parsed.connection_id[15] == 0x00 &&
                parsed.broker_process_id == 1234 &&
                parsed.broker_creation_time == 987654321,
            "worker CLI parses the launcher's exact canonical instance, connection and broker identity");

    PortableHelperProcessIdentity worker_identity;
    PortableHelperProcessIdentity broker_identity;
    require(!validate_portable_helper_session_worker_process_identity(
                parsed, &worker_identity, &broker_identity, &error) &&
                worker_identity.process_id == 0 && broker_identity.process_id == 0,
            "ordinary test process cannot claim the staged interactive SYSTEM worker identity");

    const auto rejects = [&](std::vector<std::wstring> arguments,
                             const char* description) {
        PortableHelperSessionWorkerLaunchArguments output;
        output.broker_process_id = 99;
        std::string detail;
        require(!parse_portable_helper_session_worker_command_line(
                    arguments, &output, &detail) &&
                    !detail.empty() && output.broker_process_id == 0 &&
                    output.broker_creation_time == 0 &&
                    std::all_of(output.instance_id.begin(),
                                output.instance_id.end(),
                                [](uint8_t value) { return value == 0; }),
                description);
    };

    auto malformed = canonical;
    malformed.pop_back();
    rejects(std::move(malformed),
            "worker CLI rejects a missing broker creation-time argument");
    malformed = canonical;
    malformed.push_back(L"--tray");
    rejects(std::move(malformed),
            "worker CLI rejects trailing switches instead of entering the tray host");
    malformed = canonical;
    malformed[6] = L"--portable-helper-parent-pid";
    rejects(std::move(malformed),
            "worker CLI rejects reordered or unknown switches");
    malformed = canonical;
    malformed[3] = L"00112233445566778899AABBCCDDEEFF";
    rejects(std::move(malformed),
            "worker CLI accepts only the launcher's lowercase hexadecimal identifiers");
    malformed = canonical;
    malformed[5] = L"00000000000000000000000000000000";
    rejects(std::move(malformed),
            "worker CLI rejects an empty per-connection identifier");
    malformed = canonical;
    malformed[7] = L"01234";
    rejects(std::move(malformed),
            "worker CLI rejects noncanonical decimal process IDs");
    malformed = canonical;
    malformed[7] = L"4294967296";
    rejects(std::move(malformed),
            "worker CLI rejects process IDs outside the Win32 range");
    malformed = canonical;
    malformed[9] = L"18446744073709551616";
    rejects(std::move(malformed),
            "worker CLI rejects an overflowing process creation time");
}

void portable_helper_input_journal() {
    using namespace chast::host;
    PortableHelperInputJournalScope scope;
    scope.instance_id[0] = 0x41;
    scope.connection_id[0] = 0x92;
    scope.windows_session_id = 7;
    scope.generation = 12;
    std::string error;
    PortableHelperInputJournalImage durable;
    int persist_calls = 0;
    bool fail_next_persist = false;
    const auto persist = [&](const PortableHelperInputJournalImage& image,
                             std::string* persist_error) {
        ++persist_calls;
        if (fail_next_persist) {
            fail_next_persist = false;
            if (persist_error != nullptr) *persist_error = "injected store failure";
            return false;
        }
        durable = image;
        return true;
    };
    const auto key_intent = [](uint64_t sequence, bool down) {
        chast::InputSideEffectIntent intent;
        intent.sequence = sequence;
        intent.api = chast::InputSideEffectApi::SendInput;
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = 'A';
        input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        intent.inputs.push_back(input);
        return intent;
    };
    const auto result_for = [](const chast::InputSideEffectIntent& intent,
                               bool succeeded, uint32_t last_error = ERROR_SUCCESS) {
        return chast::InputSideEffectResult{
            intent.sequence, intent.api, succeeded ? 1u : 0u,
            last_error, succeeded};
    };

    PortableHelperInputJournal journal(scope, persist);
    require(journal.Initialize(&error) && !durable.pending,
            "input journal creates an initial empty generation record");
    const auto down = key_intent(1, true);
    require(journal.Prepare(down, &error) && durable.pending &&
                durable.pending_sequence == 1 && durable.committed.keys.empty() &&
                durable.possible.keys.size() == 1,
            "key-down write-ahead state is durable before the OS call");
    std::vector<uint8_t> encoded;
    PortableHelperInputJournalImage decoded;
    require(encode_portable_helper_input_journal(durable, &encoded, &error) &&
                decode_portable_helper_input_journal(encoded, &decoded, &error) &&
                decoded.pending && decoded.scope.generation == scope.generation &&
                decoded.possible.keys.size() == 1,
            "journal wire image round-trips a crash-window pending key");
    auto corrupted = encoded;
    corrupted[0] ^= 0x01;
    require(!decode_portable_helper_input_journal(corrupted, &decoded, &error),
            "journal decoder rejects an invalid magic value");
    corrupted = encoded;
    corrupted[73] = 1;
    require(!decode_portable_helper_input_journal(corrupted, &decoded, &error),
            "journal decoder rejects nonzero reserved header bytes");
    corrupted = encoded;
    corrupted.push_back(0);
    require(!decode_portable_helper_input_journal(corrupted, &decoded, &error),
            "journal decoder rejects trailing unframed bytes");

    PortableHelperInputJournal recovered(scope,
        [](const PortableHelperInputJournalImage&, std::string*) { return true; });
    require(recovered.Restore(durable, &error) &&
                recovered.has_pending_operation() &&
                !recovered.Prepare(key_intent(2, false), &error),
            "recovery never resumes an unresolved OS input operation");
    require(recovered.MarkReleased(&error) && !recovered.has_pending_operation() &&
                recovered.image().committed.keys.empty() &&
                !recovered.Prepare(key_intent(2, false), &error),
            "independent cleanup may clear a pending journal and terminally close its generation");
    require(encode_portable_helper_input_journal(recovered.image(), &encoded,
                                                 &error) &&
                decode_portable_helper_input_journal(encoded, &decoded, &error) &&
                decoded.released && !decoded.pending &&
                decoded.committed.keys.empty() && decoded.possible.keys.empty(),
            "journal v2 round-trips a durable released-generation tombstone");
    PortableHelperInputJournal released_recovery(scope,
        [](const PortableHelperInputJournalImage&, std::string*) { return true; });
    require(released_recovery.Restore(decoded, &error) &&
                released_recovery.recovery_only() &&
                !released_recovery.Prepare(key_intent(2, false), &error) &&
                released_recovery.MarkReleased(&error),
            "restoring a released generation stays terminal and is idempotently cleanable");

    const auto mismatched = result_for(down, true);
    auto wrong_sequence = mismatched;
    ++wrong_sequence.sequence;
    require(!journal.Complete(wrong_sequence, &error) &&
                journal.has_pending_operation(),
            "journal rejects a duplicate or out-of-order API result");
    require(journal.Complete(mismatched, &error) && !durable.pending &&
                durable.last_sequence == 1 && durable.committed.keys.size() == 1,
            "successful key-down result commits its held state");
    const auto up = key_intent(2, false);
    PortableHelperInputJournal recovered_committed(scope,
        [](const PortableHelperInputJournalImage&, std::string*) { return true; });
    require(recovered_committed.Restore(journal.image(), &error) &&
                recovered_committed.recovery_only() &&
                !recovered_committed.Prepare(up, &error),
            "recovery cannot resume injection from a committed but held key state");
    require(journal.Prepare(up, &error) && durable.pending &&
                durable.possible.keys.size() == 1 &&
                journal.Complete(result_for(up, true), &error) &&
                durable.committed.keys.empty(),
            "key-up is write-ahead recorded before the committed state is cleared");
    require(!journal.Complete(result_for(up, true), &error),
            "a completed API result cannot be replayed");

    PortableHelperInputJournalScope failed_scope = scope;
    ++failed_scope.generation;
    PortableHelperInputJournalImage failed_durable;
    PortableHelperInputJournal failed_journal(failed_scope,
        [&](const PortableHelperInputJournalImage& image, std::string*) {
            failed_durable = image;
            return true;
        });
    require(failed_journal.Initialize(&error), "initialize failure-window journal");
    const auto uncertain_down = key_intent(1, true);
    require(failed_journal.Prepare(uncertain_down, &error),
            "prepare possible key state before simulating an API failure");
    require(!failed_journal.Complete(
                result_for(uncertain_down, false, ERROR_ACCESS_DENIED), &error) &&
                failed_durable.pending && failed_durable.possible.keys.size() == 1,
            "failed API result retains the conservative recovery record");
    require(failed_journal.MarkReleased(&error) && !failed_durable.pending &&
                failed_durable.possible.keys.empty(),
            "confirmed external release durably clears uncertain held state");

    PortableHelperInputJournalScope store_scope = scope;
    store_scope.generation += 2;
    PortableHelperInputJournalImage store_durable;
    bool store_failure = false;
    PortableHelperInputJournal store_journal(store_scope,
        [&](const PortableHelperInputJournalImage& image, std::string* store_error) {
            if (store_failure) {
                store_failure = false;
                if (store_error != nullptr) *store_error = "injected persistence failure";
                return false;
            }
            store_durable = image;
            return true;
        });
    require(store_journal.Initialize(&error), "initialize persistence-failure journal");
    const auto store_down = key_intent(1, true);
    store_failure = true;
    require(!store_journal.Prepare(store_down, &error) &&
                !store_journal.has_pending_operation() && !store_durable.pending,
            "prepare persistence failure vetoes the OS operation");
    require(store_journal.Prepare(store_down, &error),
            "prepare can proceed after a proven pre-call persistence failure");
    store_failure = true;
    require(!store_journal.Complete(result_for(store_down, true), &error) &&
                store_durable.pending && store_journal.has_pending_operation(),
            "result persistence failure preserves write-ahead recovery data");
    require(store_journal.MarkReleased(&error) && !store_durable.pending,
            "external cleanup resolves a result-persistence failure");

    PortableHelperInputJournalScope stateless_scope = scope;
    stateless_scope.generation += 3;
    PortableHelperInputJournalImage stateless_durable;
    int stateless_persist_calls = 0;
    PortableHelperInputJournal stateless_journal(stateless_scope,
        [&](const PortableHelperInputJournalImage& image, std::string*) {
            ++stateless_persist_calls;
            stateless_durable = image;
            return true;
        });
    require(stateless_journal.Initialize(&error), "initialize stateless journal");
    const int initial_persist_calls = stateless_persist_calls;
    chast::InputSideEffectIntent motion;
    motion.sequence = 1;
    motion.inputs.resize(1);
    motion.inputs[0].type = INPUT_MOUSE;
    motion.inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE;
    require(stateless_journal.Prepare(motion, &error) &&
                stateless_journal.Complete(result_for(motion, true), &error) &&
                stateless_persist_calls == initial_persist_calls,
            "stateless mouse motion avoids unnecessary journal writes");

    PortableHelperInputJournalScope touch_scope = scope;
    touch_scope.generation += 4;
    PortableHelperInputJournalImage touch_durable;
    PortableHelperInputJournal touch_journal(touch_scope,
        [&](const PortableHelperInputJournalImage& image, std::string*) {
            touch_durable = image;
            return true;
        });
    require(touch_journal.Initialize(&error), "initialize touch journal");
    const auto touch_intent = [&](uint64_t sequence, DWORD flags, int32_t x) {
        chast::InputSideEffectIntent intent;
        intent.sequence = sequence;
        intent.api = chast::InputSideEffectApi::InjectTouchInput;
        POINTER_TOUCH_INFO touch{};
        touch.pointerInfo.pointerType = PT_TOUCH;
        touch.pointerInfo.pointerId = 0;
        touch.pointerInfo.pointerFlags = flags;
        touch.pointerInfo.ptPixelLocation.x = x;
        touch.pointerInfo.ptPixelLocation.y = 50;
        intent.touch_contacts.push_back(touch);
        return intent;
    };
    const auto touch_down = touch_intent(1, POINTER_FLAG_DOWN |
        POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT, 10);
    require(touch_journal.Prepare(touch_down, &error) &&
                touch_journal.Complete(result_for(touch_down, true), &error) &&
                touch_durable.committed.touches.size() == 1,
            "touch down persists an active contact");
    const auto touch_up = touch_intent(2, POINTER_FLAG_UP, 10);
    require(touch_journal.Prepare(touch_up, &error) &&
                touch_journal.Complete(result_for(touch_up, true), &error) &&
                touch_durable.committed.touches.empty(),
            "touch up clears only the acknowledged contact");

    PortableHelperInputJournalScope pen_scope = scope;
    pen_scope.generation += 5;
    PortableHelperInputJournalImage pen_durable;
    PortableHelperInputJournal pen_journal(pen_scope,
        [&](const PortableHelperInputJournalImage& image, std::string*) {
            pen_durable = image;
            return true;
        });
    require(pen_journal.Initialize(&error), "initialize pen journal");
    const auto pen_intent = [](uint64_t sequence, DWORD flags) {
        chast::InputSideEffectIntent intent;
        intent.sequence = sequence;
        intent.api = chast::InputSideEffectApi::InjectSyntheticPointerInput;
        POINTER_TYPE_INFO pointer{};
        pointer.type = PT_PEN;
        pointer.penInfo.pointerInfo.pointerType = PT_PEN;
        pointer.penInfo.pointerInfo.pointerId = 0;
        pointer.penInfo.pointerInfo.pointerFlags = flags;
        pointer.penInfo.pointerInfo.ptPixelLocation.x = 20;
        pointer.penInfo.pointerInfo.ptPixelLocation.y = 30;
        intent.pointer_inputs.push_back(pointer);
        return intent;
    };
    const auto pen_down = pen_intent(1, POINTER_FLAG_DOWN |
        POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
    const auto pen_up = pen_intent(2, POINTER_FLAG_UP);
    require(pen_journal.Prepare(pen_down, &error) &&
                pen_journal.Complete(result_for(pen_down, true), &error) &&
                pen_durable.committed.pen_down &&
                pen_journal.Prepare(pen_up, &error) &&
                pen_journal.Complete(result_for(pen_up, true), &error) &&
                !pen_durable.committed.pen_down,
            "pen down/up state transitions round-trip through the journal");
    require(persist_calls > 0, "input journal persistence seam was exercised");
}

void portable_helper_input_journal_file_store() {
    using namespace chast::host;
    if (!process_can_harden_machine_state()) {
        std::printf("skip protected input journal files (elevated admin or SYSTEM token required)\n");
        return;
    }

    const auto directory = std::filesystem::temp_directory_path() /
        ("chast-input-journal-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    struct DirectoryCleanup final {
        std::filesystem::path path;
        ~DirectoryCleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{directory};
    std::string error;
    require(chast::harden_machine_state_directory(directory.wstring(), &error),
            "create and harden an isolated input journal directory");

    PortableHelperInputJournalScope scope;
    scope.instance_id[0] = 0x51;
    scope.connection_id[0] = 0xA4;
    scope.windows_session_id = 9;
    scope.generation = 25;
    auto store = detail::open_portable_helper_input_journal_file_store_for_test(
        scope, directory.wstring(), &error);
    require(store != nullptr, "open a scoped store in the protected test directory");
    if (!store) return;
    const std::wstring record_path = store->record_path();

    auto journal = std::make_unique<PortableHelperInputJournal>(scope,
        [&store](const PortableHelperInputJournalImage& image,
                 std::string* persist_error) {
            return store->Persist(image, persist_error);
        });
    require(journal->Initialize(&error) && std::filesystem::exists(record_path),
            "initialize atomically creates the first empty on-disk generation");
    PortableHelperInputJournalImage loaded;
    bool found = false;
    require(store->Load(&loaded, &found, &error) && found && !loaded.pending,
            "load validates and decodes the protected initial image");

    auto competing_store = detail::open_portable_helper_input_journal_file_store_for_test(
        scope, directory.wstring(), &error);
    PortableHelperInputJournalImage fresh;
    fresh.scope = scope;
    require(competing_store != nullptr &&
                !competing_store->Persist(fresh, &error),
            "a fresh store refuses to overwrite an existing generation record");
    competing_store.reset();

    chast::InputSideEffectIntent key_down;
    key_down.sequence = 1;
    key_down.api = chast::InputSideEffectApi::SendInput;
    INPUT key{};
    key.type = INPUT_KEYBOARD;
    key.ki.wVk = 'B';
    key_down.inputs.push_back(key);
    require(journal->Prepare(key_down, &error),
            "flush a write-ahead possible key state to the protected file");
    journal.reset();
    store.reset();

    store = detail::open_portable_helper_input_journal_file_store_for_test(
        scope, directory.wstring(), &error);
    require(store != nullptr, "reopen the store after simulating a worker crash");
    if (!store) return;
    require(store->Load(&loaded, &found, &error) && found && loaded.pending &&
                loaded.pending_sequence == 1 && loaded.possible.keys.size() == 1,
            "restart recovers the durable pending state without resuming injection");
    PortableHelperInputJournal recovered(scope,
        [&store](const PortableHelperInputJournalImage& image,
                 std::string* persist_error) {
            return store->Persist(image, persist_error);
        });
    require(recovered.Restore(loaded, &error) &&
                store->ResumeLoadedRecord(recovered, &error) &&
                !recovered.Prepare(key_down, &error),
            "restored pending generation is recovery-only and remains closed to new input");
    require(recovered.MarkReleased(&error) &&
                store->Load(&loaded, &found, &error) && found &&
                loaded.released && !loaded.pending &&
                loaded.committed.keys.empty() && loaded.possible.keys.empty(),
            "confirmed external cleanup atomically persists a released tombstone");
    PortableHelperInputJournal released_recovery(scope,
        [&store](const PortableHelperInputJournalImage& image,
                 std::string* persist_error) {
            return store->Persist(image, persist_error);
        });
    require(released_recovery.Restore(loaded, &error) &&
                store->ResumeLoadedRecord(released_recovery, &error) &&
                !released_recovery.Prepare(key_down, &error),
            "recovered released file record cannot reactivate its generation");
    store.reset();
    auto released_replay_store =
        detail::open_portable_helper_input_journal_file_store_for_test(
            scope, directory.wstring(), &error);
    fresh = {};
    fresh.scope = scope;
    require(released_replay_store != nullptr &&
                !released_replay_store->Persist(fresh, &error),
            "a new store cannot replace a released tombstone with a fresh generation");

    PortableHelperInputJournalScope live_scope = scope;
    live_scope.generation += 2;
    auto live_store = detail::open_portable_helper_input_journal_file_store_for_test(
        live_scope, directory.wstring(), &error);
    require(live_store != nullptr, "open an isolated generation for observer integration");
    if (!live_store) return;
    auto live_journal = std::make_unique<PortableHelperInputJournal>(live_scope,
        [&live_store](const PortableHelperInputJournalImage& image,
                      std::string* persist_error) {
            return live_store->Persist(image, persist_error);
        });
    require(live_journal->Initialize(&error),
            "initialize the journal used by the dry-run input observer");
    InputSideEffectObserver observer;
    observer.prepare = [journal = live_journal.get()](
        const InputSideEffectIntent& intent) {
      std::string observer_error;
      return journal->Prepare(intent, &observer_error);
    };
    observer.complete = [journal = live_journal.get()](
        const InputSideEffectResult& result) {
      std::string observer_error;
      return journal->Complete(result, &observer_error);
    };
    auto dry_run_injector = InputInjector::create(
        true, &error, {}, {}, {}, {}, std::move(observer));
    require(dry_run_injector != nullptr,
            "create a dry-run injector wired to durable journal callbacks");
    if (!dry_run_injector) return;
    InputEvent dry_down;
    dry_down.kind = InputKind::Key;
    dry_down.key = {KeyAction::Down, 0x04};
    InputEvent dry_up = dry_down;
    dry_up.key.action = KeyAction::Up;
    require(dry_run_injector->submit_and_wait(dry_down) &&
                live_store->Load(&loaded, &found, &error) && found &&
                loaded.committed.keys.size() == 1 &&
                dry_run_injector->submit_and_wait(dry_up) &&
                live_store->Load(&loaded, &found, &error) && found &&
                loaded.committed.keys.empty(),
            "InputInjector prepare/result callbacks persist key-down and key-up without desktop input");
    dry_run_injector->stop();
    dry_run_injector.reset();

    PortableHelperInputJournalScope corrupt_scope = scope;
    ++corrupt_scope.generation;
    auto corrupt_store = detail::open_portable_helper_input_journal_file_store_for_test(
        corrupt_scope, directory.wstring(), &error);
    PortableHelperInputJournalImage corrupt_initial;
    corrupt_initial.scope = corrupt_scope;
    require(corrupt_store != nullptr &&
                corrupt_store->Persist(corrupt_initial, &error),
            "create a separate generation for on-disk corruption checks");
    const std::wstring corrupt_path = corrupt_store->record_path();
    corrupt_store.reset();
    {
        HANDLE file = CreateFileW(corrupt_path.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
        DWORD written = 0;
        const uint8_t invalid_magic = 0;
        const bool corrupted = file != INVALID_HANDLE_VALUE &&
            WriteFile(file, &invalid_magic, sizeof(invalid_magic), &written, nullptr) &&
            written == sizeof(invalid_magic) && FlushFileBuffers(file);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        require(corrupted, "corrupt an isolated journal record for fail-closed coverage");
    }
    corrupt_store = detail::open_portable_helper_input_journal_file_store_for_test(
        corrupt_scope, directory.wstring(), &error);
    require(corrupt_store != nullptr &&
                !corrupt_store->Load(&loaded, &found, &error),
            "on-disk malformed journal data is rejected instead of reset");
}

void portable_helper_authorization() {
    using namespace chast::host;
    PortableHelperInstanceId instance{};
    PortableHelperConnectionId connection{};
    instance[0] = 0x31;
    connection[0] = 0x72;
    constexpr uint32_t windows_session = 8;
    constexpr uint32_t operations =
        static_cast<uint32_t>(PortableHelperOperation::CaptureFrames) |
        static_cast<uint32_t>(PortableHelperOperation::SendKeyboardInput);
    std::string error;

    PortableHelperLocalApprovalGate approval_gate;
    PortableHelperPendingApproval pending_approval;
    PortableHelperLocalApprovalReceipt approval_receipt;
    require(!approval_gate.QueueRemoteRequest(
                instance, connection, 0, operations, &pending_approval, nullptr),
            "remote helper request cannot queue a Session 0 approval");
    require(!approval_gate.QueueRemoteRequest(
                instance, connection, windows_session, operations | (1u << 7),
                &pending_approval, nullptr),
            "remote helper request cannot queue unknown capabilities");
    require(approval_gate.QueueRemoteRequest(
                instance, connection, windows_session, operations,
                &pending_approval, &error) &&
                pending_approval.view().requested_operations == operations &&
                approval_gate.pending_count() == 1,
            "remote request only creates an opaque pending local-approval item");
    bool wrong_thread_approval_succeeded = false;
    std::thread non_ui_approval([&] {
        std::string thread_error;
        wrong_thread_approval_succeeded = approval_gate.Approve(
            std::move(pending_approval), operations,
            [](const PortableHelperApprovalRequestView&, uint32_t,
               std::string*) { return true; },
            &approval_receipt, &thread_error);
    });
    non_ui_approval.join();
    require(!wrong_thread_approval_succeeded && pending_approval &&
                approval_gate.pending_count() == 1,
            "remote/network worker threads cannot approve a queued helper request");
    PortableHelperPendingApproval duplicate_pending;
    require(!approval_gate.QueueRemoteRequest(
                instance, connection, windows_session, operations,
                &duplicate_pending, &error),
            "one connection cannot flood the local approval queue");
    require(approval_gate.Approve(
                std::move(pending_approval),
                static_cast<uint32_t>(PortableHelperOperation::CaptureFrames),
                [connection](
                    const PortableHelperApprovalRequestView& request,
                    uint32_t approved_operations, std::string*) {
                  return request.connection_id == connection &&
                      request.windows_session_id == 8 &&
                      approved_operations == static_cast<uint32_t>(
                          PortableHelperOperation::CaptureFrames);
                },
                &approval_receipt, &error) &&
                approval_receipt && approval_gate.pending_count() == 0,
            "local approval revalidates scope and can reduce requested capabilities");
    PortableHelperAuthorization reduced_authorization;
    PortableHelperGrant reduced_grant;
    require(reduced_authorization.BeginGrant(std::move(approval_receipt),
                                             &reduced_grant, &error) &&
                reduced_grant.operations == static_cast<uint32_t>(
                    PortableHelperOperation::CaptureFrames),
            "one-use approval receipt mints only the locally approved capability mask");
    require(!reduced_authorization.BeginGrant(std::move(approval_receipt),
                                              &reduced_grant, &error),
            "a consumed local approval receipt cannot mint a second grant");

    PortableHelperConnectionId stale_connection = connection;
    stale_connection[2] = 0x21;
    PortableHelperPendingApproval stale_approval;
    require(approval_gate.QueueRemoteRequest(
                instance, stale_connection, windows_session, operations,
                &stale_approval, &error),
            "queue a second approval request for stale-state coverage");
    PortableHelperLocalApprovalReceipt stale_receipt;
    require(!approval_gate.Approve(
                std::move(stale_approval), operations,
                [](const PortableHelperApprovalRequestView&, uint32_t,
                   std::string* error) {
                  if (error != nullptr) *error = "connection is no longer active";
                  return false;
                },
                &stale_receipt, &error) &&
                !stale_receipt && approval_gate.pending_count() == 0,
            "changed connection state consumes the pending request without issuing a receipt");

    PortableHelperConnectionId denied_connection = connection;
    denied_connection[3] = 0x47;
    PortableHelperPendingApproval denied_approval;
    require(approval_gate.QueueRemoteRequest(
                instance, denied_connection, windows_session, operations,
                &denied_approval, &error) &&
                approval_gate.Deny(std::move(denied_approval), &error) &&
                approval_gate.pending_count() == 0,
            "local denial removes the remote request without minting a capability");

    PortableHelperConnectionId disconnected_connection = connection;
    disconnected_connection[4] = 0x53;
    PortableHelperPendingApproval disconnected_approval;
    require(approval_gate.QueueRemoteRequest(
                instance, disconnected_connection, windows_session, operations,
                &disconnected_approval, &error),
            "queue a connection approval before disconnect cancellation");
    approval_gate.CancelConnection(disconnected_connection);
    PortableHelperLocalApprovalReceipt disconnected_receipt;
    require(!approval_gate.Approve(
                std::move(disconnected_approval), operations,
                [](const PortableHelperApprovalRequestView&, uint32_t,
                   std::string*) { return true; },
                &disconnected_receipt, &error) &&
                !disconnected_receipt && approval_gate.pending_count() == 0,
            "disconnect cancellation makes a queued approval unusable");

    PortableHelperAuthorization authorization;
    PortableHelperGrant grant;
    require(!begin_local_approval_test_grant(
                authorization, instance, connection, 0, operations, &grant,
                &error),
            "helper authorization rejects Session 0 grants");
    require(!begin_local_approval_test_grant(
                authorization, instance, connection, windows_session,
                operations | (1u << 7), &grant, &error),
            "helper authorization rejects unknown operation bits");
    require(begin_local_approval_test_grant(
                authorization, instance, connection, windows_session,
                operations, &grant, &error),
            "explicitly authorized helper session receives a fresh scoped grant");
    require(grant.generation == 1 && grant.operations == operations &&
                authorization.state() ==
                    PortableHelperAuthorizationState::AwaitingWorkerReady,
            "grant starts fail-closed while worker readiness is pending");
    require(!authorization.Authorize(instance, connection, windows_session,
                                     grant.generation,
                                     PortableHelperOperation::CaptureFrames),
            "desktop capabilities remain denied before WorkerReady");
    require(!begin_local_approval_test_grant(
                authorization, instance, connection, windows_session,
                operations, &grant, &error),
            "a second grant cannot replace an active pending generation");

    PortableHelperMessage start_request;
    require(make_portable_helper_session_worker_start_request(
                grant, 41, &start_request, &error),
            "serialize the locally authorized grant into a correlated service start request");
    PortableHelperGrant imported_parent_grant;
    require(parse_portable_helper_session_worker_start_request(
                start_request, instance, windows_session,
                &imported_parent_grant, &error) &&
                imported_parent_grant.connection_id == grant.connection_id &&
                imported_parent_grant.generation == grant.generation &&
                imported_parent_grant.operations == grant.operations,
            "helper service imports only the exact instance and session-scoped parent grant");
    require(!parse_portable_helper_session_worker_start_request(
                start_request, instance, windows_session + 1,
                &imported_parent_grant, &error),
            "helper service rejects a parent grant for a different Windows session");
    PortableHelperAuthorization delegated_authorization;
    require(delegated_authorization.AcceptParentAuthorizedGrant(
                grant, windows_session, &error) &&
                delegated_authorization.state() ==
                    PortableHelperAuthorizationState::AwaitingWorkerReady,
            "helper service keeps the parent-issued capability closed until its worker is ready");
    PortableHelperSecret delegated_proof{};
    require(make_portable_helper_worker_ready_proof(grant, &delegated_proof,
                                                     &error) &&
                delegated_authorization.ConfirmBrokerManagedWorkerReady(
                    grant, delegated_proof, &error) &&
                delegated_authorization.state() ==
                    PortableHelperAuthorizationState::Ready &&
                delegated_authorization.Authorize(
                    instance, connection, windows_session, grant.generation,
                    PortableHelperOperation::CaptureFrames),
            "parent accepts broker WorkerReady only with the correlated grant proof");
    SecureZeroMemory(delegated_proof.data(), delegated_proof.size());
    PortableHelperSecret forged_proof{};
    require(!delegated_authorization.ConfirmBrokerManagedWorkerReady(
                grant, forged_proof, &error),
            "parent rejects a forged or replayed broker WorkerReady proof");

    PortableHelperAuthorization worker_gate;
    require(!worker_gate.AcceptWorkerGrant(grant, windows_session + 1, &error) &&
                worker_gate.state() == PortableHelperAuthorizationState::Idle,
            "worker-side grant import rejects a different Windows session");
    require(worker_gate.AcceptWorkerGrant(grant, windows_session, &error) &&
                worker_gate.state() == PortableHelperAuthorizationState::Ready &&
                worker_gate.Authorize(instance, connection, windows_session,
                                      grant.generation,
                                      PortableHelperOperation::CaptureFrames) &&
                worker_gate.Authorize(instance, connection, windows_session,
                                      grant.generation,
                                      PortableHelperOperation::SendKeyboardInput),
            "authenticated worker stores the received grant as its local operation gate");
    require(!worker_gate.AcceptWorkerGrant(grant, windows_session, &error),
            "worker-side grant import cannot replace an active capability");

    InputEvent imported_key;
    imported_key.kind = InputKind::Key;
    imported_key.key.action = KeyAction::Down;
    imported_key.key.hid_usage = 0x04;
    PortableHelperMessage worker_input_request;
    require(make_portable_helper_input_request(
                worker_gate, grant, 99, imported_key, &worker_input_request,
                &error),
            "worker can validate a typed input request against its imported grant");
    std::promise<void> input_entered;
    auto input_entered_future = input_entered.get_future();
    std::promise<void> finish_input;
    auto finish_input_future = finish_input.get_future();
    std::vector<INPUT> applied_inputs;
    auto injector = InputInjector::create(
        true, &error,
        [&](const INPUT& input) {
          applied_inputs.push_back(input);
          if (applied_inputs.size() == 1) {
            input_entered.set_value();
            finish_input_future.wait();
          }
        },
        [] { return GetKeyboardLayout(0); });
    require(injector != nullptr,
            "worker input adapter creates its session-scoped injector");
    auto input_task = std::async(std::launch::async, [&] {
      return apply_portable_helper_worker_input_request(
          worker_gate, *injector, worker_input_request, &error);
    });
    const bool input_started = input_entered_future.wait_for(2s) ==
                               std::future_status::ready;
    const bool revocation_started =
        worker_gate.BeginRevocation([] { return true; }, &error);
    const bool lease_held = !worker_gate.WaitForOperations(10, &error);
    finish_input.set_value();
    const bool input_applied = input_task.get();
    const bool lease_drained = worker_gate.WaitForOperations(1000, &error);
    const bool revoke_finished = worker_gate.FinishRevocation(
        [&] {
          injector->release_held();
          injector->stop();
          return true;
        }, &error);
    require(input_started && revocation_started && lease_held && input_applied &&
                lease_drained && revoke_finished &&
                worker_gate.state() == PortableHelperAuthorizationState::Revoked &&
                !worker_gate.Authorize(instance, connection, windows_session,
                                       grant.generation,
                                       PortableHelperOperation::CaptureFrames) &&
                !applied_inputs.empty(),
            "worker releases input only after injection completes and capability is revoked");

    PortableHelperProcessIdentity expected_worker;
    expected_worker.process_id = 42;
    expected_worker.session_id = windows_session;
    expected_worker.creation_time = 123456;
    expected_worker.image_path = L"C:\\ProgramData\\Chast\\helper.exe";
    expected_worker.user_sid = L"S-1-5-18";
    expected_worker.logon_sid = L"S-1-5-5-0-123";
    PortableHelperProcessIdentity actual_worker = expected_worker;
    PortableHelperSecret proof{};
    require(make_portable_helper_worker_ready_proof(grant, &proof, &error),
            "worker-ready proof binds a challenge and the entire grant scope");

    class SyntheticCapturer final : public IDxgiCapturer {
     public:
      SyntheticCapturer() {
        frame_.cpu_bgra = std::make_shared<const std::vector<uint8_t>>(
            std::vector<uint8_t>{0, 0, 255, 255, 0, 255, 0, 255,
                                 255, 0, 0, 255, 255, 255, 255, 255});
        frame_.cpu_image = true;
        frame_.width = 2;
        frame_.height = 2;
        frame_.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        frame_.frame_number = 7;
      }

      CaptureResult capture(std::chrono::milliseconds) override {
        return CaptureResult::Frame;
      }
      const FrameInfo& frame() const override { return frame_; }
      const CursorInfo& cursor() const override { return cursor_; }
      const DisplayInfo& display() const override { return display_; }
      AdapterDevice adapter_device() const override { return {}; }
      Error reinitialize() override { return Error::success(); }
      Error set_nv12_scale(UINT) override { return Error::success(); }
      const Error& last_error() const override { return error_; }

     private:
      FrameInfo frame_;
      CursorInfo cursor_;
      DisplayInfo display_;
      Error error_ = Error::success();
    } synthetic_capture;

    PortableHelperAuthorization capture_broker;
    PortableHelperAuthorization capture_worker_gate;
    PortableHelperGrant capture_grant;
    PortableHelperConnectionId capture_connection{};
    PortableHelperSecret capture_proof{};
    require(secure_random(capture_connection.data(), capture_connection.size()) &&
                begin_local_approval_test_grant(
                    capture_broker, instance, capture_connection,
                    windows_session,
                    static_cast<uint32_t>(PortableHelperOperation::CaptureFrames),
                    &capture_grant, &error),
            "create a scoped capture grant for a worker");
    require(make_portable_helper_worker_ready_proof(
                capture_grant, &capture_proof, &error) &&
                capture_broker.ConfirmWorkerReady(
                    capture_grant, expected_worker, actual_worker,
                    capture_proof, &error),
            "authenticate the capture grant against a worker identity");
    require(capture_worker_gate.AcceptWorkerGrant(capture_grant,
                                                   windows_session, &error),
            "worker imports the authenticated capture grant");
    PortableHelperMessage synthetic_capture_request;
    require(make_portable_helper_capture_request(
                capture_worker_gate, capture_grant, 100,
                &synthetic_capture_request, &error),
            "worker builds a fixed-scope capture request");
    PortableHelperCapturedFrame captured_frame;
    require(capture_portable_helper_worker_frame(
                capture_worker_gate, synthetic_capture,
                synthetic_capture_request, &captured_frame, &error) &&
                captured_frame.width == 2 && captured_frame.height == 2 &&
                captured_frame.frame_number == 7 &&
                captured_frame.png.size() > 8 &&
                captured_frame.png[0] == 137 && captured_frame.png[1] == 80 &&
                captured_frame.png[2] == 78 && captured_frame.png[3] == 71 &&
                captured_frame.png[4] == 13 && captured_frame.png[5] == 10 &&
                captured_frame.png[6] == 26 && captured_frame.png[7] == 10,
            "worker capture adapter encodes its leased BGRA frame as PNG");
    require(capture_worker_gate.Revoke([] { return true; }, &error) &&
                !capture_worker_gate.Authorize(
                    instance, capture_connection, windows_session,
                    capture_grant.generation,
                    PortableHelperOperation::CaptureFrames),
            "capture worker cannot start another frame after revocation");

    PortableHelperProcessIdentity wrong_worker = actual_worker;
    ++wrong_worker.process_id;
    require(!authorization.ConfirmWorkerReady(grant, expected_worker,
                                               wrong_worker, proof, &error),
            "WorkerReady rejects a different SCM worker process");
    PortableHelperProcessIdentity wrong_session_worker = expected_worker;
    ++wrong_session_worker.session_id;
    require(!authorization.ConfirmWorkerReady(grant, wrong_session_worker,
                                               wrong_session_worker, proof,
                                               &error),
            "WorkerReady rejects an otherwise valid worker in another Windows session");
    PortableHelperSecret invalid_proof = proof;
    invalid_proof[0] ^= 0x80;
    require(!authorization.ConfirmWorkerReady(grant, expected_worker,
                                               actual_worker, invalid_proof,
                                               &error),
            "WorkerReady rejects a forged challenge proof");
    require(authorization.ConfirmWorkerReady(grant, expected_worker,
                                              actual_worker, proof, &error),
            "WorkerReady opens the capability gate only after identity and proof checks");
    require(authorization.Authorize(instance, connection, windows_session,
                                    grant.generation,
                                    PortableHelperOperation::CaptureFrames) &&
                authorization.Authorize(instance, connection, windows_session,
                                        grant.generation,
                                        PortableHelperOperation::SendKeyboardInput),
            "only operations included in the ready grant are authorized");
    require(!authorization.Authorize(instance, connection, windows_session,
                                     grant.generation,
                                     PortableHelperOperation::SendPointerInput) &&
                !authorization.Authorize(instance, connection, windows_session + 1,
                                         grant.generation,
                                         PortableHelperOperation::CaptureFrames),
            "ungranted operations and other Windows sessions are denied");

    PortableHelperMessage capture_request;
    require(make_portable_helper_capture_request(authorization, grant, 100,
                                                  &capture_request, &error) &&
                capture_request.type ==
                    PortableHelperMessageType::CaptureFrameRequest &&
                capture_request.payload.size() == 28,
            "capture uses a fixed scoped request instead of an arbitrary command");
    {
        PortableHelperAuthorization::OperationLease capture_lease;
        PortableHelperOperation operation{};
        require(acquire_portable_helper_desktop_request(
                    authorization, capture_request, &capture_lease, &operation,
                    nullptr, &error) &&
                    operation == PortableHelperOperation::CaptureFrames &&
                    static_cast<bool>(capture_lease),
                "capture request acquires a capability lease before side effects");
    }

    InputEvent requested_key;
    requested_key.kind = InputKind::Key;
    requested_key.key.action = KeyAction::Down;
    requested_key.key.hid_usage = 0x04;
    PortableHelperMessage input_request;
    require(make_portable_helper_input_request(authorization, grant, 101,
                                                requested_key, &input_request,
                                                &error) &&
                input_request.type ==
                    PortableHelperMessageType::InputEventRequest &&
                input_request.payload.size() == 52,
            "keyboard input uses the existing fixed InputEvent schema and scope");
    PortableHelperAuthorization::OperationLease input_lease;
    PortableHelperOperation input_operation{};
    InputEvent decoded_key;
    require(acquire_portable_helper_desktop_request(
                authorization, input_request, &input_lease, &input_operation,
                &decoded_key, &error) &&
                input_operation == PortableHelperOperation::SendKeyboardInput &&
                decoded_key.kind == InputKind::Key &&
                decoded_key.key.hid_usage == requested_key.key.hid_usage &&
                decoded_key.key.action == requested_key.key.action,
            "worker validates and leases a typed keyboard event before injection");

    InputEvent requested_pointer;
    requested_pointer.kind = InputKind::Mouse;
    requested_pointer.mouse.action = MouseAction::Move;
    requested_pointer.mouse.x = 15;
    requested_pointer.mouse.y = 27;
    require(!make_portable_helper_input_request(authorization, grant, 102,
                                                requested_pointer, &input_request,
                                                &error),
            "input encoder refuses a pointer event outside the granted operation mask");
    PortableHelperMessage wrong_session = capture_request;
    wrong_session.payload[24] ^= 1;
    PortableHelperAuthorization::OperationLease rejected_lease;
    require(!acquire_portable_helper_desktop_request(
                authorization, wrong_session, &rejected_lease, &input_operation,
                nullptr, &error) && !static_cast<bool>(rejected_lease),
            "worker rejects a desktop request bound to another Windows session");
    PortableHelperMessage stale_generation = capture_request;
    stale_generation.payload[16] ^= 1;
    require(!acquire_portable_helper_desktop_request(
                authorization, stale_generation, &rejected_lease,
                &input_operation, nullptr, &error),
            "worker rejects a desktop request from a stale connection generation");
    PortableHelperMessage wrong_connection = capture_request;
    wrong_connection.payload[0] ^= 1;
    require(!acquire_portable_helper_desktop_request(
                authorization, wrong_connection, &rejected_lease,
                &input_operation, nullptr, &error),
            "worker rejects a desktop request scoped to another connection");
    PortableHelperMessage generic_command = capture_request;
    generic_command.type = PortableHelperMessageType::Acknowledgement;
    require(!acquire_portable_helper_desktop_request(
                authorization, generic_command, &rejected_lease,
                &input_operation, nullptr, &error),
            "worker rejects generic protocol messages at the desktop operation gate");
    PortableHelperMessage malformed_input = input_request;
    malformed_input.payload.pop_back();
    require(!acquire_portable_helper_desktop_request(
                authorization, malformed_input, &rejected_lease, &input_operation,
                &decoded_key, &error),
            "worker rejects truncated typed input payloads before acquiring a lease");

    bool input_was_denied_before_release = false;
    std::promise<bool> cancellation_result;
    auto cancellation_completed = cancellation_result.get_future();
    std::promise<bool> release_result;
    auto release_completed = release_result.get_future();
    auto revoke = std::async(std::launch::async, [&] {
        return authorization.Revoke([&] {
            cancellation_result.set_value(!authorization.Authorize(
                instance, connection, windows_session, grant.generation,
                PortableHelperOperation::SendKeyboardInput));
            return true;
        }, [&] {
            input_was_denied_before_release = !authorization.Authorize(
                instance, connection, windows_session, grant.generation,
                PortableHelperOperation::SendKeyboardInput);
            release_result.set_value(input_was_denied_before_release);
            return true;
        }, &error);
    });
    require(until([&] {
                return authorization.state() ==
                    PortableHelperAuthorizationState::Revoking;
            }),
            "revocation closes the operation gate before waiting for active work");
    require(cancellation_completed.wait_for(3s) == std::future_status::ready &&
                cancellation_completed.get(),
            "revocation signals worker cancellation only after closing the gate");
    require(revoke.wait_for(30ms) == std::future_status::timeout,
            "revocation waits while an authorized input side effect holds its lease");
    require(release_completed.wait_for(0ms) == std::future_status::timeout,
            "held input is not released while an authorized operation remains in flight");
    input_lease = PortableHelperAuthorization::OperationLease{};
    require(release_completed.wait_for(3s) == std::future_status::ready &&
                release_completed.get() &&
                revoke.wait_for(3s) == std::future_status::ready && revoke.get(),
            "revocation completes only after the in-flight input lease is released");
    require(input_was_denied_before_release &&
                authorization.state() == PortableHelperAuthorizationState::Revoked &&
                !authorization.Authorize(instance, connection, windows_session,
                                         grant.generation,
                                         PortableHelperOperation::CaptureFrames),
            "revocation denies operations before input cleanup and invalidates the old generation");

    PortableHelperGrant next_grant;
    require(begin_local_approval_test_grant(
                authorization, instance, connection, windows_session,
                operations, &next_grant, &error) &&
                next_grant.generation == grant.generation + 1,
            "reconnect receives a strictly newer authorization generation");
    require(!authorization.Authorize(instance, connection, windows_session,
                                     grant.generation,
                                     PortableHelperOperation::CaptureFrames),
            "a prior connection generation cannot control a reconnected session");
    require(make_portable_helper_worker_ready_proof(next_grant, &proof, &error) &&
                authorization.ConfirmWorkerReady(next_grant, expected_worker,
                                                  actual_worker, proof, &error),
            "new generation performs a fresh worker-ready challenge");
    PortableHelperAuthorization::OperationLease pending_capture;
    require(authorization.Acquire(
                instance, connection, windows_session, next_grant.generation,
                PortableHelperOperation::CaptureFrames, &pending_capture, &error),
            "capture holds a lease while the supervisor begins bounded revocation");
    bool gate_closed_before_cancel = false;
    require(authorization.BeginRevocation([&] {
                gate_closed_before_cancel = !authorization.Authorize(
                    instance, connection, windows_session, next_grant.generation,
                    PortableHelperOperation::SendKeyboardInput);
                return true;
            }, &error) && gate_closed_before_cancel,
            "split revocation closes the gate before signaling worker cancellation");
    require(!authorization.WaitForOperations(20, &error),
            "supervisor can bound its wait for a stuck desktop operation");
    bool release_attempted_too_early = false;
    require(!authorization.FinishRevocation([&] {
                release_attempted_too_early = true;
                return true;
            }, &error) && !release_attempted_too_early &&
                authorization.state() == PortableHelperAuthorizationState::Revoking,
            "input release is refused until all operation leases have drained");
    pending_capture = PortableHelperAuthorization::OperationLease{};
    require(authorization.WaitForOperations(3000, &error),
            "operation drain completes after the worker releases its lease");
    require(!authorization.FinishRevocation([] { return false; }, &error) &&
                authorization.state() == PortableHelperAuthorizationState::Revoked &&
                !authorization.Authorize(instance, connection, windows_session,
                                         next_grant.generation,
                                         PortableHelperOperation::SendKeyboardInput),
            "failed input cleanup still revokes capability and requires channel teardown");
}

void portable_helper_worker_supervision() {
    using namespace chast::host;
    constexpr uint32_t session = 12;
    constexpr uint32_t operations =
        static_cast<uint32_t>(PortableHelperOperation::SendKeyboardInput);
    const auto make_ready = [&](PortableHelperAuthorization& authorization,
                                uint8_t id,
                                PortableHelperGrant* grant) {
        PortableHelperInstanceId instance{};
        PortableHelperConnectionId connection{};
        instance[0] = id;
        connection[0] = static_cast<uint8_t>(id + 1);
        PortableHelperProcessIdentity worker;
        worker.process_id = 500 + id;
        worker.session_id = session;
        worker.creation_time = 90000 + id;
        worker.image_path = L"C:\\ProgramData\\Chast\\worker.exe";
        worker.user_sid = L"S-1-5-18";
        worker.logon_sid = L"S-1-5-5-0-456";
        PortableHelperSecret proof{};
        std::string error;
        require(begin_local_approval_test_grant(
                    authorization, instance, connection, session, operations,
                    grant, &error),
                "supervisor fixture creates an explicitly scoped grant");
        require(make_portable_helper_worker_ready_proof(*grant, &proof, &error) &&
                    authorization.ConfirmWorkerReady(*grant, worker, worker,
                                                     proof, &error),
                "supervisor fixture authenticates the expected worker");
        return std::pair{instance, connection};
    };

    // Fast cooperative shutdown never terminates a healthy worker, and input
    // release is sequenced after the capability gate is closed.
    PortableHelperAuthorization cooperative;
    PortableHelperGrant cooperative_grant;
    const auto [cooperative_instance, cooperative_connection] =
        make_ready(cooperative, 21, &cooperative_grant);
    std::vector<std::string> cooperative_order;
    bool cooperative_gate_closed = false;
    PortableHelperWorkerRevocationCallbacks cooperative_callbacks;
    cooperative_callbacks.request_cancellation = [&](std::string*) {
        cooperative_order.emplace_back("cancel");
        cooperative_gate_closed = !cooperative.Authorize(
            cooperative_instance, cooperative_connection, session,
            cooperative_grant.generation,
            PortableHelperOperation::SendKeyboardInput);
        return true;
    };
    cooperative_callbacks.terminate_worker = [&](std::string*) {
        cooperative_order.emplace_back("terminate");
        return true;
    };
    cooperative_callbacks.wait_for_worker_exit =
        [&](uint32_t, std::string*) {
            cooperative_order.emplace_back("wait-exit");
            return true;
        };
    cooperative_callbacks.release_input = [&](std::string*) {
        cooperative_order.emplace_back("release-input");
        return cooperative_gate_closed;
    };
    cooperative_callbacks.close_channel = [&](std::string*) {
        cooperative_order.emplace_back("close-channel");
        return true;
    };
    std::string error;
    require(revoke_portable_helper_worker(cooperative, cooperative_callbacks,
                                           100, 100, &error) &&
                cooperative_order ==
                    std::vector<std::string>{"cancel", "wait-exit",
                                             "release-input", "close-channel"} &&
                cooperative.state() ==
                    PortableHelperAuthorizationState::Revoked,
            "cooperative revocation closes the gate, releases input, then closes IPC");

    // Even when no operation is in flight, the worker itself must not remain
    // alive after revocation if it ignores the cooperative stop request.
    PortableHelperAuthorization idle_hung;
    PortableHelperGrant idle_hung_grant;
    const auto [idle_instance, idle_connection] =
        make_ready(idle_hung, 26, &idle_hung_grant);
    std::vector<std::string> idle_hung_order;
    int exit_waits = 0;
    PortableHelperWorkerRevocationCallbacks idle_hung_callbacks;
    idle_hung_callbacks.request_cancellation = [&](std::string*) {
        idle_hung_order.emplace_back("cancel");
        return true;
    };
    idle_hung_callbacks.terminate_worker = [&](std::string*) {
        idle_hung_order.emplace_back("terminate");
        return true;
    };
    idle_hung_callbacks.wait_for_worker_exit =
        [&](uint32_t, std::string*) {
            idle_hung_order.emplace_back(++exit_waits == 1 ? "grace-wait"
                                                           : "wait-exit");
            return exit_waits > 1;
        };
    idle_hung_callbacks.release_input = [&](std::string*) {
        idle_hung_order.emplace_back("release-input");
        return !idle_hung.Authorize(
            idle_instance, idle_connection, session, idle_hung_grant.generation,
            PortableHelperOperation::SendKeyboardInput);
    };
    idle_hung_callbacks.close_channel = [&](std::string*) {
        idle_hung_order.emplace_back("close-channel");
        return true;
    };
    require(revoke_portable_helper_worker(idle_hung, idle_hung_callbacks,
                                           100, 10, &error) &&
                idle_hung_order ==
                    std::vector<std::string>{"cancel", "grace-wait", "terminate",
                                             "wait-exit", "release-input",
                                             "close-channel"},
            "revocation also terminates an idle worker that ignores cooperative stop");

    // A held operation forces escalation. Releasing input is forbidden until
    // the injected process supervisor observes worker exit and the lease drains.
    PortableHelperAuthorization escalated;
    PortableHelperGrant escalated_grant;
    const auto [escalated_instance, escalated_connection] =
        make_ready(escalated, 31, &escalated_grant);
    PortableHelperAuthorization::OperationLease operation_lease;
    require(escalated.Acquire(escalated_instance, escalated_connection, session,
                              escalated_grant.generation,
                              PortableHelperOperation::SendKeyboardInput,
                              &operation_lease, &error),
            "supervisor fixture holds an in-flight input operation");
    std::mutex worker_mutex;
    std::condition_variable worker_changed;
    bool termination_requested = false;
    bool worker_exited = false;
    std::thread worker_thread(
        [lease = std::move(operation_lease), &worker_mutex, &worker_changed,
         &termination_requested, &worker_exited]() mutable {
            std::unique_lock lock(worker_mutex);
            worker_changed.wait(lock, [&] { return termination_requested; });
            lock.unlock();
            lease = PortableHelperAuthorization::OperationLease{};
            lock.lock();
            worker_exited = true;
            lock.unlock();
            worker_changed.notify_all();
        });
    std::vector<std::string> escalated_order;
    PortableHelperWorkerRevocationCallbacks escalated_callbacks;
    escalated_callbacks.request_cancellation = [&](std::string*) {
        escalated_order.emplace_back("cancel");
        return true;
    };
    escalated_callbacks.terminate_worker = [&](std::string*) {
        escalated_order.emplace_back("terminate");
        {
            std::lock_guard lock(worker_mutex);
            termination_requested = true;
        }
        worker_changed.notify_all();
        return true;
    };
    escalated_callbacks.wait_for_worker_exit =
        [&](uint32_t timeout_ms, std::string*) {
            escalated_order.emplace_back("wait-exit");
            std::unique_lock lock(worker_mutex);
            const bool exited = worker_changed.wait_for(
                lock, std::chrono::milliseconds(timeout_ms),
                [&] { return worker_exited; });
            lock.unlock();
            if (exited && worker_thread.joinable()) worker_thread.join();
            return exited;
        };
    escalated_callbacks.release_input = [&](std::string*) {
        escalated_order.emplace_back("release-input");
        return !escalated.Authorize(
            escalated_instance, escalated_connection, session,
            escalated_grant.generation,
            PortableHelperOperation::SendKeyboardInput);
    };
    escalated_callbacks.close_channel = [&](std::string*) {
        escalated_order.emplace_back("close-channel");
        return true;
    };
    require(revoke_portable_helper_worker(escalated, escalated_callbacks,
                                           20, 1000, &error) &&
                escalated_order ==
                    std::vector<std::string>{"cancel", "terminate", "wait-exit",
                                             "release-input", "close-channel"} &&
                escalated.state() == PortableHelperAuthorizationState::Revoked,
            "stuck worker is terminated and observed before operation drain and input release");
    if (worker_thread.joinable()) worker_thread.join();

    // If termination itself fails, close IPC and leave held input untouched;
    // a later supervisor retry may finish once the lease has drained.
    PortableHelperAuthorization fail_closed;
    PortableHelperGrant fail_closed_grant;
    const auto [fail_closed_instance, fail_closed_connection] =
        make_ready(fail_closed, 41, &fail_closed_grant);
    PortableHelperAuthorization::OperationLease held;
    require(fail_closed.Acquire(fail_closed_instance, fail_closed_connection,
                                session, fail_closed_grant.generation,
                                PortableHelperOperation::SendKeyboardInput,
                                &held, &error),
            "fail-closed fixture holds an operation lease");
    bool released_while_stuck = false;
    bool closed_after_termination_failure = false;
    bool worker_exited_after_retry = false;
    PortableHelperWorkerRevocationCallbacks failed_callbacks;
    failed_callbacks.request_cancellation = [](std::string*) { return true; };
    failed_callbacks.terminate_worker = [](std::string* detail) {
        if (detail != nullptr) *detail = "injected termination failure";
        return false;
    };
    failed_callbacks.wait_for_worker_exit = [&](uint32_t, std::string*) {
        return worker_exited_after_retry;
    };
    failed_callbacks.release_input = [&](std::string*) {
        released_while_stuck = true;
        return true;
    };
    failed_callbacks.close_channel = [&](std::string*) {
        closed_after_termination_failure = true;
        return true;
    };
    require(!revoke_portable_helper_worker(fail_closed, failed_callbacks,
                                            10, 10, &error) &&
                !released_while_stuck && closed_after_termination_failure &&
                fail_closed.state() ==
                    PortableHelperAuthorizationState::Revoking,
            "failed worker termination closes IPC but never releases input under a live lease");
    held = PortableHelperAuthorization::OperationLease{};
    worker_exited_after_retry = true;
    require(revoke_portable_helper_worker(fail_closed, failed_callbacks,
                                           100, 10, &error) &&
                fail_closed.state() ==
                    PortableHelperAuthorizationState::Revoked,
            "a later supervisor retry finishes cleanup once the stuck operation drains");
}

int portable_helper_control_server_child(int argc, char** argv) {
    using namespace chast::host;
    const bool worker_control_server = argc == 5 &&
        std::string(argv[1]) == "--portable-helper-worker-control-server";
    const bool worker_no_release_control_server = argc == 5 &&
        std::string(argv[1]) ==
            "--portable-helper-worker-no-release-control-server";
    const bool basic_control_server = argc == 5 &&
        std::string(argv[1]) == "--portable-helper-control-server";
    const bool negative_input_control_server = argc == 5 &&
        std::string(argv[1]) ==
            "--portable-helper-negative-input-control-server";
    if (!worker_control_server && !worker_no_release_control_server &&
        !basic_control_server && !negative_input_control_server)
        return 2;
    std::vector<uint8_t> bytes;
    if (!chast::hex_decode(argv[2], bytes) ||
        bytes.size() != chast::host::kPortableHelperInstanceIdBytes)
        return 3;
    chast::host::PortableHelperInstanceId instance{};
    std::copy(bytes.begin(), bytes.end(), instance.begin());
    const uint32_t parent_pid = static_cast<uint32_t>(std::stoul(argv[3]));
    const uint64_t parent_creation = std::stoull(argv[4]);
    chast::host::PortableHelperProcessIdentity parent;
    std::string error;
    if (!chast::host::query_portable_helper_process_identity(
            parent_pid, &parent, &error) ||
        parent.creation_time != parent_creation)
        return 4;
    auto pipe = chast::host::PortableHelperPipe::CreateServer(
        instance, parent.logon_sid, &error);
    if (!pipe) return 5;
    HANDLE parent_process = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (parent_process == nullptr || stop_event == nullptr) {
        if (parent_process != nullptr) CloseHandle(parent_process);
        if (stop_event != nullptr) CloseHandle(stop_event);
        return 6;
    }
    std::unique_ptr<chast::host::PortableHelperServiceSessionWorker>
        session_worker;
    chast::host::PortableHelperServiceControlRequestHandler request_handler;
    if (worker_control_server || worker_no_release_control_server) {
        chast::host::PortableHelperServiceSpec spec;
        spec.instance_id = instance;
        spec.executable_path = executable_path();
        spec.service_name = chast::host::portable_helper_service_name(instance);
        spec.parent = parent;
        chast::host::PortableHelperProcessIdentity broker;
        if (!chast::host::current_portable_helper_process_identity(
                &broker, &error)) {
            CloseHandle(parent_process);
            CloseHandle(stop_event);
            return 7;
        }
        const std::wstring executable = spec.executable_path;
        auto launcher = [executable, broker](
            const chast::host::PortableHelperServiceSpec& launch_spec,
            const chast::host::PortableHelperGrant& grant, HANDLE* process,
            std::string* launch_error) {
            const std::wstring command = L"\"" + executable +
                L"\" --portable-helper-session-worker-auth-client " +
                widen(hex_encode(launch_spec.instance_id.data(),
                                 launch_spec.instance_id.size())) + L" " +
                widen(hex_encode(grant.connection_id.data(),
                                 grant.connection_id.size())) + L" " +
                std::to_wstring(broker.process_id) + L" " +
                std::to_wstring(broker.creation_time);
            std::vector<wchar_t> command_line(command.begin(), command.end());
            command_line.push_back(L'\0');
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION created{};
            if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr,
                                nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                nullptr, &startup, &created)) {
                if (launch_error != nullptr)
                    *launch_error = "cannot launch isolated service worker peer";
                return false;
            }
            CloseHandle(created.hThread);
            *process = created.hProcess;
            return true;
        };
        if (worker_no_release_control_server) {
            session_worker =
                std::make_unique<chast::host::PortableHelperServiceSessionWorker>(
                    std::move(spec), std::move(launcher));
        } else {
            session_worker =
                std::make_unique<chast::host::PortableHelperServiceSessionWorker>(
                    std::move(spec), std::move(launcher),
                    [](const chast::host::PortableHelperGrant&,
                       std::string*) { return true; });
        }
        request_handler = [&session_worker](
            chast::host::PortableHelperPipe& control,
            const chast::host::PortableHelperMessage& request,
            bool* stop_service, std::string* handler_error) {
            return session_worker->HandleControlRequest(
                control, request, stop_service, handler_error);
        };
    } else if (negative_input_control_server) {
        request_handler = [](chast::host::PortableHelperPipe& control,
                            const chast::host::PortableHelperMessage& request,
                            bool* stop_service, std::string* handler_error) {
            if (stop_service == nullptr ||
                request.type != chast::host::PortableHelperMessageType::InputEventRequest ||
                request.request_id == 0) {
                if (handler_error != nullptr)
                    *handler_error = "negative-input test server received an unexpected request";
                return false;
            }
            *stop_service = false;
            chast::host::PortableHelperMessage acknowledgement;
            acknowledgement.type =
                chast::host::PortableHelperMessageType::Acknowledgement;
            acknowledgement.request_id = request.request_id;
            acknowledgement.payload = {0};
            return control.Send(std::move(acknowledgement), 5000,
                                handler_error);
        };
    }
    const bool ok = chast::host::run_portable_helper_service_control_channel(
        std::move(pipe), parent, parent_process, stop_event,
        [stop_event] { SetEvent(stop_event); }, &error,
        std::move(request_handler));
    if (session_worker) {
        std::string cleanup_error;
        if (!session_worker->Shutdown(&cleanup_error)) {
            CloseHandle(parent_process);
            CloseHandle(stop_event);
            return 8;
        }
    }
    CloseHandle(parent_process);
    CloseHandle(stop_event);
    return ok ? 0 : 7;
}

int portable_helper_pipe_child(int argc, char** argv) {
    const bool session_worker_client = argc == 6 &&
        std::string(argv[1]) == "--portable-helper-session-worker-client";
    const bool session_worker_auth_client = argc == 6 &&
        std::string(argv[1]) == "--portable-helper-session-worker-auth-client";
    const bool session_worker_bad_ready_client = argc == 6 &&
        std::string(argv[1]) == "--portable-helper-session-worker-bad-ready-client";
    const bool session_worker_no_stop_ack_client = argc == 6 &&
        std::string(argv[1]) ==
            "--portable-helper-session-worker-no-stop-ack-client";
    const bool session_worker_mode =
        session_worker_client || session_worker_auth_client ||
        session_worker_bad_ready_client || session_worker_no_stop_ack_client;
    const bool authorization_client = argc == 5 &&
        std::string(argv[1]) == "--portable-helper-auth-client";
    const bool supervised_worker = argc == 5 &&
        std::string(argv[1]) == "--portable-helper-supervisor-client";
    const bool reject_server_identity = argc == 6 &&
        std::string(argv[5]) == "--reject-server-logon-sid";
    const bool idle_client = argc == 6 &&
        std::string(argv[5]) == "--idle-pipe-client";
    if ((!session_worker_mode && argc != 5 && !reject_server_identity &&
         !idle_client) || (session_worker_mode && argc != 6))
        return 2;
    std::vector<uint8_t> instance_bytes;
    if (!chast::hex_decode(argv[2], instance_bytes) ||
        instance_bytes.size() != chast::host::kPortableHelperInstanceIdBytes)
        return 3;
    chast::host::PortableHelperInstanceId instance_id{};
    std::copy(instance_bytes.begin(), instance_bytes.end(), instance_id.begin());
    chast::host::PortableHelperInstanceId connection_id{};
    if (session_worker_mode) {
        std::vector<uint8_t> connection_bytes;
        if (!chast::hex_decode(argv[3], connection_bytes) ||
            connection_bytes.size() !=
                chast::host::kPortableHelperInstanceIdBytes)
            return 3;
        std::copy(connection_bytes.begin(), connection_bytes.end(),
                  connection_id.begin());
    }
    const int parent_pid_index = session_worker_mode ? 4 : 3;
    const int parent_creation_index = session_worker_mode ? 5 : 4;
    const uint32_t parent_pid =
        static_cast<uint32_t>(std::stoul(argv[parent_pid_index]));
    const uint64_t parent_creation =
        std::stoull(argv[parent_creation_index]);
    chast::host::PortableHelperProcessIdentity parent;
    std::string error;
    if (!chast::host::query_portable_helper_process_identity(
            parent_pid, &parent, &error) ||
        parent.creation_time != parent_creation)
        return 4;
    if (reject_server_identity) parent.logon_sid += L"-forged";
    auto pipe = session_worker_mode
        ? chast::host::PortableHelperPipe::ConnectSessionWorkerClient(
              instance_id, connection_id, parent, 5000, &error)
        : chast::host::PortableHelperPipe::ConnectClient(
              instance_id, parent, 5000, &error);
    if (reject_server_identity) return pipe ? 8 : 0;
    if (idle_client) {
        if (!pipe) return 9;
        Sleep(500);
        return 0;
    }
    if (!pipe) {
        std::fprintf(stderr, "portable pipe child connect: %s\n", error.c_str());
        return 5;
    }
    if (session_worker_auth_client) {
        chast::host::PortableHelperSessionWorkerLaunchArguments launch_arguments;
        launch_arguments.instance_id = instance_id;
        launch_arguments.connection_id = connection_id;
        launch_arguments.broker_process_id = parent.process_id;
        launch_arguments.broker_creation_time = parent.creation_time;
        chast::host::PortableHelperGrant grant;
        chast::host::PortableHelperProcessIdentity worker;
        chast::host::PortableHelperProcessIdentity authenticated_broker;
        chast::host::PortableHelperAuthorization worker_authorization;
        // This child is an ordinary interactive test process, not the
        // Session-0 SYSTEM broker required by the product import routine.
        // Exercise the coordinator's grant/WorkerReady exchange with a test
        // peer while preserving the production broker identity checks.
        if (!chast::host::current_portable_helper_process_identity(
                &worker, &error) ||
            !pipe->AuthenticatedPeer(&authenticated_broker, &error) ||
            authenticated_broker.process_id != parent.process_id ||
            authenticated_broker.creation_time != parent.creation_time ||
            authenticated_broker.session_id != parent.session_id ||
            authenticated_broker.user_sid != parent.user_sid ||
            authenticated_broker.logon_sid != parent.logon_sid ||
            !chast::host::receive_portable_helper_grant(
                *pipe, &grant, 5000, &error) ||
            grant.instance_id != launch_arguments.instance_id ||
            grant.connection_id != launch_arguments.connection_id ||
            grant.windows_session_id != worker.session_id ||
            grant.generation == 0 ||
            !worker_authorization.AcceptWorkerGrant(
                grant, worker.session_id, &error) ||
            !worker_authorization.Authorize(
                instance_id, connection_id, worker.session_id,
                grant.generation,
                chast::host::PortableHelperOperation::CaptureFrames) ||
            !worker_authorization.Authorize(
                instance_id, connection_id, worker.session_id,
                grant.generation,
                chast::host::PortableHelperOperation::SendKeyboardInput) ||
            grant.windows_session_id != parent.session_id ||
            grant.windows_session_id != worker.session_id) {
            std::fprintf(stderr, "portable session worker handshake: %s\n",
                         error.c_str());
            return 18;
        }
        class SessionLoopCapturer final : public chast::IDxgiCapturer {
         public:
          SessionLoopCapturer() {
            frame_.cpu_bgra = std::make_shared<const std::vector<uint8_t>>(
                std::vector<uint8_t>{0, 0, 255, 255, 0, 255, 0, 255,
                                     255, 0, 0, 255, 255, 255, 255, 255});
            frame_.cpu_image = true;
            frame_.width = 2;
            frame_.height = 2;
            frame_.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            frame_.frame_number = 23;
          }
          chast::CaptureResult capture(std::chrono::milliseconds) override {
            return chast::CaptureResult::Frame;
          }
          const chast::FrameInfo& frame() const override { return frame_; }
          const chast::CursorInfo& cursor() const override { return cursor_; }
          const chast::DisplayInfo& display() const override { return display_; }
          chast::AdapterDevice adapter_device() const override { return {}; }
          chast::Error reinitialize() override { return chast::Error::success(); }
          chast::Error set_nv12_scale(UINT) override { return chast::Error::success(); }
          const chast::Error& last_error() const override { return error_; }

         private:
          chast::FrameInfo frame_;
          chast::CursorInfo cursor_;
          chast::DisplayInfo display_;
          chast::Error error_ = chast::Error::success();
        } capturer;
        auto injector = chast::InputInjector::create(true, &error);
        if (!injector ||
            !chast::host::run_portable_helper_session_worker_request_loop(
                *pipe, worker_authorization, grant, &capturer, injector.get(),
                &error)) {
            std::fprintf(stderr, "portable session worker request loop: %s\n",
                         error.c_str());
            return 19;
        }
        return 0;
    }
    if (session_worker_no_stop_ack_client) {
        chast::host::PortableHelperMessage stop;
        return pipe->Receive(&stop, 5000, &error) &&
                       stop.type == chast::host::PortableHelperMessageType::Stop &&
                       stop.request_id != 0 && stop.payload.empty()
                   ? 0
                   : 23;
    }
    if (session_worker_bad_ready_client) {
        chast::host::PortableHelperGrant grant;
        if (!chast::host::receive_portable_helper_grant(*pipe, &grant, 5000,
                                                        &error))
            return 20;
        chast::host::PortableHelperMessage forged;
        forged.type = chast::host::PortableHelperMessageType::WorkerReady;
        forged.instance_id = grant.instance_id;
        forged.request_id = grant.generation;
        forged.payload.assign(chast::host::PortableHelperSecret{}.size(), 0xa5);
        if (!pipe->Send(std::move(forged), 5000, &error)) return 21;
        Sleep(10000);
        return 22;
    }
    if (authorization_client || supervised_worker) {
        chast::host::PortableHelperGrant grant;
        if (!chast::host::receive_portable_helper_grant(*pipe, &grant, 5000,
                                                        &error) ||
            !chast::host::send_portable_helper_worker_ready(*pipe, grant, 5000,
                                                            &error)) {
            std::fprintf(stderr, "portable authorization child: %s\n",
                         error.c_str());
            return 10;
        }
        chast::InputEvent key;
        key.kind = chast::InputKind::Key;
        key.key.action = chast::KeyAction::Down;
        key.key.hid_usage = 0x04;
        chast::host::PortableHelperMessage request;
        request.type = chast::host::PortableHelperMessageType::InputEventRequest;
        request.instance_id = grant.instance_id;
        request.request_id = 177;
        request.payload.insert(request.payload.end(), grant.connection_id.begin(),
                               grant.connection_id.end());
        for (unsigned shift = 0; shift < 64; shift += 8)
            request.payload.push_back(static_cast<uint8_t>(grant.generation >> shift));
        for (unsigned shift = 0; shift < 32; shift += 8)
            request.payload.push_back(
                static_cast<uint8_t>(grant.windows_session_id >> shift));
        auto input_payload = chast::make_input_event_payload(key);
        request.payload.insert(request.payload.end(), input_payload.begin(),
                               input_payload.end());
        if (!pipe->Send(std::move(request), 5000, &error)) {
            std::fprintf(stderr, "portable authorization child input: %s\n",
                         error.c_str());
            return 11;
        }
        chast::host::PortableHelperMessage acknowledgement;
        if (!pipe->Receive(&acknowledgement, 5000, &error) ||
            acknowledgement.type !=
                chast::host::PortableHelperMessageType::Acknowledgement ||
            acknowledgement.request_id != 177 ||
            acknowledgement.payload != std::vector<uint8_t>{1})
            return 12;
        chast::host::PortableHelperMessage capture_request;
        std::vector<uint8_t> expected_scope(grant.connection_id.begin(),
                                            grant.connection_id.end());
        for (unsigned shift = 0; shift < 64; shift += 8)
            expected_scope.push_back(
                static_cast<uint8_t>(grant.generation >> shift));
        for (unsigned shift = 0; shift < 32; shift += 8)
            expected_scope.push_back(
                static_cast<uint8_t>(grant.windows_session_id >> shift));
        if (!pipe->Receive(&capture_request, 5000, &error) ||
            capture_request.type !=
                chast::host::PortableHelperMessageType::CaptureFrameRequest ||
            capture_request.request_id != 178 ||
            capture_request.payload != expected_scope)
            return 13;
        chast::host::PortableHelperMessage capture_ack;
        capture_ack.type =
            chast::host::PortableHelperMessageType::Acknowledgement;
        capture_ack.request_id = 178;
        capture_ack.payload = {2};
        if (!pipe->Send(std::move(capture_ack), 5000, &error)) return 14;
        chast::host::PortableHelperMessage forwarded_input;
        std::vector<uint8_t> expected_input = expected_scope;
        expected_input.insert(expected_input.end(), input_payload.begin(),
                              input_payload.end());
        if (!pipe->Receive(&forwarded_input, 5000, &error) ||
            forwarded_input.type !=
                chast::host::PortableHelperMessageType::InputEventRequest ||
            forwarded_input.request_id != 179 ||
            forwarded_input.payload != expected_input)
            return 15;
        chast::host::PortableHelperMessage input_ack;
        input_ack.type =
            chast::host::PortableHelperMessageType::Acknowledgement;
        input_ack.request_id = 179;
        input_ack.payload = {3};
        if (!pipe->Send(std::move(input_ack), 5000, &error)) return 16;
        if (supervised_worker) {
            chast::host::PortableHelperMessage stop;
            if (!pipe->Receive(&stop, 5000, &error) ||
                stop.type != chast::host::PortableHelperMessageType::Stop)
                return 17;
            chast::host::PortableHelperMessage stop_ack;
            stop_ack.type =
                chast::host::PortableHelperMessageType::Acknowledgement;
            stop_ack.request_id = stop.request_id;
            stop_ack.payload = {1};
            return pipe->Send(std::move(stop_ack), 5000, &error) ? 0 : 18;
        }
        return 0;
    }
    chast::host::PortableHelperMessage message;
    if (!pipe->Receive(&message, 5000, &error) ||
        message.type != chast::host::PortableHelperMessageType::ServiceChallenge ||
        message.request_id != 77 || message.payload != std::vector<uint8_t>{1, 2, 3})
        return 6;
    message = {};
    message.type = chast::host::PortableHelperMessageType::Acknowledgement;
    message.request_id = 77;
    message.payload = {4, 5, 6};
    if (!pipe->Send(std::move(message), 5000, &error)) {
        std::fprintf(stderr, "portable pipe child send: %s\n", error.c_str());
        return 7;
    }
    if (session_worker_client) {
        chast::host::PortableHelperMessage transfer_begin;
        if (!pipe->Receive(&transfer_begin, 5000, &error) ||
            transfer_begin.type !=
                chast::host::PortableHelperMessageType::Acknowledgement ||
            transfer_begin.request_id != 78 ||
            transfer_begin.payload.size() != 4)
            return 8;
        const uint32_t chunk_count =
            static_cast<uint32_t>(transfer_begin.payload[0]) |
            (static_cast<uint32_t>(transfer_begin.payload[1]) << 8) |
            (static_cast<uint32_t>(transfer_begin.payload[2]) << 16) |
            (static_cast<uint32_t>(transfer_begin.payload[3]) << 24);
        if (chunk_count < 2 || chunk_count > 128) return 9;
        chast::host::PortableHelperMessage transfer_ready;
        transfer_ready.type =
            chast::host::PortableHelperMessageType::Acknowledgement;
        transfer_ready.request_id = 78;
        transfer_ready.payload = {1};
        if (!pipe->Send(std::move(transfer_ready), 5000, &error)) return 10;
        std::vector<chast::host::PortableHelperMessage> chunks;
        chunks.reserve(chunk_count);
        for (uint32_t index = 0; index < chunk_count; ++index) {
            chast::host::PortableHelperMessage chunk;
            if (!pipe->Receive(&chunk, 5000, &error) ||
                chunk.type !=
                    chast::host::PortableHelperMessageType::CaptureFrameChunk ||
                chunk.request_id != 79)
                return 11;
            chunks.push_back(std::move(chunk));
        }
        for (auto& chunk : chunks) {
            if (!pipe->Send(std::move(chunk), 5000, &error)) return 12;
        }
    }
    return 0;
}

void portable_helper_service_channel_transport() {
    using namespace chast::host;
    PortableHelperProcessIdentity parent;
    std::wstring logon_sid;
    std::string error;
    require(current_portable_helper_process_identity(&parent, &error) &&
                parent.session_id != 0 &&
                current_portable_helper_logon_sid(&logon_sid, &error) &&
                logon_sid == parent.logon_sid,
            "control-channel test captures its exact interactive parent identity");
    PortableHelperInstanceId instance{};
    require(secure_random(instance.data(), instance.size()),
            "control-channel test generates a unique endpoint instance");
    const auto executable = executable_path();
    const std::wstring command = L"\"" + executable +
        L"\" --portable-helper-control-server " +
        widen(hex_encode(instance.data(), instance.size())) + L" " +
        std::to_wstring(parent.process_id) + L" " +
        std::to_wstring(parent.creation_time);
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    require(CreateProcessW(executable.c_str(), command_line.data(), nullptr,
                           nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &child) != FALSE,
            "launch an isolated helper control-channel server process");
    CloseHandle(child.hThread);
    const auto stop_child = [&] {
        if (WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0)
            TerminateProcess(child.hProcess, ERROR_CANCELLED);
        WaitForSingleObject(child.hProcess, 5000);
    };
    const auto check_child = [&](bool condition, const char* message) {
        if (condition) return;
        stop_child();
        CloseHandle(child.hProcess);
        child.hProcess = nullptr;
        require(false, message);
    };

    PortableHelperProcessIdentity expected_server;
    check_child(query_portable_helper_process_identity(
                    child.dwProcessId, &expected_server, &error),
                "control-channel test queries the server process identity");
    auto client = PortableHelperPipe::ConnectClient(instance, expected_server,
                                                     5000, &error);
    check_child(client != nullptr,
                "host connects only to the exact local helper process");

    PortableHelperMessage status;
    status.type = PortableHelperMessageType::Status;
    status.request_id = 10;
    check_child(client->Send(std::move(status), 5000, &error),
                "host sends a status request over the authenticated pipe");
    PortableHelperMessage response;
    check_child(client->Receive(&response, 5000, &error) &&
                    response.type == PortableHelperMessageType::Acknowledgement &&
                    response.request_id == 10 &&
                    response.payload == std::vector<uint8_t>{1},
                "service acknowledges the status request with the correlated ID");

    PortableHelperMessage stop;
    stop.type = PortableHelperMessageType::Stop;
    stop.request_id = 11;
    check_child(client->Send(std::move(stop), 5000, &error),
                "host requests orderly helper shutdown");
    check_child(client->Receive(&response, 5000, &error) &&
                    response.type == PortableHelperMessageType::Acknowledgement &&
                    response.request_id == 11 &&
                    response.payload == std::vector<uint8_t>{1},
                "service acknowledges Stop before leaving its control loop");
    client.reset();
    check_child(WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0,
                "authenticated Stop causes the isolated helper service to exit");
    DWORD exit_code = 1;
    check_child(GetExitCodeProcess(child.hProcess, &exit_code) && exit_code == 0,
                "helper control server exits successfully after orderly stop");
    CloseHandle(child.hProcess);
    child.hProcess = nullptr;
}

void portable_helper_parent_rpc_rejection_revokes_gate() {
    using namespace chast::host;
    PortableHelperProcessIdentity parent;
    std::string error;
    require(current_portable_helper_process_identity(&parent, &error) &&
                parent.session_id != 0,
            "parent-RPC rejection test captures its interactive parent identity");
    PortableHelperInstanceId instance{};
    PortableHelperConnectionId connection_id{};
    require(secure_random(instance.data(), instance.size()) &&
                secure_random(connection_id.data(), connection_id.size()),
            "parent-RPC rejection test creates unique helper and connection scopes");

    const auto executable = executable_path();
    const std::wstring command = L"\"" + executable +
        L"\" --portable-helper-negative-input-control-server " +
        widen(hex_encode(instance.data(), instance.size())) + L" " +
        std::to_wstring(parent.process_id) + L" " +
        std::to_wstring(parent.creation_time);
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    require(CreateProcessW(executable.c_str(), command_line.data(), nullptr,
                           nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &child) != FALSE,
            "launch a test helper that returns a negative correlated input ACK");
    CloseHandle(child.hThread);
    const auto stop_child = [&] {
        if (WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0)
            (void)TerminateProcess(child.hProcess, ERROR_CANCELLED);
        (void)WaitForSingleObject(child.hProcess, 5000);
    };
    const auto check_child = [&](bool condition, const char* message) {
        if (condition) return;
        stop_child();
        CloseHandle(child.hProcess);
        child.hProcess = nullptr;
        require(false, message);
    };

    PortableHelperProcessIdentity helper_identity;
    check_child(query_portable_helper_process_identity(
                    child.dwProcessId, &helper_identity, &error),
                "parent-RPC rejection test queries the helper process identity");
    auto pipe = PortableHelperPipe::ConnectClient(instance, helper_identity,
                                                   5000, &error);
    check_child(pipe != nullptr,
                "parent-RPC rejection test authenticates its helper pipe peer");

    PortableHelperAuthorization authorization;
    PortableHelperGrant grant;
    const uint32_t operations = static_cast<uint32_t>(
        PortableHelperOperation::SendKeyboardInput);
    PortableHelperSecret proof{};
    check_child(begin_local_approval_test_grant(
                    authorization, instance, connection_id,
                    parent.session_id, operations, &grant, &error) &&
                    make_portable_helper_worker_ready_proof(
                        grant, &proof, &error) &&
                    authorization.ConfirmBrokerManagedWorkerReady(
                        grant, proof, &error),
                "parent-RPC rejection test establishes a locally approved ready grant");
    SecureZeroMemory(proof.data(), proof.size());

    PortableHelperServiceConnection service_connection;
    service_connection.instance_id = instance;
    service_connection.channel = std::move(pipe);
    InputEvent event;
    event.kind = InputKind::Key;
    event.key.action = KeyAction::Down;
    event.key.hid_usage = 0x04;
    std::string rpc_error;
    const bool accepted = request_portable_helper_input_event(
        service_connection, authorization, grant, 301, event, 5000,
        &rpc_error);
    check_child(!accepted && service_connection.channel == nullptr &&
                    authorization.state() ==
                        PortableHelperAuthorizationState::Revoked,
                "a negative correlated input ACK closes the pipe and revokes the parent forwarding gate");
    check_child(WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0,
                "the isolated helper observes parent-channel closure after a rejected RPC");
    DWORD exit_code = 1;
    check_child(GetExitCodeProcess(child.hProcess, &exit_code) && exit_code == 7,
                "helper exits through its failed-control-channel cleanup path");
    CloseHandle(child.hProcess);
    child.hProcess = nullptr;
}

void portable_helper_service_session_worker_control_loop() {
    using namespace chast::host;
    PortableHelperProcessIdentity parent;
    std::string error;
    require(current_portable_helper_process_identity(&parent, &error) &&
                parent.session_id != 0,
            "worker-control integration test captures its interactive parent identity");
    PortableHelperInstanceId instance{};
    PortableHelperConnectionId connection{};
    require(secure_random(instance.data(), instance.size()) &&
                secure_random(connection.data(), connection.size()),
            "worker-control integration test creates unique service and connection scopes");

    const auto executable = executable_path();
    const std::wstring command = L"\"" + executable +
        L"\" --portable-helper-worker-control-server " +
        widen(hex_encode(instance.data(), instance.size())) + L" " +
        std::to_wstring(parent.process_id) + L" " +
        std::to_wstring(parent.creation_time);
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    require(CreateProcessW(executable.c_str(), command_line.data(), nullptr,
                           nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &child) != FALSE,
            "launch an isolated helper with the session-worker control loop");
    CloseHandle(child.hThread);
    const auto stop_child = [&] {
        if (child.hProcess == nullptr) return;
        if (WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0)
            (void)TerminateProcess(child.hProcess, ERROR_CANCELLED);
        (void)WaitForSingleObject(child.hProcess, 5000);
    };
    const auto check_child = [&](bool condition, const char* message) {
        if (condition) return;
        stop_child();
        if (child.hProcess != nullptr) CloseHandle(child.hProcess);
        child.hProcess = nullptr;
        require(false, message);
    };

    PortableHelperProcessIdentity helper_identity;
    check_child(query_portable_helper_process_identity(
                    child.dwProcessId, &helper_identity, &error),
                "worker-control integration test queries the helper identity");
    auto control = PortableHelperPipe::ConnectClient(
        instance, helper_identity, 5000, &error);
    check_child(control != nullptr,
                "parent connects only to the exact worker-control helper");

    constexpr uint32_t operations =
        static_cast<uint32_t>(PortableHelperOperation::CaptureFrames) |
        static_cast<uint32_t>(PortableHelperOperation::SendKeyboardInput);
    PortableHelperAuthorization parent_authorization;
    PortableHelperGrant parent_grant;
    require(begin_local_approval_test_grant(
                parent_authorization, instance, connection,
                parent.session_id, operations, &parent_grant, &error),
            "parent creates a capability only after simulated local approval");

    PortableHelperGrant wrong_session_grant = parent_grant;
    ++wrong_session_grant.windows_session_id;
    PortableHelperMessage start;
    require(make_portable_helper_session_worker_start_request(
                wrong_session_grant, 200, &start, &error) &&
                control->Send(std::move(start), 5000, &error),
            "parent sends a malformed-scope worker start for negative coverage");
    PortableHelperMessage response;
    check_child(control->Receive(&response, 5000, &error) &&
                    response.type == PortableHelperMessageType::Acknowledgement &&
                    response.request_id == 200 &&
                    response.payload == std::vector<uint8_t>{0},
                "helper rejects a grant targeting another Windows session");

    PortableHelperServiceConnection service_connection;
    service_connection.instance_id = instance;
    service_connection.channel = std::move(control);
    PortableHelperPipe& control_pipe = *service_connection.channel;
    check_child(start_portable_helper_session_worker(
                    service_connection, parent_authorization, parent_grant,
                    10000, &error) &&
                    parent_authorization.state() ==
                        PortableHelperAuthorizationState::Ready,
                "parent caller starts the worker and opens its request gate only after correlated ACK and WorkerReady proof");

    PortableHelperCapturedFrame frame;
    check_child(request_portable_helper_capture_frame(
                    service_connection, parent_authorization, parent_grant,
                    202, 5000, &frame, &error) &&
                    frame.width == 2 && frame.height == 2 &&
                    frame.frame_number == 23 && !frame.png.empty(),
                "parent RPC atomically receives only the worker's grant-bound captured PNG chunks");
    if (!frame.png.empty())
        SecureZeroMemory(frame.png.data(), frame.png.size());

    PortableHelperCapturedFrame concurrent_frame_a;
    PortableHelperCapturedFrame concurrent_frame_b;
    auto capture_a = std::async(std::launch::async, [&] {
        std::string request_error;
        return request_portable_helper_capture_frame(
            service_connection, parent_authorization, parent_grant, 208,
            5000, &concurrent_frame_a, &request_error);
    });
    auto capture_b = std::async(std::launch::async, [&] {
        std::string request_error;
        return request_portable_helper_capture_frame(
            service_connection, parent_authorization, parent_grant, 209,
            5000, &concurrent_frame_b, &request_error);
    });
    check_child(capture_a.get() && capture_b.get() &&
                    concurrent_frame_a.width == 2 &&
                    concurrent_frame_b.width == 2 &&
                    !concurrent_frame_a.png.empty() &&
                    !concurrent_frame_b.png.empty(),
                "concurrent parent capture callers serialize complete request/response pairs without cross-correlating frames");
    SecureZeroMemory(concurrent_frame_a.png.data(),
                     concurrent_frame_a.png.size());
    SecureZeroMemory(concurrent_frame_b.png.data(),
                     concurrent_frame_b.png.size());

    InputEvent key;
    key.kind = InputKind::Key;
    key.key.action = KeyAction::Down;
    key.key.hid_usage = 0x04;
    check_child(request_portable_helper_input_event(
                    service_connection, parent_authorization, parent_grant,
                    203, key, 5000, &error),
                "parent RPC waits for the worker's correlated typed input completion acknowledgement");

    check_child(stop_portable_helper_session_worker(
                    service_connection, parent_authorization, parent_grant,
                    10000, &error) &&
                    parent_authorization.state() ==
                        PortableHelperAuthorizationState::Revoked,
                "parent caller revokes its forwarding gate after helper confirms worker exit");

    PortableHelperConnectionId second_connection{};
    require(secure_random(second_connection.data(), second_connection.size()),
            "single-use worker test creates a fresh connection generation");
    PortableHelperAuthorization second_authorization;
    PortableHelperGrant second_grant;
    require(begin_local_approval_test_grant(
                second_authorization, instance, second_connection,
                parent.session_id, operations, &second_grant, &error) &&
                make_portable_helper_session_worker_start_request(
                    second_grant, 207, &start, &error) &&
                control_pipe.Send(std::move(start), 5000, &error),
            "parent tries to reuse the UAC-approved helper for a second connection");
    check_child(control_pipe.Receive(&response, 5000, &error) &&
                    response.type == PortableHelperMessageType::Acknowledgement &&
                    response.request_id == 207 &&
                    response.payload == std::vector<uint8_t>{0},
                "one helper authorization cannot be replayed to start another session worker");

    PortableHelperMessage status;
    status.type = PortableHelperMessageType::Status;
    status.request_id = 205;
    require(control_pipe.Send(std::move(status), 5000, &error),
            "helper remains available for cleanup after its worker stops");
    check_child(control_pipe.Receive(&response, 5000, &error) &&
                    response.type == PortableHelperMessageType::Acknowledgement &&
                    response.request_id == 205 &&
                    response.payload == std::vector<uint8_t>{1},
                "helper reports control readiness after per-connection cleanup");

    PortableHelperMessage stop_service;
    stop_service.type = PortableHelperMessageType::Stop;
    stop_service.request_id = 206;
    require(control_pipe.Send(std::move(stop_service), 5000, &error),
            "parent stops the now-idle helper service");
    check_child(control_pipe.Receive(&response, 5000, &error) &&
                    response.type == PortableHelperMessageType::Acknowledgement &&
                    response.request_id == 206 &&
                    response.payload == std::vector<uint8_t>{1},
                "helper acknowledges service stop after worker cleanup");
    service_connection.Reset();
    check_child(WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0,
                "worker-control helper exits after clean session-worker shutdown");
    DWORD exit_code = 1;
    check_child(GetExitCodeProcess(child.hProcess, &exit_code) && exit_code == 0,
                "worker-control helper child exits successfully");
    CloseHandle(child.hProcess);
    child.hProcess = nullptr;
}

void portable_helper_service_requires_emergency_input_release() {
    using namespace chast::host;
    PortableHelperProcessIdentity parent;
    std::string error;
    require(current_portable_helper_process_identity(&parent, &error) &&
                parent.session_id != 0,
            "input-release policy test captures the interactive parent identity");
    PortableHelperInstanceId instance{};
    PortableHelperConnectionId connection{};
    require(secure_random(instance.data(), instance.size()) &&
                secure_random(connection.data(), connection.size()),
            "input-release policy test creates isolated scope identifiers");
    const auto executable = executable_path();
    const std::wstring command = L"\"" + executable +
        L"\" --portable-helper-worker-no-release-control-server " +
        widen(hex_encode(instance.data(), instance.size())) + L" " +
        std::to_wstring(parent.process_id) + L" " +
        std::to_wstring(parent.creation_time);
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    require(CreateProcessW(executable.c_str(), command_line.data(), nullptr,
                           nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &child) != FALSE,
            "launch a helper without an emergency input-release provider");
    CloseHandle(child.hThread);
    auto cleanup_child = [&] {
        if (WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0)
            (void)TerminateProcess(child.hProcess, ERROR_CANCELLED);
        (void)WaitForSingleObject(child.hProcess, 5000);
        CloseHandle(child.hProcess);
    };

    PortableHelperProcessIdentity helper_identity;
    if (!query_portable_helper_process_identity(child.dwProcessId,
                                               &helper_identity, &error)) {
        cleanup_child();
        require(false, "query the helper missing its input-release provider");
    }
    auto control = PortableHelperPipe::ConnectClient(
        instance, helper_identity, 5000, &error);
    if (!control) {
        cleanup_child();
        require(false, "connect to the helper input-release policy test");
    }

    PortableHelperAuthorization authorization;
    PortableHelperGrant grant;
    const uint32_t input_operation = static_cast<uint32_t>(
        PortableHelperOperation::SendKeyboardInput);
    require(begin_local_approval_test_grant(
                authorization, instance, connection, parent.session_id,
                input_operation, &grant, &error),
            "create a locally authorized keyboard grant for policy rejection");
    PortableHelperMessage start;
    require(make_portable_helper_session_worker_start_request(
                grant, 301, &start, &error) &&
                control->Send(std::move(start), 5000, &error),
            "request a keyboard worker without an emergency cleanup provider");
    PortableHelperMessage response;
    const bool received = control->Receive(&response, 5000, &error);
    if (!received || response.type != PortableHelperMessageType::Acknowledgement ||
        response.request_id != 301 ||
        response.payload != std::vector<uint8_t>{0} ||
        authorization.state() !=
            PortableHelperAuthorizationState::AwaitingWorkerReady) {
        std::fprintf(stderr,
                     "input-release refusal: received=%d type=%u id=%llu "
                     "payload-size=%zu payload=%u auth-state=%u error=%s\n",
                     received ? 1 : 0, static_cast<unsigned>(response.type),
                     static_cast<unsigned long long>(response.request_id),
                     response.payload.size(),
                     response.payload.empty() ? 255u : response.payload[0],
                     static_cast<unsigned>(authorization.state()), error.c_str());
    }
    const bool rejected = received &&
        response.type == PortableHelperMessageType::Acknowledgement &&
        response.request_id == 301 &&
        response.payload == std::vector<uint8_t>{0} &&
        authorization.state() == PortableHelperAuthorizationState::AwaitingWorkerReady;
    require(authorization.Revoke([] { return true; }, [] { return true; },
                                 &error),
            "parent revokes its pending grant after helper rejects unsafe input startup");
    require(rejected,
            "helper refuses input capability until emergency release is configured");

    PortableHelperMessage stop;
    stop.type = PortableHelperMessageType::Stop;
    stop.request_id = 302;
    require(control->Send(std::move(stop), 5000, &error) &&
                control->Receive(&response, 5000, &error) &&
                response.type == PortableHelperMessageType::Acknowledgement &&
                response.request_id == 302 &&
                response.payload == std::vector<uint8_t>{1},
            "a rejected input grant leaves the helper available for safe shutdown");
    control.reset();
    require(WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0,
            "input-release policy helper exits after clean Stop");
    DWORD exit_code = 1;
    require(GetExitCodeProcess(child.hProcess, &exit_code) && exit_code == 0,
            "input-release policy helper returns success");
    CloseHandle(child.hProcess);
}

void portable_helper_session_worker_pipe() {
    using namespace chast::host;
    PortableHelperProcessIdentity broker;
    std::wstring logon_sid;
    std::string error;
    require(current_portable_helper_process_identity(&broker, &error) &&
                broker.session_id != 0 &&
                current_portable_helper_logon_sid(&logon_sid, &error) &&
                logon_sid == broker.logon_sid,
            "worker endpoint test captures its interactive broker identity");

    PortableHelperInstanceId instance{}, connection{};
    require(secure_random(instance.data(), instance.size()) &&
                secure_random(connection.data(), connection.size()),
            "worker endpoint test generates independent instance and session IDs");
    PortableHelperInstanceId zero_connection{};
    require(PortableHelperPipe::SessionWorkerEndpointName(
                instance, zero_connection).empty(),
            "worker endpoint derivation rejects an empty connection scope");
    const std::wstring control_name = PortableHelperPipe::EndpointName(instance);
    const std::wstring worker_name =
        PortableHelperPipe::SessionWorkerEndpointName(instance, connection);
    require(!control_name.empty() && !worker_name.empty() &&
                control_name != worker_name &&
                worker_name.find(widen(hex_encode(connection.data(),
                                                  connection.size()))) !=
                    std::wstring::npos,
            "session worker endpoint is distinct and scoped to its connection");

    auto control = PortableHelperPipe::CreateServer(instance, logon_sid, &error);
    require(control != nullptr,
            "create the existing per-instance broker control endpoint");
    auto worker = PortableHelperPipe::CreateSessionWorkerServer(
        instance, connection, logon_sid, &error);
    require(worker != nullptr,
            "create a separate authenticated endpoint for one session worker");
    PortableHelperInstanceId wrong_connection{};
    require(secure_random(wrong_connection.data(), wrong_connection.size()),
            "generate a distinct connection scope for a negative endpoint probe");
    require(!PortableHelperPipe::ConnectSessionWorkerClient(
                instance, wrong_connection, broker, 0, &error),
            "a different connection scope cannot attach to this worker endpoint");

    const auto executable = executable_path();
    const auto launch_client = [&](bool worker_client,
                                   const PortableHelperInstanceId& channel_id,
                                   PROCESS_INFORMATION* process) {
        std::wstring command = L"\"" + executable + L"\" ";
        if (worker_client) {
            command += L"--portable-helper-session-worker-client ";
            command += widen(hex_encode(instance.data(), instance.size())) + L" ";
            command += widen(hex_encode(channel_id.data(), channel_id.size())) + L" ";
        } else {
            command += L"--portable-helper-pipe-client ";
            command += widen(hex_encode(instance.data(), instance.size())) + L" ";
        }
        command += std::to_wstring(broker.process_id) + L" " +
                   std::to_wstring(broker.creation_time);
        std::vector<wchar_t> command_line(command.begin(), command.end());
        command_line.push_back(L'\0');
        STARTUPINFOW startup{sizeof(startup)};
        return CreateProcessW(executable.c_str(), command_line.data(), nullptr,
                              nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                              &startup, process) != FALSE;
    };

    PROCESS_INFORMATION control_client{}, worker_client{};
    require(launch_client(false, {}, &control_client),
            "launch isolated client for the broker control endpoint");
    CloseHandle(control_client.hThread);
    const auto terminate_if_live = [](HANDLE process) {
        if (process != nullptr && process != INVALID_HANDLE_VALUE &&
            WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
            TerminateProcess(process, ERROR_CANCELLED);
            (void)WaitForSingleObject(process, 5000);
        }
    };
    try {
        require(launch_client(true, connection, &worker_client),
                "launch an isolated client for the session-worker endpoint");
        CloseHandle(worker_client.hThread);

        PortableHelperProcessIdentity expected_control, expected_worker;
        require(query_portable_helper_process_identity(
                    control_client.dwProcessId, &expected_control, &error) &&
                    query_portable_helper_process_identity(
                        worker_client.dwProcessId, &expected_worker, &error),
                "capture exact child identities before accepting either pipe");
        require(control->Accept(expected_control, 5000, &error) &&
                    worker->Accept(expected_worker, 5000, &error),
                "each endpoint authenticates only its own process connection");

        PortableHelperMessage challenge;
        challenge.type = PortableHelperMessageType::ServiceChallenge;
        challenge.request_id = 77;
        challenge.payload = {1, 2, 3};
        require(control->Send(challenge, 5000, &error),
                "send a control-channel challenge independently");
        require(worker->Send(std::move(challenge), 5000, &error),
                "send a session-worker challenge independently");

        PortableHelperMessage control_ack, worker_ack;
        require(control->Receive(&control_ack, 5000, &error) &&
                    control_ack.request_id == 77 &&
                    control_ack.payload == std::vector<uint8_t>({4, 5, 6}) &&
                    worker->Receive(&worker_ack, 5000, &error) &&
                    worker_ack.request_id == 77 &&
                    worker_ack.payload == std::vector<uint8_t>({4, 5, 6}),
                "control and worker pipes keep independent authenticated traffic");

        PortableHelperCapturedFrame original_frame;
        original_frame.width = 1024;
        original_frame.height = 1024;
        original_frame.frame_number = 0x1020304050607080ull;
        std::vector<uint8_t> bgra(
            static_cast<size_t>(original_frame.width) *
            original_frame.height * 4);
        uint32_t noise = 0x6d2b79f5u;
        for (size_t pixel = 0; pixel < bgra.size(); pixel += 4) {
            noise ^= noise << 13;
            noise ^= noise >> 17;
            noise ^= noise << 5;
            bgra[pixel] = static_cast<uint8_t>(noise);
            bgra[pixel + 1] = static_cast<uint8_t>(noise >> 8);
            bgra[pixel + 2] = static_cast<uint8_t>(noise >> 16);
            bgra[pixel + 3] = 255;
        }
        require(encode_png_bgra(bgra.data(), original_frame.width,
                                original_frame.height,
                                original_frame.width * 4,
                                original_frame.png, &error) &&
                    original_frame.png.size() > kPortableHelperMaxPayloadBytes,
                "encode a valid PNG large enough to require multiple IPC frames");
        PortableHelperGrant frame_grant;
        frame_grant.instance_id = instance;
        frame_grant.connection_id = connection;
        frame_grant.generation = 7;
        frame_grant.windows_session_id = broker.session_id;
        frame_grant.operations = static_cast<uint32_t>(
            PortableHelperOperation::CaptureFrames);
        std::vector<PortableHelperMessage> frame_chunks;
        require(make_portable_helper_capture_frame_chunks(
                    frame_grant, 79, original_frame, &frame_chunks, &error) &&
                    frame_chunks.size() > 1,
                "split a captured PNG into grant-scoped bounded chunks");
        PortableHelperMessage transfer_begin;
        transfer_begin.type = PortableHelperMessageType::Acknowledgement;
        transfer_begin.request_id = 78;
        const uint32_t chunk_count =
            static_cast<uint32_t>(frame_chunks.size());
        for (unsigned shift = 0; shift < 32; shift += 8)
            transfer_begin.payload.push_back(
                static_cast<uint8_t>(chunk_count >> shift));
        require(worker->Send(std::move(transfer_begin), 5000, &error),
                "announce the bounded chunk count to the isolated worker peer");
        PortableHelperMessage transfer_ready;
        require(worker->Receive(&transfer_ready, 5000, &error) &&
                    transfer_ready.type ==
                        PortableHelperMessageType::Acknowledgement &&
                    transfer_ready.request_id == 78 &&
                    transfer_ready.payload == std::vector<uint8_t>{1},
                "worker peer is ready for the PNG chunk roundtrip");
        require(send_portable_helper_captured_frame(
                    *worker, frame_grant, 79, original_frame, 5000, &error),
                "send the bounded PNG response over the authenticated pipe");
        PortableHelperCapturedFrame returned_frame;
        require(receive_portable_helper_captured_frame(
                    *worker, frame_grant, 79, &returned_frame, 5000, &error) &&
                    returned_frame.width == original_frame.width &&
                    returned_frame.height == original_frame.height &&
                    returned_frame.frame_number == original_frame.frame_number &&
                    returned_frame.png == original_frame.png,
                "reassemble an exact greater-than-1-MiB PNG pipe roundtrip");

        std::vector<PortableHelperMessage> echoed_chunks;
        require(make_portable_helper_capture_frame_chunks(
                    frame_grant, 79, returned_frame, &echoed_chunks, &error),
                "recreate captured-frame chunks for malformed-order checks");
        auto reordered_chunks = echoed_chunks;
        std::swap(reordered_chunks[0], reordered_chunks[1]);
        require(!reassemble_portable_helper_capture_frame_chunks(
                    frame_grant, 79, reordered_chunks, &returned_frame, &error) &&
                    returned_frame.png.empty(),
                "reject reordered PNG chunks and clear partial output");
        auto wrong_scope_chunks = echoed_chunks;
        wrong_scope_chunks[0].payload[0] ^= 0x01;
        require(!reassemble_portable_helper_capture_frame_chunks(
                    frame_grant, 79, wrong_scope_chunks, &returned_frame,
                    &error),
                "reject PNG chunks bound to a different connection");
        auto truncated_chunks = echoed_chunks;
        truncated_chunks.back().payload.pop_back();
        require(!reassemble_portable_helper_capture_frame_chunks(
                    frame_grant, 79, truncated_chunks, &returned_frame,
                    &error),
                "reject truncated PNG chunk payloads");

        require(WaitForSingleObject(control_client.hProcess, 5000) ==
                    WAIT_OBJECT_0 &&
                    WaitForSingleObject(worker_client.hProcess, 5000) ==
                    WAIT_OBJECT_0,
                "both isolated channel peers exit after their own exchange");
        DWORD control_exit = 1, worker_exit = 1;
        require(GetExitCodeProcess(control_client.hProcess, &control_exit) &&
                    GetExitCodeProcess(worker_client.hProcess, &worker_exit) &&
                    control_exit == 0 && worker_exit == 0,
                "both authenticated channel exchanges complete successfully");
    } catch (...) {
        terminate_if_live(control_client.hProcess);
        terminate_if_live(worker_client.hProcess);
        CloseHandle(control_client.hProcess);
        if (worker_client.hProcess != nullptr)
            CloseHandle(worker_client.hProcess);
        throw;
    }
    CloseHandle(control_client.hProcess);
    CloseHandle(worker_client.hProcess);
}

void portable_helper_session_worker_handshake() {
    using namespace chast::host;
    PortableHelperProcessIdentity broker;
    std::wstring logon_sid;
    std::string error;
    require(current_portable_helper_process_identity(&broker, &error) &&
                broker.session_id != 0 &&
                current_portable_helper_logon_sid(&logon_sid, &error) &&
                logon_sid == broker.logon_sid,
            "session-worker handshake captures its interactive broker identity");

    PortableHelperServiceSpec spec;
    require(secure_random(spec.instance_id.data(), spec.instance_id.size()),
            "session-worker handshake generates a helper instance ID");
    PortableHelperConnectionId connection{};
    require(secure_random(connection.data(), connection.size()),
            "session-worker handshake generates a connection generation scope");
    spec.executable_path = executable_path();
    spec.parent = broker;

    constexpr uint32_t operations =
        static_cast<uint32_t>(PortableHelperOperation::CaptureFrames) |
        static_cast<uint32_t>(PortableHelperOperation::SendKeyboardInput);
    PortableHelperAuthorization authorization;
    PortableHelperGrant grant;
    require(begin_local_approval_test_grant(
                authorization, spec.instance_id, connection,
                broker.session_id, operations, &grant, &error),
            "create a test capability after simulated local approval");

    std::unique_ptr<PortableHelperPipe> channel;
    HANDLE worker_process = nullptr;
    PortableHelperProcessIdentity worker_identity;
    struct Cleanup final {
        HANDLE* process;
        std::unique_ptr<PortableHelperPipe>* channel;
        ~Cleanup() {
            if (channel != nullptr) channel->reset();
            if (process == nullptr || *process == nullptr ||
                *process == INVALID_HANDLE_VALUE)
                return;
            if (WaitForSingleObject(*process, 0) != WAIT_OBJECT_0) {
                (void)TerminateProcess(*process, ERROR_CANCELLED);
                (void)WaitForSingleObject(*process, 5000);
            }
            CloseHandle(*process);
            *process = nullptr;
        }
    } cleanup{&worker_process, &channel};

    const std::wstring executable = spec.executable_path;
    const auto launcher = [&](const PortableHelperServiceSpec& launch_spec,
                              const PortableHelperGrant& launch_grant,
                              HANDLE* process, std::string* launch_error) {
        std::wstring command = L"\"" + executable +
            L"\" --portable-helper-session-worker-auth-client " +
            widen(hex_encode(launch_spec.instance_id.data(),
                             launch_spec.instance_id.size())) + L" " +
            widen(hex_encode(launch_grant.connection_id.data(),
                             launch_grant.connection_id.size())) + L" " +
            std::to_wstring(broker.process_id) + L" " +
            std::to_wstring(broker.creation_time);
        std::vector<wchar_t> command_line(command.begin(), command.end());
        command_line.push_back(L'\0');
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION created{};
        if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr,
                            nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                            &startup, &created)) {
            if (launch_error != nullptr)
                *launch_error = "cannot launch isolated session-worker test peer";
            return false;
        }
        CloseHandle(created.hThread);
        *process = created.hProcess;
        return true;
    };

    require(start_portable_helper_session_worker_channel(
                spec, authorization, grant, launcher, 5000, &channel,
                &worker_process, &worker_identity, &error),
            "launch, authenticate and complete WorkerReady on the session pipe");
    require(channel != nullptr && worker_process != nullptr &&
                worker_identity.process_id == GetProcessId(worker_process) &&
                worker_identity.session_id == broker.session_id &&
                _wcsicmp(worker_identity.image_path.c_str(),
                         spec.executable_path.c_str()) == 0 &&
                authorization.state() ==
                    PortableHelperAuthorizationState::Ready,
            "the coordinator returns only the exact ready worker and channel");

    require(send_portable_helper_capture_request(
                *channel, authorization, grant, 501, 5000, &error),
            "the ready session worker accepts a scoped capture request");
    PortableHelperCapturedFrame captured;
    require(receive_portable_helper_captured_frame(
                *channel, grant, 501, &captured, 5000, &error) &&
                captured.width == 2 && captured.height == 2 &&
                captured.frame_number == 23 && !captured.png.empty(),
            "the session worker returns the leased capture as bounded PNG chunks");

    InputEvent key;
    key.kind = InputKind::Key;
    key.key.action = KeyAction::Down;
    key.key.hid_usage = 0x04;
    require(send_portable_helper_input_request(
                *channel, authorization, grant, 502, key, 5000, &error),
            "the ready session worker accepts a typed keyboard input request");
    PortableHelperMessage input_ack;
    require(channel->Receive(&input_ack, 5000, &error) &&
                input_ack.type == PortableHelperMessageType::Acknowledgement &&
                input_ack.request_id == 502 &&
                input_ack.payload == std::vector<uint8_t>{1},
            "the session worker acknowledges input only after its adapter completes");

    bool released = false;
    PortableHelperWorkerRevocationCallbacks callbacks;
    require(make_win32_portable_helper_revocation_callbacks(
                worker_process, channel,
                [&](std::string*) {
                    released = true;
                    return true;
                },
                &callbacks, &error),
            "bind session-worker cleanup to the authenticated process and pipe");
    require(revoke_portable_helper_worker(authorization, callbacks, 1000, 5000,
                                          &error),
            "revoke and stop the authenticated session worker in bounded order");
    DWORD exit_code = 1;
    require(released && authorization.state() ==
                             PortableHelperAuthorizationState::Revoked &&
                channel == nullptr &&
                WaitForSingleObject(worker_process, 0) == WAIT_OBJECT_0 &&
                GetExitCodeProcess(worker_process, &exit_code) && exit_code == 0,
            "revocation releases input only after the session worker exits");
}

void portable_helper_supervisor_releases_input_after_worker_exit_without_ack() {
    using namespace chast::host;
    PortableHelperProcessIdentity broker;
    std::string error;
    require(current_portable_helper_process_identity(&broker, &error) &&
                broker.session_id != 0,
            "worker cleanup-ack test captures its interactive broker identity");

    PortableHelperInstanceId instance{};
    PortableHelperConnectionId connection{};
    require(secure_random(instance.data(), instance.size()) &&
                secure_random(connection.data(), connection.size()),
            "worker cleanup-ack test generates scoped endpoint identifiers");
    auto channel = PortableHelperPipe::CreateSessionWorkerServer(
        instance, connection, broker.logon_sid, &error);
    require(channel != nullptr,
            "create an authenticated worker pipe for missing cleanup acknowledgement");

    const auto executable = executable_path();
    std::wstring command = L"\"" + executable +
        L"\" --portable-helper-session-worker-no-stop-ack-client " +
        widen(hex_encode(instance.data(), instance.size())) + L" " +
        widen(hex_encode(connection.data(), connection.size())) + L" " +
        std::to_wstring(broker.process_id) + L" " +
        std::to_wstring(broker.creation_time);
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION created{};
    require(CreateProcessW(executable.c_str(), command_line.data(), nullptr,
                           nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &created) != FALSE,
            "launch an isolated worker peer that deliberately omits cleanup acknowledgement");
    CloseHandle(created.hThread);
    HANDLE worker_process = created.hProcess;
    struct Cleanup final {
      std::unique_ptr<PortableHelperPipe>* channel;
      HANDLE* process;
      ~Cleanup() {
        if (channel != nullptr) channel->reset();
        if (process == nullptr || *process == nullptr) return;
        if (WaitForSingleObject(*process, 0) != WAIT_OBJECT_0) {
          (void)TerminateProcess(*process, ERROR_CANCELLED);
          (void)WaitForSingleObject(*process, 5000);
        }
        CloseHandle(*process);
        *process = nullptr;
      }
    } cleanup{&channel, &worker_process};

    PortableHelperProcessIdentity worker;
    const bool queried_worker = query_portable_helper_process_identity(
        GetProcessId(worker_process), &worker, &error);
    if (!queried_worker)
        std::fprintf(stderr, "query missing-ack worker identity: %s\n",
                     error.c_str());
    require(queried_worker,
            "query the exact child before requesting worker cleanup");
    error.clear();
    const bool authenticated_worker = channel->Accept(worker, 5000, &error);
    if (!authenticated_worker) {
        DWORD child_exit_code = STILL_ACTIVE;
        (void)GetExitCodeProcess(worker_process, &child_exit_code);
        std::fprintf(stderr,
                     "authenticate missing-ack worker pipe peer: %s "
                     "(child exit code %lu)\n",
                     error.c_str(), static_cast<unsigned long>(child_exit_code));
    }
    require(authenticated_worker,
            "authenticate the exact child before requesting worker cleanup");

    PortableHelperAuthorization authorization;
    PortableHelperGrant grant;
    constexpr uint32_t input_operations = static_cast<uint32_t>(
        PortableHelperOperation::SendKeyboardInput);
    require(begin_local_approval_test_grant(
                authorization, instance, connection, broker.session_id,
                input_operations, &grant, &error),
            "create a scoped input grant for the no-ack shutdown case");
    PortableHelperSecret proof{};
    PortableHelperProcessIdentity authenticated_peer;
    require(make_portable_helper_worker_ready_proof(grant, &proof, &error) &&
                channel->AuthenticatedPeer(&authenticated_peer, &error) &&
                authorization.ConfirmWorkerReady(grant, worker,
                                                  authenticated_peer, proof,
                                                  &error),
            "mark only the authenticated worker process ready for revocation");
    SecureZeroMemory(proof.data(), proof.size());

    bool external_release_called = false;
    PortableHelperWorkerRevocationCallbacks callbacks;
    require(make_win32_portable_helper_revocation_callbacks(
                worker_process, channel,
                [&](std::string*) {
                  external_release_called = true;
                  return true;
                },
                &callbacks, &error),
            "bind Stop and input-release callbacks to the authenticated worker");
    std::string release_error;
    require(!callbacks.release_input(&release_error) &&
                !external_release_called && !release_error.empty(),
            "external input cleanup is blocked while the worker may still inject");
    const bool revoked = revoke_portable_helper_worker(
        authorization, callbacks, 1000, 5000, &error);
    require(!revoked && !error.empty() &&
                authorization.state() ==
                    PortableHelperAuthorizationState::Revoked &&
                external_release_called && channel == nullptr &&
                WaitForSingleObject(worker_process, 0) == WAIT_OBJECT_0,
            "missing Stop acknowledgement is reported, but input is released after confirmed worker exit");
}

void portable_helper_winlogon_token_fail_closed() {
    using namespace chast::host;
    PortableHelperProcessIdentity parent;
    std::string error;
    require(current_portable_helper_process_identity(&parent, &error) &&
                parent.session_id != 0,
            "winlogon-token policy test runs under an interactive parent");

    PortableHelperInstanceId instance{};
    require(secure_random(instance.data(), instance.size()),
            "winlogon-token policy test generates a helper instance");
    std::wstring staging_directory;
    std::wstring staged_image;
    require(portable_helper_staging_paths(instance, &staging_directory,
                                           &staged_image, &error),
            "derive a canonical staged helper path without creating files");
    PortableHelperServiceSpec spec;
    require(make_portable_helper_service_spec(instance, staged_image, parent,
                                              &spec, &error),
            "bind the test worker-token request to the interactive parent");

    HANDLE token = nullptr;
    error.clear();
    const bool opened = open_portable_helper_session_worker_token(
        spec, &token, &error);
    if (token != nullptr) CloseHandle(token);
    require(!opened && token == nullptr && !error.empty(),
            "an interactive host process cannot obtain the Session 0 SYSTEM worker token");

    PortableHelperGrant grant;
    grant.instance_id = instance;
    require(secure_random(grant.connection_id.data(), grant.connection_id.size()),
            "worker launch policy test creates a scoped connection ID");
    grant.generation = 1;
    grant.windows_session_id = parent.session_id;
    grant.operations = static_cast<uint32_t>(PortableHelperOperation::CaptureFrames);
    HANDLE process = nullptr;
    error.clear();
    const bool launched = launch_portable_helper_session_worker_process(
        spec, grant, &process, &error);
    if (process != nullptr) {
        TerminateProcess(process, ERROR_CANCELLED);
        WaitForSingleObject(process, 5000);
        CloseHandle(process);
    }
    require(!launched && process == nullptr && !error.empty(),
            "an interactive host process cannot launch a SYSTEM desktop worker");
}

void portable_helper_session_worker_rejects_forged_ready() {
    using namespace chast::host;
    PortableHelperProcessIdentity broker;
    std::wstring logon_sid;
    std::string error;
    require(current_portable_helper_process_identity(&broker, &error) &&
                broker.session_id != 0 &&
                current_portable_helper_logon_sid(&logon_sid, &error) &&
                logon_sid == broker.logon_sid,
            "negative worker-start test captures the broker logon identity");

    PortableHelperServiceSpec spec;
    require(secure_random(spec.instance_id.data(), spec.instance_id.size()),
            "negative worker-start test generates a helper instance ID");
    PortableHelperConnectionId connection{};
    require(secure_random(connection.data(), connection.size()),
            "negative worker-start test generates a connection ID");
    spec.executable_path = executable_path();
    spec.parent = broker;
    PortableHelperAuthorization authorization;
    PortableHelperGrant grant;
    const uint32_t operations =
        static_cast<uint32_t>(PortableHelperOperation::CaptureFrames) |
        static_cast<uint32_t>(PortableHelperOperation::SendKeyboardInput);
    require(begin_local_approval_test_grant(
                authorization, spec.instance_id, connection,
                broker.session_id, operations, &grant, &error),
            "negative worker-start test creates a scoped pending grant");

    std::unique_ptr<PortableHelperPipe> channel;
    HANDLE worker_process = nullptr;
    PortableHelperProcessIdentity worker_identity;
    struct Cleanup final {
        HANDLE* process;
        std::unique_ptr<PortableHelperPipe>* channel;
        ~Cleanup() {
            if (channel != nullptr) channel->reset();
            if (process == nullptr || *process == nullptr ||
                *process == INVALID_HANDLE_VALUE)
                return;
            if (WaitForSingleObject(*process, 0) != WAIT_OBJECT_0) {
                (void)TerminateProcess(*process, ERROR_CANCELLED);
                (void)WaitForSingleObject(*process, 5000);
            }
            CloseHandle(*process);
            *process = nullptr;
        }
    } cleanup{&worker_process, &channel};

    bool launched = false;
    auto mismatched = grant;
    ++mismatched.windows_session_id;
    const PortableHelperSessionWorkerLauncher should_not_launch =
        [&](const PortableHelperServiceSpec&, const PortableHelperGrant&,
            HANDLE*, std::string*) {
          launched = true;
          return false;
        };
    require(!start_portable_helper_session_worker_channel(
                spec, authorization, mismatched, should_not_launch, 100, &channel,
                &worker_process, &worker_identity, &error) &&
                !launched && channel == nullptr && worker_process == nullptr &&
                authorization.state() ==
                    PortableHelperAuthorizationState::AwaitingWorkerReady,
            "a grant targeting another session is rejected before process launch");

    const std::wstring executable = spec.executable_path;
    const PortableHelperSessionWorkerLauncher forged_ready_launcher =
        [&](const PortableHelperServiceSpec& launch_spec,
            const PortableHelperGrant& launch_grant, HANDLE* process,
            std::string* launch_error) {
          std::wstring command = L"\"" + executable +
              L"\" --portable-helper-session-worker-bad-ready-client " +
              widen(hex_encode(launch_spec.instance_id.data(),
                               launch_spec.instance_id.size())) + L" " +
              widen(hex_encode(launch_grant.connection_id.data(),
                               launch_grant.connection_id.size())) + L" " +
              std::to_wstring(broker.process_id) + L" " +
              std::to_wstring(broker.creation_time);
          std::vector<wchar_t> command_line(command.begin(), command.end());
          command_line.push_back(L'\0');
          STARTUPINFOW startup{sizeof(startup)};
          PROCESS_INFORMATION created{};
          if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr,
                              nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                              &startup, &created)) {
            if (launch_error != nullptr)
              *launch_error = "cannot launch forged WorkerReady test peer";
            return false;
          }
          CloseHandle(created.hThread);
          *process = created.hProcess;
          return true;
        };
    require(!start_portable_helper_session_worker_channel(
                spec, authorization, grant, forged_ready_launcher, 5000, &channel,
                &worker_process, &worker_identity, &error) &&
                channel == nullptr && worker_process == nullptr &&
                authorization.state() ==
                    PortableHelperAuthorizationState::Revoked,
            "invalid WorkerReady proof is rejected, child terminated and grant revoked");
}

void portable_helper_pipe_transport() {
    using namespace chast::host;
    PortableHelperProcessIdentity parent;
    std::string error;
    require(current_portable_helper_process_identity(&parent, &error),
            "query the helper IPC host process identity");
    require(!parent.logon_sid.empty(),
            "helper process identity includes its Windows logon SID");
    std::wstring authorized_logon_sid;
    require(current_portable_helper_logon_sid(&authorized_logon_sid, &error) &&
                authorized_logon_sid == parent.logon_sid,
            "derive the pipe ACL SID explicitly from the authorized interactive token");
    PortableHelperInstanceId instance_id{};
    require(secure_random(instance_id.data(), instance_id.size()) &&
                std::any_of(instance_id.begin(), instance_id.end(),
                            [](uint8_t byte) { return byte != 0; }),
            "generate a unique helper IPC instance id");
    require(!PortableHelperPipe::CreateServer(instance_id, L"not-a-sid", &error),
            "reject an invalid explicitly authorized logon SID");
    auto pipe = PortableHelperPipe::CreateServer(instance_id, authorized_logon_sid,
                                                  &error);
    require(pipe != nullptr, "create a local helper pipe restricted to SYSTEM, administrators and the host logon");
    PortableHelperProcessIdentity unauthenticated_peer;
    require(!pipe->AuthenticatedPeer(&unauthenticated_peer, &error),
            "a new server pipe exposes no peer identity before Accept");
    PortableHelperMessage premature;
    premature.type = PortableHelperMessageType::ServiceChallenge;
    premature.request_id = 1;
    require(!pipe->Send(std::move(premature), 100, &error),
            "an unaccepted server pipe cannot send protocol messages");

    const auto executable = executable_path();
    const auto launch_client = [&](const PortableHelperInstanceId& client_instance,
                                   bool reject_server, bool idle = false) {
        std::wstring command = L"\"" + executable +
            L"\" --portable-helper-pipe-client " +
            widen(hex_encode(client_instance.data(), client_instance.size())) + L" " +
            std::to_wstring(parent.process_id) + L" " +
            std::to_wstring(parent.creation_time);
        if (reject_server) command += L" --reject-server-logon-sid";
        if (idle) command += L" --idle-pipe-client";
        std::vector<wchar_t> command_line(command.begin(), command.end());
        command_line.push_back(L'\0');
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        require(CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr,
                               FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                               &process) != FALSE,
                "launch the isolated local IPC client process");
        CloseHandle(process.hThread);
        return process;
    };
    PROCESS_INFORMATION child = launch_client(instance_id, false);
    PortableHelperProcessIdentity expected_child;
    require(query_portable_helper_process_identity(child.dwProcessId,
                                                    &expected_child, &error),
            "query the exact expected helper client identity");
    require(pipe->Accept(expected_child, 5000, &error),
            "accept only the expected PID, session, image, creation time and SID");
    PortableHelperProcessIdentity authenticated_child;
    require(pipe->AuthenticatedPeer(&authenticated_child, &error) &&
                authenticated_child.process_id == expected_child.process_id &&
                authenticated_child.creation_time == expected_child.creation_time,
            "the pipe retains the OS-measured identity of its authenticated peer");
    PortableHelperMessage challenge;
    challenge.type = PortableHelperMessageType::ServiceChallenge;
    challenge.request_id = 77;
    challenge.payload = {1, 2, 3};
    PortableHelperMessage invalid;
    invalid.type = PortableHelperMessageType::ServiceChallenge;
    invalid.payload.resize(kPortableHelperMaxPayloadBytes + 1);
    require(!pipe->Send(std::move(invalid), 5000, &error),
            "reject an oversized outgoing helper message locally");
    require(pipe->Send(std::move(challenge), 5000, &error),
            "a locally rejected frame does not consume the next send sequence");
    PortableHelperMessage acknowledgement;
    require(pipe->Receive(&acknowledgement, 5000, &error) &&
                acknowledgement.type == PortableHelperMessageType::Acknowledgement &&
                acknowledgement.request_id == 77 &&
                acknowledgement.payload == std::vector<uint8_t>{4, 5, 6},
            "receive the child acknowledgement with independent sequence checks");
    require(WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0,
            "portable helper IPC client exits after the exchange");
    DWORD exit_code = 1;
    require(GetExitCodeProcess(child.hProcess, &exit_code) && exit_code == 0,
            "portable helper IPC client verified the server identity and frame");
    CloseHandle(child.hProcess);
    require(!pipe->Receive(&acknowledgement, 1000, &error),
            "a disconnected helper peer terminates pending receive");
    require(!pipe->Send(std::move(challenge), 1000, &error),
            "a disconnected helper pipe cannot be reused");

    PortableHelperInstanceId timeout_instance{};
    require(secure_random(timeout_instance.data(), timeout_instance.size()),
            "generate a helper instance id for timeout cleanup");
    auto timed_out = PortableHelperPipe::CreateServer(
        timeout_instance, authorized_logon_sid, &error);
    require(timed_out != nullptr, "create a helper pipe for accept-timeout coverage");
    const auto timeout_started = std::chrono::steady_clock::now();
    require(!timed_out->Accept(parent, 50, &error),
            "accept returns false when no helper client connects before its deadline");
    require(std::chrono::steady_clock::now() - timeout_started <
                std::chrono::seconds(2),
            "accept timeout cancels pending pipe I/O promptly");
    auto reclaimed = PortableHelperPipe::CreateServer(
        timeout_instance, authorized_logon_sid, &error);
    require(reclaimed != nullptr,
            "timed-out accept closes its endpoint so the instance can be reclaimed");

    PortableHelperInstanceId mismatch_instance{};
    require(secure_random(mismatch_instance.data(), mismatch_instance.size()),
            "generate a helper instance id for peer-identity rejection");
    auto mismatch_server = PortableHelperPipe::CreateServer(
        mismatch_instance, authorized_logon_sid, &error);
    require(mismatch_server != nullptr,
            "create a helper pipe for exact peer-identity rejection");
    PROCESS_INFORMATION mismatch_child = launch_client(mismatch_instance, true);
    PortableHelperProcessIdentity expected_mismatch_child;
    require(query_portable_helper_process_identity(
                mismatch_child.dwProcessId, &expected_mismatch_child, &error),
            "query the helper client identity in peer-rejection coverage");
    require(mismatch_server->Accept(expected_mismatch_child, 5000, &error),
            "the test peer identity is established before client-side server rejection");
    require(WaitForSingleObject(mismatch_child.hProcess, 5000) == WAIT_OBJECT_0,
            "client exits after rejecting the mismatched expected server identity");
    DWORD mismatch_exit_code = 1;
    require(GetExitCodeProcess(mismatch_child.hProcess, &mismatch_exit_code) &&
                mismatch_exit_code == 0,
            "client rejects the server whose interactive logon SID does not match");
    CloseHandle(mismatch_child.hProcess);
    require(!mismatch_server->Receive(&acknowledgement, 1000, &error),
            "peer identity rejection leaves no reusable server connection");

    PortableHelperInstanceId receive_timeout_instance{};
    require(secure_random(receive_timeout_instance.data(),
                          receive_timeout_instance.size()),
            "generate an instance id for read-timeout cleanup");
    auto receive_timeout_server = PortableHelperPipe::CreateServer(
        receive_timeout_instance, authorized_logon_sid, &error);
    require(receive_timeout_server != nullptr,
            "create a pipe for receive-timeout coverage");
    PROCESS_INFORMATION idle_child = launch_client(receive_timeout_instance,
                                                   false, true);
    PortableHelperProcessIdentity expected_idle_child;
    require(query_portable_helper_process_identity(
                idle_child.dwProcessId, &expected_idle_child, &error),
            "query the idle helper client identity");
    require(receive_timeout_server->Accept(expected_idle_child, 5000, &error),
            "accept the idle local helper client");
    require(!receive_timeout_server->Receive(&acknowledgement, 50, &error),
            "receive returns false when no frame arrives before its deadline");
    require(!receive_timeout_server->Send(std::move(challenge), 1000, &error),
            "a receive-timeout pipe is closed and cannot be reused");
    require(WaitForSingleObject(idle_child.hProcess, 5000) == WAIT_OBJECT_0,
            "idle child exits after the timed-out server closes the pipe");
    DWORD idle_exit_code = 1;
    require(GetExitCodeProcess(idle_child.hProcess, &idle_exit_code) &&
                idle_exit_code == 0,
            "idle child observed clean timeout teardown");
    CloseHandle(idle_child.hProcess);
}

void portable_helper_authorization_pipe() {
    using namespace chast::host;
    PortableHelperProcessIdentity parent;
    std::string error;
    require(current_portable_helper_process_identity(&parent, &error) &&
                parent.session_id != 0,
            "authorization handshake runs under an interactive parent session");
    std::wstring authorized_logon_sid;
    require(current_portable_helper_logon_sid(&authorized_logon_sid, &error),
            "read the current logon SID for the isolated authorization pipe");
    PortableHelperInstanceId instance{};
    PortableHelperConnectionId connection{};
    require(secure_random(instance.data(), instance.size()) &&
                secure_random(connection.data(), connection.size()),
            "generate fresh helper and connection identifiers");
    auto pipe = PortableHelperPipe::CreateServer(instance, authorized_logon_sid,
                                                  &error);
    require(pipe != nullptr,
            "create a local authorization pipe scoped to the current logon");

    const auto executable = executable_path();
    std::wstring command = L"\"" + executable +
        L"\" --portable-helper-supervisor-client " +
        widen(hex_encode(instance.data(), instance.size())) + L" " +
        std::to_wstring(parent.process_id) + L" " +
        std::to_wstring(parent.creation_time);
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    require(CreateProcessW(executable.c_str(), command_line.data(), nullptr,
                           nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &child) != FALSE,
            "launch an isolated local worker-protocol peer");
    CloseHandle(child.hThread);
    PortableHelperProcessIdentity expected_worker;
    require(query_portable_helper_process_identity(child.dwProcessId,
                                                    &expected_worker, &error),
            "capture the worker process identity expected by the pipe server");
    require(pipe->Accept(expected_worker, 5000, &error),
            "accept only the exact local worker PID and token identity");
    PortableHelperProcessIdentity authenticated_worker;
    require(pipe->AuthenticatedPeer(&authenticated_worker, &error) &&
                authenticated_worker.process_id == expected_worker.process_id &&
                authenticated_worker.creation_time == expected_worker.creation_time,
            "WorkerReady consumes the process identity captured by pipe authentication");

    constexpr uint32_t operations =
        static_cast<uint32_t>(PortableHelperOperation::CaptureFrames) |
        static_cast<uint32_t>(PortableHelperOperation::SendKeyboardInput);
    PortableHelperAuthorization authorization;
    PortableHelperGrant grant;
    require(begin_local_approval_test_grant(
                authorization, instance, connection, parent.session_id,
                operations, &grant, &error),
            "issue a generation-scoped test grant after simulated local approval");
    require(send_portable_helper_grant(*pipe, grant, 5000, &error),
            "send the fixed-size secret grant only over the authenticated pipe");
    require(receive_portable_helper_worker_ready(
                *pipe, authorization, grant, expected_worker, 5000, &error),
            "accept WorkerReady only after process identity and HMAC proof checks");
    {
        PortableHelperAuthorization::OperationLease input_lease;
        PortableHelperOperation input_operation{};
        InputEvent input_event;
        require(receive_portable_helper_desktop_request(
                    *pipe, authorization, &input_lease, &input_operation,
                    &input_event, 5000, &error) &&
                    input_operation ==
                        PortableHelperOperation::SendKeyboardInput &&
                    input_event.kind == InputKind::Key &&
                    input_event.key.hid_usage == 0x04,
                "authenticated IPC routes each typed input through the capability lease gate");
    }
    PortableHelperMessage desktop_ack;
    desktop_ack.type = PortableHelperMessageType::Acknowledgement;
    desktop_ack.request_id = 177;
    desktop_ack.payload = {1};
    require(pipe->Send(std::move(desktop_ack), 5000, &error),
            "acknowledge the typed desktop request only after validation");
    require(send_portable_helper_capture_request(
                *pipe, authorization, grant, 178, 5000, &error),
            "send a frame-capture request through the capability-scoped pipe API");
    PortableHelperMessage capture_ack;
    require(pipe->Receive(&capture_ack, 5000, &error) &&
                capture_ack.type == PortableHelperMessageType::Acknowledgement &&
                capture_ack.request_id == 178 &&
                capture_ack.payload == std::vector<uint8_t>{2},
            "isolated worker receives the typed capture request and replies");
    InputEvent forwarded_key;
    forwarded_key.kind = InputKind::Key;
    forwarded_key.key.action = KeyAction::Down;
    forwarded_key.key.hid_usage = 0x04;
    require(send_portable_helper_input_request(
                *pipe, authorization, grant, 179, forwarded_key, 5000, &error),
            "send validated keyboard input through the capability-scoped pipe API");
    PortableHelperMessage input_ack;
    require(pipe->Receive(&input_ack, 5000, &error) &&
                input_ack.type == PortableHelperMessageType::Acknowledgement &&
                input_ack.request_id == 179 &&
                input_ack.payload == std::vector<uint8_t>{3},
            "isolated worker receives the fixed-schema input request");
    require(authorization.Authorize(instance, connection, parent.session_id,
                                    grant.generation,
                                    PortableHelperOperation::CaptureFrames) &&
                !authorization.Authorize(instance, connection, parent.session_id,
                                         grant.generation,
                                         PortableHelperOperation::SendPointerInput),
            "the live pipe handshake enables only the exact granted operation set");
    bool input_release_called = false;
    PortableHelperWorkerRevocationCallbacks callbacks;
    require(make_win32_portable_helper_revocation_callbacks(
                child.hProcess, pipe,
                [&](std::string*) {
                    input_release_called = true;
                    return true;
                },
                &callbacks, &error),
            "bind revocation to the test-owned worker handle and authenticated pipe");
    const bool revoked = revoke_portable_helper_worker(
        authorization, callbacks, 1000, 5000, &error);
    if (!revoked) {
        if (WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0)
            TerminateProcess(child.hProcess, ERROR_CANCELLED);
        WaitForSingleObject(child.hProcess, 5000);
        CloseHandle(child.hProcess);
        require(false, "supervisor sends Stop and waits for the actual worker process");
    }
    require(input_release_called &&
                !authorization.Authorize(instance, connection, parent.session_id,
                                         grant.generation,
                                         PortableHelperOperation::CaptureFrames),
            "supervisor releases input only after revoking the live capability");
    require(WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0,
            "authenticated worker exits on the supervised Stop message");
    DWORD exit_code = 1;
    require(GetExitCodeProcess(child.hProcess, &exit_code) && exit_code == 0,
            "actual test worker completed the authenticated revoke protocol");
    CloseHandle(child.hProcess);
}

void elevation_handoff() {
    using namespace chast::host;
    auto denied = ElevationHandoff::start([](const std::wstring&, DWORD& error) -> HANDLE {
        error = ERROR_CANCELLED; return nullptr;
    });
    require(until([&] { return denied->finished(); }) && denied->poll().code == ElevationCode::Cancelled,
            "local UAC cancellation completes without stopping the host");
    std::string error;
    require(!ElevatedChild::receive(std::wstring(32, L'0'), GetCurrentProcessId(), 1, &error, false),
            "child rejects a reused or forged parent creation time before IPC");

    const auto launch = [](const std::wstring& args, DWORD& error) -> HANDLE {
        const auto path = executable_path();
        auto command = L"\"" + path + L"\" " + args;
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &process)) { error = GetLastError(); return nullptr; }
        CloseHandle(process.hThread); return process.hProcess;
    };
    for (const bool wrong_identity : {false, true}) {
        Fixture f; f.config.settings.max_viewers = 1;
        const auto secret = generate_totp_secret();
        if (!wrong_identity) require(protect_totp_secret(secret, SecretScope::CurrentUser,
            f.config.settings.totp_secret, &error), "protect original user's two-factor secret");
        f.start();
        ElevationSnapshot snapshot;
        snapshot.settings = f.config.settings;
        snapshot.config_path = narrow((f.root / L"原主机.ini").wstring());
        snapshot.fingerprint = f.host->status().fingerprint;
        auto task = ElevationHandoff::start(launch, process_is_elevated());
        require(until([&] { return task->poll().code != ElevationCode::Pending; }, 10s) &&
                task->poll().code == ElevationCode::Restarting, "authenticate the actual child and duplicate original token");
        require(f.host->running(), "old listener stays alive throughout preparation");
        task->commit();
        f.host->stop(); f.host.reset();
        if (wrong_identity) snapshot.fingerprint.assign(64, '0');
        require(task->supply(snapshot, &error), "send settings snapshot over authenticated IPC");
        require(until([&] { return task->finished(); }, 15s), "handoff reaches a terminal state");
        if (wrong_identity) {
            require(task->poll().code == ElevationCode::Failed, "identity change aborts the replacement process");
            f.start(); auto restored = f.connect();
            require(restored->peer_alive(), "original host can rebind and authenticate after failed takeover");
        } else {
            require(task->poll().code == ElevationCode::Elevated, "child reports same-identity listener ready");
            QuicClientConfig cfg; cfg.host = "127.0.0.1"; cfg.port = f.port;
            cfg.pinned_fingerprint = snapshot.fingerprint; cfg.password = "parity-test-password";
            std::vector<uint8_t> key;
            require(parse_totp_secret(secret, key), "decode test authenticator secret");
            char code[7]{};
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::snprintf(code, sizeof(code), "%06u", totp_code(key, static_cast<uint64_t>(seconds)));
            cfg.totp = code;
            auto viewer = QuicSession::create_client(cfg, &error);
            require(viewer && viewer->connect(4s).ok(), "SChannel uses original user's cached key after impersonation ends");
            ServerInfo info;
            require(viewer->handshake(info, 4s).ok(), "password plus original user's DPAPI/TOTP authenticate after handoff");
            ClientStats stats;
            require(viewer->finish_client(stats, 3s).ok(), "handed-off session closes cleanly");
            viewer.reset();
        }
    }
    auto cancelled = ElevationHandoff::start(launch, process_is_elevated());
    require(until([&] { return cancelled->poll().code == ElevationCode::Restarting; }, 10s), "child prepared for cancellation");
    cancelled->cancel();
    require(until([&] { return cancelled->finished(); }, 10s) && cancelled->poll().code == ElevationCode::Cancelled,
            "cancellation before commit closes prepared child without binding a port");
}

void clipboard_directions() {
    for (const auto direction : {ClipboardDirection::Both, ClipboardDirection::HostToViewer,
                                 ClipboardDirection::ViewerToHost, ClipboardDirection::Disabled}) {
        Fixture f;
        f.config.settings.clipboard_direction = direction;
        auto revision = std::make_shared<std::atomic<unsigned>>(0);
        auto reads = std::make_shared<std::atomic<unsigned>>(0);
        auto writes = std::make_shared<std::atomic<unsigned>>(0);
        auto file_writes = std::make_shared<std::atomic<unsigned>>(0);
        const auto source = f.root / "copy-source.txt";
        const std::string bytes = "clipboard bytes with browser transfer disabled";
        { std::ofstream out(source); out << bytes; }
        f.config.clipboard_read = [revision, reads](ClipboardContent& item) {
            ++*reads;
            const auto text = "local-" + std::to_string(revision->load());
            item = {}; item.formats.push_back({ClipboardFormat::Text, {text.begin(), text.end()}});
            return true;
        };
        f.config.clipboard_read_files = [revision, source](std::vector<std::string>& paths) {
            if (*revision < 2) return false;
            paths = {source.string()}; return true;
        };
        f.config.clipboard_write = [writes](const ClipboardContent&) { ++*writes; return true; };
        f.config.clipboard_write_files = [file_writes, bytes](const std::vector<std::string>& paths) {
            if (paths.size() != 1) return false;
            std::ifstream in(paths.front(), std::ios::binary);
            if (std::string((std::istreambuf_iterator<char>(in)), {}) != bytes) return false;
            ++*file_writes; return true;
        };
        f.start();
        ServerInfo info;
        auto viewer = f.connect(SessionMode::Desktop, kPermissionAll, {}, kDisplayAuto, &info);
        require(info.clipboard_direction == direction, "host announces its local clipboard direction");
        // A normal browser request still works even with clipboard disabled.
        require(viewer->request_file_list(41, "").ok(), "browser transfer has an independent grant");
        FileListResult listing;
        require(until([&] { return viewer->take_file_list_result(listing); }) && listing.code == FileCode::Ok,
                "clipboard direction does not restrict ordinary directory browsing");
        f.host->set_permissions(kPermissionAll & ~kPermissionFileTransfer);
        require(until([&] { return !(viewer->permissions() & kPermissionFileTransfer); }), "ordinary files revoked");
        require(!viewer->request_file_list(42, "").ok(), "browser stays disabled during clipboard transfer");
        if (clipboard_from_host(direction)) {
            ClipboardContent content;
            // The session opens with one sync of the host's current clipboard.
            require(until([&] { return viewer->take_clipboard_content(content); }) && content.text() == "local-0",
                    "opening clipboard sync carries the current host text");
            require(until([&] { return *reads >= 2; }), "outbound clipboard establishes a baseline");
            *revision = 1;
            require(until([&] { return viewer->take_clipboard_content(content); }) && content.text() == "local-1",
                    "permitted host text arrives after baseline");
            *revision = 2;
            require(until([&] { return viewer->take_clipboard_content(content) && content.find(ClipboardFormat::FileList); }),
                    "host announces copied files with browser permission disabled");
            require(viewer->request_file_download(43, ".chast-clipboard/0").ok(), "clipboard download uses its own direction");
            FileDownloadResult answer;
            require(until([&] { return viewer->take_file_download_result(answer); }) && answer.code == FileCode::Ok,
                    "clipboard download accepted independently of browser transfer");
            FileOffer offer;
            require(until([&] { return viewer->take_file_offer(offer); }) && offer.clipboard,
                    "outgoing clipboard offer retains its permission class");
            require(viewer->send_file_answer(offer.transfer_id, FileCode::Ok, 0).ok(), "accept copied file");
            std::string landed;
            require(until([&] {
                FileChunk chunk;
                if (viewer->receive_file_chunk(chunk, 20ms).ok()) landed.append(reinterpret_cast<char*>(chunk.data.data()), chunk.data.size());
                return landed.size() == bytes.size();
            }) && landed == bytes, "clipboard download bytes arrive intact");
            require(viewer->send_file_result(offer.transfer_id, FileCode::Ok, landed.size()).ok(), "complete copied file");
        } else {
            *revision = 2;
            require(!viewer->request_file_download(43, ".chast-clipboard/0").ok(), "forbidden clipboard download refused");
        }
        const auto text_sent = viewer->send_clipboard(91, "remote text");
        require(text_sent.ok() == clipboard_to_host(direction), "legacy clipboard text obeys direction");
        ClipboardContent content; content.seq = 92;
        content.formats.push_back({ClipboardFormat::Text, {'r','i','c','h'}});
        require(viewer->send_clipboard_content(content).ok() == clipboard_to_host(direction), "rich clipboard obeys direction");
        FileOffer upload; upload.transfer_id = 77; upload.name = "incoming.txt"; upload.clipboard = true;
        upload.size = bytes.size();
        require(sha256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), upload.sha256), "hash copied file");
        if (clipboard_to_host(direction)) {
            require(until([&] { return *writes > 0; }), "allowed incoming text writes the host clipboard");
            content = {}; content.seq = 93;
            content.formats.push_back({ClipboardFormat::FileList, encode_clipboard_file_list({{upload.name, upload.size, {}}})});
            require(viewer->send_clipboard_content(content).ok(), "announce viewer clipboard files");
            // Rich content uses another stream; wait for staging before the offer.
            std::this_thread::sleep_for(300ms);
            require(viewer->offer_file(upload).ok(), "clipboard upload independent of browser permission");
            FileAnswer answer;
            require(until([&] { return viewer->take_file_answer(answer); }) && answer.code == FileCode::Ok,
                    "host accepts announced clipboard upload");
            require(viewer->send_file_data(upload.transfer_id, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()).ok() &&
                    viewer->finish_file_upload(upload.transfer_id).ok(), "clipboard stream sends and finishes without file grant");
            FileResult result;
            require(until([&] { return viewer->take_file_result(result); }) && result.code == FileCode::Ok &&
                    until([&] { return *file_writes == 1; }), "copied file bytes reach the host clipboard sink");
        } else {
            require(!viewer->offer_file(upload).ok(), "forbidden clipboard file upload refused");
        }
        require(clipboard_from_host(direction) || *reads == 0, "receive-only host never reads its local clipboard");
        require(clipboard_to_host(direction) || *writes == 0, "send-only host never writes peer clipboard data");
        if (direction == ClipboardDirection::Both) {
            // Revoke an active clipboard stream while ordinary files remain
            // granted. The marked stream must not borrow their permission.
            f.host->set_permissions(kPermissionAll);
            require(until([&] { return (viewer->permissions() & kPermissionFileTransfer) != 0; }), "restore independent browser grant");
            upload.transfer_id = 78; upload.name = "revoked-copy.txt";
            content = {}; content.seq = 94;
            content.formats.push_back({ClipboardFormat::FileList, encode_clipboard_file_list({{upload.name, upload.size, {}}})});
            require(viewer->send_clipboard_content(content).ok(), "announce clipboard stream for revocation");
            std::this_thread::sleep_for(300ms);
            require(viewer->offer_file(upload).ok(), "offer clipboard stream for revocation");
            FileAnswer answer;
            require(until([&] { return viewer->take_file_answer(answer); }) && answer.code == FileCode::Ok,
                    "clipboard stream accepted before revocation");
            require(viewer->send_file_data(upload.transfer_id, reinterpret_cast<const uint8_t*>(bytes.data()), 4).ok(),
                    "clipboard transfer starts before revocation");
            f.host->set_permissions(kPermissionAll & ~kPermissionClipboard);
            require(until([&] { return !(viewer->permissions() & kPermissionClipboard); }) &&
                    !viewer->send_file_data(upload.transfer_id, reinterpret_cast<const uint8_t*>(bytes.data()), 4).ok() &&
                    !viewer->finish_file_upload(upload.transfer_id).ok(), "active clipboard stream stops despite ordinary file grant");
            require(viewer->request_file_list(44, "").ok() &&
                    until([&] { return viewer->take_file_list_result(listing); }) && listing.code == FileCode::Ok,
                    "clipboard revocation leaves browser transfer available");
        }
        f.host->set_permissions(kPermissionAll & ~(kPermissionClipboard | kPermissionFileTransfer));
        require(until([&] { return !(viewer->permissions() & kPermissionClipboard); }) &&
                !viewer->send_clipboard(94, "revoked").ok() && !viewer->offer_file(upload).ok(),
                "revoking clipboard closes text and copied-file paths");
        viewer.reset();
    }
}

void finish(std::unique_ptr<QuicSession>& viewer) {
    if (viewer) {
        ClientStats stats;
        require(viewer->finish_client(stats, 3s).ok(), "viewer ends cleanly");
        viewer.reset();
    }
}
void session_power_requests() {
    struct Counts { std::atomic<int> system{0}, display{0}, created{0}; };
    struct Request final : ISessionPowerRequest {
        Request(std::shared_ptr<Counts> counts, bool screen) : counts(std::move(counts)), screen(screen) {
            ++this->counts->system; ++this->counts->created;
            if (screen) ++this->counts->display;
        }
        ~Request() override { if (screen) --counts->display; --counts->system; }
        std::shared_ptr<Counts> counts;
        bool screen;
    };
    auto counts = std::make_shared<Counts>();
    const auto factory = [counts](bool screen, std::string*) {
        return std::make_unique<Request>(counts, screen);
    };
    {
        std::atomic<bool> waiting{false}, refuse{false};
        std::atomic<int> approvals{0};
        Fixture f;
        f.config.service_approval = true;
        f.config.settings.auth_mode = HostAuthMode::Click;
        f.config.power_request_factory = factory;
        f.config.approve_client = [&](const ClientRequest& request) {
            if (++approvals == 1) {
                waiting = true;
                until([&] { return refuse.load(); }, 5s);
                return ClientApproval{false, 0};
            }
            return ClientApproval{true, request.permissions};
        };
        f.start();
        require(counts->created == 0, "idle listener holds no power request");
        auto denied = std::async(std::launch::async, [&] {
            try { auto viewer = f.connect(SessionMode::FileTransfer); return false; }
            catch (const std::exception&) { return true; }
        });
        require(until([&] { return waiting.load(); }) && counts->created == 0,
                "connection awaiting approval cannot hold the host awake");
        refuse = true;
        require(denied.get() && counts->created == 0, "refused connection creates no power request");
        auto desktop = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput);
        auto files = f.connect(SessionMode::FileTransfer);
        auto terminal = f.connect(SessionMode::Terminal);
        require(counts->system == 3 && counts->display == 1,
                "view-only desktop keeps display awake; files and terminal keep only system awake");
        finish(desktop);
        require(until([&] { return counts->system == 2 && counts->display == 0; }),
                "desktop exit releases its display request while other sessions stay awake");
        finish(files);
        require(until([&] { return counts->system == 1; }), "one connection cannot release another's power request");
        f.host->stop();
        require(counts->system == 0 && counts->display == 0, "host shutdown releases the last request");
    }
    {
        Fixture f;
        f.config.service_mode = false;
        f.config.settings.max_viewers = 1;
        f.config.power_request_factory = factory;
        f.start();
        for (int i = 0; i < 2; ++i) {
            auto files = f.connect(SessionMode::FileTransfer);
            require(counts->system == 1 && counts->display == 0, "sequential user host acquires per connection");
            finish(files);
            require(until([&] { return counts->system == 0; }), "sequential host releases before next viewer");
        }
    }
    {
        Fixture f;
        f.config.settings.keep_awake = false;
        f.config.power_request_factory = factory;
        const int before = counts->created;
        f.start();
        auto files = f.connect(SessionMode::FileTransfer);
        require(counts->created == before, "disabled local policy creates no power request");
        finish(files);
    }
    {
        Fixture f;
        f.config.power_request_factory = factory;
        f.config.capture_factory = [](const DisplayInfo&, const CaptureConfig&, std::string* error) {
            *error = "power lifetime capture failure";
            return std::unique_ptr<IDxgiCapturer>{};
        };
        f.start();
        const int before = counts->created;
        bool failed = false;
        try { auto viewer = f.connect(); } catch (const std::exception&) { failed = true; }
        require(failed && until([&] { return counts->created > before && counts->system == 0; }),
                "capture failure after admission releases the request");
    }
    {
        std::atomic<int> attempts{0};
        Fixture f;
        f.config.power_request_factory = [&](bool, std::string* error) {
            ++attempts; *error = "power requests unavailable";
            return std::unique_ptr<ISessionPowerRequest>{};
        };
        f.start();
        auto files = f.connect(SessionMode::FileTransfer);
        require(attempts == 1 && files->send_chat("still connected").ok(),
                "power request failure does not abort an authenticated session");
        finish(files);
    }
}
bool keyframe(QuicSession& viewer) {
    return until([&] {
        FrameHeader header;
        std::vector<uint8_t> data;
        return viewer.receive_frame(header, data, 50ms).ok() && header.idr && !data.empty();
    });
}
bool codec_update(QuicSession& viewer, VideoCodec codec) {
    // A real viewer consumes old in-flight video while awaiting StreamInfo.
    // Otherwise v52 correctly pauses its delivery window after 500 ms.
    return until([&] {
        ServerInfo info;
        if (viewer.take_stream_update(info) && info.codec == codec) return true;
        FrameHeader frame;
        std::vector<uint8_t> bytes;
        viewer.receive_frame(frame, bytes, 20ms);
        return false;
    });
}
bool audio(QuicSession& viewer, std::chrono::milliseconds timeout = 1s) {
    AudioHeader header;
    std::vector<uint8_t> data;
    return viewer.receive_audio(header, data, timeout).ok() && !data.empty();
}
void drain_audio(QuicSession& viewer) {
    while (audio(viewer, 1ms)) {}
}
void set_policy(QuicSession& viewer, uint32_t keys, uint32_t values) {
    require(viewer.request_remote_config(keys, values).ok(), "send global policy change");
    RemoteConfigState state;
    require(until([&] { return viewer.take_remote_config_state(state); }) &&
            state.code == RemoteConfigCode::Ok && (state.flags & keys) == (values & keys),
            "global policy change acknowledged");
}

void static_desktop_and_followers() {
    Fixture f;
    std::atomic<int> login_calls{0};
    f.config.os_login = [&](const std::string&, std::string*) { ++login_calls; return true; };
    f.start();
    auto publisher = f.connect();
    require(keyframe(*publisher), "publisher receives initial IDR");
    require(audio(*publisher), "publisher receives system audio");
    f.host->set_permissions(kPermissionAll & ~(kPermissionAudio | kPermissionInput));
    require(until([&] { return (publisher->permissions() & (kPermissionAudio | kPermissionInput)) == 0; }),
            "publisher relinquishes audio and control");
    drain_audio(*publisher);
    auto follower = f.connect();
    require(keyframe(*follower), "late follower receives IDR on a completely static desktop");
    require(follower->permissions() & kPermissionInput, "follower acquires available control lease");
    require(audio(*follower), "follower hears audio while publisher is muted");
    require(!audio(*publisher, 120ms), "publisher stays muted");

    require(follower->request_display_follow(kDisplayFollowAll).ok(), "follower requests display follow");
    DisplayFollowState follow;
    require(until([&] { return follower->take_display_follow_state(follow); }) &&
            follow.code == DisplayFollowCode::Ok, "each subscriber can independently enable display follow");
    require(follower->request_display_follow(0).ok(), "follower disables display follow");
    require(until([&] { return follower->take_display_follow_state(follow); }) &&
            follow.code == DisplayFollowCode::Ok && follow.flags == 0, "disable-follow request completes");
    require(follower->request_os_login("test-only-password").ok(), "follower requests OS login");
    OsLoginCode login;
    std::string reason;
    require(until([&] { return follower->take_os_login_result(login, reason); }) &&
            login == OsLoginCode::Ok && login_calls == 1, "follower OS login reaches fake backend and replies");
    require(follower->request_session_list().ok(), "WTS permission survives stream description");
    SessionListResult list;
    require(until([&] { return follower->take_session_list_result(list); }) &&
            list.code == SessionListCode::Ok && list.sessions.size() == 2, "follower receives session list");
    for (const auto& [id, expected] : std::vector<std::pair<uint32_t, SessionSelectCode>>{
            {1, SessionSelectCode::Ok}, {999, SessionSelectCode::NotFound}, {2, SessionSelectCode::Failed}}) {
        require(follower->request_session_select(id).ok(), "follower requests session selection");
        SessionSelectResult selected;
        require(until([&] { return follower->take_session_select_result(selected); }) &&
                selected.code == expected, "follower receives validated session-selection result");
    }

    f.host->set_permissions(kPermissionAll & ~kPermissionAudio);
    require(until([&] { return !(follower->permissions() & kPermissionAudio); }), "mute follower independently");
    drain_audio(*follower);
    require(!audio(*follower, 120ms) && !audio(*publisher, 120ms), "both muted viewers receive no new audio");
    f.host->set_permissions(kPermissionAll);
    require(until([&] { return (follower->permissions() & kPermissionAudio) != 0; }), "restore follower audio grant");
    require(audio(*follower), "restored follower resumes audio");

    FrameHeader frame;
    std::vector<uint8_t> payload;
    while (publisher->receive_frame(frame, payload, 1ms).ok()) {}
    const unsigned before_request = f.media->encodes + f.media->repeats;
    require(publisher->request_keyframe().ok() &&
            until([&] { return f.media->encodes + f.media->repeats > before_request; }) &&
            keyframe(*publisher), "explicit IDR request works without desktop activity");
    // Empty queued frames so the next IDR proves the encoder fallback ran.
    while (publisher->receive_frame(frame, payload, 1ms).ok()) {}
    const unsigned encoded = f.media->encodes;
    f.media->repeat_supported = false;
    require(publisher->request_keyframe().ok() && keyframe(*publisher) && f.media->encodes > encoded,
            "repeat-unsupported encoder imports retained GPU surface");
    const unsigned calls = f.media->encodes + f.media->repeats;
    std::this_thread::sleep_for(160ms);
    // RustDesk static_refresh repeats an idle monitor in a bounded burst
    // (at most 30 fps) so rate control can refine the last picture.
    const unsigned refined = f.media->encodes + f.media->repeats - calls;
    require(refined <= 6, "idle desktop refinement is capped at 30 fps");

    f.media->hold_frames = true;
    f.media->reset_requested = true;
    require(until([&] { return f.media->resets > 0; }), "capture reset observed");
    while (publisher->receive_frame(frame, payload, 1ms).ok()) {}
    require(publisher->request_keyframe().ok(), "request IDR during capture reset");
    require(!publisher->receive_frame(frame, payload, 180ms).ok(), "old desktop is not replayed after reset");
    f.media->hold_frames = false;
    require(keyframe(*publisher), "new desktop frame resumes with IDR");
    finish(publisher);
    require(f.media->audio_sources == 1, "shared audio source survives the original viewer leaving");
    drain_audio(*follower);
    require(audio(*follower), "audio continues after publisher departure");
    finish(follower);
}

void global_policy(bool frozen) {
    Fixture f;
    f.config.settings.allow_permission_change = !frozen;
    f.start();
    auto publisher = f.connect();
    auto follower = f.connect();
    auto files = f.connect(SessionMode::FileTransfer);
    auto terminal = f.connect(SessionMode::Terminal);
    for (auto* viewer : {files.get(), terminal.get()}) {
        require(viewer->request_stream_options({}).ok(), "non-desktop session sends stream options");
        StreamOptionsResult options;
        require(until([&] { return viewer->take_stream_options_result(options); }) &&
                options.code == StreamOptionsCode::Unsupported,
                "non-desktop session receives an explicit stream-options result");
    }
    require(audio(*publisher) && audio(*follower), "both desktop sessions initially receive audio");
    require(follower->request_session_select(2).ok(), "view-only follower may request WTS selection");
    SessionSelectResult selected;
    require(until([&] { return follower->take_session_select_result(selected); }) &&
            selected.code == SessionSelectCode::Busy, "follower cannot bypass control lease");
    constexpr uint32_t keys = kRemoteConfigAudio | kRemoteConfigClipboard |
        kRemoteConfigFileTransfer | kRemoteConfigTerminal;
    constexpr PermissionMask revoked = kPermissionAudio | kPermissionClipboard |
        kPermissionFileTransfer | kPermissionTerminal;
    set_policy(*publisher, keys, 0);
    require(until([&] {
        return ((publisher->permissions() | follower->permissions() |
                 files->permissions() | terminal->permissions()) & revoked) == 0;
    }), frozen ? "global revocation reaches every session despite frozen UI permissions"
               : "global revocation reaches desktop, file and control sessions");
    drain_audio(*publisher);
    drain_audio(*follower);
    require(!audio(*publisher, 100ms) && !audio(*follower, 100ms), "global audio disable gates both recipients");
    require(files->request_file_list(1, "").status() == Status::PermissionDenied,
            "revoked file session cannot browse");
    bool refused = false;
    try { auto unavailable = f.connect(SessionMode::FileTransfer); }
    catch (const std::runtime_error& error) {
        refused = std::string(error.what()).find("permission denied") != std::string::npos;
    }
    require(refused, "new file-only connection is refused while globally disabled");
    auto denied = f.connect();
    require((denied->permissions() & revoked) == 0, "new desktop sessions see disabled security defaults");
    set_policy(*publisher, keys, keys);
    std::this_thread::sleep_for(150ms);
    require(((publisher->permissions() | follower->permissions() | files->permissions() |
              terminal->permissions() | denied->permissions()) & revoked) == 0,
            "reopening global policy never silently regrants existing sessions");
    auto newcomer = f.connect();
    require((newcomer->permissions() & revoked) == revoked && audio(*newcomer),
            "new authenticated session receives re-enabled capabilities and audio");
    require(newcomer->request_remote_config(0, 0).ok(), "follower can query global policy");
    RemoteConfigState state;
    require(until([&] { return newcomer->take_remote_config_state(state); }) &&
            (state.flags & keys) == keys, "follower sees current shared settings");
    finish(newcomer);
    finish(denied);
    finish(terminal);
    finish(files);
    finish(follower);
    finish(publisher);
}

struct LoopbackEcho {
    SOCKET listener = INVALID_SOCKET;
    std::atomic<bool> echoed{false};
    std::atomic<bool> closed{false};
    std::atomic<bool> stopping{false};
    std::thread worker;
    uint16_t port = 0;
    LoopbackEcho() {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(listener != INVALID_SOCKET, "create the loopback echo listener");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        require(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
                "bind the loopback echo listener");
        require(listen(listener, 1) == 0, "listen on the loopback echo port");
        int length = sizeof(addr);
        require(getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &length) == 0,
                "read the loopback echo port");
        port = ntohs(addr.sin_port);
        worker = std::thread([this] {
            SOCKET client = INVALID_SOCKET;
            char buffer[256];
            while (!stopping.load()) {
                fd_set ready;
                FD_ZERO(&ready);
                FD_SET(client == INVALID_SOCKET ? listener : client, &ready);
                timeval wait{0, 100000};
                const int signalled = select(0, &ready, nullptr, nullptr, &wait);
                if (signalled <= 0) {
                    continue;
                }
                if (client == INVALID_SOCKET) {
                    client = accept(listener, nullptr, nullptr);
                    continue;
                }
                const int got = recv(client, buffer, sizeof(buffer), 0);
                if (got <= 0) {
                    closed.store(true);
                    break;
                }
                if (send(client, buffer, got, 0) == got) {
                    echoed.store(true);
                }
            }
            if (client != INVALID_SOCKET) {
                closesocket(client);
            }
        });
    }
    ~LoopbackEcho() {
        stopping.store(true);
        if (worker.joinable()) worker.join();
        if (listener != INVALID_SOCKET) closesocket(listener);
    }
};

void running_services_revocation() {
    LoopbackEcho echo;
    Fixture f;
    f.config.settings.allow_port_forward = true;
    std::mutex log_mutex;
    std::vector<std::string> logs;
    f.config.log = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(log_mutex);
        logs.push_back(line);
    };
    f.start();
    auto viewer = f.connect();
    require((viewer->permissions() & (kPermissionPortForward | kPermissionTerminal)) ==
                (kPermissionPortForward | kPermissionTerminal),
            "desktop session receives forwarding and console grants");

    require(viewer->request_port_forward(1, "127.0.0.1", echo.port).ok(),
            "register a loopback forwarding target");
    PortForwardResult forward_result;
    require(until([&] { return viewer->take_port_forward_result(forward_result); }) &&
            forward_result.code == PortForwardCode::Ok, "the host approves the target");
    PortForwardHandle handle = 0;
    require(viewer->open_port_forward_connection(1, 7, handle).ok(),
            "open one forwarded connection");
    require(viewer->wait_port_forward_connection(handle, 4s).ok(),
            "the host dials the loopback service");
    const char ping[] = "ping";
    require(viewer->send_port_forward_data(handle,
                reinterpret_cast<const uint8_t*>(ping), sizeof(ping) - 1, 1s).ok(),
            "bytes enter the host relay");
    std::vector<uint8_t> chunk;
    require(until([&] {
        const Error received =
            viewer->receive_port_forward_data(handle, chunk, 100ms);
        return received.ok() &&
               std::string(chunk.begin(), chunk.end()) == "ping";
    }, 4s) && echo.echoed.load(), "the loopback service echoes through the host");

    require(viewer->request_terminal(1, 80, 24).ok(), "request a console");
    TerminalResult terminal;
    require(until([&] { return viewer->take_terminal_result(terminal); }) &&
            terminal.code == TerminalCode::Ok, "the host starts the console");
    require(viewer->open_terminal_input(1).ok(), "open the console uplink");
    const std::string command = "echo chast-revocation\r\n";
    require(viewer->send_terminal_input(1,
                reinterpret_cast<const uint8_t*>(command.data()), command.size(), 1s).ok(),
            "type into the console");
    bool printed = false;
    require(until([&] {
        std::vector<uint8_t> output;
        bool ended = false;
        while (viewer->take_terminal_output(1, output, ended)) {
            if (std::string(output.begin(), output.end())
                    .find("chast-revocation") != std::string::npos) {
                printed = true;
            }
        }
        return printed;
    }, 6s), "the console echoes the command");

    f.host->set_permissions(kPermissionAll &
                            ~(kPermissionPortForward | kPermissionTerminal));
    require(until([&] {
        return (viewer->permissions() & (kPermissionPortForward | kPermissionTerminal)) == 0;
    }), "revocation reaches the viewer as a capability update");
    require(until([&] {
        return !viewer->send_port_forward_data(handle,
                   reinterpret_cast<const uint8_t*>(ping), sizeof(ping) - 1, 100ms).ok();
    }), "the revoked forward refuses new bytes");
    require(until([&] {
        return !viewer->send_terminal_input(1,
                   reinterpret_cast<const uint8_t*>(command.data()), command.size(), 100ms).ok();
    }), "the revoked console refuses new keystrokes");
    require(until([&] { return echo.closed.load(); }, 4s),
            "the host relay closes the forwarded TCP socket");
    require(until([&] {
        std::lock_guard<std::mutex> lock(log_mutex);
        for (const std::string& line : logs) {
            if (line.find("terminal access was revoked") != std::string::npos) {
                return true;
            }
        }
        return false;
    }, 4s), "the host relay tears the console and its child down");
    finish(viewer);
}

void pending_approval() {
    Fixture f;
    f.config.service_mode = false;
    f.config.settings.auth_mode = HostAuthMode::Click;
    std::atomic<bool> waiting{false}, release{false};
    f.config.approve_client = [&](const ClientRequest& request) {
        if (request.hello.audit_ref == "bbbbbbbb") {
            waiting = true;
            until([&] { return release.load(); }, 6s);
        }
        return ClientApproval{true, request.permissions};
    };
    f.start();
    auto publisher = f.connect();
    auto pending = std::async(std::launch::async, [&] {
        return f.connect(SessionMode::Desktop, kPermissionAll, "bbbbbbbb");
    });
    require(until([&] { return waiting.load(); }), "second connection waits for approval");
    set_policy(*publisher, kRemoteConfigClipboard, 0);
    set_policy(*publisher, kRemoteConfigClipboard, kRemoteConfigClipboard);
    release = true;
    auto viewer = pending.get();
    require((viewer->permissions() & kPermissionClipboard) == 0,
            "off/on during approval cannot resurrect the pending permission grant");
    auto fresh = f.connect();
    require(fresh->permissions() & kPermissionClipboard, "fresh approval uses current policy defaults");
    finish(fresh);
    finish(viewer);
    finish(publisher);
}

void publisher_only_audio() {
    Fixture f;
    f.start();
    auto publisher = f.connect();
    auto follower = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionAudio);
    require(audio(*publisher), "publisher audio is independent of a muted follower");
    require(!audio(*follower, 120ms), "follower denied at admission receives no audio");
    finish(follower);
    finish(publisher);
}

void late_audio_source() {
    Fixture f;
    f.config.settings.allow_audio = false;
    f.start();
    auto publisher = f.connect();
    require(f.media->audio_sources == 0, "disabled policy opens no audio source");
    set_policy(*publisher, kRemoteConfigAudio, kRemoteConfigAudio);
    auto follower = f.connect();
    require(until([&] { return (follower->permissions() & kPermissionAudio) != 0; }) && audio(*follower),
            "enabling audio opens source and stream for a new authorized viewer");
    require(!audio(*publisher, 100ms), "enabling source does not grant audio to the old viewer");
    finish(follower);
    finish(publisher);
}

void desktop_switch_during_admission() {
    Fixture f;
    std::atomic<unsigned> follows{0};
    f.config.follow_desktop = [&](std::string*, bool* switched) {
        // Both the connection and the GPU service attach their own thread.
        // Simulate a switch on the connection's first explicit desktop poll.
        if (switched) *switched = ++follows == 1;
        return true;
    };
    f.start();
    auto viewer = f.connect();
    require(keyframe(*viewer) && f.media->resets > 0,
            "desktop switch during admission waits for rebuilt capture and encoder");
    finish(viewer);
}

// An IDR is the largest picture the encoder produces, so a viewer that keeps
// asking for one must not be answered on every request: requests inside the
// one-second window merge into a single deferred keyframe.
void viewer_keyframe_requests_are_bounded() {
    Fixture f;
    f.start();
    auto viewer = f.connect();
    require(keyframe(*viewer), "viewer receives its first keyframe");
    unsigned requests = 0, idrs = 0;
    auto next_request = std::chrono::steady_clock::now();
    const auto end = std::chrono::steady_clock::now() + 2300ms;
    while (std::chrono::steady_clock::now() < end) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_request) {
            require(viewer->request_keyframe().ok(), "viewer repeats its keyframe request");
            ++requests;
            next_request = now + 100ms;
        }
        FrameHeader header;
        std::vector<uint8_t> data;
        if (viewer->receive_frame(header, data, 10ms).ok() && header.idr) ++idrs;
    }
    require(requests >= 20, "the viewer really asked frame after frame");
    require(idrs >= 1 && idrs <= 3,
            "a request flood is answered at most once per keyframe window");
    finish(viewer);
}

struct TwoDisplays {
    Fixture fixture;
    std::shared_ptr<host_test::MediaState> left = fixture.media;
    std::shared_ptr<host_test::MediaState> right = std::make_shared<host_test::MediaState>();
    std::atomic<bool> refuse_right{false};
    std::atomic<bool> reversed{false};
    std::atomic<bool> remove_left{false}, remove_right{false};
    std::atomic<int> right_origin{640};
    std::atomic<unsigned> right_height{480}, right_rotation{0}, right_adapter{0};
    explicit TwoDisplays(VideoCodec codec = VideoCodec::H264, bool adaptive = false) {
        fixture.config.settings.codec = codec;
        fixture.config.adaptive_bitrate = adaptive;
        fixture.config.displays = [this] {
            auto a = host_test::display();
            auto b = a;
            b.device_name = L"test-display-2";
            b.primary = false;
            b.width = 800;
            b.height = right_height;
            b.rotation_degrees = right_rotation;
            b.adapter_luid.LowPart = right_adapter;
            const int origin = right_origin;
            b.rect = {origin, 0, origin + 800, static_cast<LONG>(b.height)};
            left->set_display(a); right->set_display(b);
            if (remove_left && remove_right) return std::vector<DisplayInfo>{};
            if (remove_left) return std::vector<DisplayInfo>{b};
            if (remove_right) return std::vector<DisplayInfo>{a};
            return reversed ? std::vector<DisplayInfo>{b, a} : std::vector<DisplayInfo>{a, b};
        };
        fixture.config.capture_factory = [this](const DisplayInfo& display,
                const CaptureConfig&, std::string* error) -> std::unique_ptr<IDxgiCapturer> {
            if (display.width == 800 && refuse_right) {
                *error = "simulated disconnected output";
                return nullptr;
            }
            return std::make_unique<host_test::Capture>(display.width == 800 ? right : left, display);
        };
        fixture.config.cursor_sampler = [this](const DisplayInfo& display, CursorInfo& cursor) {
            const auto state = display.width == 800 ? right : left;
            std::lock_guard lock(state->cursor_mutex); cursor = state->pointer; return true;
        };
        fixture.config.encoder_factory = [this](ID3D11Device*, const EncoderConfig& cfg, std::string*) {
            return std::make_unique<host_test::Encoder>(cfg.width == 800 ? right : left, cfg);
        };
        fixture.start();
    }
    ~TwoDisplays() { fixture.host->stop(); }
};

bool picture(QuicSession& viewer, uint16_t width) {
    return until([&] {
        FrameHeader header;
        std::vector<uint8_t> data;
        return viewer.receive_frame(header, data, 50ms).ok() && header.idr &&
            header.width == width && data.size() == 6 && data.back() == width / 16;
    });
}
void switch_to(QuicSession& viewer, uint16_t target, DisplaySwitchCode expected) {
    require(viewer.request_display_switch(target).ok(), "send independent display selection");
    uint16_t returned = kDisplayAuto;
    DisplaySwitchCode result{};
    std::string reason;
    require(until([&] { return viewer.take_display_switch_result(returned, result, reason); }) &&
            returned == target && result == expected, "display selection returns expected result");
}

CaptureDisplaysResult subscriptions(QuicSession& viewer, CaptureDisplaysRequest request,
                                    DisplaySwitchCode expected = DisplaySwitchCode::Ok) {
    static uint32_t sequence = 0;
    request.request_id = ++sequence;
    require(viewer.request_capture_displays(request).ok(), "send production display subscription request");
    CaptureDisplaysResult result;
    require(until([&] { return viewer.take_capture_displays_result(result) && result.request_id == request.request_id; }) &&
            result.code == expected, "production host answers subscription operation");
    return result;
}
void subscription_pictures(QuicSession& viewer, const CaptureDisplaysResult& result,
                           std::optional<VideoCodec> codec = {}) {
    std::map<uint32_t, uint16_t> waiting;
    for (const auto& stream : result.streams) {
        waiting.emplace(stream.stream_id, stream.info.width);
        require(viewer.request_display_keyframe(stream.stream_id).ok(), "request subscribed display keyframe");
    }
    require(until([&] {
        FrameHeader header;
        std::vector<uint8_t> payload;
        if (viewer.receive_frame(header, payload, 20ms).ok()) {
            auto found = waiting.find(header.stream_id);
            if (found != waiting.end() && header.idr && (!codec || header.codec == *codec) &&
                header.width == found->second && payload.size() == 6 && payload.back() == found->second / 16)
                waiting.erase(found);
        }
        return waiting.empty();
    }), "every subscribed stream sends its own static picture and keyframe");
}
void multiple_displays_one_connection() {
    TwoDisplays f;
    auto viewer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    require(picture(*viewer, 640), "initial desktop stream works before multi-display activation");
    auto both = subscriptions(*viewer, {0, {1, 1, 99}, {}, {}});
    require(both.streams.size() == 2 && both.streams[0].stream_id != both.streams[1].stream_id,
            "one view-only connection subscribes two distinct displays");
    subscription_pictures(*viewer, both);
    require(f.left->capture_created == 1 && f.right->capture_created == 1 &&
            f.left->encoder_created == 1 && f.right->encoder_created == 1,
            "multi-display connection creates exactly one pipeline per display");
    StreamOptions quality;
    quality.codec = StreamCodecPreference::H264;
    quality.max_fps = 10;
    require(viewer->request_stream_options(quality).ok(), "set common FPS for two static display streams");
    StreamOptionsResult quality_result;
    require(until([&] { return viewer->take_stream_options_result(quality_result); }) && quality_result.code == StreamOptionsCode::Ok,
            "multi-display quality preference is accepted");
    std::set<uint32_t> rate_updates;
    require(until([&] {
        DisplayStreamInfo info;
        while (viewer->take_display_stream_update(info))
            if (info.info.framerate == 10) rate_updates.insert(info.stream_id);
        return rate_updates.size() == 2;
    }), "both static displays announce their shared FPS without waiting for desktop motion");
    const auto left_id = both.streams[0].stream_id, right_id = both.streams[1].stream_id;
    auto other = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    require(picture(*other, 640) && f.left->capture_created == 1,
            "another connection shares a multi-stream viewer's display service");
    auto retained = subscriptions(*viewer, {0, {}, {1}, {}});
    require(retained.streams.size() == 1 && retained.streams[0].stream_id == right_id,
            "sub keeps the listed stream without changing its ID");
    subscription_pictures(*viewer, retained);
    require(other->request_keyframe().ok() && picture(*other, 640) && f.left->capture_destroyed == 0,
            "removing one subscription leaves another connection's picture intact");
    both = subscriptions(*viewer, {0, {0}, {}, {}});
    require(both.streams.size() == 2 && both.streams[0].stream_id != left_id &&
            both.streams[1].stream_id != left_id, "re-added display gets a fresh connection stream ID");
    subscription_pictures(*viewer, both);
    f.right->reset_requested = true;
    subscription_pictures(*viewer, both);
    require(f.right->resets == 1 && f.left->capture_created == 1 && f.right->capture_created == 1,
            "one display reset preserves both subscriptions and capture ownership");

    const std::string contents = "file upload survives a connection with zero desktop subscriptions";
    FileOffer offer;
    offer.transfer_id = 93; offer.name = "empty-video.txt"; offer.size = contents.size();
    require(sha256(reinterpret_cast<const uint8_t*>(contents.data()), contents.size(), offer.sha256), "hash empty-video upload");
    require(viewer->offer_file(offer).ok(), "start upload before unsubscribing all video");
    FileAnswer answer;
    require(until([&] { return viewer->take_file_answer(answer); }) && answer.code == FileCode::Ok, "accept empty-video upload");
    const auto split = contents.size() / 2;
    require(viewer->send_file_data(offer.transfer_id, reinterpret_cast<const uint8_t*>(contents.data()), split).ok(),
            "send upload prefix with video active");
    auto empty = subscriptions(*viewer, {});
    require(empty.streams.empty(), "empty set stops every video subscription");
    require(until([&] { return f.right->capture_destroyed == 1; }) && f.left->capture_destroyed == 0,
            "zero subscriptions release unused pipelines while another connection retains its source");
    FrameHeader header; std::vector<uint8_t> payload;
    while (viewer->receive_frame(header, payload, 10ms).ok()) {}
    require(!viewer->receive_frame(header, payload, 100ms).ok() && viewer->peer_alive(),
            "zero subscriptions send no further video and keep the connection open");
    require(audio(*viewer), "system audio continues with no video subscriptions");
    require(viewer->send_file_data(offer.transfer_id, reinterpret_cast<const uint8_t*>(contents.data() + split),
            contents.size() - split).ok() && viewer->finish_file_upload(offer.transfer_id).ok(),
            "complete upload while video is unsubscribed");
    FileResult file;
    require(until([&] { return viewer->take_file_result(file); }) && file.code == FileCode::Ok,
            "upload session and digest survive empty video selection");
    std::ifstream saved(f.fixture.root / offer.name, std::ios::binary);
    const std::string landed((std::istreambuf_iterator<char>(saved)), {});
    require(landed == contents, "uploaded bytes remain correct across empty video selection");
    saved.close();
    StreamOptions options;
    options.codec = StreamCodecPreference::Hevc;
    require(viewer->request_stream_options(options).ok(), "change encoding vote while video is unsubscribed");
    StreamOptionsResult option_result;
    require(until([&] { return viewer->take_stream_options_result(option_result); }) && option_result.code == StreamOptionsCode::Ok,
            "empty video selection still accepts stream preferences");
    both = subscriptions(*viewer, {0, {}, {}, {0, 1}});
    require(both.streams.size() == 2, "set reopens both sources within the same connection");
    subscription_pictures(*viewer, both, VideoCodec::Hevc);
    finish(viewer);
    require(other->request_keyframe().ok() && picture(*other, 640), "multi-stream viewer exit leaves another viewer usable");
    finish(other);
    require(until([&] { return f.left->capture_created == f.left->capture_destroyed &&
                             f.right->capture_created == f.right->capture_destroyed; }) &&
            f.left->wrong_thread == 0 && f.right->wrong_thread == 0,
            "all multi-stream GPU resources release on their owning workers");
}
void multiple_display_topology() {
    TwoDisplays f;
    auto viewer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    auto both = subscriptions(*viewer, {0, {}, {}, {0, 1}});
    subscription_pictures(*viewer, both);
    std::map<uint16_t, uint32_t> ids;
    for (const auto& stream : both.streams) ids[stream.display_index] = stream.stream_id;
    auto updated = [&](auto predicate) {
        CaptureDisplaysResult result;
        require(until([&] {
            while (viewer->take_capture_displays_result(result)) if (predicate(result)) return true;
            FrameHeader h; std::vector<uint8_t> data; viewer->receive_frame(h, data, 10ms);
            return false;
        }, 5s), "host publishes topology subscription update without a selection request");
        return result;
    };
    f.reversed = true;
    auto reversed = updated([&](const auto& result) {
        if (result.streams.size() != 2) return false;
        for (const auto& stream : result.streams)
            if (stream.stream_id == ids[0] && stream.display_index == 1) return true;
        return false;
    });
    subscription_pictures(*viewer, reversed);
    require(f.left->resets == 0 && f.right->resets == 0,
            "index reorder preserves existing capture and encoder resources");
    auto observer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    auto observed = subscriptions(*observer, {0, {}, {}, {0}});
    subscription_pictures(*observer, observed);
    f.right_origin = -800;
    auto moved = updated([&](const auto& result) {
        for (const auto& stream : result.streams)
            if (stream.stream_id == ids[1] && stream.info.origin_x == -800) return true;
        return false;
    });
    subscription_pictures(*viewer, moved);
    require(until([&] {
        DisplayStreamInfo info;
        while (observer->take_display_stream_update(info)) if (info.info.origin_x == -800) return true;
        CaptureDisplaysResult result;
        while (observer->take_capture_displays_result(result))
            for (const auto& stream : result.streams) if (stream.info.origin_x == -800) return true;
        FrameHeader h; std::vector<uint8_t> data; observer->receive_frame(h, data, 10ms);
        return false;
    }), "every observer receives the new origin for its selected display");
    require(f.right->resets == 1 && f.left->resets == 0,
            "layout geometry rebuild is shared once and leaves the other source intact");
    f.right_height = 600;
    f.right_rotation = 90;
    auto resized = updated([&](const auto& result) {
        for (const auto& stream : result.streams)
            if (stream.stream_id == ids[1] && stream.info.displays[0].rotation_degrees == 90) return true;
        return false;
    });
    subscription_pictures(*viewer, resized);
    const auto matches_size = [&](const DisplayStreamInfo& info) {
        return info.stream_id == ids[1] && info.info.height == 600 && info.info.source_height == 600;
    };
    // A complete topology result supersedes any earlier incremental metadata.
    bool resized_info = std::any_of(resized.streams.begin(), resized.streams.end(), matches_size);
    bool resized_frame = false;
    require(viewer->request_display_keyframe(ids[1]).ok(), "request resized display keyframe");
    require(until([&] {
        DisplayStreamInfo info;
        while (viewer->take_display_stream_update(info))
            resized_info = matches_size(info) || resized_info;
        CaptureDisplaysResult result;
        while (viewer->take_capture_displays_result(result))
            resized_info = std::any_of(result.streams.begin(), result.streams.end(), matches_size) || resized_info;
        FrameHeader h; std::vector<uint8_t> data;
        if (viewer->receive_frame(h, data, 10ms).ok() && h.stream_id == ids[1] && h.idr && h.height == 600)
            resized_frame = true;
        return resized_info && resized_frame;
    }), "resize and rotation announce new dimensions on the original stream");
    require(f.right->resets == 2 && f.left->resets == 0,
            "resize and rotation rebuild only the affected shared source");
    f.remove_left = true;
    auto retained = updated([&](const auto& result) {
        return result.streams.size() == 1 && result.streams[0].stream_id == ids[1] &&
            result.streams[0].display_index == 0;
    });
    subscription_pictures(*viewer, retained);
    require(until([&] { return f.left->capture_destroyed == 1; }) && viewer->peer_alive(),
            "unplugged source is retired instead of aliasing the surviving index zero");
    f.remove_right = true;
    updated([](const auto& result) { return result.streams.empty(); });
    require(viewer->peer_alive() && audio(*viewer), "all outputs disappearing keeps session services alive");
    f.remove_right = false;
    auto restored = updated([&](const auto& result) {
        return result.streams.size() == 1 && result.streams[0].display_index == 0 &&
            result.streams[0].stream_id != ids[1];
    });
    subscription_pictures(*viewer, restored);
    require(viewer->peer_alive(), "a surviving primary display resumes after total unplug");
    subscriptions(*viewer, {});
    f.remove_left = false;
    updated([](const auto& result) { return result.streams.empty(); });
    ServerInfo list;
    require(until([&] { return viewer->take_stream_update(list) && list.displays.size() == 2; }),
            "empty subscription still receives newly available displays");
    FrameHeader h; std::vector<uint8_t> bytes;
    while (viewer->receive_frame(h, bytes, 10ms).ok()) {}
    require(!viewer->receive_frame(h, bytes, 100ms).ok(), "explicit empty set does not auto-subscribe after hotplug");
    auto again = subscriptions(*viewer, {0, {}, {}, {0, 1}});
    subscription_pictures(*viewer, again);
    finish(observer);
    std::map<uint16_t, uint32_t> new_ids;
    for (const auto& stream : again.streams) new_ids[stream.display_index] = stream.stream_id;
    f.right_adapter = 1;
    auto replaced = updated([&](const auto& result) {
        return result.streams.size() == 1 && result.streams[0].stream_id == new_ids[1];
    });
    subscription_pictures(*viewer, replaced);
    auto readded = subscriptions(*viewer, {0, {0}, {}, {}});
    require(readded.streams.size() == 2 &&
            std::none_of(readded.streams.begin(), readded.streams.end(), [&](const auto& stream) {
                return stream.stream_id == new_ids[0];
            }), "same display name on a replacement adapter gets a fresh stream identity");
    subscription_pictures(*viewer, readded);
    finish(viewer);
}

void multiple_display_recovery_isolation() {
    TwoDisplays f(VideoCodec::Hevc);
    auto viewer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    auto both = subscriptions(*viewer, {0, {}, {}, {0, 1}});
    subscription_pictures(*viewer, both);
    f.right->reinit_fails = true;
    f.right->reset_requested = true;
    require(until([&] { return f.right->resets > 0; }), "one source attempts its failed reset");
    auto left = both;
    std::erase_if(left.streams, [](const auto& stream) { return stream.display_index != 0; });
    subscription_pictures(*viewer, left);
    require(viewer->peer_alive() && audio(*viewer), "failed reset does not close the other screen or audio");
    const auto selected = subscriptions(*viewer, {0, {}, {}, {0, 1}});
    require(selected.streams.size() == both.streams.size() &&
            selected.streams[0].stream_id == both.streams[0].stream_id &&
            selected.streams[1].stream_id == both.streams[1].stream_id,
            "selection during recovery preserves both stream identities");
    auto observer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0, nullptr,
        kVideoCodecH264, StreamCodecPreference::H264);
    require(picture(*observer, 640), "another viewer can join a healthy screen while a source is unavailable");
    subscription_pictures(*viewer, left, VideoCodec::H264);
    const auto encoders_before_recovery = f.right->encoder_created.load();
    f.right->fail_encode_codecs = kVideoCodecAll;
    f.right->reinit_fails = false;
    require(until([&] {
        FrameHeader header; std::vector<uint8_t> bytes;
        viewer->receive_frame(header, bytes, 10ms);
        return f.right->encoder_created > encoders_before_recovery;
    }),
            "recovered capture attempts to rebuild its unavailable hardware encoder");
    auto during_encoder_failure = subscriptions(*viewer, {0, {}, {}, {0, 1}});
    require(during_encoder_failure.streams.size() == 2 && viewer->peer_alive(),
            "encoder recovery preserves subscriptions and accepts controls");
    subscription_pictures(*viewer, left, VideoCodec::H264);
    auto later = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0, nullptr,
        kVideoCodecH264, StreamCodecPreference::H264);
    require(picture(*later, 640), "unavailable encoder does not block another viewer's healthy display");
    f.right->fail_encode_codecs = 0;
    subscription_pictures(*viewer, both, VideoCodec::H264);
    require(f.left->resets == 0 && f.left->capture_created == 1 && f.right->capture_created == 1,
            "failed source recovers with the current codec without rebuilding neighbours");
    finish(observer);
    finish(later);
    subscription_pictures(*viewer, both, VideoCodec::Hevc);
    finish(viewer);
}

void multiple_display_failures_and_votes() {
    TwoDisplays f;
    auto a = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0, nullptr,
        kVideoCodecAll, StreamCodecPreference::Hevc);
    require(picture(*a, 640), "start multi-display rollback scenario");
    f.refuse_right = true;
    auto refused = subscriptions(*a, {0, {}, {}, {1}}, DisplaySwitchCode::Failed);
    require(refused.streams.size() == 1 && refused.streams[0].display_index == 0,
            "failed replacement retains the original subscription");
    subscription_pictures(*a, refused, VideoCodec::Hevc);
    f.refuse_right = false;
    f.right->fail_encode_codecs = kVideoCodecAll;
    refused = subscriptions(*a, {0, {1}, {}, {}}, DisplaySwitchCode::Failed);
    subscription_pictures(*a, refused, VideoCodec::Hevc);
    require(f.left->capture_created == 1 && f.left->capture_destroyed == 0,
            "failed target encoder does not replace the original source");
    f.right->fail_encode_codecs = 0;
    auto both = subscriptions(*a, {0, {}, {}, {0, 1}});
    subscription_pictures(*a, both, VideoCodec::Hevc);
    const auto invalid = subscriptions(*a, {0, {}, {99}, {}});
    require(invalid.streams.size() == 2, "invalid sub request preserves both active streams");
    auto b = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0, nullptr,
        kVideoCodecAll, StreamCodecPreference::H264);
    auto c = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1, nullptr,
        kVideoCodecAll, StreamCodecPreference::H264);
    subscription_pictures(*a, both, VideoCodec::H264);
    require(picture(*b, 640) && picture(*c, 800), "two H264 voters win over one HEVC viewer with two subscriptions");
    finish(b); finish(c);
    subscription_pictures(*a, both, VideoCodec::Hevc);
    finish(a);
}

void multiple_displays_backpressure() {
    TwoDisplays f;
    f.left->moving = f.right->moving = true;
    auto viewer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    auto both = subscriptions(*viewer, {0, {1}, {}, {}});
    subscription_pictures(*viewer, both);
    auto other = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1);
    const auto until_stalled = std::chrono::steady_clock::now() + 800ms;
    unsigned other_frames = 0;
    while (std::chrono::steady_clock::now() < until_stalled) {
        FrameHeader frame; std::vector<uint8_t> bytes;
        if (other->receive_frame(frame, bytes, 20ms).ok()) ++other_frames;
    }
    require(other_frames > 4, "another viewer continues while the two-stream viewer stops reading");
    const auto changed_at = std::chrono::steady_clock::now();
    const auto retained = subscriptions(*viewer, {0, {}, {1}, {}});
    require(retained.streams.size() == 1 && std::chrono::steady_clock::now() - changed_at < 1s,
            "stalled multi-stream viewer can still change subscriptions promptly");
    subscription_pictures(*viewer, retained);
    both = subscriptions(*viewer, {0, {0}, {}, {}});
    subscription_pictures(*viewer, both);
    std::map<uint32_t, unsigned> counts;
    require(until([&] {
        FrameHeader frame; std::vector<uint8_t> bytes;
        if (viewer->receive_frame(frame, bytes, 20ms).ok()) ++counts[frame.stream_id];
        return counts[both.streams[0].stream_id] >= 5 && counts[both.streams[1].stream_id] >= 5;
    }), "both moving displays make progress after the delivery window recovers");
    require(f.left->capture_created == 2 && f.right->capture_created == 1,
            "resubscribe rebuilds only a released source and preserves the other viewer's source");
    finish(viewer); finish(other);
}

void multiple_display_cursors() {
    TwoDisplays f;
    CursorInfo left, right;
    left.visible = left.position_valid = left.shape_valid = true;
    left.position = {80, 60};
    left.width = left.height = 4; left.content_id = 123;
    left.hotspot = {2, 3};
    left.bitmap.assign(64, 0x80);
    right = left; right.visible = false; right.position.x = -560;
    f.left->set_cursor(left); f.right->set_cursor(right);
    auto viewer = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    auto other = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1);
    CursorUpdate legacy;
    require(until([&] { return viewer->take_cursor_update(legacy); }) && legacy.visible &&
            legacy.x == 80 && legacy.y == 60 && legacy.hotspot_x == 2 && legacy.hotspot_y == 3 &&
            legacy.sprite == left.bitmap, "legacy viewer receives selected screen hotspot and sprite");
    auto both = subscriptions(*viewer, {0, {1}, {}, {}});
    auto stream_id = [](const CaptureDisplaysResult& result, uint16_t index) {
        for (const auto& entry : result.streams) if (entry.display_index == index) return entry.stream_id;
        throw std::runtime_error("missing cursor source");
    };
    const auto left_id = stream_id(both, 0), right_id = stream_id(both, 1);
    std::map<uint32_t, CursorUpdate> cursors;
    auto drain = [&] {
        DisplayCursorUpdate update;
        while (viewer->take_display_cursor_update(update))
            coalesce_cursor_update(cursors[update.stream_id], std::move(update.cursor));
    };
    require(until([&] { drain(); return cursors.size() == 2; }) &&
            cursors[left_id].visible && !cursors[right_id].visible && cursors[left_id].x == 80 &&
            cursors[right_id].x == -560 && cursors[left_id].hotspot_x == 2 &&
            cursors[left_id].hotspot_y == 3 && cursors[left_id].sprite == left.bitmap &&
            cursors[right_id].sprite == right.bitmap,
            "multi-stream activation snapshots each source including an already-delivered static shape");
    cursors.clear();
    require(viewer->request_cursor_shape(left_id, left.content_id).ok(),
            "viewer can ask for a cursor sprite again");
    require(until([&] {
        drain();
        return cursors.count(left_id) != 0 && cursors[left_id].sprite == left.bitmap;
    }), "host resends a cached sprite without waiting for pointer motion");
    cursors.clear();
    require(viewer->request_cursor_shape(left_id, left.content_id + 1).ok(),
            "viewer can ask for a shape the host has already left");
    std::this_thread::sleep_for(150ms);
    drain();
    require(cursors.count(left_id) == 0, "a request for an unknown cursor shape is ignored");
    require(until([&] { return other->take_cursor_update(legacy); }) && !legacy.visible &&
            legacy.x == -560, "another viewer retains its independently selected source cursor");
    require(other->request_cursor_shape(kCursorCurrentStream, right.content_id).ok() &&
            until([&] { return other->take_cursor_update(legacy); }) &&
            legacy.sprite == right.bitmap,
            "legacy cursor re-request resends the selected screen's sprite");

    left.visible = false; left.position.x = 672;
    right.visible = true; right.position.x = 32;
    f.left->set_cursor(left); f.right->set_cursor(right);
    require(until([&] { drain(); return !cursors[left_id].visible && cursors[right_id].visible &&
            cursors[left_id].x == 672 && cursors[right_id].x == 32; }),
            "cross-screen motion hides the old screen and uses the new screen's local coordinates");
    require(until([&] { return other->take_cursor_update(legacy) && legacy.visible && legacy.x == 32; }),
            "shared source sends the same new cursor to the other view-only connection");

    f.left->moving = f.right->moving = true;
    std::this_thread::sleep_for(700ms); // Neither viewer consumes video: the v52 window expires.
    left.position.x = 700; right.position.x = 60;
    f.left->set_cursor(left); f.right->set_cursor(right);
    require(until([&] { drain(); return cursors[left_id].x == 700 && cursors[right_id].x == 60; }),
            "current cursor bypasses both streams' stalled video backlog");
    require(until([&] { return other->take_cursor_update(legacy) && legacy.x == 60; }),
            "legacy cursor also keeps moving while its video window is blocked");
    auto kept = subscriptions(*viewer, {0, {}, {1}, {}});
    require(kept.streams.size() == 1 && kept.streams[0].stream_id == right_id,
            "retaining a screen preserves its cursor stream identity");
    cursors.clear();
    left.position.x = 720; right.position.x = 80;
    f.left->set_cursor(left); f.right->set_cursor(right);
    require(until([&] { drain(); return cursors.count(right_id) && cursors[right_id].x == 80; }) &&
            cursors.size() == 1, "unsubscribed screen stops publishing cursor state");
    both = subscriptions(*viewer, {0, {0}, {}, {}});
    const auto new_left_id = stream_id(both, 0);
    require(new_left_id != left_id && until([&] { drain(); return cursors.count(new_left_id) != 0; }) &&
            cursors[new_left_id].sprite == left.bitmap, "re-added screen has a new identity and complete cursor snapshot");
    subscriptions(*viewer, {});
    DisplayCursorUpdate update;
    require(!viewer->take_display_cursor_update(update), "empty set clears queued per-screen cursors");
    right.position.x = 100; f.right->set_cursor(right);
    require(until([&] { return other->take_cursor_update(legacy) && legacy.x == 100; }),
            "one viewer's empty set leaves another viewer's cursor subscription intact");
    finish(viewer); finish(other);
}

void independent_displays() {
    TwoDisplays f;
    ServerInfo a_info, b_info;
    auto a = f.fixture.connect(SessionMode::Desktop, kPermissionAll, {}, 0, &a_info);
    auto b = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1, &b_info);
    require(a_info.width == 640 && a_info.origin_x == 0 &&
            b_info.width == 800 && b_info.origin_x == 640, "Hello selects each connection's own display");
    require(picture(*a, 640) && picture(*b, 800), "two displays produce distinct encoded payloads");
    require(f.left->capture_created == 1 && f.right->capture_created == 1 &&
            f.left->encoder_created == 1 && f.right->encoder_created == 1,
            "one capture and encoder per subscribed display");
    auto c = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    require(picture(*c, 640) && f.left->capture_created == 1 && f.left->encoder_created == 1,
            "late same-display viewer shares existing pipeline and gets static IDR");
    const std::string contents = "file transfer remains open across independent display selection";
    FileOffer offer;
    offer.transfer_id = 91;
    offer.name = "display-selection.txt";
    offer.size = contents.size();
    require(sha256(reinterpret_cast<const uint8_t*>(contents.data()), contents.size(), offer.sha256),
            "hash transfer spanning display switch");
    require(a->offer_file(offer).ok(), "begin file transfer before display switch");
    FileAnswer answer;
    require(until([&] { return a->take_file_answer(answer); }) && answer.code == FileCode::Ok,
            "host accepts transfer before subscription changes");
    const size_t split = contents.size() / 2;
    require(a->send_file_data(offer.transfer_id,
            reinterpret_cast<const uint8_t*>(contents.data()), split).ok(), "send first half before switch");
    const auto c_updates = c->stream_updates();
    switch_to(*a, 1, DisplaySwitchCode::Ok);
    ServerInfo changed;
    require(until([&] { return a->take_stream_update(changed) && changed.width == 800; }) &&
            changed.origin_x == 640 && picture(*a, 800), "first viewer switches its own subscription");
    require(c->request_keyframe().ok() && picture(*c, 640) && c->stream_updates() == c_updates,
            "first viewer switching does not redirect remaining viewers");
    require(f.left->capture_destroyed == 0 && f.left->encoder_destroyed == 0 &&
            f.right->capture_created == 1 && f.right->encoder_created == 1,
            "switch reuses target and preserves source resources still in use");
    require(a->send_file_data(offer.transfer_id,
            reinterpret_cast<const uint8_t*>(contents.data() + split), contents.size() - split).ok() &&
            a->finish_file_upload(offer.transfer_id).ok(), "finish same transfer after switch");
    FileResult uploaded;
    require(until([&] { return a->take_file_result(uploaded); }) && uploaded.code == FileCode::Ok &&
            uploaded.received == contents.size(), "display switch preserves transfer state and digest");
    std::ifstream saved(f.fixture.root / offer.name, std::ios::binary);
    const std::string received((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
    require(received == contents, "transferred file matches across switch");
    saved.close();
    require(f.left->audio_sources == 1, "different displays still share one system audio source");
    finish(a);
    require(b->request_keyframe().ok() && picture(*b, 800) &&
            f.right->capture_created == 1 && f.right->encoder_created == 1,
            "first viewer leaving does not rebuild the remaining pipeline");
    switch_to(*b, 0, DisplaySwitchCode::Ok);
    require(until([&] { return b->take_stream_update(changed) && changed.width == 640; }) && picture(*b, 640),
            "view-only viewer switches without obtaining input control");
    require(!(b->permissions() & kPermissionInput), "display selection does not grant input control");
    require(!b->request_set_display_mode(0, 640, 480, 60).ok(),
            "view-only viewer still cannot change physical display mode");
    require(until([&] { return f.right->capture_destroyed == 1 && f.right->encoder_destroyed == 1; }),
            "last unsubscribe releases unused display pipeline");
    finish(c);
    require(b->request_keyframe().ok() && picture(*b, 640) &&
            f.left->capture_created == 1 && f.left->encoder_created == 1,
            "original display subscriber leaving preserves resources for the new subscriber");
    finish(b);
    require(until([&] { return f.left->capture_destroyed == 1 && f.left->encoder_destroyed == 1; }),
            "last viewer releases capture and encoder");
}

void refused_display_keeps_stream() {
    TwoDisplays f;
    auto a = f.fixture.connect();
    auto b = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput);
    require(picture(*a, 640) && picture(*b, 640), "both viewers start on shared display");
    switch_to(*b, 99, DisplaySwitchCode::NotFound);
    f.refuse_right = true;
    switch_to(*b, 1, DisplaySwitchCode::Failed);
    require(a->request_keyframe().ok() && b->request_keyframe().ok() &&
            picture(*a, 640) && picture(*b, 640), "invalid or unavailable target preserves both subscriptions");
    require(f.left->capture_created == 1 && f.left->encoder_created == 1 &&
            f.left->capture_destroyed == 0, "failed selection does not replace working media resources");
    f.refuse_right = false;
    switch_to(*b, 1, DisplaySwitchCode::Ok);
    require(picture(*b, 800), "same connection can retry after target recovers");
    finish(a);
    finish(b);
}

void concurrent_display_admission() {
    TwoDisplays f;
    std::vector<std::future<std::unique_ptr<QuicSession>>> connecting;
    for (uint16_t index : std::array<uint16_t, 4>{0, 1, 0, 1}) {
        connecting.push_back(std::async(std::launch::async, [&f, index] {
            return f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, index);
        }));
    }
    std::vector<std::unique_ptr<QuicSession>> viewers;
    for (auto& pending : connecting) viewers.push_back(pending.get());
    for (size_t i = 0; i < viewers.size(); ++i)
        require(picture(*viewers[i], i % 2 ? 800 : 640), "concurrent subscription receives its requested screen");
    require(f.left->capture_created == 1 && f.left->encoder_created == 1 &&
            f.right->capture_created == 1 && f.right->encoder_created == 1,
            "simultaneous joins create exactly one pipeline per display");
    f.reversed = true;
    auto renamed = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    require(picture(*renamed, 800) && f.right->capture_created == 1 && f.right->encoder_created == 1,
            "renumbered display list reuses stable device identity");
    for (auto& viewer : viewers) finish(viewer);
    require(renamed->request_keyframe().ok() && picture(*renamed, 800),
            "renumbered subscription survives all earlier viewers leaving");
    finish(renamed);
}

void display_codec_intersection() {
    TwoDisplays f(VideoCodec::Hevc);
    ServerInfo info;
    auto a = f.fixture.connect(SessionMode::Desktop, kPermissionAll, {}, 0, &info, kVideoCodecAll, StreamCodecPreference::Auto);
    require(info.codec == VideoCodec::Hevc && picture(*a, 640), "first display starts in HEVC");
    auto b = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1,
                              &info, kVideoCodecH264);
    require(info.codec == VideoCodec::H264 && picture(*b, 800), "second display admits H264-only viewer");
    auto c = f.fixture.connect(SessionMode::Desktop, kPermissionAll, {}, 1, &info);
    require(info.codec == VideoCodec::H264 && picture(*c, 800), "same display uses common decoder format");
    require(until([&] { return a->take_stream_update(info) && info.codec == VideoCodec::H264; }) && picture(*a, 640),
            "a restricted receiver on another display changes the global codec");
    bool refused = false;
    try { auto d = f.fixture.connect(SessionMode::Desktop, kPermissionAll, {}, 0, &info, kVideoCodecHevc); }
    catch (const std::exception&) { refused = true; }
    require(refused, "a new viewer with no global common decoder is refused");
    switch_to(*b, 0, DisplaySwitchCode::Ok);
    require(until([&] { return b->take_stream_update(info) && info.width == 640 &&
                              info.codec == VideoCodec::H264; }) && picture(*b, 640),
            "switch carries viewer decoder capabilities into target subscription");
    require(c->request_keyframe().ok() && picture(*c, 800), "old display continues while restriction follows the connection");
    finish(b);
    require(codec_update(*a, VideoCodec::Hevc) && picture(*a, 640),
            "restriction removal restores the preferred codec on the first display");
    require(codec_update(*c, VideoCodec::Hevc) && picture(*c, 800),
            "restriction removal restores the preferred codec on the other display");
    require(f.left->capture_created == 1 && f.right->capture_created == 1,
            "negotiation and switching keep each display capture alive");
    finish(a); finish(c);
}

void multiuser_quality() {
    TwoDisplays f;
    ServerInfo info;
    auto a = f.fixture.connect(SessionMode::Desktop, kPermissionAll, {}, 0);
    auto b = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1);
    auto options = [](QuicSession& peer, uint16_t fps, StreamQuality quality) {
        require(peer.request_stream_options({StreamCodecPreference::H264, fps, quality}).ok(), "send quality vote");
        StreamOptionsResult result;
        require(until([&] { return peer.take_stream_options_result(result); }) && result.code == StreamOptionsCode::Ok,
                "host acknowledges quality vote");
    };
    options(*a, 12, StreamQuality::Smooth);
    require(until([&] { return b->take_stream_update(info) && info.framerate == 12; }),
            "FPS vote reaches viewer of another display");
    options(*b, 30, StreamQuality::High);
    require(until([&] { return a->take_stream_update(info) && info.framerate == 12 && info.bitrate > 1000000; }),
            "recent High quality applies while smallest FPS cap remains");
    const auto high_rate = info.bitrate;
    options(*a, 20, StreamQuality::Smooth);
    require(until([&] { return a->take_stream_update(info) && info.framerate == 20 && info.bitrate == high_rate; }),
            "FPS-only change preserves another viewer's newer quality setting");
    options(*a, 20, StreamQuality::Smooth);
    require(until([&] { return a->take_stream_update(info) && info.bitrate < high_rate; }),
            "reselecting previous preset becomes the latest global quality vote");
    options(*b, 30, StreamQuality::High);
    require(until([&] { return a->take_stream_update(info) && info.bitrate == high_rate; }),
            "another viewer can explicitly restore its preferred quality");
    finish(b);
    require(until([&] { return a->take_stream_update(info) && info.framerate == 20 && info.bitrate < high_rate; }),
            "leaving quality owner restores remaining viewer's setting");
    // Custom image quality: percent/50 is the bitrate ratio, and the answer
    // reports the effective percent; preset answers report 0 instead.
    StreamOptions custom_options;
    custom_options.codec = StreamCodecPreference::H264;
    custom_options.max_fps = 20;
    custom_options.quality = StreamQuality::Custom;
    custom_options.quality_percent = 75;
    require(a->request_stream_options(custom_options).ok(), "send custom quality vote");
    StreamOptionsResult custom_result;
    require(until([&] { return a->take_stream_options_result(custom_result); }) &&
                custom_result.code == StreamOptionsCode::Ok &&
                custom_result.quality == StreamQuality::Custom &&
                custom_result.quality_percent == 75,
            "custom quality answer carries the effective percent");
    require(until([&] { return a->take_stream_update(info) && info.framerate == 20 && info.bitrate == high_rate; }),
            "custom 75% maps to the High bitrate ratio");
    custom_options.quality_percent = 25;
    require(a->request_stream_options(custom_options).ok(), "lower custom quality");
    require(until([&] { return a->take_stream_options_result(custom_result); }) &&
                custom_result.code == StreamOptionsCode::Ok &&
                custom_result.quality_percent == 25,
            "lowered custom quality is acknowledged");
    require(until([&] { return a->take_stream_update(info) && info.framerate == 20 && info.bitrate < high_rate; }),
            "custom 25% lowers the bitrate again");
    custom_options.quality = StreamQuality::High;
    custom_options.quality_percent = 0;
    require(a->request_stream_options(custom_options).ok(), "return to a preset after custom quality");
    require(until([&] { return a->take_stream_options_result(custom_result); }) &&
                custom_result.code == StreamOptionsCode::Ok &&
                custom_result.quality == StreamQuality::High && custom_result.quality_percent == 0,
            "a preset answer carries no custom percent");
    require(until([&] { return a->take_stream_update(info) && info.framerate == 20 && info.bitrate == high_rate; }),
            "a preset vote replaces the custom ratio");
    switch_to(*a, 1, DisplaySwitchCode::Ok);
    require(until([&] { return a->take_stream_update(info) && info.width == 800 && info.framerate == 20; }) &&
            picture(*a, 800), "quality participant survives independent display switch");
    finish(a);
}

void global_codec_preferences() {
    TwoDisplays f;
    ServerInfo info;
    const auto open = [&](uint16_t display) {
        return f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, display,
                                 &info, kVideoCodecAll, StreamCodecPreference::Auto);
    };
    auto a = open(0);
    auto b = open(1);
    auto c = open(1);
    require(info.codec == VideoCodec::Hevc && picture(*a, 640) && picture(*b, 800) && picture(*c, 800),
            "Auto starts every display with usable HEVC");
    auto vote = [&](QuicSession& viewer, StreamCodecPreference preference, VideoCodec expected) {
        require(viewer.request_stream_options({preference, 0, StreamQuality::Balanced}).ok(), "send global codec vote");
        StreamOptionsResult answer;
        require(until([&] { return viewer.take_stream_options_result(answer); }) &&
                answer.code == StreamOptionsCode::Ok && answer.codec == expected,
                "vote response reports actual global codec even when preference differs");
    };
    auto changed = [&](QuicSession& viewer, VideoCodec codec) {
        require(codec_update(viewer, codec),
                "global codec announcement reaches another connection");
    };
    vote(*a, StreamCodecPreference::H264, VideoCodec::H264);
    changed(*b, VideoCodec::H264);
    changed(*c, VideoCodec::H264);
    vote(*b, StreamCodecPreference::Hevc, VideoCodec::H264);
    vote(*c, StreamCodecPreference::Hevc, VideoCodec::Hevc);
    changed(*a, VideoCodec::Hevc);
    changed(*b, VideoCodec::Hevc);
    vote(*b, StreamCodecPreference::Auto, VideoCodec::H264);
    changed(*a, VideoCodec::H264);
    changed(*c, VideoCodec::H264);
    switch_to(*a, 1, DisplaySwitchCode::Ok);
    require(picture(*a, 800), "switch preserves the connection's vote and uses the target subscription");
    finish(a);
    changed(*b, VideoCodec::Hevc);
    changed(*c, VideoCodec::Hevc);
    require(picture(*b, 800) && picture(*c, 800), "remaining viewers decode after the winning voter leaves");
    require(f.left->capture_created == 1 && f.right->capture_created == 1,
            "global votes and switch never recreate display capture");
    finish(b); finish(c);
}

void runtime_software_recovery() {
    for (const int scenario : {0, 1, 2}) {
        const auto initial = scenario == 0 ? VideoCodec::H264 : VideoCodec::Hevc;
        TwoDisplays f(initial);
        auto a = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
        auto b = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1,
            nullptr, scenario == 2 ? kVideoCodecHevc : kVideoCodecAll);
        require(picture(*a, 640) && picture(*b, 800), "both hardware displays send initial IDRs");
        f.left->fail_hardware = true;
        require(a->request_keyframe().ok(), "trigger hardware encode failure on a static desktop");
        if (scenario == 2) {
            require(until([&] { return f.left->hardware_errors >= 3; }),
                    "repeat and direct encode failures reach the retry path");
            std::this_thread::sleep_for(500ms);
            require(a->peer_alive() && audio(*a) && f.left->software_created == 0 && f.left->resets == 0,
                    "incompatible decoder vote pauses only video, retaining audio and capture");
            finish(b);
        }
        bool left_announced = false;
        bool right_announced = !b;
        require(until([&] {
            ServerInfo info;
            if (a->take_stream_update(info))
                left_announced = info.codec == VideoCodec::Vp9 &&
                    info.encoder == "deterministic software encoder";
            if (b && b->take_stream_update(info))
                right_announced = info.codec == VideoCodec::Vp9 &&
                    info.encoder == "deterministic test encoder";
            FrameHeader header;
            std::vector<uint8_t> bytes;
            a->receive_frame(header, bytes, 10ms);
            if (b) b->receive_frame(header, bytes, 10ms);
            return left_announced && right_announced;
        }, 5s), "runtime recovery announces the software backend and compatible codec on both connections");
        require(a->request_keyframe().ok() && picture(*a, 640) &&
                    (!b || (b->request_keyframe().ok() && picture(*b, 800))),
                "remaining connections receive new IDRs after fallback");
        require(a->peer_alive() && audio(*a) && (!b || (b->peer_alive() && audio(*b))),
                "runtime encoder fallback keeps remaining connections and audio alive");
        require(f.left->software_created == 1 && f.right->software_created == 0 &&
                    f.left->resets == 0 && f.right->resets == 0,
                "only failed display switches backend and neither capture is rebuilt");
        finish(a); finish(b);
    }
}

// Reuse the existing deterministic media boundary and real QUIC transport.
// No webcam or desktop access is needed for shared-camera negotiation.
void shared_camera_service() {
    Fixture f;
    auto camera_media = std::make_shared<host_test::MediaState>();
    camera_media->moving = true;
    camera_media->capture_fps = 12;
    f.config.settings.allow_camera = true;
    f.config.settings.codec = VideoCodec::Hevc;
    f.config.cameras = [] { return std::vector<CameraDevice>{{1, "test camera", "test-camera-link", true}}; };
    f.config.camera_capture_factory = [camera_media](const CameraDevice&, LUID, uint32_t, std::string*) {
        auto info = host_test::display();
        info.device_name = L"test-camera-link";
        info.width = 320; info.height = 240; info.rect = {0, 0, 320, 240};
        return std::make_unique<host_test::Capture>(camera_media, info);
    };
    f.config.encoder_factory = [camera_media, desktop = f.media](ID3D11Device*, const EncoderConfig& cfg, std::string*) {
        return std::make_unique<host_test::Encoder>(cfg.width == 320 ? camera_media : desktop, cfg);
    };
    const auto open_camera = [](QuicSession& viewer, CameraCode expected = CameraCode::Ok) {
        require(viewer.request_camera_list().ok(), "request shared camera list");
        CameraList list;
        require(until([&] { return viewer.take_camera_list(list); }) && list.cameras.size() == 1,
                "camera enumeration is announced");
        require(viewer.request_camera_start(list.cameras.front().id).ok(), "request shared camera start");
        CameraResult result;
        require(until([&] { return viewer.take_camera_result(result); }, 5s) && result.code == expected,
                "camera start answers with the expected negotiation result");
    };
    const auto camera_frame = [](QuicSession& viewer, VideoCodec codec, bool first = false) {
        bool seen = false;
        return until([&] {
            FrameHeader header;
            std::vector<uint8_t> bytes;
            if (!viewer.receive_camera_frame(header, bytes, 30ms).ok()) return false;
            if (first && !seen) require(header.idr, "first camera picture is a usable IDR");
            seen = true;
            return header.codec == codec && header.width == 320 && header.height == 240 &&
                bytes.size() == 6 && bytes.back() == 20 && (!first || header.idr);
        });
    };
    f.start();
    auto solo = f.connect(SessionMode::Camera, kPermissionCamera, {}, kDisplayAuto,
        nullptr, kVideoCodecAll, StreamCodecPreference::H264);
    open_camera(*solo);
    require(camera_frame(*solo, VideoCodec::H264, true) && f.media->capture_created == 0,
            "camera-only streaming starts without creating a desktop capture");
    StreamOptions solo_options;
    solo_options.codec = StreamCodecPreference::Hevc;
    solo_options.max_fps = 10;
    require(solo->request_stream_options(solo_options).ok(), "change the only camera's codec vote");
    StreamOptionsResult solo_result;
    require(until([&] { return solo->take_stream_options_result(solo_result); }) &&
                solo_result.code == StreamOptionsCode::Ok && solo_result.codec == VideoCodec::Hevc &&
                solo_result.fps == 10 && camera_frame(*solo, VideoCodec::Hevc),
            "a camera-only session can change its actual encoding format");
    finish(solo);
    require(until([&] { return camera_media->capture_destroyed == 1 &&
        camera_media->encoder_created == camera_media->encoder_destroyed; }),
        "camera-only disconnect releases its capture and encoders");
    auto desktop = f.connect();
    require(keyframe(*desktop), "desktop starts before camera negotiation");
    open_camera(*desktop);
    require(camera_frame(*desktop, VideoCodec::Hevc, true), "attached camera uses the desktop connection's vote");
    auto voter = f.connect(SessionMode::Desktop, kPermissionAll, {}, 0, nullptr,
        kVideoCodecAll, StreamCodecPreference::H264);
    auto majority = f.connect(SessionMode::Desktop, kPermissionAll, {}, 0, nullptr,
        kVideoCodecAll, StreamCodecPreference::H264);
    require(codec_update(*desktop, VideoCodec::H264) && camera_frame(*desktop, VideoCodec::H264),
            "two H264 votes beat one HEVC connection with both desktop and camera");
    finish(voter); finish(majority);
    require(codec_update(*desktop, VideoCodec::Hevc) && camera_frame(*desktop, VideoCodec::Hevc),
            "desktop and attached camera restore the remaining connection's codec");

    auto camera_only = f.connect(SessionMode::Camera, kPermissionCamera, {}, kDisplayAuto,
        nullptr, kVideoCodecH264);
    open_camera(*camera_only);
    require(camera_frame(*camera_only, VideoCodec::H264, true) && codec_update(*desktop, VideoCodec::H264),
            "camera-only decoder capabilities constrain desktop and camera together");
    StreamOptions options;
    options.codec = StreamCodecPreference::Hevc;
    options.max_fps = 10;
    options.quality = StreamQuality::Smooth;
    require(camera_only->request_stream_options(options).ok(), "change camera-only video options");
    StreamOptionsResult applied;
    require(until([&] { return camera_only->take_stream_options_result(applied); }) &&
                applied.code == StreamOptionsCode::Ok && applied.codec == VideoCodec::H264 &&
                applied.fps == 10 && applied.quality == StreamQuality::Smooth,
            "camera-only options report the negotiated codec and shared quality/FPS");
    require(camera_only->request_camera_stop().ok(), "stop camera while retaining its options");
    require(codec_update(*desktop, VideoCodec::Hevc), "stopping camera releases its active policy");
    open_camera(*camera_only);
    require(camera_frame(*camera_only, VideoCodec::H264, true) && codec_update(*desktop, VideoCodec::H264),
            "camera can restart on the same connection with accepted options");
    options.max_fps = 60;
    require(camera_only->request_stream_options(options).ok(), "raise camera requested FPS");
    require(until([&] { return camera_only->take_stream_options_result(applied); }) &&
                applied.code == StreamOptionsCode::Ok && applied.fps == 12,
            "the shared encoder respects the camera's negotiated device rate");
    const auto encoders = camera_media->encoder_created.load();
    auto second = f.connect(SessionMode::Camera, kPermissionCamera, {}, kDisplayAuto,
        nullptr, kVideoCodecH264);
    open_camera(*second);
    require(camera_frame(*second, VideoCodec::H264, true) && camera_media->capture_created == 2 &&
                camera_media->encoder_created == encoders,
            "multiple viewers reuse one camera capture and encoder");
    require(camera_only->request_camera_stop().ok(), "stop one camera subscription");
    finish(camera_only);
    require(camera_frame(*second, VideoCodec::H264) && camera_media->capture_destroyed == 1,
            "another camera viewer continues after one leaves");

    auto incompatible = f.connect(SessionMode::Camera, kPermissionCamera, {}, kDisplayAuto,
        nullptr, kVideoCodecAv1);
    open_camera(*incompatible, CameraCode::Failed);
    require(desktop->request_keyframe().ok() && keyframe(*desktop) &&
                camera_frame(*second, VideoCodec::H264),
            "an incompatible camera viewer does not break working streams");
    finish(incompatible);
    require(second->request_camera_stop().ok(), "stop the last H264-only camera viewer");
    require(codec_update(*desktop, VideoCodec::Hevc) && camera_frame(*desktop, VideoCodec::Hevc),
            "stopping camera releases its decoder vote while its connection remains alive");
    finish(second);

    camera_media->fail_hardware = true;
    require(camera_frame(*desktop, VideoCodec::Vp9) && codec_update(*desktop, VideoCodec::Vp9),
            "runtime camera encoder failure falls back to software VP9 and renegotiates the desktop");
    require(camera_media->software_created == 1 && f.media->software_created == 0 &&
                camera_media->resets == 0 && f.media->resets == 0,
            "only the failed camera switches backend without resetting capture");
    f.host->set_permissions(kPermissionAll & ~kPermissionCamera);
    require(until([&] { return camera_media->capture_destroyed == 2; }),
            "revoking camera permission releases the last camera capture");
    require(desktop->request_keyframe().ok() && keyframe(*desktop) && audio(*desktop),
            "desktop and audio remain usable after camera revocation");
    require(camera_media->encoder_created == camera_media->encoder_destroyed && camera_media->wrong_thread == 0,
            "all camera resources are released on their owning service thread");
    finish(desktop);
}

// A camera opens on the desktop connection and shares its codec group. A
// camera that can only supply 4:2:0 must renegotiate the desktop instead of
// leaving one source at 4:4:4; closing the camera restores full color.
void shared_camera_full_color() {
    Fixture f;
    f.media->moving = true;
    f.media->original_bgra = true;
    f.config.settings.allow_camera = true;
    f.config.settings.codec = VideoCodec::Vp9;
    auto camera_media = std::make_shared<host_test::MediaState>();
    camera_media->moving = true;
    camera_media->capture_fps = 12;
    f.config.cameras = [] { return std::vector<CameraDevice>{{1, "test camera", "test-camera-link", true}}; };
    f.config.camera_capture_factory = [camera_media](const CameraDevice&, LUID, uint32_t, std::string*) {
        auto info = host_test::display();
        info.device_name = L"test-camera-link";
        info.width = 320; info.height = 240; info.rect = {0, 0, 320, 240};
        return std::make_unique<host_test::Capture>(camera_media, info);
    };
    f.config.encoder_factory = [camera_media, desktop = f.media](ID3D11Device*, const EncoderConfig& cfg, std::string*) {
        return std::make_unique<host_test::Encoder>(cfg.width == 320 ? camera_media : desktop, cfg);
    };
    const auto desktop_frame = [](QuicSession& viewer, VideoChroma chroma) {
        return until([&] {
            FrameHeader header;
            std::vector<uint8_t> bytes;
            if (!viewer.receive_frame(header, bytes, 50ms).ok()) return false;
            return !bytes.empty() && header.codec == VideoCodec::Vp9 && header.chroma == chroma;
        });
    };
    const auto camera_frame = [](QuicSession& viewer, VideoChroma chroma) {
        return until([&] {
            FrameHeader header;
            std::vector<uint8_t> bytes;
            if (!viewer.receive_camera_frame(header, bytes, 30ms).ok()) return false;
            return !bytes.empty() && header.codec == VideoCodec::Vp9 && header.width == 320 &&
                header.chroma == chroma;
        });
    };
    f.start();
    auto desktop = f.connect(SessionMode::Desktop, kPermissionAll, {}, kDisplayAuto, nullptr,
        kVideoCodecAll, StreamCodecPreference::Vp9, kVideoCodecVp9, VideoChroma::Yuv444);
    require(desktop_frame(*desktop, VideoChroma::Yuv444),
            "a full-color VP9 connection receives 4:4:4");
    require(desktop->request_camera_list().ok(), "request camera list");
    CameraList list;
    require(until([&] { return desktop->take_camera_list(list); }) && list.cameras.size() == 1,
            "camera enumeration is announced");
    require(desktop->request_camera_start(list.cameras.front().id).ok(),
            "start the 4:2:0-only camera");
    CameraResult started;
    require(until([&] { return desktop->take_camera_result(started); }, 5s) &&
                started.code == CameraCode::Ok,
            "camera start renegotiates the shared format");
    require(camera_frame(*desktop, VideoChroma::Yuv420) &&
                desktop_frame(*desktop, VideoChroma::Yuv420),
            "camera and desktop both stream 4:2:0 while the camera is open");
    require(desktop->request_camera_stop().ok(), "stop the 4:2:0-only camera");
    require(desktop_frame(*desktop, VideoChroma::Yuv444),
            "closing the camera restores the desktop's full-color format");
    finish(desktop);
    require(until([&] { return camera_media->capture_destroyed == 1 &&
                camera_media->encoder_created == camera_media->encoder_destroyed; }),
            "the camera pipeline is released on its service thread");
}

void operator_connection_management() {
    Fixture f;
    f.config.settings.max_viewers = 4;
    std::mutex chat_mutex;
    std::map<uint64_t, std::string> received;
    f.config.viewer_chat_message = [&](uint64_t id, const std::string& text) {
        std::lock_guard lock(chat_mutex); received[id] = text;
    };
    f.start();
    auto a = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput);
    auto viewers = f.host->viewers();
    require(viewers.size() == 1 && viewers.front().id && viewers.front().mode == SessionMode::Desktop,
            "admitted desktop has a stable operator ID and session mode");
    const auto aid = viewers.front().id;
    auto b = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput);
    viewers = f.host->viewers();
    require(viewers.size() == 2 && viewers.back().id != aid, "concurrent viewers have distinct IDs");
    const auto bid = viewers.back().id;
    auto files = f.connect(SessionMode::FileTransfer, kPermissionFileTransfer);
    viewers = f.host->viewers();
    require(viewers.size() == 3 && viewers.back().mode == SessionMode::FileTransfer,
            "the operator list includes files-only sessions");
    const auto fid = viewers.back().id;
    require(a->send_chat("from A").ok() && b->send_chat("from B").ok() && files->send_chat("from files").ok(),
            "each session sends a chat message");
    require(until([&] {
        std::lock_guard lock(chat_mutex);
        return received[aid] == "from A" && received[bid] == "from B" && received[fid] == "from files";
    }), "incoming chats identify the actual sender across concurrent session modes");
    const auto chat = [](QuicSession& viewer, const std::string& expected) {
        std::string text;
        return until([&] { return viewer.take_chat(text); }) && text == expected;
    };
    require(f.host->send_chat(aid, "only A") && chat(*a, "only A"),
            "reply to an older viewer despite a different most-recent connection");
    std::string other;
    require(!b->take_chat(other) && !files->take_chat(other), "targeted reply never appears in other conversations");
    require(f.host->send_chat(fid, "only files") && chat(*files, "only files"), "files-only sessions receive operator replies");
    require(f.host->set_permissions(aid, kPermissionAll & ~(kPermissionInput | kPermissionClipboard | kPermissionAudio)),
            "revoke permissions for the selected older connection");
    require(until([&] { return !(a->permissions() & (kPermissionAudio | kPermissionClipboard)); }) &&
                (b->permissions() & (kPermissionAudio | kPermissionClipboard)) == (kPermissionAudio | kPermissionClipboard),
            "selected permission changes leave another viewer's grant unchanged");
    require(f.host->set_permissions(aid, kPermissionAll) &&
                until([&] { return (a->permissions() & kPermissionAudio) != 0; }) && !(a->permissions() & kPermissionInput),
            "restoring selected permissions stays within the original approved ceiling");

    const auto path = f.root / "operator-target.txt";
    const std::string content = "only the selected connection receives these bytes";
    { std::ofstream output(path, std::ios::binary); output << content; }
    std::string error;
    require(f.host->start_file_download(bid, path.string(), &error), "queue a file for a specific desktop viewer");
    FileOffer offer;
    require(until([&] { return b->take_file_offer(offer); }), "only the selected viewer receives the file offer");
    FileOffer unrelated;
    require(!a->take_file_offer(unrelated) && !files->take_file_offer(unrelated), "file offer is not routed to the latest viewer");
    require(b->send_file_answer(offer.transfer_id, FileCode::Ok, 0).ok(), "selected viewer accepts its file");
    std::string transferred;
    require(until([&] {
        FileChunk chunk;
        if (b->receive_file_chunk(chunk, 20ms).ok()) {
            require(chunk.transfer_id == offer.transfer_id && chunk.offset == transferred.size(), "target file offsets are contiguous");
            transferred.append(reinterpret_cast<const char*>(chunk.data.data()), chunk.data.size());
        }
        return transferred.size() == content.size();
    }) && transferred == content, "targeted file content arrives intact");
    require(b->send_file_result(offer.transfer_id, FileCode::Ok, transferred.size()).ok(), "selected viewer completes the download");

    require(f.host->disconnect_viewer(aid), "disconnect one selected viewer without stopping the listener");
    require(until([&] { return !a->peer_alive(); }, 4s), "selected viewer is disconnected");
    a.reset();
    require(b->peer_alive() && files->peer_alive() && f.host->running() &&
                f.host->send_chat(bid, "B stays") && chat(*b, "B stays"),
            "other viewers and the listener remain usable after targeted disconnect");
    auto newcomer = f.connect(SessionMode::Terminal, kPermissionTerminal);
    viewers = f.host->viewers();
    const auto nid = viewers.back().id;
    require(nid != aid && nid != bid && nid != fid && viewers.back().mode == SessionMode::Terminal,
            "a replacement connection gets a new ID and the freed admission slot");
    require(!f.host->send_chat(aid, "stale") && !f.host->set_permissions(aid, 0) &&
                !f.host->start_file_download(aid, path.string(), &error) && !f.host->disconnect_viewer(aid),
            "all stale-ID actions fail without retargeting the replacement viewer");
    require(f.host->send_chat(nid, "terminal target") && chat(*newcomer, "terminal target") &&
                newcomer->send_chat("terminal response").ok(), "control-only sessions support targeted two-way chat");
    require(until([&] { std::lock_guard lock(chat_mutex); return received[nid] == "terminal response"; }),
            "control-only chat callback retains the connection ID");
    finish(newcomer); finish(files); finish(b);

    Fixture single;
    single.config.settings.max_viewers = 1;
    single.start();
    auto old = single.connect(SessionMode::FileTransfer, kPermissionFileTransfer);
    const auto oldid = single.host->viewers().front().id;
    require(single.host->start_file_download(oldid, path.string(), &error),
            "queue a file immediately before ending a sequential session");
    require(single.host->disconnect_viewer(oldid) && until([&] { return !old->peer_alive(); }, 4s),
            "targeted disconnect also works on the sequential listener");
    old.reset();
    require(until([&] { return single.host->status().state == HostState::Listening; }),
            "sequential listener returns to listening after a local disconnect");
    auto next = single.connect(SessionMode::FileTransfer, kPermissionFileTransfer);
    const auto newid = single.host->viewers().front().id;
    require(newid != oldid && newid != aid && !single.host->send_chat(oldid, "stale sequential"),
            "IDs are not reused across sequential sessions or different host instances");
    require(single.host->send_chat(newid, "fresh sequential") && chat(*next, "fresh sequential"),
            "next sequential viewer receives only its own queued messages");
    require(!until([&] { return next->take_file_offer(unrelated); }, 200ms),
            "queued operator files never cross into the next sequential session");
    finish(next);
}

void service_connection_management() {
    host::ServiceControlServer control;
    Fixture f;
    f.config.service_approval = true;
    f.config.settings.auth_mode = HostAuthMode::Click;
    f.config.settings.password_hash.clear();
    f.config.approve_client = [&](const ClientRequest& request) { return control.approve(request); };
    f.config.viewer_chat_message = [&](uint64_t id, const std::string& text) { control.receive_chat(id, text); };
    f.start();
    const auto pipe = L"\\\\.\\pipe\\chast-control-test-" + std::to_wstring(GetCurrentProcessId());
    std::string error;
    require(control.start_for_test(*f.host, pipe, &error, [] {
        return host::ServiceRegistrationInfo{"service-device", true};
    }), "start a logon-restricted local management pipe");
    host::ServiceControlClient client;
    client.start_for_test(pipe, GetCurrentProcessId());
    require(until([&] { return client.connected(); }), "mutual process and logon identity validation connects the manager");
    const auto information = client.information();
    require(information.device_id == "service-device" && information.registration_enabled &&
                !information.fingerprint.empty() && information.fingerprint == f.host->status().fingerprint &&
                information.auth_mode == HostAuthMode::Click && !information.password_configured && !information.two_factor_enabled,
            "manager receives public identity and effective authentication state from the running host");
    auto actions = client.actions();
    auto declined = std::async(std::launch::async, [&] {
        try { auto viewer = f.connect(SessionMode::FileTransfer, kPermissionFileTransfer); return false; }
        catch (const std::exception&) { return true; }
    });
    require(until([&] { return client.approvals().size() == 1; }), "manager receives a request it may decline");
    const auto denied_id = client.approvals().front().id;
    require(client.answer(denied_id, false, 0) && declined.get(), "local refusal rejects the waiting QUIC connection");
    require(until([&] { return client.approvals().empty(); }) && !client.answer(denied_id, true, kPermissionAll),
            "completed approval IDs cannot be reused to admit a different viewer");
    const auto connect_approved = [&] {
        auto waiting = std::async(std::launch::async, [&] { return f.connect(SessionMode::FileTransfer, kPermissionFileTransfer); });
        require(until([&] { return client.approvals().size() == 1; }), "service Click publishes a pending request without sharing credentials");
        const auto request = client.approvals().front();
        require(request.mode == SessionMode::FileTransfer && request.permissions == kPermissionFileTransfer &&
                    client.answer(request.id, true, kPermissionAll), "local approval retains the request's permission ceiling");
        auto viewer = waiting.get();
        require(viewer->permissions() == kPermissionFileTransfer, "approved files-only connection cannot gain desktop permissions");
        return viewer;
    };
    auto a = connect_approved();
    require(until([&] { return actions.viewers().size() == 1; }), "service manager lists admitted connections");
    const auto aid = actions.viewers().front().id;
    auto b = connect_approved();
    require(until([&] { return actions.viewers().size() == 2; }), "service manager lists both clients independently");
    const auto bid = actions.viewers().back().id;
    require(actions.chat(aid, "target A through IPC"), "queue a chat for the older service connection");
    std::string chat;
    require(until([&] { return a->take_chat(chat); }) && chat == "target A through IPC" && !b->take_chat(chat),
            "pipe dispatch routes chat only to the explicit target");
    require(b->send_chat("B replies through IPC").ok(), "viewer sends a reply to the local service manager");
    require(until([&] {
        for (const auto& notice : client.take_notices())
            if (notice.incoming && notice.id == bid && notice.text == "B replies through IPC") return true;
        return false;
    }), "service chat events retain sender identity");
    require(actions.permissions(aid, 0) && until([&] { return a->permissions() == 0; }),
            "local service manager revokes the selected connection's permissions");
    require(b->permissions() == kPermissionFileTransfer, "permission changes preserve the other connection");
    require(actions.permissions(aid, kPermissionAll) && until([&] { return a->permissions() == kPermissionFileTransfer; }),
            "service permission restoration cannot exceed admission");
    const auto path = f.root / "operator-service.txt";
    const std::string content = "file opened under the authenticated operator token";
    { std::ofstream output(path, std::ios::binary); output << content; }
    require(actions.file(bid, path.string(), &error), "queue a user-authorized file through the service pipe");
    FileOffer offer;
    require(until([&] { return b->take_file_offer(offer); }), "service hashes the operator file under the caller token");
    require(b->send_file_answer(offer.transfer_id, FileCode::Ok, 0).ok(), "viewer accepts the service file");
    std::string received;
    require(until([&] {
        FileChunk chunk;
        if (b->receive_file_chunk(chunk, 20ms).ok()) {
            require(chunk.transfer_id == offer.transfer_id && chunk.offset == received.size(), "service file offsets remain contiguous");
            received.append(reinterpret_cast<const char*>(chunk.data.data()), chunk.data.size());
        }
        return received.size() == content.size();
    }) && received == content, "service reopens the file with the operator token and delivers intact bytes");
    require(b->send_file_result(offer.transfer_id, FileCode::Ok, received.size()).ok(), "service file completes");
    FileOffer unrelated;
    require(!a->take_file_offer(unrelated), "service file never reaches the other viewer");
    require(actions.disconnect(aid) && until([&] { return !a->peer_alive(); }), "service manager disconnects the explicit target");
    a.reset();
    require(b->peer_alive() && f.host->running(), "other viewer and host survive operator disconnect");

    control.stop();
    require(until([&] { return !client.connected(); }), "a lost service endpoint disables local actions");
    require(client.information().device_id.empty() && client.information().fingerprint.empty(),
            "endpoint loss clears stale service identity");
    require(!actions.chat(bid, "offline"), "an offline service action is rejected");
    require(control.start_for_test(*f.host, pipe, &error, [] {
        return host::ServiceRegistrationInfo{"restarted-device", false};
    }), "restart the endpoint with a new worker-instance identity");
    require(until([&] { return client.connected() && actions.viewers().size() == 1; }), "manager reconnects after the endpoint restarts");
    require(client.information().device_id == "restarted-device" && !client.information().registration_enabled,
            "reconnected manager replaces the previous registration snapshot");
    const auto fresh = actions.viewers().front().id;
    require(fresh != bid && !actions.chat(bid, "stale after restart") && !actions.disconnect(bid),
            "old UI IDs never target a new worker even when remote numeric IDs are identical");
    require(actions.chat(fresh, "fresh target") && until([&] { return b->take_chat(chat); }) && chat == "fresh target",
            "fresh service selection works after restart");
    finish(b);
    auto abandoned = std::async(std::launch::async, [&] {
        try { auto viewer = f.connect(SessionMode::FileTransfer, kPermissionFileTransfer); return false; }
        catch (const std::exception&) { return true; }
    });
    require(until([&] { return client.approvals().size() == 1; }), "another request waits for its local operator");
    client.stop();
    require(abandoned.get(), "closing the local manager cancels outstanding approval instead of admitting the viewer");
    host::ServiceControlClient untrusted;
    untrusted.start_for_test(pipe, GetCurrentProcessId() + 1);
    require(!until([&] { return untrusted.connected(); }, 500ms), "manager refuses a pipe owned by an unexpected process");
    untrusted.stop();
    host::ServiceControlClient forged_service;
    forged_service.start_for_test(pipe, 0);
    require(!until([&] { return forged_service.connected(); }, 500ms),
            "production identity checks reject a user-owned pipe impersonating the SYSTEM service");
    forged_service.stop();
    control.stop();
    require(control.start_for_test(*f.host, pipe, &error), "service approval starts without a connected operator");
    auto missing = std::async(std::launch::async, [&] {
        try { auto viewer = f.connect(SessionMode::FileTransfer, kPermissionFileTransfer); return false; }
        catch (const std::exception&) { return true; }
    });
    require(missing.get(), "service Click refuses the viewer when no authenticated local operator is present");
    control.stop();
}

void application_latency() {
    Fixture f;
    f.config.settings.max_viewers = 1;
    f.config.adaptive_bitrate = true;
    f.config.adaptive_fps = true;
    f.start();
    ServerInfo info;
    auto viewer = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0, &info);
    require(info.framerate == 15 && keyframe(*viewer), "adaptive session begins at RustDesk initial FPS");
    require(viewer->send_latency_probe().ok() && until([&] { return viewer->latency().replies > 0; }),
            "client clock synchronization still works with reverse QoS probes");
    require(until([&] { return f.host->status().stats.qos_probe_replies >= 4; }, 6s),
            "host measures authenticated application round trips");
    require(until([&] { return viewer->take_stream_update(info) && info.framerate > 15; }),
            "good application replies raise shared FPS on a static desktop");
    const auto idle = f.host->status().stats;
    require(idle.repeats > 0 && idle.video_pending_frames == 0,
            "static refinements are best effort and claim no delivery-window credit");
    finish(viewer);
}

void video_backpressure() {
    Fixture f;
    f.config.settings.max_viewers = 1;
    f.config.adaptive_bitrate = true;
    f.config.adaptive_fps = true;
    f.media->moving = true;
    f.start();
    auto viewer = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput);
    require(keyframe(*viewer), "moving video begins with an IDR");
    require(until([&] {
        const auto stats = f.host->status().stats;
        return stats.qos_probe_replies >= 4 && stats.video_pending_ms > 2000 && stats.framerate <= 5;
    }, 7s), "unconsumed video throttles shared QoS while control probes still return");
    const auto blocked = f.host->status().stats;
    require(blocked.video_pending_frames > 0 && blocked.video_pending_frames <= 60 && blocked.encoded < 60,
            "a stalled application has a bounded delivery window");
    require(viewer->send_latency_probe().ok() && until([&] { return viewer->latency().replies > 0; }),
            "clock synchronization remains responsive during video backpressure");
    uint64_t expected = 1;
    unsigned pictures = 0;
    bool fresh_idr = false;
    require(until([&] {
        FrameHeader header;
        std::vector<uint8_t> bytes;
        if (viewer->receive_frame(header, bytes, 50ms).ok()) {
            require(header.seq == expected++, "resumed video retains contiguous transport sequences");
            ++pictures;
            fresh_idr |= header.idr;
        }
        const auto stats = f.host->status().stats;
        return fresh_idr && pictures > blocked.video_pending_frames && stats.video_receipts > blocked.video_receipts &&
               stats.video_pending_ms < 500 && stats.framerate > 5;
    }, 12s), "consumption resumes at a fresh IDR and shared FPS recovers");
    require(f.media->capture_created == 1 && f.media->encoder_created == 1,
            "network recovery preserves the shared capture and encoder");
    finish(viewer);
}

void sender_queue_freshness() {
    Fixture f;
    f.config.settings.max_viewers = 1;
    f.media->moving = true;
    auto paused = std::make_shared<std::atomic<bool>>(false);
    auto resume = std::make_shared<std::atomic<bool>>(false);
    f.config.packet = [paused, resume](const FrameHeader&, const uint8_t*, size_t) {
        if (paused->exchange(true)) return;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!resume->load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
    };
    f.start();
    auto viewer = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput);
    require(until([&] { return paused->load(); }), "pause the sender after its first delivered packet");
    std::this_thread::sleep_for(400ms);
    resume->store(true);
    require(keyframe(*viewer), "consume the initial video receipt after the sender pause");
    require(until([&] {
        FrameHeader header;
        std::vector<uint8_t> bytes;
        viewer->receive_frame(header, bytes, 20ms);
        const auto stats = f.host->status().stats;
        return stats.video_age_resyncs > 0 && stats.video_expired_frames > 0;
    }), "host status exposes expired local frames and age-triggered recovery");
    require(f.host->status().stats.video_max_queue_wait_ms <= 250,
            "delivered local queue waiting stays within its age budget");
    require(f.media->encoder_created == 1, "sender age recovery retains the shared encoder");
    finish(viewer);
}

void slow_viewer_independent_display() {
    TwoDisplays f;
    f.left->moving = f.right->moving = true;
    auto slow = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0);
    auto fast = f.fixture.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 1);
    require(picture(*slow, 640) && picture(*fast, 800), "viewers start on independent moving displays");
    const auto end = std::chrono::steady_clock::now() + 2500ms;
    unsigned received = 0;
    while (std::chrono::steady_clock::now() < end) {
        FrameHeader header;
        std::vector<uint8_t> bytes;
        if (fast->receive_frame(header, bytes, 100ms).ok()) {
            require(header.width == 800, "healthy viewer stays on its selected display");
            ++received;
        }
    }
    require(received >= 20, "stalled viewer does not block another display's video delivery");
    const auto before_switch = std::chrono::steady_clock::now();
    switch_to(*slow, 1, DisplaySwitchCode::Ok);
    require(std::chrono::steady_clock::now() - before_switch < 1s,
            "stalled viewer can switch displays without waiting for its video queue");
    require(picture(*slow, 800) && picture(*fast, 800), "both subscriptions resume on the shared target");
    require(f.right->capture_created == 1 && f.right->encoder_created == 1,
            "slow viewer switch reuses the existing target pipeline");
    finish(slow);
    require(picture(*fast, 800), "healthy viewer continues after slow viewer leaves");
    finish(fast);
}

// Explicit opt-in: captures the local display but injects no input and never
// changes modes, privacy, login state or topology. The normal suite stays
// deterministic and does not need a GPU or interactive desktop.
void hardware_shared_display(bool automatic = false) {
    Fixture f;
    f.config.adaptive_bitrate = true;
    f.config.adaptive_fps = true;
    HostServerConfig defaults;
    std::atomic<unsigned> captures{0}, encoders{0};
    f.config.displays = defaults.displays;
    f.config.follow_desktop = defaults.follow_desktop;
    f.config.desktop_status = defaults.desktop_status;
    f.config.capture_factory = [&](const DisplayInfo& display, const CaptureConfig& cfg, std::string* error) {
        ++captures;
        return defaults.capture_factory(display, cfg, error);
    };
    f.config.encoder_factory = [&](ID3D11Device* device, const EncoderConfig& cfg, std::string* error) {
        ++encoders;
        return defaults.encoder_factory(device, cfg, error);
    };
    f.start();
    ComPtr<ID3D11Device> a_device, b_device;
    auto device = [](ComPtr<ID3D11Device>& result) {
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                    result.put(), nullptr, nullptr)), "create viewer GPU device");
    };
    device(a_device); device(b_device);
    const auto a_codecs = supported_decoder_codecs(a_device.get());
    const auto b_codecs = supported_decoder_codecs(b_device.get());
    require(a_codecs != 0 && b_codecs != 0, "probe actual NV12 hardware decoder capabilities");
    ServerInfo a_info, b_info;
    const auto preference = automatic ? StreamCodecPreference::Auto : StreamCodecPreference::H264;
    auto a = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, kDisplayAuto, &a_info, a_codecs, preference);
    auto b = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, kDisplayAuto, &b_info, b_codecs, preference);
    require((a_codecs & video_codec_bit(a_info.codec)) && (b_codecs & video_codec_bit(b_info.codec)),
            "Hello chooses a codec each actual viewer GPU can decode");
    std::string error;
    unsigned a_pictures = 0, b_pictures = 0;
    DecoderConfig cfg;
    cfg.codec = a_info.codec;
    cfg.d3d11_device = a_device.get();
    cfg.gpu_handler = [&](const GpuFrame&) { ++a_pictures; return Error::success(); };
    auto a_decoder = create_decoder(cfg, &error);
    cfg.codec = b_info.codec;
    cfg.d3d11_device = b_device.get();
    cfg.gpu_handler = [&](const GpuFrame&) { ++b_pictures; return Error::success(); };
    auto b_decoder = create_decoder(cfg, &error);
    require(a_decoder && b_decoder, "open two independent hardware-stream decoders");
    auto decode = [&](QuicSession& viewer, IVideoDecoder& decoder, unsigned& pictures, unsigned count,
                      std::optional<uint32_t> expected_stream = {}) {
        const auto target = pictures + count;
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (pictures < target && std::chrono::steady_clock::now() < deadline) {
            FrameHeader header;
            std::vector<uint8_t> data;
            auto received = viewer.receive_frame(header, data, 150ms);
            if (!received.ok()) {
                require(received.status() == Status::WaitTimeout && viewer.request_keyframe().ok(),
                        "request hardware IDR on static desktop");
                continue;
            }
            if (expected_stream && header.stream_id != *expected_stream) continue;
            std::vector<DecodedFrame> output;
            require(decoder.decode(data.data(), data.size(), header.pts_ms, header.keyframe, output).ok(),
                    "shared GPU access unit decodes without reference errors");
            require(output.empty(), "decoder delivers GPU textures without CPU fallback");
        }
        require(pictures >= target, "viewer decodes requested picture count");
    };
    decode(*a, *a_decoder, a_pictures, 8);
    decode(*b, *b_decoder, b_pictures, 8);
    require(captures == 1 && encoders == 1, "two real QUIC viewers share one GPU capture and encoder");
    uint16_t current_display = 0;
    for (size_t index = 0; index < a_info.displays.size(); ++index)
        if (a_info.displays[index].left == a_info.origin_x && a_info.displays[index].top == a_info.origin_y)
            current_display = static_cast<uint16_t>(index);
    auto selected = subscriptions(*a, {0, {}, {}, {current_display}});
    require(selected.streams.size() == 1, "GPU viewer enters subscription-set mode");
    decode(*a, *a_decoder, a_pictures, 4, selected.streams[0].stream_id);
    require(subscriptions(*a, {}).streams.empty(), "GPU viewer can temporarily unsubscribe all video");
    selected = subscriptions(*a, {0, {current_display}, {}, {}});
    require(selected.streams.size() == 1 && selected.streams[0].stream_id != 0,
            "GPU resubscription receives a fresh stream ID");
    require(a->request_display_keyframe(selected.streams[0].stream_id).ok(), "request targeted GPU stream IDR");
    decode(*a, *a_decoder, a_pictures, 4, selected.streams[0].stream_id);
    require(captures == 1 && encoders == 1, "GPU resubscription reuses source retained by the second connection");
    finish(a);
    require(b->request_keyframe().ok(), "remaining viewer requests hardware IDR");
    decode(*b, *b_decoder, b_pictures, 8);
    require(captures == 1 && encoders == 1, "remaining decoder continues without GPU resource replacement");
    finish(b);
}

void negotiated_shared_codec() {
    Fixture f;
    f.config.settings.codec = VideoCodec::Hevc;
    f.start();
    ServerInfo initial;
    auto a = f.connect(SessionMode::Desktop, kPermissionAll, {}, 0, &initial);
    require(initial.codec == VideoCodec::Hevc && keyframe(*a), "unrestricted viewer starts with configured HEVC");
    auto b = f.connect(SessionMode::Desktop, kPermissionAll & ~kPermissionInput, {}, 0, &initial, kVideoCodecH264);
    require(initial.codec == VideoCodec::H264 && keyframe(*b), "H264-only viewer receives a compatible Hello and IDR");
    ServerInfo changed;
    require(until([&] { return a->take_stream_update(changed) && changed.codec == VideoCodec::H264; }),
            "existing viewer learns common codec before incompatible payloads arrive");
    require(a->request_stream_options({StreamCodecPreference::Hevc, 0, StreamQuality::High}).ok(),
            "request codec incompatible with another viewer");
    StreamOptionsResult options;
    require(until([&] { return a->take_stream_options_result(options); }) &&
            options.code == StreamOptionsCode::Ok && options.codec == VideoCodec::H264,
            "unusable vote is retained while the result stays compatible with all viewers");
    require(a->request_keyframe().ok() && b->request_keyframe().ok() && keyframe(*a) && keyframe(*b),
            "refused codec does not break either stream");
    finish(b);
    require(until([&] { return a->take_stream_update(changed) && changed.codec == VideoCodec::Hevc; }) && keyframe(*a),
            "last restrictive receiver leaving restores preferred codec");
    require(f.media->capture_created == 1, "codec negotiation never recreates desktop capture");
    finish(a);

    Fixture restricted;
    restricted.config.settings.codec = VideoCodec::Hevc;
    restricted.start();
    auto first = restricted.connect(SessionMode::Desktop, kPermissionAll, {}, 0, &initial, kVideoCodecH264);
    auto later = restricted.connect();
    require(initial.codec == VideoCodec::H264 && keyframe(*first) && keyframe(*later),
            "reverse joining order also selects common decoding format");
    require(restricted.media->encoder_created == 1, "compatible later viewer reuses negotiated encoder");
    finish(first);
    require(until([&] { return later->take_stream_update(changed) && changed.codec == VideoCodec::Hevc; }),
            "first restrictive receiver leaving updates remaining connection");
    finish(later);
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    if (argc >= 2 &&
        (std::string(argv[1]) == "--portable-helper-control-server" ||
         std::string(argv[1]) ==
             "--portable-helper-negative-input-control-server" ||
         std::string(argv[1]) == "--portable-helper-worker-control-server" ||
         std::string(argv[1]) == "--portable-helper-worker-no-release-control-server"))
        return portable_helper_control_server_child(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "--elevated-handoff") return elevation_child(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "--portable-helper-pipe-client")
        return portable_helper_pipe_child(argc, argv);
    if (argc >= 2 &&
        std::string(argv[1]) == "--portable-helper-session-worker-client")
        return portable_helper_pipe_child(argc, argv);
    if (argc >= 2 &&
        std::string(argv[1]) == "--portable-helper-session-worker-auth-client")
        return portable_helper_pipe_child(argc, argv);
    if (argc >= 2 &&
        std::string(argv[1]) == "--portable-helper-session-worker-bad-ready-client")
        return portable_helper_pipe_child(argc, argv);
    if (argc >= 2 &&
        std::string(argv[1]) == "--portable-helper-session-worker-no-stop-ack-client")
        return portable_helper_pipe_child(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "--portable-helper-auth-client")
        return portable_helper_pipe_child(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "--portable-helper-supervisor-client")
        return portable_helper_pipe_child(argc, argv);
    const auto run = [](const char* name, auto test) {
        std::printf("\n%s\n", name);
        try { test(); }
        catch (const std::exception& error) { ++failures; std::printf("FAIL %s\n", error.what()); }
    };
    if (argc == 2 && std::string(argv[1]) == "--portable-helper-authorization") {
        run("portable helper crash-safe input journal",
            portable_helper_input_journal);
        run("portable helper protected durable input journal store",
            portable_helper_input_journal_file_store);
        run("portable helper worker command-line validation",
            portable_helper_worker_command_line);
        run("portable helper generation-scoped authorization", portable_helper_authorization);
        run("portable helper bounded worker revocation supervision",
            portable_helper_worker_supervision);
        run("portable helper authenticated SCM control-channel loop",
            portable_helper_service_channel_transport);
        run("portable helper parent RPC rejection revokes forwarding gate",
            portable_helper_parent_rpc_rejection_revokes_gate);
        run("portable helper service session-worker control loop",
            portable_helper_service_session_worker_control_loop);
        run("portable helper requires emergency input release",
            portable_helper_service_requires_emergency_input_release);
        run("portable helper per-session worker pipe isolation",
            portable_helper_session_worker_pipe);
        run("portable helper session-worker launch and ready handshake",
            portable_helper_session_worker_handshake);
        run("portable helper releases input after worker exit without Stop acknowledgement",
            portable_helper_supervisor_releases_input_after_worker_exit_without_ack);
        run("portable helper winlogon token launch policy",
            portable_helper_winlogon_token_fail_closed);
        run("portable helper session-worker startup fail-closed paths",
            portable_helper_session_worker_rejects_forged_ready);
        run("portable helper authenticated grant and WorkerReady exchange",
            portable_helper_authorization_pipe);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--elevation") {
        run("portable helper protocol framing and replay protection", portable_helper_protocol);
        run("portable helper crash-safe input journal",
            portable_helper_input_journal);
        run("portable helper protected durable input journal store",
            portable_helper_input_journal_file_store);
        run("portable helper worker command-line validation",
            portable_helper_worker_command_line);
        run("portable helper generation-scoped authorization", portable_helper_authorization);
        run("portable helper bounded worker revocation supervision",
            portable_helper_worker_supervision);
        run("portable helper authenticated SCM control-channel loop",
            portable_helper_service_channel_transport);
        run("portable helper parent RPC rejection revokes forwarding gate",
            portable_helper_parent_rpc_rejection_revokes_gate);
        run("portable helper service session-worker control loop",
            portable_helper_service_session_worker_control_loop);
        run("portable helper requires emergency input release",
            portable_helper_service_requires_emergency_input_release);
        run("portable helper per-session worker pipe isolation",
            portable_helper_session_worker_pipe);
        run("portable helper session-worker launch and ready handshake",
            portable_helper_session_worker_handshake);
        run("portable helper releases input after worker exit without Stop acknowledgement",
            portable_helper_supervisor_releases_input_after_worker_exit_without_ack);
        run("portable helper winlogon token launch policy",
            portable_helper_winlogon_token_fail_closed);
        run("portable helper session-worker startup fail-closed paths",
            portable_helper_session_worker_rejects_forged_ready);
        run("portable helper authenticated grant and WorkerReady exchange",
            portable_helper_authorization_pipe);
        run("portable helper authenticated named-pipe transport", portable_helper_pipe_transport);
        run("elevation permission and cancellation", elevation_permissions);
        run("authenticated local elevation handoff", elevation_handoff);
        WSACleanup(); return failures ? 1 : 0;
    }
    if (argc == 2 && (std::string(argv[1]) == "--hardware" || std::string(argv[1]) == "--hardware-auto")) {
        run("hardware shared display", [&] { hardware_shared_display(std::string(argv[1]) == "--hardware-auto"); });
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--topology") {
        run("multiple display topology changes", multiple_display_topology);
        run("multiple display isolated recovery", multiple_display_recovery_isolation);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--display-streams") {
        run("multiple display streams in one connection", multiple_displays_one_connection);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--freshness") {
        run("sender queue age and observable recovery", sender_queue_freshness);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--cursors") {
        run("multiple display cursor snapshots", multiple_display_cursors);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--camera") {
        run("shared camera service and codec negotiation", shared_camera_service);
        run("camera 4:2:0 fallback on a full-color desktop", shared_camera_full_color);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--connections") {
        run("operator connection management", operator_connection_management);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--service-control") {
        run("authenticated service connection management", service_connection_management);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--power") {
        run("session power request ownership", session_power_requests);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--clipboard-direction") {
        run("clipboard directions and independent file permissions", clipboard_directions);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--revocation") {
        run("running services stop when their permission is revoked",
            running_services_revocation);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--keyframe-rate") {
        run("viewer keyframe request rate limiting", viewer_keyframe_requests_are_bounded);
        WSACleanup();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--backpressure") {
        run("video application backpressure and recovery", video_backpressure);
        run("sender queue age and observable recovery", sender_queue_freshness);
        WSACleanup();
        return failures ? 1 : 0;
    }
    run("elevation permission and cancellation", elevation_permissions);
    run("authenticated local elevation handoff", elevation_handoff);
    run("clipboard directions and independent file permissions", clipboard_directions);
    run("running services stop when their permission is revoked",
        running_services_revocation);
    run("session power request ownership", session_power_requests);
    run("static desktop, follower requests and audio", static_desktop_and_followers);
    run("global policy", [] { global_policy(false); });
    run("global policy with frozen UI permissions", [] { global_policy(true); });
    run("policy changed during approval", pending_approval);
    run("publisher-only audio", publisher_only_audio);
    run("audio enabled after admission", late_audio_source);
    run("desktop changed during admission", desktop_switch_during_admission);
    run("viewer keyframe request rate limiting", viewer_keyframe_requests_are_bounded);
    run("independent display subscriptions and resource lifetime", independent_displays);
    run("multiple display streams in one connection", multiple_displays_one_connection);
    run("multiple display rollback and one vote per connection", multiple_display_failures_and_votes);
    run("multiple display backpressure and fair recovery", multiple_displays_backpressure);
    run("multiple display independent cursors", multiple_display_cursors);
    run("multiple display topology changes", multiple_display_topology);
    run("multiple display isolated recovery", multiple_display_recovery_isolation);
    run("failed selection preserves subscriptions", refused_display_keeps_stream);
    run("concurrent admission and display renumbering", concurrent_display_admission);
    run("shared codec negotiation", negotiated_shared_codec);
    run("decoder capabilities follow display subscriptions", display_codec_intersection);
    run("multiuser quality across displays", multiuser_quality);
    run("global codec preference voting", global_codec_preferences);
    run("runtime hardware to software recovery", runtime_software_recovery);
    run("shared camera service and codec negotiation", shared_camera_service);
    run("camera 4:2:0 fallback on a full-color desktop", shared_camera_full_color);
    run("operator connection management", operator_connection_management);
    run("authenticated service connection management", service_connection_management);
    run("host application latency probes", application_latency);
    run("video application backpressure and recovery", video_backpressure);
    run("sender queue age and observable recovery", sender_queue_freshness);
    run("slow viewer independent display", slow_viewer_independent_display);
    WSACleanup();
    std::printf("host parity: %d failed scenario(s)\n", failures);
    return failures ? 1 : 0;
}
