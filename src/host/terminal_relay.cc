// Winsock 2 must precede windows.h, which chast/quic.h pulls in through
// chast/error.h; otherwise the SDK sees winsock 1 first and ws2tcpip.h fails.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "chast/terminal_relay.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

namespace chast {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kConsoleChunk = kMaxTerminalChunkBytes;
// Console output buffered per attached terminal before the reader thread stops
// draining ConPTY; console input buffered before pump stops pulling from the
// transport. Both are what turns a busy viewer into backpressure instead of
// unbounded memory. A detached console never blocks: its output keeps flowing
// into the scrollback ring instead.
constexpr size_t kOutputHighWater = 512u * 1024u;
constexpr size_t kInputHighWater = 64u * 1024u;
constexpr auto kSendTimeout = std::chrono::milliseconds(50);
constexpr auto kSweepInterval = std::chrono::minutes(1);

// ConPTY lives in kernel32 but the import library of older SDKs does not
// export it, so the entry points are resolved dynamically. HPCON is a void*,
// which keeps the signatures SDK-independent.
using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, void**);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(void*, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(void*);

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
    ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
#endif

struct ConPtyApi {
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;

    bool available() const { return create && resize && close; }
};

const ConPtyApi& conpty() {
    static const ConPtyApi api = [] {
        ConPtyApi loaded;
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel) {
            return loaded;
        }
        loaded.create = reinterpret_cast<CreatePseudoConsoleFn>(
            GetProcAddress(kernel, "CreatePseudoConsole"));
        loaded.resize = reinterpret_cast<ResizePseudoConsoleFn>(
            GetProcAddress(kernel, "ResizePseudoConsole"));
        loaded.close = reinterpret_cast<ClosePseudoConsoleFn>(
            GetProcAddress(kernel, "ClosePseudoConsole"));
        return loaded;
    }();
    return api;
}

std::wstring shell_command() {
    std::wstring path = L"C:\\Windows\\System32\\cmd.exe";
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"COMSPEC", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        path.assign(buffer, length);
    }
    return L"\"" + path + L"\"";
}

std::string describe(const TerminalRequest& request) {
    return std::to_string(request.columns) + "x" + std::to_string(request.rows);
}

}  // namespace

struct TerminalConsole {
    // Session-scoped id: the CTTY stream key the current owner uses. It is
    // rewritten on every attach because a reconnecting viewer has no history.
    uint32_t terminal_id = 0;
    // Host-scoped id published in the result; 0 when the console is not a
    // service. Immutable once registered.
    uint32_t service_id = 0;
    uint16_t columns = 80;
    uint16_t rows = 24;
    bool persistent = false;
    bool attached = true;
    Clock::time_point detached_at{};

    void* pseudo_console = nullptr;
    HANDLE input = nullptr;   // our write end into ConPTY
    HANDLE output = nullptr;  // our read end out of ConPTY
    HANDLE process = nullptr;
    std::thread reader;
    std::thread writer;

    std::mutex mutex;
    std::condition_variable cv;
    // Output waiting for the current owner. While detached this stays empty;
    // the reader writes to the scrollback ring only.
    std::deque<std::vector<uint8_t>> from_console;
    size_t from_console_bytes = 0;
    // Copy of the scrollback taken at attach, sent ahead of live output.
    std::deque<std::vector<uint8_t>> replay;
    size_t replay_bytes = 0;
    std::deque<std::vector<uint8_t>> scrollback;
    size_t scrollback_bytes = 0;
    std::deque<std::vector<uint8_t>> to_console;
    size_t to_console_bytes = 0;
    bool closing = false;
    bool reader_done = false;
    bool writer_failed = false;
    bool writer_done = false;
    bool input_ended = false;
    bool exited = false;
    DWORD exit_code = 0;
    bool finished = false;
};

namespace {

// Detached services outlive the HostServer that created them, so the registry
// is a leaked process-wide singleton: process exit reclaims it and no static
// destructor can race the sweeper thread. The lock order is registry first,
// then a console's own mutex, never the reverse.
struct ServiceRegistry {
    std::mutex mutex;
    std::vector<std::shared_ptr<TerminalConsole>> services;
    uint32_t next_id = 0;
};

ServiceRegistry& service_registry() {
    static ServiceRegistry* registry = new ServiceRegistry();
    return *registry;
}

// The job exists so console children die with this process even when they were
// detached at the time. The handle is deliberately never closed: closing it is
// what triggers JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, and process exit does that.
HANDLE console_job() {
    static HANDLE job = [] {
        HANDLE handle = CreateJobObjectW(nullptr, nullptr);
        if (!handle) {
            return static_cast<HANDLE>(nullptr);
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits))) {
            CloseHandle(handle);
            return static_cast<HANDLE>(nullptr);
        }
        return handle;
    }();
    return job;
}

void shutdown_console(TerminalConsole& console) {
    {
        std::lock_guard<std::mutex> lock(console.mutex);
        console.closing = true;
        console.cv.notify_all();
    }
    if (console.process) {
        // The shell belongs to this host; it must not survive an explicit end.
        TerminateProcess(console.process, 1);
    }
    if (console.pseudo_console) {
        conpty().close(console.pseudo_console);
        console.pseudo_console = nullptr;
    }
    if (console.reader.joinable()) {
        // ReadFile may be parked on the pipe; closing the pseudo console
        // closes its write end, and the cancel makes the read return at once.
        CancelSynchronousIo(console.reader.native_handle());
        console.reader.join();
    }
    if (console.writer.joinable()) {
        CancelSynchronousIo(console.writer.native_handle());
        console.writer.join();
    }
    if (console.input) {
        CloseHandle(console.input);
        console.input = nullptr;
    }
    if (console.output) {
        CloseHandle(console.output);
        console.output = nullptr;
    }
    if (console.process) {
        CloseHandle(console.process);
        console.process = nullptr;
    }
}

void sweep_services();

void ensure_sweeper() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(kSweepInterval);
                sweep_services();
            }
        }).detach();
    });
}

// A detached service dies when its shell exits or when no viewer attaches
// within kTerminalServiceIdleSeconds. The sweep runs on its own thread, so it
// only marks and unlinks under the registry lock, then terminates outside it.
void sweep_services() {
    ServiceRegistry& registry = service_registry();
    std::vector<std::shared_ptr<TerminalConsole>> doomed;
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        const Clock::time_point now = Clock::now();
        for (auto it = registry.services.begin(); it != registry.services.end();) {
            TerminalConsole& console = **it;
            bool drop = false;
            {
                std::lock_guard<std::mutex> console_lock(console.mutex);
                if (!console.attached) {
                    const bool idle =
                        now - console.detached_at >=
                        std::chrono::seconds(kTerminalServiceIdleSeconds);
                    drop = idle || console.finished || console.closing ||
                           console.reader_done;
                }
            }
            if (!drop && console.process &&
                WaitForSingleObject(console.process, 0) == WAIT_OBJECT_0) {
                // A detached shell can exit on its own (the user typed
                // `exit` before losing the viewer). Nothing can attach to it.
                drop = true;
            }
            if (!drop) {
                ++it;
                continue;
            }
            doomed.push_back(*it);
            it = registry.services.erase(it);
        }
    }
    for (const auto& console : doomed) {
        shutdown_console(*console);
    }
}

uint32_t allocate_service_id_locked(ServiceRegistry& registry) {
    if (registry.next_id == 0) {
        // Unpredictable so a viewer cannot guess its way into someone else's
        // shell; the id is only discoverable by being told it.
        std::random_device device;
        registry.next_id = device();
        if (registry.next_id == 0) {
            registry.next_id = 1;
        }
    }
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const uint32_t id = registry.next_id++;
        if (id == 0) {
            continue;
        }
        const bool taken = std::any_of(
            registry.services.begin(), registry.services.end(),
            [id](const std::shared_ptr<TerminalConsole>& service) {
                return service->service_id == id;
            });
        if (!taken) {
            return id;
        }
    }
    return 0;
}

bool register_service(const std::shared_ptr<TerminalConsole>& console) {
    ServiceRegistry& registry = service_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    if (registry.services.size() >= kMaxTerminalServices) {
        return false;
    }
    const uint32_t id = allocate_service_id_locked(registry);
    if (id == 0) {
        return false;
    }
    console->service_id = id;
    registry.services.push_back(console);
    ensure_sweeper();
    return true;
}

void remove_service(const std::shared_ptr<TerminalConsole>& console) {
    if (console->service_id == 0) {
        return;
    }
    ServiceRegistry& registry = service_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto it = std::find(registry.services.begin(), registry.services.end(), console);
    if (it != registry.services.end()) {
        registry.services.erase(it);
    }
}

// Takes an attached=false service over for a new session. Everything queued
// for the old owner is stale and dropped; the scrollback is copied into the
// replay queue so the new viewer starts with the history it missed.
bool acquire_service(uint32_t service_id, uint32_t terminal_id,
                     std::shared_ptr<TerminalConsole>& console) {
    ServiceRegistry& registry = service_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (const auto& service : registry.services) {
        if (service->service_id != service_id) {
            continue;
        }
        std::lock_guard<std::mutex> console_lock(service->mutex);
        if (service->attached || service->closing || service->finished ||
            service->reader_done || service->writer_failed) {
            return false;
        }
        if (service->process &&
            WaitForSingleObject(service->process, 0) == WAIT_OBJECT_0) {
            return false;
        }
        service->terminal_id = terminal_id;
        service->attached = true;
        service->detached_at = Clock::time_point{};
        service->replay.clear();
        service->replay_bytes = 0;
        for (const std::vector<uint8_t>& chunk : service->scrollback) {
            service->replay_bytes += chunk.size();
            service->replay.push_back(chunk);
        }
        service->from_console.clear();
        service->from_console_bytes = 0;
        service->to_console.clear();
        service->to_console_bytes = 0;
        service->input_ended = false;
        console = service;
        return true;
    }
    return false;
}

void assign_to_job(HANDLE process) {
    const HANDLE job = console_job();
    if (job) {
        AssignProcessToJobObject(job, process);
    }
}

}  // namespace

TerminalRelay::TerminalRelay(QuicSession& session, bool allow_persist,
                             std::function<void()> viewer_activity)
    : session_(session), allow_persist_(allow_persist),
      viewer_activity_(std::move(viewer_activity)) {}

TerminalRelay::~TerminalRelay() {
    for (const auto& console : live_) {
        if (console->persistent) {
            // The viewer is gone, not the shell: park it for the next session.
            detach_console(console);
            continue;
        }
        stop_console(*console);
    }
    live_.clear();
}

void TerminalRelay::stop_console(TerminalConsole& console) {
    shutdown_console(console);
}

void TerminalRelay::start_console(const TerminalRequest& request,
                                  std::vector<std::string>& events) {
    const ConPtyApi& api = conpty();
    const auto fail = [&](const std::string& reason) {
        ++failures_;
        session_.send_terminal_result(TerminalResult{
            request.terminal_id, 0, TerminalCode::Failed, reason});
        events.push_back("terminal " + std::to_string(request.terminal_id) + ": " + reason);
    };
    if (!api.available()) {
        fail("ConPTY is not available on this Windows build");
        return;
    }
    HANDLE input_read = nullptr;
    HANDLE input_write = nullptr;
    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    if (!CreatePipe(&input_read, &input_write, nullptr, 0) ||
        !CreatePipe(&output_read, &output_write, nullptr, 0)) {
        for (HANDLE handle : {input_read, input_write, output_read, output_write}) {
            if (handle) {
                CloseHandle(handle);
            }
        }
        fail("cannot create the console pipes");
        return;
    }
    void* pseudo = nullptr;
    const COORD size{static_cast<SHORT>(request.columns), static_cast<SHORT>(request.rows)};
    const HRESULT created = api.create(size, input_read, output_write, 0, &pseudo);
    // ConPTY owns both ends it was handed, even when creation failed.
    CloseHandle(input_read);
    CloseHandle(output_write);
    if (FAILED(created) || !pseudo) {
        CloseHandle(input_write);
        CloseHandle(output_read);
        fail("cannot create a pseudo console");
        return;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    // A client whose standard handles are invalid opens CONIN$/CONOUT$ of its
    // own console, which is the pseudoconsole. Without this the shell inherits
    // this process's redirected handles, reads EOF on the first prompt and
    // exits before printing anything into the console.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullptr;
    startup.StartupInfo.hStdOutput = nullptr;
    startup.StartupInfo.hStdError = nullptr;
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    startup.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attribute_size));
    const bool attributes_ready =
        startup.lpAttributeList &&
        InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size) &&
        UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                  PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pseudo, sizeof(pseudo),
                                  nullptr, nullptr);
    std::wstring command = shell_command();
    PROCESS_INFORMATION process{};
    const BOOL spawned =
        attributes_ready &&
        CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                       EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo,
                       &process);
    if (startup.lpAttributeList) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    }
    if (!spawned) {
        api.close(pseudo);
        CloseHandle(input_write);
        CloseHandle(output_read);
        fail("cannot start the shell");
        return;
    }
    CloseHandle(process.hThread);
    assign_to_job(process.hProcess);

    const bool want_persist = (request.flags & kTerminalRequestPersist) != 0;
    auto console = std::make_shared<TerminalConsole>();
    console->terminal_id = request.terminal_id;
    console->columns = request.columns;
    console->rows = request.rows;
    console->pseudo_console = pseudo;
    console->input = input_write;
    console->output = output_read;
    console->process = process.hProcess;
    console->persistent = want_persist && allow_persist_;
    TerminalConsole& state = *console;

    state.reader = std::thread([&state] {
        std::vector<uint8_t> buffer(kConsoleChunk);
        for (;;) {
            DWORD got = 0;
            const BOOL ok = ReadFile(state.output, buffer.data(),
                                     static_cast<DWORD>(buffer.size()), &got, nullptr);
            std::unique_lock<std::mutex> lock(state.mutex);
            if (!ok || got == 0) {
                state.reader_done = true;
                state.cv.notify_all();
                return;
            }
            // The ring always takes the bytes; only an attached console also
            // queues them for a viewer. Trimming keeps a detached shell's
            // output bounded without ever blocking the pipe.
            state.scrollback.emplace_back(buffer.begin(), buffer.begin() + got);
            state.scrollback_bytes += got;
            while (state.scrollback_bytes > kTerminalScrollbackBytes &&
                   !state.scrollback.empty()) {
                state.scrollback_bytes -= state.scrollback.front().size();
                state.scrollback.pop_front();
            }
            state.cv.wait(lock, [&state] {
                return state.closing || !state.attached ||
                       state.from_console_bytes < kOutputHighWater;
            });
            if (state.closing) {
                return;
            }
            if (state.attached) {
                state.from_console.emplace_back(buffer.begin(), buffer.begin() + got);
                state.from_console_bytes += got;
            }
            lock.unlock();
            state.cv.notify_all();
        }
    });
    state.writer = std::thread([&state] {
        for (;;) {
            std::vector<uint8_t> chunk;
            bool close_input = false;
            {
                std::unique_lock<std::mutex> lock(state.mutex);
                state.cv.wait(lock, [&state] {
                    return state.closing || !state.to_console.empty() ||
                           state.input_ended;
                });
                if (state.closing) {
                    return;
                }
                if (!state.to_console.empty()) {
                    chunk = std::move(state.to_console.front());
                    state.to_console.pop_front();
                    state.to_console_bytes -= chunk.size();
                } else {
                    close_input = true;
                }
            }
            if (close_input) {
                // The viewer half-closed its input: close the pipe so the
                // console sees EOF, which is what a shell exits on. Persistent
                // consoles never take this path, so a detached service keeps
                // its stdin open for the next viewer.
                HANDLE handle = nullptr;
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    handle = state.input;
                    state.input = nullptr;
                    state.writer_done = true;
                    state.cv.notify_all();
                }
                if (handle) {
                    CloseHandle(handle);
                }
                return;
            }
            DWORD wrote = 0;
            if (!WriteFile(state.input, chunk.data(), static_cast<DWORD>(chunk.size()),
                           &wrote, nullptr)) {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.writer_failed = true;
                state.cv.notify_all();
                return;
            }
        }
    });

    if (console->persistent && !register_service(console)) {
        // The budget is full. The console still starts, but the result carries
        // no service id, so the viewer knows it cannot come back to it.
        console->persistent = false;
        events.push_back("terminal " + std::to_string(request.terminal_id) +
                         ": the persistent service budget is full; running a session console");
    }

    const uint32_t id = request.terminal_id;
    live_.push_back(console);

    const Error answered = session_.send_terminal_result(
        TerminalResult{id, console->service_id, TerminalCode::Ok, {}});
    if (!answered.ok()) {
        live_.pop_back();
        if (console->persistent) {
            detach_console(console);
        } else {
            stop_console(*console);
        }
        ++failures_;
        events.push_back("terminal " + std::to_string(id) +
                         ": the viewer left before the console was ready");
        return;
    }
    ++started_;
    events.push_back("terminal " + std::to_string(id) + ": started a shell under ConPTY (" +
                     describe(request) +
                     (console->persistent
                          ? "); service #" + std::to_string(console->service_id)
                          : std::string(")")));
}

void TerminalRelay::attach_console(const TerminalRequest& request,
                                   std::vector<std::string>& events) {
    const auto refuse = [&](TerminalCode code, const std::string& reason) {
        ++failures_;
        session_.send_terminal_result(
            TerminalResult{request.terminal_id, 0, code, reason});
        events.push_back("terminal " + std::to_string(request.terminal_id) + ": " + reason);
    };
    if (!allow_persist_) {
        refuse(TerminalCode::Refused, "terminal persistence is disabled on this host");
        return;
    }
    std::shared_ptr<TerminalConsole> console;
    if (!acquire_service(request.service_id, request.terminal_id, console)) {
        refuse(TerminalCode::NotFound,
               "terminal service #" + std::to_string(request.service_id) +
                   " is not waiting");
        return;
    }

    const Error answered = session_.send_terminal_result(TerminalResult{
        request.terminal_id, console->service_id, TerminalCode::Ok, {}});
    if (!answered.ok()) {
        // The viewer left before the handshake finished; park the console again
        // rather than kill a shell the operator may still want.
        detach_console(console);
        ++failures_;
        events.push_back("terminal " + std::to_string(request.terminal_id) +
                         ": the viewer left before service #" +
                         std::to_string(console->service_id) + " was attached");
        return;
    }

    apply_resize(*console,
                 TerminalSize{request.terminal_id, request.columns, request.rows}, events);
    live_.push_back(console);
    ++resumed_;
    events.push_back("terminal " + std::to_string(request.terminal_id) +
                     ": attached to service #" + std::to_string(console->service_id));
}

void TerminalRelay::apply_resize(TerminalConsole& console, const TerminalSize& size,
                                 std::vector<std::string>& events) {
    if (!console.pseudo_console) {
        return;
    }
    const COORD dimensions{static_cast<SHORT>(size.columns), static_cast<SHORT>(size.rows)};
    if (FAILED(conpty().resize(console.pseudo_console, dimensions))) {
        events.push_back("terminal " + std::to_string(console.terminal_id) +
                         ": cannot resize the console");
        return;
    }
    console.columns = size.columns;
    console.rows = size.rows;
    events.push_back("terminal " + std::to_string(console.terminal_id) + ": resized to " +
                     std::to_string(size.columns) + "x" + std::to_string(size.rows));
}

void TerminalRelay::detach_console(const std::shared_ptr<TerminalConsole>& console) {
    bool orphan = false;
    {
        ServiceRegistry& registry = service_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        if (std::find(registry.services.begin(), registry.services.end(), console) ==
            registry.services.end()) {
            orphan = true;
        } else {
            std::lock_guard<std::mutex> console_lock(console->mutex);
            console->attached = false;
            console->detached_at = Clock::now();
            // Nothing queued for the old owner may reach the next one: its
            // window size, its unread bytes and its half-typed line are stale.
            console->from_console.clear();
            console->from_console_bytes = 0;
            console->replay.clear();
            console->replay_bytes = 0;
            console->to_console.clear();
            console->to_console_bytes = 0;
            console->input_ended = false;
            console->cv.notify_all();
        }
    }
    if (orphan) {
        // The service was reaped while this relay still thought it owned it;
        // the shell must not keep running unowned.
        shutdown_console(*console);
    }
}

void TerminalRelay::finish(const std::shared_ptr<TerminalConsole>& console,
                           TerminalCode code, const std::string& reason,
                           std::vector<std::string>& events) {
    {
        std::lock_guard<std::mutex> lock(console->mutex);
        if (console->finished) {
            return;
        }
        console->finished = true;
    }
    // The transport ignores a stop for a terminal that is already gone, which
    // is the common case when the session is the thing that ended.
    session_.send_terminal_stop(console->terminal_id, code, reason);
    remove_service(console);
    stop_console(*console);
    events.push_back("terminal " + std::to_string(console->terminal_id) + ": " + reason);
}

std::vector<std::string> TerminalRelay::pump() {
    std::vector<std::string> events;
    TerminalRequest request;
    while (session_.take_terminal_request(request)) {
        if (viewer_activity_) viewer_activity_();
        if ((request.flags & kTerminalRequestAttach) != 0) {
            attach_console(request, events);
        } else {
            start_console(request, events);
        }
    }
    TerminalSize resize;
    while (session_.take_terminal_resize(resize)) {
        if (viewer_activity_) viewer_activity_();
        for (const auto& console : live_) {
            if (console->terminal_id == resize.terminal_id) {
                apply_resize(*console, resize, events);
                break;
            }
        }
    }
    TerminalStop stop;
    while (session_.take_terminal_stop(stop)) {
        if (viewer_activity_) viewer_activity_();
        for (size_t i = 0; i < live_.size(); ++i) {
            if (live_[i]->terminal_id != stop.terminal_id) {
                continue;
            }
            const std::string reason =
                stop.reason.empty() ? std::string("the viewer ended the terminal")
                                    : stop.reason;
            finish(live_[i], stop.code, reason, events);
            live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    for (size_t i = 0; i < live_.size();) {
        const Outcome outcome = drive(live_[i], events);
        if (outcome == Outcome::Keep) {
            ++i;
            continue;
        }
        live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return events;
}

TerminalRelay::Outcome TerminalRelay::drive(const std::shared_ptr<TerminalConsole>& console,
                                            std::vector<std::string>& events) {
    const uint32_t id = console->terminal_id;
    // Viewer -> console. Pulling stops at high water, which lets the
    // transport's receive window push back on the viewer.
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(console->mutex);
            if (console->input_ended || console->writer_failed || console->closing ||
                console->to_console_bytes >= kInputHighWater) {
                break;
            }
        }
        std::vector<uint8_t> data;
        bool ended = false;
        if (!session_.take_terminal_input(id, data, ended)) {
            // A persistent console treats the viewer's half-close as a
            // disconnect: closing stdin would end the shell the service exists
            // to keep. A session console keeps the old EOF semantics.
            if (ended && !console->persistent) {
                std::lock_guard<std::mutex> lock(console->mutex);
                console->input_ended = true;
                console->cv.notify_all();
            }
            break;
        }
        if (!data.empty() && viewer_activity_) viewer_activity_();
        std::lock_guard<std::mutex> lock(console->mutex);
        console->to_console_bytes += data.size();
        console->to_console.push_back(std::move(data));
        console->cv.notify_all();
    }
    bool input_broken = false;
    {
        std::lock_guard<std::mutex> lock(console->mutex);
        input_broken = console->writer_failed;
    }
    if (input_broken) {
        if (console->persistent) {
            detach_console(console);
            ++failures_;
            events.push_back("terminal " + std::to_string(id) +
                             ": detached because the console input pipe broke; service #" +
                             std::to_string(console->service_id) + " keeps running");
            return Outcome::Failed;
        }
        finish(console, TerminalCode::Failed, "the console input pipe broke", events);
        ++failures_;
        return Outcome::Failed;
    }
    // Console -> viewer. Replayed history goes first, then live bytes. A full
    // QUIC send queue keeps the chunk for the next frame; anything else means
    // the stream or the session is gone.
    for (;;) {
        std::vector<uint8_t> chunk;
        bool from_replay = false;
        {
            std::lock_guard<std::mutex> lock(console->mutex);
            if (!console->replay.empty()) {
                chunk = std::move(console->replay.front());
                console->replay.pop_front();
                console->replay_bytes -= chunk.size();
                from_replay = true;
            } else if (!console->from_console.empty()) {
                chunk = std::move(console->from_console.front());
                console->from_console.pop_front();
                console->from_console_bytes -= chunk.size();
                console->cv.notify_all();
            } else {
                break;
            }
        }
        const size_t size = chunk.size();
        const Error sent =
            session_.send_terminal_output(id, chunk.data(), size, kSendTimeout);
        if (!sent.ok()) {
            if (sent.status() == Status::WaitTimeout) {
                std::lock_guard<std::mutex> lock(console->mutex);
                if (from_replay) {
                    console->replay_bytes += size;
                    console->replay.push_front(std::move(chunk));
                } else {
                    console->from_console_bytes += size;
                    console->from_console.push_front(std::move(chunk));
                }
                break;
            }
            if (console->persistent) {
                detach_console(console);
                ++failures_;
                events.push_back("terminal " + std::to_string(id) +
                                 ": detached (cannot send output: " + sent.to_string() +
                                 "); service #" + std::to_string(console->service_id) +
                                 " keeps running");
                return Outcome::Failed;
            }
            finish(console, TerminalCode::Failed,
                   "cannot send console output: " + sent.to_string(), events);
            ++failures_;
            return Outcome::Failed;
        }
    }

    if (!console->exited && console->process &&
        WaitForSingleObject(console->process, 0) == WAIT_OBJECT_0) {
        console->exited = true;
        GetExitCodeProcess(console->process, &console->exit_code);
        events.push_back("terminal " + std::to_string(id) + ": the shell exited with code " +
                         std::to_string(console->exit_code));
    }
    if (console->exited) {
        bool drained = false;
        {
            std::lock_guard<std::mutex> lock(console->mutex);
            drained = console->from_console.empty() && console->replay.empty() &&
                      console->reader_done;
        }
        if (drained) {
            const Error closed = session_.finish_terminal_output(id);
            if (!closed.ok() && closed.status() != Status::Unavailable) {
                finish(console, TerminalCode::Failed,
                       "cannot close the console output: " + closed.to_string(), events);
                ++failures_;
                return Outcome::Failed;
            }
            const std::string reason =
                "the shell exited with code " + std::to_string(console->exit_code);
            finish(console, TerminalCode::Ok, reason, events);
            return Outcome::Done;
        }
    }
    return Outcome::Keep;
}

}  // namespace chast
