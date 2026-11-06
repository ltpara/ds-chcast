// Explicit opt-in hardware acceptance. Never registered with CTest: this
// disconnects physical outputs temporarily and requires an installed driver.
#include "chast/host_server.h"
#include "chast/decoder.h"
#include "chast/com_ptr.h"
#include "chast/capture.h"
#include "chast/encoder.h"
#include "chast/display_privacy.h"
#include "chast/virtual_display.h"
#include "win/display_topology.h"
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

using namespace std::chrono_literals;
namespace {
int failures = 0;
bool check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", text);
    if (!ok) ++failures;
    return ok;
}
bool until(const std::function<bool()>& predicate, int seconds = 15) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(50ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
using chast::display_topology::Snapshot;
bool same_topology(const Snapshot& saved) {
    Snapshot now;
    std::string error;
    if (!chast::display_topology::query(now, QDC_ONLY_ACTIVE_PATHS, &error) ||
        !chast::display_topology::valid(now) || now.paths.size() != saved.paths.size()) return false;
    for (const auto& a : saved.paths) {
        bool found = false;
        for (const auto& b : now.paths) {
            if (a.targetInfo.id != b.targetInfo.id ||
                a.targetInfo.adapterId.LowPart != b.targetInfo.adapterId.LowPart ||
                a.targetInfo.adapterId.HighPart != b.targetInfo.adapterId.HighPart) continue;
            const auto& x = saved.modes[a.sourceInfo.modeInfoIdx].sourceMode;
            const auto& y = now.modes[b.sourceInfo.modeInfoIdx].sourceMode;
            found = x.width == y.width && x.height == y.height &&
                    x.position.x == y.position.x && x.position.y == y.position.y &&
                    a.targetInfo.rotation == b.targetInfo.rotation;
        }
        if (!found) return false;
    }
    return true;
}
bool private_topology() {
    Snapshot now;
    std::string error;
    if (!chast::display_topology::query(now, QDC_ONLY_ACTIVE_PATHS, &error) || now.paths.size() != 1)
        return false;
    const auto& p = now.paths[0];
    DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
    name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(name), p.targetInfo.adapterId, p.targetInfo.id};
    return DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS &&
           chast::is_usbmmidd_monitor(name.monitorDevicePath);
}
bool gpu_picture(chast::QuicSession& session) {
    chast::ComPtr<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, nullptr))) return false;
    bool received = false;
    chast::DecoderConfig config;
    config.d3d11_device = device.get();
    config.gpu_handler = [&](const chast::GpuFrame& frame) {
        D3D11_TEXTURE2D_DESC desc{};
        if (frame.texture) frame.texture->GetDesc(&desc);
        received = frame.texture && frame.width && frame.height &&
                   desc.Usage == D3D11_USAGE_DEFAULT && desc.CPUAccessFlags == 0;
        return chast::Error::success();
    };
    std::string error;
    auto decoder = chast::create_decoder(config, &error);
    if (!decoder || !session.request_keyframe().ok()) return false;
    bool started = false;
    return until([&] {
        chast::FrameHeader header;
        std::vector<uint8_t> payload;
        if (!session.receive_frame(header, payload, 500ms).ok()) return false;
        if (!started && !header.keyframe) return false;
        started = true;
        std::vector<chast::DecodedFrame> cpu;
        if (!decoder->decode(payload.data(), payload.size(), header.pts_ms, header.keyframe, cpu).ok()
            || !cpu.empty()) return false;
        return received;
    });
}
bool toggle(chast::QuicSession& viewer, bool wanted) {
    if (!viewer.set_privacy_mode(wanted).ok()) return false;
    bool enabled = !wanted;
    chast::PrivacyModeCode code = chast::PrivacyModeCode::Failed;
    std::string reason;
    const bool received = until([&] { return viewer.take_privacy_mode_state(enabled, code, reason); });
    std::printf("privacy wanted=%d actual=%d code=%d reason=%s\n", wanted, enabled,
                static_cast<int>(code), reason.c_str());
    return received && enabled == wanted && code == chast::PrivacyModeCode::Ok;
}
bool gpu_output(const chast::DisplayInfo& display) {
    std::string error;
    chast::CaptureConfig config;
    config.nv12.enabled = true;
    auto capture = chast::create_shared_capturer(display, config, &error);
    if (!capture || !until([&] { return capture->capture(100ms) == chast::CaptureResult::Frame &&
                                      capture->frame().nv12_texture; }, 8)) {
        std::printf("GPU output failed: %s\n", error.c_str()); return false;
    }
    const auto& frame = capture->frame();
    D3D11_TEXTURE2D_DESC desc{};
    frame.nv12_texture->GetDesc(&desc);
    if (desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags) return false;
    chast::EncoderConfig ec;
    ec.width = frame.nv12_width; ec.height = frame.nv12_height;
    auto encoder = chast::create_encoder(frame.device, ec, &error);
    if (!encoder) { std::printf("encoder failed: %s\n", error.c_str()); return false; }
    std::vector<chast::EncodedPacket> packets;
    if (!encoder->encode(frame.nv12_texture, 0, true, packets).ok() || packets.empty()) return false;
    bool received = false;
    chast::DecoderConfig dc;
    dc.d3d11_device = frame.device;
    dc.gpu_handler = [&](const chast::GpuFrame& decoded) {
        D3D11_TEXTURE2D_DESC texture{};
        if (decoded.texture) decoded.texture->GetDesc(&texture);
        received = decoded.texture && decoded.width == ec.width && decoded.height == ec.height &&
                   texture.Usage == D3D11_USAGE_DEFAULT && !texture.CPUAccessFlags;
        return chast::Error::success();
    };
    auto decoder = chast::create_decoder(dc, &error);
    if (!decoder) return false;
    for (const auto& packet : packets) {
        std::vector<chast::DecodedFrame> cpu;
        if (!decoder->decode(packet.data.data(), packet.data.size(), packet.pts_ms, packet.keyframe, cpu).ok() || !cpu.empty()) return false;
    }
    std::printf("GPU %ux%u encoder=%s shared_import=%d decoded=%d\n", ec.width, ec.height,
                encoder->implementation().c_str(), encoder->shared_import(), received);
    return received;
}
int provider_cases() {
    std::string error;
    auto recovery = chast::make_display_privacy_backend("1920x1080", chast::default_virtual_display_package_dir());
    if (!check(recovery->arm(&error), "guardian armed before monitor count/mode changes")) return 1;
    auto provider = chast::make_virtual_display_provider(chast::VirtualDisplayProviderKind::Usbmmidd,
                                                        chast::default_virtual_display_package_dir());
    const auto verify_outputs = [&](unsigned count, unsigned width = 0, unsigned height = 0) {
        unsigned found = 0;
        Snapshot now;
        if (!chast::display_topology::query(now, QDC_ONLY_ACTIVE_PATHS, &error)) return false;
        for (const auto& p : now.paths) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
            name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(name), p.targetInfo.adapterId, p.targetInfo.id};
            if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS || !chast::is_usbmmidd_monitor(name.monitorDevicePath)) continue;
            const auto device = chast::virtual_display_device_name(name.monitorDevicePath);
            for (const auto& display : chast::enumerate_displays()) {
                if (display.device_name != device) continue;
                if (width && (display.width != width || display.height != height)) return false;
                if (!gpu_output(display)) return false;
                ++found;
            }
        }
        return found == count;
    };
    bool ok = true;
    for (unsigned count : {1u, 2u, 4u, 1u}) {
        ok = provider->set_monitor_count(count, &error) && verify_outputs(count);
        std::printf("monitor count=%u result=%d detail=%s\n", count, ok, error.c_str());
        if (!check(ok, "all requested virtual outputs capture/encode/decode on GPU")) break;
    }
    if (ok) for (const auto mode : {chast::VirtualDisplayMode{1920,1080,60},
                                    chast::VirtualDisplayMode{2560,1440,60},
                                    chast::VirtualDisplayMode{3840,2160,60}}) {
        ok = provider->select_mode(mode, &error) && verify_outputs(1, mode.width, mode.height);
        std::printf("mode=%ux%u result=%d detail=%s\n", mode.width, mode.height, ok, error.c_str());
        if (!check(ok, "requested resolution produces a GPU decoded frame")) break;
    }
    check(provider->set_monitor_count(0, &error) && provider->status().active_monitors == 0,
          "all virtual monitors can be disabled");
    check(recovery->restore(&error), "restore original topology after provider checks");
    return failures ? 1 : 0;
}
bool kill_own_guard() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    DWORD guard_pid = 0;
    if (snapshot != INVALID_HANDLE_VALUE) {
        if (Process32FirstW(snapshot, &entry)) do {
            if (entry.th32ParentProcessID == GetCurrentProcessId() &&
                _wcsicmp(entry.szExeFile, L"chast-display-guard.exe") == 0) guard_pid = entry.th32ProcessID;
        } while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
    }
    HANDLE guard = guard_pid ? OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, guard_pid) : nullptr;
    const bool terminated = guard && TerminateProcess(guard, 79) && WaitForSingleObject(guard, 5000) == WAIT_OBJECT_0;
    if (guard) CloseHandle(guard);
    return terminated;
}

int child(const std::wstring& scenario, const wchar_t* ready_name) {
    Snapshot saved;
    std::string error;
    if (!check(chast::display_topology::query(saved, QDC_ONLY_ACTIVE_PATHS, &error), "snapshot before session")) return 1;
    if (scenario == L"provider") return provider_cases();
    if (scenario == L"preflight-failure" || scenario == L"commit-failure") {
        chast::VirtualDisplayPrivacy privacy(chast::make_display_privacy_backend(
            "1920x1080", chast::default_virtual_display_package_dir()));
        unsigned calls = 0;
        const bool commit_failure = scenario == L"commit-failure";
        const bool enabled = privacy.enable([&](const std::wstring& monitor, bool commit, std::string* failure) {
            ++calls;
            if (!commit && commit_failure) {
                const auto device = chast::virtual_display_device_name(monitor);
                for (const auto& display : chast::enumerate_displays())
                    if (display.device_name == device) return gpu_output(display);
                return false;
            }
            if (commit) check(private_topology(), "physical outputs are off before injected stream failure");
            *failure = "injected GPU validation failure";
            return false;
        }, &error);
        check(!enabled && error == "injected GPU validation failure" && calls == (commit_failure ? 2u : 1u),
              "injected failure is reported at the intended activation stage");
        check(!privacy.active() && !privacy.recovery_pending() && same_topology(saved),
              "failed activation restores original topology and clears recovery state");
        check(!chast::virtual_display_privacy_busy(), "failed activation releases the desktop lease");
        return failures ? 1 : 0;
    }
    if (scenario == L"arm") {
        auto backend = chast::make_display_privacy_backend("1920x1080", chast::default_virtual_display_package_dir());
        if (!check(backend->arm(&error), "independent guardian acknowledges snapshot")) {
            std::printf("%s\n", error.c_str()); return 1;
        }
        // Skip all destructors, exactly as an abruptly terminated worker would.
        TerminateProcess(GetCurrentProcess(), 0);
        return 1;
    }
    const bool management = scenario.starts_with(L"management-");
    const bool existing_output = scenario == L"management-existing";
    struct ExistingOutputCleanup {
        Snapshot original;
        bool active = false;
        ~ExistingOutputCleanup() {
            if (active) SetDisplayConfig(static_cast<UINT>(original.paths.size()), original.paths.data(),
                static_cast<UINT>(original.modes.size()), original.modes.data(),
                SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
        }
    } existing_cleanup{saved, existing_output};
    if (existing_output) {
        auto provider = chast::make_virtual_display_provider(chast::VirtualDisplayProviderKind::Usbmmidd,
            chast::default_virtual_display_package_dir());
        if (!check(provider->set_monitor_count(1, &error) &&
                   chast::display_topology::query(saved, QDC_ONLY_ACTIVE_PATHS, &error),
                   "operator enables a virtual output before the remote session")) return 1;
    }
    chast::HostServerConfig cfg;
    cfg.settings.port = 45538;
    cfg.settings.bind_address = "127.0.0.1";
    cfg.settings.carrier = false;
    cfg.settings.token_auth = true;
    cfg.settings.access_token = "local-hardware-acceptance";
    cfg.settings.token_mode = chast::HostTokenMode::Reusable;
    cfg.settings.allow_audio = false;
    cfg.settings.allow_privacy_mode = true;
    cfg.settings.privacy_mode_impl = "virtual_display";
    cfg.dry_run_input = true;
    cfg.finish_timeout_ms = 200;
    cfg.log = [](const std::string& line) { std::printf("host: %s\n", line.c_str()); };
    auto host = chast::HostServer::create(cfg, &error);
    if (!check(host != nullptr, "host created")) { std::printf("%s\n", error.c_str()); return 1; }
    host->start();
    chast::QuicClientConfig cc;
    cc.port = cfg.settings.port;
    cc.access_token = cfg.settings.access_token;
    cc.pinned_fingerprint = host->status().fingerprint;
    auto viewer = chast::QuicSession::create_client(cc, &error);
    chast::ServerInfo info;
    if (!check(viewer && viewer->connect(5s).ok() && viewer->handshake(info, 30s).ok(), "viewer connected")) return 1;
    if (!check(gpu_picture(*viewer), "physical desktop decodes into GPU texture")) return 1;
    if (management) {
        using A = chast::DisplayControlAction;
        using C = chast::DisplayControlCode;
        chast::DisplayControlState state;
        const auto control = [&](A action, uint8_t value, uint32_t target = 0, C expected = C::Ok) {
            if (!viewer->request_display_control({action, value, target}).ok()) return false;
            const bool received = until([&] { return viewer->take_display_control_state(state); }, 30);
            if (!received || state.code != expected) std::printf("management failure: %s\n", state.reason.c_str());
            return received && state.code == expected;
        };
        if (scenario == L"management-restricted-job") {
            check(control(A::SetCount, 2, 0, C::Failed) && !state.managed,
                  "display management refuses activation when guardian cannot escape the Job");
            check(same_topology(saved) && !chast::virtual_display_privacy_busy() && gpu_picture(*viewer),
                  "guardian refusal leaves physical layout and GPU stream usable");
            viewer.reset(); host->stop();
            return failures ? 1 : 0;
        }
        if (existing_output) {
            if (!check(control(A::Query, 0), "query pre-existing virtual output identity")) return 1;
            const auto active = std::find_if(state.outputs.begin(), state.outputs.end(), [](const auto& output) { return output.active; });
            if (!check(active != state.outputs.end(), "pre-existing active virtual output is enumerated")) return 1;
            const auto id = active->id;
            check(control(A::ToggleOutput, 0, id) && chast::attached_virtual_display_count() == 0 && gpu_picture(*viewer),
                  "authorized viewer can temporarily disable a pre-existing owned virtual output");
            check(control(A::ToggleOutput, 1, id) && chast::attached_virtual_display_count() == 1 && gpu_picture(*viewer),
                  "same identity re-enables the pre-existing output");
            check(control(A::ToggleOutput, 0, id), "disable pre-existing output before disconnect");
            viewer.reset(); host->stop();
            check(until([&] { return same_topology(saved); }), "disconnect restores pre-existing output modes and position");
            return failures ? 1 : 0;
        }
        if (!check(control(A::SetCount, 2) && state.managed && chast::attached_virtual_display_count() == 2 &&
                   gpu_picture(*viewer), "temporary extended desktop active with GPU video")) return 1;
        if (scenario == L"management-guardian-exit") {
            check(kill_own_guard(), "kill only this management session's guardian");
            check(until([&] { return same_topology(saved); }) && gpu_picture(*viewer),
                  "guardian loss restores original topology and preserves the ordinary GPU desktop stream");
            check(until([&] { return viewer->take_display_control_state(state); }) && !state.managed && state.code == C::Failed,
                  "viewer receives actual management recovery state after guardian loss");
            viewer.reset(); host->stop();
            return failures ? 1 : 0;
        }
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name);
        if (!ready) return 1;
        SetEvent(ready); CloseHandle(ready);
        Sleep(INFINITE);
    }
    if (scenario == L"restricted-job") {
        check(viewer->set_privacy_mode(true).ok(), "request privacy inside a job that disallows breakaway");
        bool enabled = true;
        chast::PrivacyModeCode code = chast::PrivacyModeCode::Ok;
        std::string reason;
        check(until([&] { return viewer->take_privacy_mode_state(enabled, code, reason); }) &&
              !enabled && code == chast::PrivacyModeCode::Failed &&
              reason.find("independent display recovery helper") != std::string::npos,
              "privacy refuses activation when independent recovery cannot start");
        std::printf("restricted job: %s\n", reason.c_str());
        check(same_topology(saved) && !chast::virtual_display_privacy_busy(),
              "restricted job leaves the physical topology and lease untouched");
        check(gpu_picture(*viewer), "GPU video continues after restricted-job refusal");
        viewer.reset(); host->stop();
        return failures ? 1 : 0;
    }
    if (!check(toggle(*viewer, true), "virtual privacy enabled")) return 1;
    if (!check(private_topology(), "only verified virtual display is active") ||
        !check(gpu_picture(*viewer), "private desktop decodes into GPU texture without CPU frames")) return 1;
    if (scenario == L"guardian-exit") {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        DWORD guard_pid = 0;
        if (snapshot != INVALID_HANDLE_VALUE) {
            if (Process32FirstW(snapshot, &entry)) do {
                if (entry.th32ParentProcessID == GetCurrentProcessId() &&
                    _wcsicmp(entry.szExeFile, L"chast-display-guard.exe") == 0) guard_pid = entry.th32ProcessID;
            } while (Process32NextW(snapshot, &entry));
            CloseHandle(snapshot);
        }
        HANDLE guard = guard_pid ? OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, guard_pid) : nullptr;
        bool terminated = guard && TerminateProcess(guard, 79) && WaitForSingleObject(guard, 5000) == WAIT_OBJECT_0;
        if (guard) CloseHandle(guard);
        check(terminated, "terminate only this worker's independent recovery process");
        check(until([&] { return same_topology(saved) && !viewer->peer_alive(); }),
              "host detects guardian loss, restores its snapshot and closes the private session");
        viewer.reset(); host->stop();
        return failures ? 1 : 0;
    }
    if (scenario == L"crash" || scenario == L"job") {
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name);
        if (!ready) return 1;
        SetEvent(ready);
        CloseHandle(ready);
        Sleep(INFINITE);
    }
    if (!check(toggle(*viewer, false), "privacy disabled") ||
        !check(until([&] { return same_topology(saved); }), "original targets, resolution and positions restored") ||
        !check(gpu_picture(*viewer), "GPU stream survives privacy exit")) return 1;
    if (!check(toggle(*viewer, true), "second activation reattaches detached virtual output") ||
        !check(private_topology() && gpu_picture(*viewer), "second activation streams private GPU desktop")) return 1;
    host->set_permissions(chast::kPermissionAll & ~chast::kPermissionPrivacyMode);
    if (!check(until([&] { return same_topology(saved); }), "permission revocation restores original topology") ||
        !check(gpu_picture(*viewer), "GPU stream survives permission revocation")) return 1;
    host->set_permissions(chast::kPermissionAll);
    if (!check(until([&] { return (viewer->permissions() & chast::kPermissionPrivacyMode) != 0; }), "privacy permission restored")) return 1;
    // Drain the unsolicited revocation notification before a new request.
    bool enabled; chast::PrivacyModeCode code; std::string reason;
    while (viewer->take_privacy_mode_state(enabled, code, reason)) {}
    if (!check(toggle(*viewer, true), "privacy enabled before disconnect")) return 1;
    viewer.reset();
    if (!check(until([&] { return same_topology(saved); }), "viewer disconnect restores original topology")) return 1;
    host->stop();
    return failures ? 1 : 0;
}
bool scenario(const wchar_t* name, const Snapshot& saved) {
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, 32768);
    const std::wstring event_name = L"Local\\chast-live-" + std::to_wstring(GetCurrentProcessId());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
    HANDLE job = nullptr;
    const std::wstring scenario_name(name);
    const bool management = scenario_name.starts_with(L"management-");
    const bool job_case = scenario_name == L"job" || scenario_name == L"management-job";
    const bool restricted_job = scenario_name == L"restricted-job" || scenario_name == L"management-restricted-job";
    const bool kill = scenario_name == L"crash" || scenario_name == L"management-crash" || job_case;
    if (job_case || restricted_job) {
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job_case) limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            if (job) CloseHandle(job);
            if (ready) CloseHandle(ready);
            return check(false, "create acceptance job with explicit breakaway policy");
        }
    }
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --child " + name + L" " + event_name;
    const std::wstring child_log = std::wstring(exe) + L"." + name + L".log";
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE output = CreateFileW(child_log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        if (ready) CloseHandle(ready);
        if (job) CloseHandle(job);
        return check(false, "open child acceptance log handles");
    }
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = output;
    startup.hStdInput = input;
    PROCESS_INFORMATION process{};
    bool ok = CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                             nullptr, nullptr, &startup, &process) != FALSE;
    CloseHandle(output);
    CloseHandle(input);
    if (ok && job) ok = AssignProcessToJobObject(job, process.hProcess) != FALSE;
    if (ok) ResumeThread(process.hThread);
    if (ok) {
        HANDLE waits[] = {process.hProcess, ready};
        const DWORD result = WaitForMultipleObjects(kill ? 2 : 1, waits, FALSE, 120000);
        if (kill && result == WAIT_OBJECT_0 + 1) {
            if (management) check(chast::attached_virtual_display_count() == 2,
                                  "managed outputs remain active immediately before forced termination");
            else check(private_topology(), "physical outputs remain off immediately before forced termination");
            if (job) { CloseHandle(job); job = nullptr; }
            else TerminateProcess(process.hProcess, 77);
            ok = WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0;
        } else {
            DWORD code = 1;
            GetExitCodeProcess(process.hProcess, &code);
            ok = !kill && result == WAIT_OBJECT_0 && code == 0;
            if (!ok) std::printf("child result wait=%lu exit=%lu\n", result, code);
        }
    }
    if (!ok && process.hProcess) TerminateProcess(process.hProcess, 78);
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    if (job) CloseHandle(job);
    if (ready) CloseHandle(ready);
    {
        std::ifstream log{std::filesystem::path(child_log)};
        const std::string contents{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
        std::fwrite(contents.data(), 1, contents.size(), stdout);
    }
    const bool restored = until([&] { return same_topology(saved) && !chast::virtual_display_privacy_busy(); }, 20);
    check(restored, "guardian restores original topology and releases lease");
    if (!restored) {
        auto copy = saved;
        const LONG result = SetDisplayConfig(static_cast<UINT>(copy.paths.size()), copy.paths.data(),
            static_cast<UINT>(copy.modes.size()), copy.modes.data(), SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
        std::printf("acceptance supervisor emergency restore=%ld\n", result);
    }
    std::printf("hardware scenario: %ls\n", name);
    return check(ok && restored, "hardware scenario completed");
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 4 && std::wstring(argv[1]) == L"--child") return child(argv[2], argv[3]);
    if (argc != 2 || (std::wstring(argv[1]) != L"--run-hardware" && std::wstring(argv[1]) != L"--run-failures" && std::wstring(argv[1]) != L"--run-management")) {
        std::printf("Explicit opt-in: --run-hardware, --run-failures or --run-management (temporarily disconnects physical displays)\n");
        return 2;
    }
    Snapshot saved; std::string error;
    if (!chast::display_topology::query(saved, QDC_ONLY_ACTIVE_PATHS, &error) || !chast::display_topology::valid(saved)) return 1;
    if (std::wstring(argv[1]) == L"--run-management") {
        for (const wchar_t* name : {L"management-existing", L"management-restricted-job", L"management-guardian-exit",
                                   L"management-crash", L"management-job"})
            if (!scenario(name, saved)) break;
    } else if (std::wstring(argv[1]) == L"--run-failures") {
        for (const wchar_t* name : {L"preflight-failure", L"commit-failure", L"restricted-job", L"guardian-exit"})
            if (!scenario(name, saved)) break;
    } else {
        for (const wchar_t* name : {L"arm", L"provider", L"session", L"crash", L"job"})
            if (!scenario(name, saved)) break;
    }
    return failures ? 1 : 0;
}
