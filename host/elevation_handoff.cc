#define NOMINMAX
#include "elevation_handoff.h"
#include "platform.h"
#include "chast/security.h"
#include "chast/user_context.h"
#include <sddl.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

namespace chast::host {
namespace {
constexpr DWORD kMaxMessage = 1024 * 1024;
constexpr DWORD kWait = 180000;
constexpr uint64_t kMagic = 0x3143415554534143ull;
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    ~Handle() { if (value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value != nullptr; }
};
void need(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
uint64_t created(HANDLE process) {
    FILETIME birth{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &birth, &exit, &kernel, &user)) return 0;
    return uint64_t(birth.dwHighDateTime) << 32 | birth.dwLowDateTime;
}
DWORD session_id() {
    DWORD id = 0xffffffff;
    ProcessIdToSessionId(GetCurrentProcessId(), &id);
    return id;
}
bool same_image(HANDLE process) {
    wchar_t path[32768]{};
    DWORD count = static_cast<DWORD>(std::size(path));
    return QueryFullProcessImageNameW(process, 0, path, &count) &&
           _wcsicmp(path, executable_path().c_str()) == 0;
}
std::wstring random_id() {
    uint8_t bytes[16]{};
    need(BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
         "cannot generate the elevation endpoint");
    std::wstring result;
    for (auto c : bytes) { result += L"0123456789abcdef"[c >> 4]; result += L"0123456789abcdef"[c & 15]; }
    return result;
}
std::wstring pipe_name(const std::wstring& id) {
    need(id.size() == 32 && id.find_first_not_of(L"0123456789abcdef") == std::wstring::npos,
         "invalid elevation endpoint");
    return L"\\\\.\\pipe\\chast-elevation-" + id;
}
std::wstring user_sid(HANDLE token) {
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<uint8_t> buffer(bytes);
    need(bytes && GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes), "cannot read the user identity");
    LPWSTR value = nullptr;
    need(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &value), "cannot encode the user identity");
    std::wstring sid(value); LocalFree(value); return sid;
}
// Enable debug access only while opening our authenticated parent. This is
// necessary when UAC was approved with a different administrator's account.
struct DebugAccess {
    Handle token;
    TOKEN_PRIVILEGES old{};
    bool changed = false;
    DebugAccess() {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token.value)) return;
        TOKEN_PRIVILEGES wanted{}; wanted.PrivilegeCount = 1;
        if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &wanted.Privileges[0].Luid)) return;
        wanted.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        DWORD bytes = sizeof(old);
        changed = AdjustTokenPrivileges(token.value, FALSE, &wanted, sizeof(old), &old, &bytes) != FALSE;
    }
    ~DebugAccess() { if (changed && !AdjustTokenPrivileges(token.value, FALSE, &old, 0, nullptr, nullptr)) std::terminate(); }
};
struct Wire {
    std::vector<uint8_t> bytes;
    size_t at = 0;
    void put(uint64_t n) { for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(n >> (i * 8))); }
    uint64_t get() {
        need(at <= bytes.size() && bytes.size() - at >= 8, "truncated elevation message");
        uint64_t n = 0; for (int i = 0; i < 8; ++i) n |= uint64_t(bytes[at++]) << (i * 8); return n;
    }
    void text(const std::string& value) { put(value.size()); bytes.insert(bytes.end(), value.begin(), value.end()); }
    std::string text(size_t limit = kMaxMessage) {
        const auto count = get(); need(count <= limit && count <= bytes.size() - at, "invalid elevation string");
        std::string s(reinterpret_cast<const char*>(bytes.data() + at), static_cast<size_t>(count));
        at += static_cast<size_t>(count); need(s.find('\0') == std::string::npos, "invalid elevation string"); return s;
    }
    void end() { need(at == bytes.size(), "trailing elevation data"); }
};
bool complete(HANDLE pipe, OVERLAPPED& ov, HANDLE stop, DWORD timeout, DWORD& count, HANDLE peer = nullptr) {
    HANDLE handles[]{stop, ov.hEvent, peer};
    if (WaitForMultipleObjects(peer ? 3 : 2, handles, FALSE, timeout) != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &count, TRUE); return false;
    }
    return GetOverlappedResult(pipe, &ov, &count, FALSE) != FALSE;
}
void transfer(HANDLE pipe, HANDLE stop, Wire& wire, bool write, DWORD timeout = kWait) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); need(bool(event), "cannot create an IPC event");
    OVERLAPPED ov{}; ov.hEvent = event.value; DWORD count = 0;
    if (!write) { wire.bytes.resize(kMaxMessage); wire.at = 0; }
    need(!wire.bytes.empty() && wire.bytes.size() <= kMaxMessage, "elevation message is too large");
    const BOOL ok = write ? WriteFile(pipe, wire.bytes.data(), static_cast<DWORD>(wire.bytes.size()), &count, &ov)
                         : ReadFile(pipe, wire.bytes.data(), static_cast<DWORD>(wire.bytes.size()), &count, &ov);
    need(ok || (GetLastError() == ERROR_IO_PENDING && complete(pipe, ov, stop, timeout, count)),
         "elevation handoff was cancelled, timed out, or disconnected");
    if (write) need(count == wire.bytes.size(), "incomplete elevation write");
    else { wire.bytes.resize(count); need(count != 0, "empty elevation message"); }
}
void send_code(HANDLE pipe, HANDLE stop, uint64_t code) {
    Wire w; w.put(kMagic); w.put(code); transfer(pipe, stop, w, true);
}
void expect(HANDLE pipe, HANDLE stop, uint64_t code) {
    Wire w; transfer(pipe, stop, w, false);
    need(w.get() == kMagic && w.get() == code, "unexpected elevation reply"); w.end();
}
HANDLE launch_uac(const std::wstring& args, DWORD& error) {
    const auto path = executable_path();
    const auto dir = std::filesystem::path(path).parent_path().wstring();
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas"; info.lpFile = path.c_str(); info.lpParameters = args.c_str();
    info.lpDirectory = dir.c_str(); info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) { error = GetLastError(); return nullptr; }
    return info.hProcess;
}
bool elevated_client(HANDLE pipe) {
    if (!ImpersonateNamedPipeClient(pipe)) return false;
    Handle token; TOKEN_ELEVATION info{}; DWORD bytes = 0;
    const bool ok = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token.value) &&
        GetTokenInformation(token.value, TokenElevation, &info, sizeof(info), &bytes) && info.TokenIsElevated;
    if (!RevertToSelf()) std::terminate();
    return ok;
}
void duplicate(HANDLE source, uint64_t value, DWORD access, HANDLE& target) {
    need(value != 0 && value <= UINTPTR_MAX && DuplicateHandle(source, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)),
        GetCurrentProcess(), &target, access, FALSE, 0), "cannot preserve an original-user handle");
}
}

struct ElevationHandoff::Impl {
    mutable std::mutex mutex;
    ElevationState state{0, ElevationCode::Pending, {}};
    Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle supplied{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    bool committed = false, finished = false, has_snapshot = false;
    std::chrono::steady_clock::time_point committed_at{};
    Wire snapshot;
    std::string fingerprint;

    void result(ElevationCode code, const std::string& reason = {}) {
        std::lock_guard lock(mutex); state.code = code; state.reason = reason;
    }
    void run(const Launcher& launch, bool require_elevated) {
        Handle child;
        try {
            need(bool(stop) && bool(supplied), "cannot create elevation events");
            Handle token, original;
            need(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token.value), "cannot open the original user token");
            need(DuplicateTokenEx(token.value, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE, nullptr,
                                 SecurityImpersonation, TokenImpersonation, &original.value), "cannot preserve the original user token");
            const auto id = random_id(); const auto name = pipe_name(id);
            // Clients cannot create another instance or replace this endpoint's ACL.
            const auto sddl = L"D:P(A;;GA;;;SY)(A;;0x12018b;;;BA)(A;;0x12018b;;;" + user_sid(token.value) + L")";
            PSECURITY_DESCRIPTOR sd = nullptr;
            need(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr), "cannot secure elevation IPC");
            SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
            Handle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, kMaxMessage, kMaxMessage, 2000, &sa));
            LocalFree(sd); need(bool(pipe), "cannot create elevation IPC");
            const auto birth = created(GetCurrentProcess()); need(birth != 0, "cannot identify the original process");
            need(WaitForSingleObject(stop.value, 0) == WAIT_TIMEOUT, "elevation was cancelled before launch");
            DWORD error = ERROR_SUCCESS;
            child.value = launch(L"--elevated-handoff " + id + L" " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(birth), error);
            if (!child) {
                result(error == ERROR_CANCELLED ? ElevationCode::Cancelled : ElevationCode::Failed,
                       error == ERROR_CANCELLED ? "the local UAC prompt was cancelled" : "cannot launch UAC (" + std::to_string(error) + ")");
            } else {
                Handle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr)); need(bool(connected), "cannot create elevation connection event");
                OVERLAPPED ov{}; ov.hEvent = connected.value; DWORD count = 0;
                const BOOL ok = ConnectNamedPipe(pipe.value, &ov);
                const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
                need(ok || code == ERROR_PIPE_CONNECTED || (code == ERROR_IO_PENDING && complete(pipe.value, ov, stop.value, kWait, count, child.value)), "elevated process did not connect");
                ULONG pid = 0, session = 0;
                need(GetNamedPipeClientProcessId(pipe.value, &pid) && pid == GetProcessId(child.value) &&
                    GetNamedPipeClientSessionId(pipe.value, &session) && session == session_id() && same_image(child.value) &&
                    WaitForSingleObject(child.value, 0) == WAIT_TIMEOUT, "elevation client identity mismatch");
                expect(pipe.value, stop.value, 1);
                need(!require_elevated || elevated_client(pipe.value), "the new process is not elevated");
                Wire handles; handles.put(kMagic); handles.put(2);
                handles.put(reinterpret_cast<uintptr_t>(original.value)); handles.put(reinterpret_cast<uintptr_t>(stop.value));
                transfer(pipe.value, stop.value, handles, true);
                expect(pipe.value, stop.value, 3);
                result(ElevationCode::Restarting);
                HANDLE waiting[]{stop.value, supplied.value};
                need(WaitForMultipleObjects(2, waiting, FALSE, kWait) == WAIT_OBJECT_0 + 1, "elevation transfer was not committed");
                Wire payload;
                { std::lock_guard lock(mutex); payload = std::move(snapshot); }
                transfer(pipe.value, stop.value, payload, true);
                Wire ready; transfer(pipe.value, stop.value, ready, false, 30000);
                need(ready.get() == kMagic && ready.get() == 5 && ready.text(128) == fingerprint, "elevated host did not retain the original identity"); ready.end();
                send_code(pipe.value, stop.value, 6);
                expect(pipe.value, stop.value, 7);
                result(ElevationCode::Elevated);
            }
        } catch (const std::exception& e) {
            const bool cancelled = stop && WaitForSingleObject(stop.value, 0) == WAIT_OBJECT_0;
            if (stop) SetEvent(stop.value);
            // The child observes the event/pipe before activation and during its
            // startup. Do not ask the old UI to rebind while that child is alive.
            if (child && WaitForSingleObject(child.value, 30000) == WAIT_TIMEOUT) {
                // Only our authenticated, just-launched child is eligible for
                // cleanup. Never resume the old listener while it is alive.
                TerminateProcess(child.value, ERROR_CANCELLED);
                WaitForSingleObject(child.value, INFINITE);
            }
            result(cancelled ? ElevationCode::Cancelled : ElevationCode::Failed, e.what());
        }
        std::lock_guard lock(mutex); finished = true;
    }
};

ElevationHandoff::ElevationHandoff() : impl_(std::make_unique<Impl>()) {}
ElevationHandoff::~ElevationHandoff() = default;
std::shared_ptr<ElevationHandoff> ElevationHandoff::start(Launcher launch, bool require_elevated) {
    auto task = std::shared_ptr<ElevationHandoff>(new ElevationHandoff);
    std::thread([task, launch = launch ? std::move(launch) : Launcher(launch_uac), require_elevated] {
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        task->impl_->run(launch, require_elevated);
        if (SUCCEEDED(com)) CoUninitialize();
    }).detach();
    return task;
}
ElevationState ElevationHandoff::poll() { std::lock_guard lock(impl_->mutex); return impl_->state; }
void ElevationHandoff::commit() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state.code == ElevationCode::Restarting && WaitForSingleObject(impl_->stop.value, 0) == WAIT_TIMEOUT)
        { impl_->committed = true; impl_->committed_at = std::chrono::steady_clock::now(); }
}
void ElevationHandoff::cancel() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->committed && impl_->stop) SetEvent(impl_->stop.value);
}
void ElevationHandoff::shutdown() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state.code != ElevationCode::Elevated && impl_->stop) SetEvent(impl_->stop.value);
}
bool ElevationHandoff::committed() const {
    std::lock_guard lock(impl_->mutex);
    // Give the queued restarting reply a chance to reach the viewer before
    // the UI closes its transport. This is bounded and does not block media.
    return impl_->committed && std::chrono::steady_clock::now() - impl_->committed_at >= std::chrono::milliseconds(350);
}
bool ElevationHandoff::finished() const { std::lock_guard lock(impl_->mutex); return impl_->finished; }
bool ElevationHandoff::supply(const ElevationSnapshot& snapshot, std::string* error) {
    std::string settings;
    if (!serialize_host_settings(snapshot.config_path, snapshot.settings, settings, error)) return false;
    Wire w; w.put(kMagic); w.put(4); w.text(snapshot.config_path); w.text(settings); w.text(snapshot.fingerprint);
    w.put(snapshot.portable); w.put(snapshot.hidden); w.put(reinterpret_cast<uintptr_t>(snapshot.instance_mutex));
    if (w.bytes.size() > kMaxMessage) { *error = "host settings exceed the handoff limit"; return false; }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->committed || impl_->has_snapshot || impl_->finished) { *error = "elevation is no longer pending"; return false; }
    impl_->has_snapshot = true; impl_->snapshot = std::move(w); impl_->fingerprint = snapshot.fingerprint;
    SetEvent(impl_->supplied.value); return true;
}

ElevationState ElevationController::state() {
    std::lock_guard lock(mutex_);
    if (stopping_) return {0, ElevationCode::Unsupported, "the host is shutting down"};
    if (process_is_elevated()) return {0, ElevationCode::Elevated, {}};
    if (current_ && !current_->finished()) return {0, ElevationCode::Busy, "a local UAC request is already pending"};
    return {0, ElevationCode::Available, {}};
}
std::shared_ptr<IElevationTask> ElevationController::request() {
    std::lock_guard lock(mutex_);
    if (stopping_ || process_is_elevated() || (current_ && !current_->finished())) return {};
    current_ = ElevationHandoff::start(); return current_;
}
std::shared_ptr<ElevationHandoff> ElevationController::current() { std::lock_guard lock(mutex_); return current_; }
void ElevationController::shutdown() {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    if (current_) current_->shutdown();
}

struct ElevatedChild::Impl {
    Handle parent, pipe, stop, local_stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, instance_mutex;
};
ElevatedChild::ElevatedChild() : impl_(std::make_unique<Impl>()) {}
ElevatedChild::~ElevatedChild() = default;
std::unique_ptr<ElevatedChild> ElevatedChild::receive(const std::wstring& id, DWORD pid,
    uint64_t birth, std::string* error, bool require_elevated) {
    try {
        need(!require_elevated || process_is_elevated(), "elevation child requires local UAC approval");
        auto child = std::unique_ptr<ElevatedChild>(new ElevatedChild);
        auto& p = *child->impl_;
        { DebugAccess access; p.parent.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid); }
        need(bool(p.parent) && birth && created(p.parent.value) == birth && same_image(p.parent.value) &&
             WaitForSingleObject(p.parent.value, 0) == WAIT_TIMEOUT, "elevation parent identity mismatch");
        DWORD session = 0;
        need(ProcessIdToSessionId(pid, &session) && session == session_id(), "elevation parent is in a different Windows session");
        const auto name = pipe_name(id);
        p.pipe.value = CreateFileW(name.c_str(), GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (p.pipe.value == INVALID_HANDLE_VALUE) p.pipe.value = nullptr;
        need(bool(p.pipe) && bool(p.local_stop), "cannot open elevation IPC");
        ULONG server_pid = 0, server_session = 0;
        need(GetNamedPipeServerProcessId(p.pipe.value, &server_pid) && server_pid == pid &&
            GetNamedPipeServerSessionId(p.pipe.value, &server_session) && server_session == session_id(), "elevation pipe server mismatch");
        DWORD mode = PIPE_READMODE_MESSAGE;
        need(SetNamedPipeHandleState(p.pipe.value, &mode, nullptr, nullptr), "cannot set elevation IPC mode");
        send_code(p.pipe.value, p.local_stop.value, 1);
        Wire handles; transfer(p.pipe.value, p.local_stop.value, handles, false);
        need(handles.get() == kMagic && handles.get() == 2, "invalid elevation handles");
        Handle original;
        duplicate(p.parent.value, handles.get(), TOKEN_QUERY | TOKEN_IMPERSONATE, original.value);
        duplicate(p.parent.value, handles.get(), SYNCHRONIZE, p.stop.value); handles.end();
        child->identity_user_token = std::shared_ptr<void>(original.value, [](void* h) { CloseHandle(h); }); original.value = nullptr;
        { ScopedUserContext user(child->identity_user_token.get()); need(user.ok(), "cannot restore the original user identity"); }
        send_code(p.pipe.value, p.stop.value, 3);
        Wire w; transfer(p.pipe.value, p.stop.value, w, false);
        need(w.get() == kMagic && w.get() == 4, "invalid elevation settings");
        auto& s = child->snapshot;
        s.config_path = w.text(32768); const auto settings_text = w.text(); s.fingerprint = w.text(128);
        const auto portable = w.get(), hidden = w.get(), mutex = w.get(); w.end();
        need(portable <= 1 && hidden <= 1 && !s.fingerprint.empty() &&
             std::filesystem::path(widen(s.config_path)).is_absolute(), "invalid elevation settings metadata");
        s.portable = portable != 0; s.hidden = hidden != 0;
        if (mutex) { duplicate(p.parent.value, mutex, SYNCHRONIZE, p.instance_mutex.value); s.instance_mutex = p.instance_mutex.value; }
        const auto parsed = parse_host_settings(settings_text, s.config_path);
        need(parsed.warning.empty(), "cannot restore the host settings snapshot"); s.settings = parsed.settings;
        if (!s.settings.totp_secret.empty()) {
            ScopedUserContext user(child->identity_user_token.get());
            std::string plain, reason; std::vector<uint8_t> key;
            need(user.ok() && (unprotect_totp_secret(s.settings.totp_secret, SecretScope::CurrentUser, plain, &reason) ||
                parse_totp_secret(s.settings.totp_secret, key)), "cannot preserve the original two-factor secret");
            if (!plain.empty()) SecureZeroMemory(plain.data(), plain.size());
        }
        need(!child->cancelled(), "elevation was cancelled before startup");
        return child;
    } catch (const std::exception& e) { *error = e.what(); return {}; }
}
bool ElevatedChild::announce_ready(const std::string& fingerprint, std::string* error) {
    try {
        need(fingerprint == snapshot.fingerprint && !cancelled(), "elevated host identity changed or takeover was cancelled");
        Wire w; w.put(kMagic); w.put(5); w.text(fingerprint);
        transfer(impl_->pipe.value, impl_->stop.value, w, true, 30000);
        expect(impl_->pipe.value, impl_->stop.value, 6);
        send_code(impl_->pipe.value, impl_->stop.value, 7);
        return true;
    } catch (const std::exception& e) { *error = e.what(); return false; }
}
bool ElevatedChild::cancelled() const {
    return !impl_->stop || WaitForSingleObject(impl_->stop.value, 0) == WAIT_OBJECT_0;
}
} // namespace chast::host
