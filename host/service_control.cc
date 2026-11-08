#define NOMINMAX
#include "service_control.h"
#include <windows.h>
#include <sddl.h>
#include <wtsapi32.h>
#include <bcrypt.h>
#include <aclapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace chast::host {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr DWORD kMaximumMessage = 512 * 1024;
constexpr uint32_t kMagic = 0x314d4343;
constexpr uint32_t kControlVersion = 2;
enum class Command : uint32_t { Poll, Permissions, Chat, File, Disconnect, Approval };
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
DWORD current_session() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return session;
}
std::wstring pipe_name() { return L"\\\\.\\pipe\\chast-host-control-v1-" + std::to_wstring(current_session()); }
std::vector<uint8_t> token_data(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0;
    GetTokenInformation(token, kind, nullptr, 0, &size);
    std::vector<uint8_t> data(size);
    if (!size || !GetTokenInformation(token, kind, data.data(), size, &size)) data.clear();
    return data;
}
bool is_system(HANDLE token) {
    const auto data = token_data(token, TokenUser);
    return !data.empty() && IsWellKnownSid(reinterpret_cast<const TOKEN_USER*>(data.data())->User.Sid,
                                         WinLocalSystemSid);
}
std::wstring logon_sid(HANDLE token) {
    const auto data = token_data(token, TokenGroups);
    if (data.empty()) return {};
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(data.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID) continue;
        LPWSTR text = nullptr;
        if (!ConvertSidToStringSidW(groups->Groups[i].Sid, &text)) return {};
        std::wstring sid(text);
        LocalFree(text);
        return sid;
    }
    return {};
}
std::string new_epoch() {
    uint8_t bytes[16]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    std::string text;
    for (auto byte : bytes) { text += "0123456789abcdef"[byte >> 4]; text += "0123456789abcdef"[byte & 15]; }
    return text;
}
struct Writer {
    std::vector<uint8_t> bytes;
    void number(uint64_t value) { for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i))); }
    void text(const std::string& value) { number(value.size()); bytes.insert(bytes.end(), value.begin(), value.end()); }
};
struct Reader {
    const std::vector<uint8_t>& bytes;
    size_t at = 0;
    bool ok = true;
    uint64_t number() {
        if (bytes.size() - at < 8) { ok = false; return 0; }
        uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[at++]) << (8 * i);
        return value;
    }
    std::string text(size_t limit = 4096) {
        const auto size = number();
        if (!ok || size > limit || size > bytes.size() - at) { ok = false; return {}; }
        std::string value(reinterpret_cast<const char*>(bytes.data() + at), static_cast<size_t>(size));
        at += static_cast<size_t>(size);
        if (value.find('\0') != std::string::npos) ok = false;
        return value;
    }
    bool done() const { return ok && at == bytes.size(); }
};
// Every overlapped operation is completed or cancelled before its stack-owned
// OVERLAPPED/event/buffer can be destroyed. No FlushFileBuffers wait on peers.
bool complete(HANDLE pipe, OVERLAPPED& ov, HANDLE stop, DWORD timeout, DWORD& transferred) {
    HANDLE events[]{stop, ov.hEvent};
    const DWORD waited = WaitForMultipleObjects(2, events, FALSE, timeout);
    if (waited != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &transferred, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, &transferred, FALSE) != FALSE;
}
bool message_io(HANDLE pipe, HANDLE stop, std::vector<uint8_t>& bytes, bool write) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    OVERLAPPED ov{}; ov.hEvent = event.value;
    DWORD transferred = 0;
    if (!write) bytes.resize(kMaximumMessage);
    if (bytes.size() > kMaximumMessage) return false;
    const BOOL done = write ? WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &transferred, &ov)
                            : ReadFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &transferred, &ov);
    if (!done && (GetLastError() != ERROR_IO_PENDING || !complete(pipe, ov, stop, 2000, transferred))) return false;
    if (write) return transferred == bytes.size();
    bytes.resize(transferred);
    return transferred != 0;
}
std::shared_ptr<void> client_token(HANDLE pipe, const std::wstring& expected_sid) {
    ULONG session = 0;
    if (!GetNamedPipeClientSessionId(pipe, &session) || session != current_session() ||
        !ImpersonateNamedPipeClient(pipe)) return {};
    Handle token;
    HANDLE copy = nullptr;
    const bool valid = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, &token.value) &&
        logon_sid(token.value) == expected_sid &&
        DuplicateTokenEx(token.value, TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation,
                         TokenImpersonation, &copy);
    if (!RevertToSelf()) std::terminate();
    if (!valid) { if (copy) CloseHandle(copy); return {}; }
    return std::shared_ptr<void>(copy, [](void* h) { CloseHandle(h); });
}
bool verified_server(HANDLE pipe, DWORD expected_test_pid) {
    ULONG pid = 0, session = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid) || !GetNamedPipeServerSessionId(pipe, &session) ||
        session != current_session()) return false;
    if (expected_test_pid) return pid == expected_test_pid;
    // A standard user cannot query a SYSTEM process token. Verify the kernel
    // pipe object's SYSTEM owner and that no other SID can create a server
    // instance or replace its ACL/owner, before sending impersonatable data.
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    bool valid = owner && IsWellKnownSid(owner, WinLocalSystemSid) && acl;
    for (DWORD i = 0; valid && i < acl->AceCount; ++i) {
        void* entry = nullptr;
        if (!GetAce(acl, i, &entry)) { valid = false; break; }
        const auto* header = static_cast<const ACE_HEADER*>(entry);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { valid = false; break; }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
        if (!IsWellKnownSid(const_cast<DWORD*>(&ace->SidStart), WinLocalSystemSid) &&
            (ace->Mask & ~DWORD(0x12018b))) valid = false;
    }
    LocalFree(descriptor);
    return valid;
}
struct Request {
    Command command = Command::Poll;
    std::string epoch, text;
    uint64_t id = 0, cursor = 0;
    PermissionMask permissions = 0;
    bool allow = false;
};
Writer encode(const Request& request) {
    Writer out;
    out.number(kMagic); out.number(kControlVersion); out.number(static_cast<uint32_t>(request.command));
    out.text(request.epoch); out.number(request.id); out.number(request.cursor);
    out.number(request.permissions); out.number(request.allow); out.text(request.text);
    return out;
}
bool decode(const std::vector<uint8_t>& bytes, Request& request) {
    Reader in{bytes};
    if (in.number() != kMagic || in.number() != kControlVersion) return false;
    const auto command = in.number();
    if (command > static_cast<uint32_t>(Command::Approval)) return false;
    request.command = static_cast<Command>(command);
    request.epoch = in.text(32); request.id = in.number(); request.cursor = in.number();
    const auto mask = in.number(); const auto allow = in.number();
    if (mask & ~uint64_t(kPermissionAll) || allow > 1) return false;
    request.permissions = static_cast<PermissionMask>(mask); request.allow = allow != 0;
    request.text = in.text(32768);
    return in.done();
}
}

struct ServiceControlServer::Impl {
    HostServer* host = nullptr;
    std::function<ServiceRegistrationInfo()> registration;
    std::string epoch;
    std::wstring sid;
    Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, pipe;
    std::thread thread;
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::condition_variable changed;
    uint64_t next_approval = 0, next_event = 0;
    Clock::time_point last_poll{};
    struct Pending { ServiceApproval view; bool answered = false; ClientApproval answer; };
    std::map<uint64_t, std::shared_ptr<Pending>> pending;
    struct Event { uint64_t sequence, id; std::string text; };
    std::deque<Event> events;

    bool start(HostServer& server, const std::wstring& name, bool test, std::string* error,
               std::function<ServiceRegistrationInfo()> registration_info) {
        if (running || thread.joinable()) return false;
        Handle token;
        if (test) OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value);
        else {
            Handle process_token;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token.value) || !is_system(process_token.value)) {
                if (error) *error = "service management requires the SYSTEM worker";
                return false;
            }
            WTSQueryUserToken(current_session(), &token.value);
        }
        sid = token ? logon_sid(token.value) : std::wstring();
        epoch = new_epoch();
        if (sid.empty() || epoch.empty() || !stop) {
            if (error) *error = "cannot identify the interactive logon or create the control endpoint";
            return false;
        }
        // FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES. Excludes
        // FILE_CREATE_PIPE_INSTANCE (also included in FILE_GENERIC_WRITE).
        const auto sddl = std::wstring(test ? L"" : L"O:SY") + L"D:P(A;;GA;;;SY)(A;;0x12018b;;;" + sid + L")";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        pipe.value = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, kMaximumMessage, kMaximumMessage, 2000, &security);
        LocalFree(descriptor);
        if (!pipe) { if (error) *error = "cannot create service control pipe (" + std::to_string(GetLastError()) + ")"; return false; }
        host = &server;
        registration = std::move(registration_info);
        { std::lock_guard lock(mutex); last_poll = {}; }
        ResetEvent(stop.value); running = true;
        thread = std::thread([this] { loop(); });
        return true;
    }
    Writer reply(const Request& request, const std::shared_ptr<void>& token) {
        bool success = true;
        std::string error;
        if (request.command != Command::Poll && request.epoch != epoch) {
            success = false; error = "The service restarted; this operation was not applied.";
        } else switch (request.command) {
            case Command::Poll: { std::lock_guard lock(mutex); last_poll = Clock::now(); break; }
            case Command::Permissions: success = host->set_permissions(request.id, request.permissions); break;
            case Command::Chat: success = host->send_chat(request.id, request.text); break;
            case Command::File: success = host->start_file_download(request.id, request.text, &error, token); break;
            case Command::Disconnect: success = host->disconnect_viewer(request.id); break;
            case Command::Approval: {
                std::lock_guard lock(mutex);
                const auto found = pending.find(request.id);
                success = found != pending.end() && !found->second->answered;
                if (success) {
                    found->second->answer = {request.allow, request.permissions & found->second->view.permissions};
                    found->second->answered = true; changed.notify_all();
                }
                break;
            }
        }
        if (!success && error.empty()) error = "The selected connection or request has ended, or the operation was refused.";
        Writer out;
        out.number(kMagic); out.number(kControlVersion); out.number(success); out.text(epoch); out.text(error);
        const auto info = host->status();
        const auto registered = registration ? registration() : ServiceRegistrationInfo{};
        out.text(registered.device_id); out.text(info.fingerprint); out.number(static_cast<uint32_t>(info.auth_mode));
        out.number(info.password_configured); out.number(info.two_factor_enabled);
        out.number(registered.enabled);
        const auto viewers = host->viewers();
        out.number(viewers.size());
        for (const auto& viewer : viewers) {
            out.number(viewer.id); out.text(viewer.peer_address); out.number(static_cast<uint32_t>(viewer.mode));
            out.number(viewer.permissions); out.number(viewer.approved_permissions);
            out.number(viewer.controls_input); out.number(viewer.disconnecting);
        }
        std::lock_guard lock(mutex);
        size_t count = 0;
        for (const auto& [id, item] : pending) if (!item->answered) ++count;
        out.number(count);
        for (const auto& [id, item] : pending) if (!item->answered) {
            out.number(id); out.text(item->view.peer_address); out.text(item->view.device_name);
            out.number(static_cast<uint32_t>(item->view.mode)); out.number(item->view.permissions); out.number(item->view.device_proof);
        }
        const uint64_t cursor = request.epoch == epoch ? request.cursor : 0;
        count = 0;
        for (const auto& event : events) if (event.sequence > cursor) ++count;
        out.number(next_event); out.number(count);
        for (const auto& event : events) if (event.sequence > cursor) {
            out.number(event.id); out.text(event.text);
        }
        return out;
    }
    void loop() {
        while (running) {
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event) break;
            OVERLAPPED ov{}; ov.hEvent = event.value;
            const BOOL connected = ConnectNamedPipe(pipe.value, &ov);
            const DWORD code = connected ? ERROR_SUCCESS : GetLastError();
            DWORD transferred = 0;
            const bool accepted = connected || code == ERROR_PIPE_CONNECTED ||
                (code == ERROR_IO_PENDING && complete(pipe.value, ov, stop.value, INFINITE, transferred));
            if (!accepted) { if (!running) break; DisconnectNamedPipe(pipe.value); continue; }
            std::vector<uint8_t> bytes;
            if (message_io(pipe.value, stop.value, bytes, false)) {
                auto token = client_token(pipe.value, sid);
                Request request;
                if (token && decode(bytes, request)) {
                    auto response = reply(request, token);
                    if (message_io(pipe.value, stop.value, response.bytes, true)) {
                        // DisconnectNamedPipe discards unread buffered bytes.
                        // The client acknowledges after reading the full reply.
                        std::vector<uint8_t> acknowledgement;
                        message_io(pipe.value, stop.value, acknowledgement, false);
                    }
                }
            }
            DisconnectNamedPipe(pipe.value);
        }
    }
};
ServiceControlServer::ServiceControlServer() : impl_(std::make_unique<Impl>()) {}
ServiceControlServer::~ServiceControlServer() { stop(); }
bool ServiceControlServer::start(HostServer& host, std::string* error, std::function<ServiceRegistrationInfo()> registration) {
    return impl_->start(host, pipe_name(), false, error, std::move(registration));
}
bool ServiceControlServer::start_for_test(HostServer& host, const std::wstring& pipe, std::string* error,
                                         std::function<ServiceRegistrationInfo()> registration) {
    return impl_->start(host, pipe, true, error, std::move(registration));
}
void ServiceControlServer::stop() {
    impl_->running = false;
    SetEvent(impl_->stop.value); impl_->changed.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->pipe) { CloseHandle(impl_->pipe.value); impl_->pipe.value = nullptr; }
}
ClientApproval ServiceControlServer::approve(const ClientRequest& request) {
    auto item = std::make_shared<Impl::Pending>();
    std::unique_lock lock(impl_->mutex);
    // No GUI or a lost GUI never turns Click/Both into password-only access.
    if (!impl_->running || Clock::now() - impl_->last_poll > 3s || impl_->pending.size() >= 8) return {};
    const auto id = ++impl_->next_approval;
    item->view = {id, request.peer_address, request.hello.device_name, request.mode, request.permissions, request.device_proof};
    impl_->pending[id] = item;
    const auto deadline = Clock::now() + 60s;
    while (impl_->running && !item->answered && Clock::now() < deadline && Clock::now() - impl_->last_poll < 3s) {
        lock.unlock();
        const bool alive = !request.peer_alive || request.peer_alive();
        lock.lock();
        if (!alive) break;
        impl_->changed.wait_for(lock, 100ms);
    }
    const auto answer = impl_->running && item->answered ? item->answer : ClientApproval{};
    impl_->pending.erase(id);
    return answer;
}
void ServiceControlServer::receive_chat(uint64_t id, const std::string& text) {
    if (!valid_chat_text(text)) return;
    std::lock_guard lock(impl_->mutex);
    impl_->events.push_back({++impl_->next_event, id, text});
    if (impl_->events.size() > 64) impl_->events.pop_front();
}

struct ServiceControlClient::Impl {
    Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::wstring pipe;
    DWORD expected_test_pid = 0;
    std::thread thread;
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool running = false, online = false;
    std::string epoch, error = "Waiting for the service worker";
    ServiceHostInfo information;
    uint64_t cursor = 0, next_id = 0;
    struct Target { std::string epoch; uint64_t remote; bool approval; };
    std::map<uint64_t, Target> targets;
    std::vector<HostViewer> viewers;
    std::vector<ServiceApproval> approvals;
    std::deque<std::pair<uint64_t, Request>> commands;
    std::vector<ServiceNotice> notices;
    uint64_t map_id(uint64_t remote, bool approval, const std::string& instance) {
        for (const auto& [id, target] : targets)
            if (target.remote == remote && target.approval == approval && target.epoch == instance) return id;
        const auto id = ++next_id;
        targets[id] = {instance, remote, approval};
        return id;
    }
    bool enqueue(uint64_t id, Command command, std::string text = {}, PermissionMask mask = 0, bool allow = false) {
        std::lock_guard lock(mutex);
        const auto found = targets.find(id);
        const bool approval = command == Command::Approval;
        const bool live = approval ? std::any_of(approvals.begin(), approvals.end(), [id](const auto& p) { return p.id == id; }) :
            std::any_of(viewers.begin(), viewers.end(), [id](const auto& v) { return v.id == id && !v.disconnecting; });
        if (!running || !online || !live || found == targets.end() || found->second.epoch != epoch || commands.size() >= 32) return false;
        Request request; request.command = command; request.epoch = epoch; request.id = found->second.remote;
        request.text = std::move(text); request.permissions = mask & kPermissionAll; request.allow = allow;
        commands.emplace_back(id, std::move(request)); wake.notify_all();
        return true;
    }
    bool exchange(const Request& request, std::vector<uint8_t>& reply) {
        Handle channel(CreateFileW(pipe.c_str(), FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr));
        if (!channel || !verified_server(channel.value, expected_test_pid)) return false;
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(channel.value, &mode, nullptr, nullptr)) return false;
        // The server identity is verified before any message permits it to
        // impersonate this client. The token is used only to read sent files.
        auto out = encode(request);
        if (!message_io(channel.value, stop.value, out.bytes, true) || !message_io(channel.value, stop.value, reply, false)) return false;
        Writer acknowledgement; acknowledgement.number(kMagic);
        return message_io(channel.value, stop.value, acknowledgement.bytes, true);
    }
    bool consume(const std::vector<uint8_t>& bytes, uint64_t action_id) {
        Reader in{bytes};
        if (in.number() != kMagic || in.number() != kControlVersion) return false;
        const bool success = in.number() != 0;
        const auto instance = in.text(32), failure = in.text();
        if (instance.size() != 32) return false;
        ServiceHostInfo info;
        info.device_id = in.text(128); info.fingerprint = in.text(128);
        const auto mode = in.number();
        if (mode > static_cast<uint32_t>(HostAuthMode::Both)) return false;
        info.auth_mode = static_cast<HostAuthMode>(mode);
        info.password_configured = in.number() != 0; info.two_factor_enabled = in.number() != 0;
        info.registration_enabled = in.number() != 0;
        std::vector<HostViewer> active;
        auto count = in.number();
        if (count > 8) return false;
        for (uint64_t i = 0; i < count; ++i) {
            HostViewer viewer;
            viewer.id = in.number(); viewer.peer_address = in.text(); viewer.mode = static_cast<SessionMode>(in.number());
            viewer.permissions = static_cast<PermissionMask>(in.number()); viewer.approved_permissions = static_cast<PermissionMask>(in.number());
            viewer.controls_input = in.number() != 0; viewer.disconnecting = in.number() != 0;
            active.push_back(std::move(viewer));
        }
        std::vector<ServiceApproval> requests;
        count = in.number(); if (count > 8) return false;
        for (uint64_t i = 0; i < count; ++i) {
            ServiceApproval item;
            item.id = in.number(); item.peer_address = in.text(); item.device_name = in.text();
            item.mode = static_cast<SessionMode>(in.number()); item.permissions = static_cast<PermissionMask>(in.number());
            item.device_proof = in.number() != 0; requests.push_back(std::move(item));
        }
        const auto newest = in.number();
        std::vector<ServiceNotice> chats;
        count = in.number(); if (count > 64) return false;
        for (uint64_t i = 0; i < count; ++i) { ServiceNotice chat; chat.id = in.number(); chat.text = in.text(); chats.push_back(std::move(chat)); }
        if (!in.done()) return false;
        std::lock_guard lock(mutex);
        for (auto& viewer : active) viewer.id = map_id(viewer.id, false, instance);
        for (auto& item : requests) item.id = map_id(item.id, true, instance);
        for (auto& chat : chats) { chat.id = map_id(chat.id, false, instance); notices.push_back(std::move(chat)); }
        if (action_id && !success) notices.push_back({action_id, failure, false});
        viewers = std::move(active); approvals = std::move(requests);
        epoch = instance; cursor = newest; online = true; error.clear();
        information = std::move(info);
        if (notices.size() > 128) notices.erase(notices.begin(), notices.end() - 128);
        while (targets.size() > 256) {
            auto stale = std::find_if(targets.begin(), targets.end(), [&](const auto& entry) {
                return std::none_of(viewers.begin(), viewers.end(), [&](const auto& v) { return v.id == entry.first; }) &&
                    std::none_of(approvals.begin(), approvals.end(), [&](const auto& p) { return p.id == entry.first; });
            });
            if (stale == targets.end()) break;
            targets.erase(stale);
        }
        return true;
    }
    void loop() {
        for (;;) {
            Request request;
            uint64_t action_id = 0;
            {
                std::lock_guard lock(mutex);
                if (!running) break;
                if (!commands.empty()) {
                    action_id = commands.front().first; request = std::move(commands.front().second); commands.pop_front();
                } else request.epoch = epoch;
                request.cursor = cursor;
            }
            std::vector<uint8_t> response;
            if (!exchange(request, response) || !consume(response, action_id)) {
                std::lock_guard lock(mutex);
                online = false; viewers.clear(); approvals.clear();
                information = {};
                error = "Service worker unavailable; reconnecting";
                if (action_id) notices.push_back({action_id, "The service did not confirm the operation. Check the connection before trying again.", false});
                // Never retry a possibly applied action, or retarget queued
                // operations after an endpoint/worker disappears.
                for (const auto& [id, queued] : commands) notices.push_back({id, "Service disconnected; queued operation cancelled.", false});
                commands.clear();
                if (notices.size() > 128) notices.erase(notices.begin(), notices.end() - 128);
            }
            std::unique_lock lock(mutex);
            wake.wait_for(lock, 250ms, [&] { return !running || !commands.empty(); });
            if (!running) break;
        }
    }
    void start(std::wstring name, DWORD pid) {
        std::lock_guard lock(mutex);
        if (running) return;
        pipe = std::move(name); expected_test_pid = pid; ResetEvent(stop.value); running = true;
        thread = std::thread([this] { loop(); });
    }
};
ServiceControlClient::ServiceControlClient() : impl_(std::make_unique<Impl>()) {}
ServiceControlClient::~ServiceControlClient() { stop(); }
void ServiceControlClient::start() { impl_->start(pipe_name(), 0); }
void ServiceControlClient::start_for_test(const std::wstring& pipe, uint32_t pid) { impl_->start(pipe, pid); }
void ServiceControlClient::stop() {
    { std::lock_guard lock(impl_->mutex); impl_->running = false; impl_->online = false; impl_->viewers.clear(); impl_->approvals.clear(); impl_->commands.clear(); impl_->information = {}; }
    SetEvent(impl_->stop.value); impl_->wake.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
}
bool ServiceControlClient::connected() const { std::lock_guard lock(impl_->mutex); return impl_->online; }
std::string ServiceControlClient::status() const { std::lock_guard lock(impl_->mutex); return impl_->error; }
ServiceHostInfo ServiceControlClient::information() const { std::lock_guard lock(impl_->mutex); return impl_->information; }
std::vector<ServiceApproval> ServiceControlClient::approvals() const { std::lock_guard lock(impl_->mutex); return impl_->approvals; }
std::vector<ServiceNotice> ServiceControlClient::take_notices() {
    std::lock_guard lock(impl_->mutex); auto notices = std::move(impl_->notices); impl_->notices.clear(); return notices;
}
bool ServiceControlClient::answer(uint64_t id, bool allow, PermissionMask permissions) { return impl_->enqueue(id, Command::Approval, {}, permissions, allow); }
ConnectionActions ServiceControlClient::actions() {
    return {
        [this] { std::lock_guard lock(impl_->mutex); return impl_->viewers; },
        [this](uint64_t id, PermissionMask mask) { return impl_->enqueue(id, Command::Permissions, {}, mask); },
        [this](uint64_t id, const std::string& text) { return valid_chat_text(text) && impl_->enqueue(id, Command::Chat, text); },
        [this](uint64_t id, const std::string& path, std::string* error) {
            if (path.empty() || path.size() > 32768 || !impl_->enqueue(id, Command::File, path)) {
                if (error) *error = "The service connection has ended or its operation queue is full.";
                return false;
            }
            return true;
        },
        [this](uint64_t id) { return impl_->enqueue(id, Command::Disconnect); }
    };
}
}
