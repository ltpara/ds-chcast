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
#include <thread>
#include <utility>

namespace chast {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kConsoleChunk = kMaxTerminalChunkBytes;
// Console output buffered per terminal before the reader thread stops
// draining ConPTY; console input buffered before pump stops pulling from the
// transport. Both are what turns a busy viewer into backpressure instead of
// unbounded memory.
constexpr size_t kOutputHighWater = 512u * 1024u;
constexpr size_t kInputHighWater = 64u * 1024u;
constexpr auto kSendTimeout = std::chrono::milliseconds(50);

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

struct TerminalRelay::Console {
    uint32_t terminal_id = 0;
    uint16_t columns = 80;
    uint16_t rows = 24;
    void* pseudo_console = nullptr;
    HANDLE input = nullptr;   // our write end into ConPTY
    HANDLE output = nullptr;  // our read end out of ConPTY
    HANDLE process = nullptr;
    std::thread reader;
    std::thread writer;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> from_console;
    size_t from_console_bytes = 0;
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

TerminalRelay::TerminalRelay(QuicSession& session) : session_(session) {}

TerminalRelay::~TerminalRelay() {
    for (const auto& console : live_) {
        stop_console(*console);
    }
    live_.clear();
}

void TerminalRelay::stop_console(Console& console) {
    {
        std::lock_guard<std::mutex> lock(console.mutex);
        console.closing = true;
        console.cv.notify_all();
    }
    if (console.process) {
        // The shell belongs to this session; it must not outlive the viewer.
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

void TerminalRelay::start_console(const TerminalRequest& request,
                                  std::vector<std::string>& events) {
    const ConPtyApi& api = conpty();
    const auto fail = [&](const std::string& reason) {
        ++failures_;
        session_.send_terminal_result(
            TerminalResult{request.terminal_id, TerminalCode::Failed, reason});
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

    auto console = std::make_unique<Console>();
    console->terminal_id = request.terminal_id;
    console->columns = request.columns;
    console->rows = request.rows;
    console->pseudo_console = pseudo;
    console->input = input_write;
    console->output = output_read;
    console->process = process.hProcess;
    Console& state = *console;

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
            state.cv.wait(lock, [&state] {
                return state.closing || state.from_console_bytes < kOutputHighWater;
            });
            if (state.closing) {
                return;
            }
            state.from_console.emplace_back(buffer.begin(), buffer.begin() + got);
            state.from_console_bytes += got;
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
                // console sees EOF, which is what a shell exits on.
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

    const uint32_t id = request.terminal_id;
    live_.push_back(std::move(console));

    const Error answered = session_.send_terminal_result(
        TerminalResult{id, TerminalCode::Ok, {}});
    if (!answered.ok()) {
        stop_console(*live_.back());
        live_.pop_back();
        ++failures_;
        events.push_back("terminal " + std::to_string(id) +
                         ": the viewer left before the console was ready");
        return;
    }
    ++started_;
    events.push_back("terminal " + std::to_string(id) + ": started a shell under ConPTY (" +
                     describe(request) + ")");
}

void TerminalRelay::apply_resize(Console& console, const TerminalSize& size,
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

void TerminalRelay::finish(Console& console, TerminalCode code, const std::string& reason,
                           std::vector<std::string>& events) {
    if (console.finished) {
        return;
    }
    console.finished = true;
    // The transport ignores a stop for a terminal that is already gone, which
    // is the common case when the session is the thing that ended.
    session_.send_terminal_stop(console.terminal_id, code, reason);
    stop_console(console);
    events.push_back("terminal " + std::to_string(console.terminal_id) + ": " + reason);
}

std::vector<std::string> TerminalRelay::pump() {
    std::vector<std::string> events;
    TerminalRequest request;
    while (session_.take_terminal_request(request)) {
        start_console(request, events);
    }
    TerminalSize resize;
    while (session_.take_terminal_resize(resize)) {
        for (const auto& console : live_) {
            if (console->terminal_id == resize.terminal_id) {
                apply_resize(*console, resize, events);
                break;
            }
        }
    }
    TerminalStop stop;
    while (session_.take_terminal_stop(stop)) {
        for (size_t i = 0; i < live_.size(); ++i) {
            if (live_[i]->terminal_id != stop.terminal_id) {
                continue;
            }
            const std::string reason =
                stop.reason.empty() ? std::string("the viewer ended the terminal")
                                    : stop.reason;
            finish(*live_[i], stop.code, reason, events);
            live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    for (size_t i = 0; i < live_.size();) {
        const Outcome outcome = drive(*live_[i], events);
        if (outcome == Outcome::Keep) {
            ++i;
            continue;
        }
        live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return events;
}

TerminalRelay::Outcome TerminalRelay::drive(Console& console,
                                            std::vector<std::string>& events) {
    const uint32_t id = console.terminal_id;
    // Viewer -> console. Pulling stops at high water, which lets the
    // transport's receive window push back on the viewer.
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(console.mutex);
            if (console.input_ended || console.writer_failed || console.closing ||
                console.to_console_bytes >= kInputHighWater) {
                break;
            }
        }
        std::vector<uint8_t> data;
        bool ended = false;
        if (!session_.take_terminal_input(id, data, ended)) {
            if (ended) {
                std::lock_guard<std::mutex> lock(console.mutex);
                console.input_ended = true;
                console.cv.notify_all();
            }
            break;
        }
        std::lock_guard<std::mutex> lock(console.mutex);
        console.to_console_bytes += data.size();
        console.to_console.push_back(std::move(data));
        console.cv.notify_all();
    }
    bool input_broken = false;
    {
        std::lock_guard<std::mutex> lock(console.mutex);
        input_broken = console.writer_failed;
    }
    if (input_broken) {
        finish(console, TerminalCode::Failed, "the console input pipe broke", events);
        ++failures_;
        return Outcome::Failed;
    }
    // Console -> viewer. A full QUIC send queue keeps the chunk for the next
    // frame; anything else means the stream or the session is gone.
    for (;;) {
        std::vector<uint8_t> chunk;
        {
            std::lock_guard<std::mutex> lock(console.mutex);
            if (console.from_console.empty()) {
                break;
            }
            chunk = std::move(console.from_console.front());
            console.from_console.pop_front();
            console.from_console_bytes -= chunk.size();
            console.cv.notify_all();
        }
        const size_t size = chunk.size();
        const Error sent =
            session_.send_terminal_output(id, chunk.data(), size, kSendTimeout);
        if (!sent.ok()) {
            if (sent.status() == Status::WaitTimeout) {
                std::lock_guard<std::mutex> lock(console.mutex);
                console.from_console_bytes += size;
                console.from_console.push_front(std::move(chunk));
                break;
            }
            finish(console, TerminalCode::Failed,
                   "cannot send console output: " + sent.to_string(), events);
            ++failures_;
            return Outcome::Failed;
        }
    }

    if (!console.exited &&
        WaitForSingleObject(console.process, 0) == WAIT_OBJECT_0) {
        console.exited = true;
        GetExitCodeProcess(console.process, &console.exit_code);
        events.push_back("terminal " + std::to_string(id) + ": the shell exited with code " +
                         std::to_string(console.exit_code));
    }
    if (console.exited) {
        bool drained = false;
        {
            std::lock_guard<std::mutex> lock(console.mutex);
            drained = console.from_console.empty() && console.reader_done;
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
                "the shell exited with code " + std::to_string(console.exit_code);
            finish(console, TerminalCode::Ok, reason, events);
            return Outcome::Done;
        }
    }
    return Outcome::Keep;
}

}  // namespace chast
