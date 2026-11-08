#pragma once

#include <windows.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "chast/elevation.h"
#include "chast/host_settings.h"

namespace chast::host {

struct ElevationSnapshot {
    std::string config_path;
    HostSettings settings;
    std::string fingerprint;
    bool portable = false;
    bool hidden = false;
    HANDLE instance_mutex = nullptr;
};

// All waits and ShellExecute's UAC dialog run on an owned worker state. No UI
// or HostServer pointer is captured, and cancellation never joins that worker.
class ElevationHandoff final : public IElevationTask,
                               public std::enable_shared_from_this<ElevationHandoff> {
public:
    using Launcher = std::function<HANDLE(const std::wstring&, DWORD&)>;
    static std::shared_ptr<ElevationHandoff> start(Launcher launch = {}, bool require_elevated = true);
    ~ElevationHandoff();
    ElevationState poll() override;
    void commit() override;
    void cancel() override;
    void shutdown();
    bool committed() const;
    bool finished() const;
    // Called by the UI after stopping the old listener and draining its saved
    // settings notifications. No secrets are placed on disk or the command line.
    bool supply(const ElevationSnapshot& snapshot, std::string* error);
private:
    struct Impl;
    ElevationHandoff();
    std::unique_ptr<Impl> impl_;
};

class ElevationController {
public:
    ElevationState state();
    std::shared_ptr<IElevationTask> request();
    std::shared_ptr<ElevationHandoff> current();
    void shutdown();
private:
    std::mutex mutex_;
    std::shared_ptr<ElevationHandoff> current_;
    bool stopping_ = false;
};

// The child authenticates the pipe server before duplicating handles or
// reading settings. Its abort event remains live until the parent exits.
class ElevatedChild {
public:
    static std::unique_ptr<ElevatedChild> receive(const std::wstring& id, DWORD parent_pid,
        uint64_t parent_created, std::string* error, bool require_elevated = true);
    ~ElevatedChild();
    ElevationSnapshot snapshot;
    std::shared_ptr<void> identity_user_token;
    bool announce_ready(const std::string& fingerprint, std::string* error);
    bool cancelled() const;
private:
    struct Impl;
    ElevatedChild();
    std::unique_ptr<Impl> impl_;
};

} // namespace chast::host
