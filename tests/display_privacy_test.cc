#include "chast/display_privacy.h"
#include "win/display_topology.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", text);
    if (!ok) ++failures;
}
struct State {
    std::vector<std::string> calls;
    std::string failure;
    bool restore_fails = false;
};
class Backend final : public chast::DisplayPrivacyBackend {
public:
    explicit Backend(std::shared_ptr<State> state) : state_(std::move(state)) {}
    bool step(const char* action, std::string* error) {
        state_->calls.emplace_back(action);
        if (state_->failure == action || (state_->restore_fails && std::string(action) == "restore")) {
            *error = std::string(action) + " failed";
            return false;
        }
        return true;
    }
    bool arm(std::string* error) override { return step("arm", error); }
    bool prepare(std::wstring* device, std::string* error) override {
        *device = L"verified-monitor-interface";
        return step("prepare", error);
    }
    bool isolate(std::string* error) override { return step("isolate", error); }
    bool restore(std::string* error) override { return step("restore", error); }
    bool intact(std::string* error) override { return step("intact", error); }
private:
    std::shared_ptr<State> state_;
};
void lifecycle() {
    for (const std::string failed : {"", "arm", "prepare", "probe", "isolate", "commit", "intact"}) {
        auto state = std::make_shared<State>();
        state->failure = failed;
        std::string error;
        {
            chast::VirtualDisplayPrivacy privacy(std::make_unique<Backend>(state));
            auto capture = [&](const std::wstring& device, bool commit, std::string* e) {
                check(device == L"verified-monitor-interface", "capture uses verified monitor identity");
                const std::string action = commit ? "commit" : "probe";
                state->calls.push_back(action);
                if (failed == action) { *e = action + " failed"; return false; }
                return true;
            };
            check(privacy.enable(capture, &error) == failed.empty(), "activation result reflects every failure boundary");
            check(privacy.active() == failed.empty(), "only fully proven privacy is active");
            check(privacy.topology_may_have_changed() == (failed != "arm"),
                  "an arm refusal leaves the existing GPU stream untouched");
            if (failed.empty()) {
                const std::vector<std::string> expected = {"arm", "prepare", "probe", "isolate", "commit", "intact"};
                check(state->calls == expected, "recovery and GPU proof precede physical-display disable");
                check(privacy.enable(capture, &error), "repeated enable checks integrity without changing topology");
            } else {
                check(!privacy.recovery_pending(), "failed activation restores before returning");
                check(!error.empty(), "failure detail survives rollback");
                if (failed == "arm") check(state->calls.size() == 1, "no topology mutation when recovery cannot arm");
                else check(state->calls.back() == "restore", "rollback runs after every post-arm failure");
                if (failed == "probe") check(state->calls == std::vector<std::string>{"arm", "prepare", "probe", "restore"},
                    "unsupported GPU never disables physical displays");
            }
        }
        if (failed.empty()) check(state->calls.back() == "restore", "disconnect/destruction restores displays");
    }
    auto state = std::make_shared<State>();
    state->failure = "commit";
    state->restore_fails = true;
    std::string error;
    chast::VirtualDisplayPrivacy privacy(std::make_unique<Backend>(state));
    auto capture = [&](const std::wstring&, bool commit, std::string* e) {
        if (commit) { *e = "GPU reset"; return false; }
        return true;
    };
    check(!privacy.enable(capture, &error) && privacy.recovery_pending(), "failed rollback retains recovery ownership");
    check(error.find("GPU reset") != std::string::npos && error.find("restore failed") != std::string::npos,
          "reports both the stream failure and outstanding restore");
    const size_t count = state->calls.size();
    check(!privacy.enable(capture, &error) && state->calls.size() == count,
          "cannot overwrite a pending recovery snapshot");
    state->restore_fails = false;
    check(privacy.disable(&error) && !privacy.recovery_pending(), "recovery can be retried after a transient failure");
    const size_t restored = state->calls.size();
    check(privacy.disable(&error) && state->calls.size() == restored, "repeated disable is harmless");
    state = std::make_shared<State>();
    chast::VirtualDisplayPrivacy exceptional(std::make_unique<Backend>(state));
    check(!exceptional.enable([](const std::wstring&, bool, std::string*) -> bool {
        throw 1;
    }, &error) && state->calls.back() == "restore", "an exception in GPU setup also restores displays");
}

chast::display_topology::Snapshot topology() {
    chast::display_topology::Snapshot snapshot;
    for (UINT i = 0; i < 2; ++i) {
        DISPLAYCONFIG_MODE_INFO source{}, target{};
        source.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
        source.id = i + 10;
        source.adapterId = {i + 100, 0};
        source.sourceMode.width = 1920;
        source.sourceMode.height = 1080;
        source.sourceMode.position = {static_cast<LONG>(1920 * i), 0};
        target.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
        target.id = i + 20;
        target.adapterId = source.adapterId;
        snapshot.modes.push_back(source);
        snapshot.modes.push_back(target);
        DISPLAYCONFIG_PATH_INFO path{};
        path.flags = DISPLAYCONFIG_PATH_ACTIVE;
        path.sourceInfo.id = source.id;
        path.sourceInfo.adapterId = source.adapterId;
        path.sourceInfo.modeInfoIdx = 2 * i;
        path.targetInfo.id = target.id;
        path.targetInfo.adapterId = target.adapterId;
        path.targetInfo.modeInfoIdx = 2 * i + 1;
        snapshot.paths.push_back(path);
    }
    return snapshot;
}
void topology_tests() {
    auto saved = topology();
    chast::display_topology::Snapshot only;
    std::string error;
    check(chast::display_topology::valid(saved), "accepts original topology with two adapters");
    check(chast::display_topology::isolate(saved, 1, {2560, 1440, 60}, only, &error), "constructs virtual-only topology");
    check(only.paths.size() == 1 && only.paths[0].targetInfo.id == 21 &&
          only.paths[0].sourceInfo.modeInfoIdx == 0 && only.modes.size() == 1,
          "only selected monitor survives and mode index is remapped");
    check(only.modes[0].sourceMode.width == 2560 && only.modes[0].sourceMode.height == 1440 &&
          only.modes[0].sourceMode.position.x == 0 && only.modes[0].sourceMode.position.y == 0,
          "virtual monitor becomes primary at the selected resolution");
    check(only.paths[0].targetInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID,
          "Windows chooses matching target signal timing");
    check(saved.paths.size() == 2 && saved.modes[2].sourceMode.position.x == 1920,
          "original restore snapshot is unchanged");
    check(!chast::display_topology::isolate(saved, 2, {1920, 1080, 60}, only, &error), "rejects missing virtual target");
    check(!chast::display_topology::isolate(saved, 1, {0, 1080, 60}, only, &error), "rejects empty mode");
    saved.paths[0].sourceInfo.modeInfoIdx = 99;
    check(!chast::display_topology::valid(saved), "rejects out-of-bounds restore mode");
    saved = topology(); saved.modes[0].adapterId.LowPart = 999;
    check(!chast::display_topology::valid(saved), "rejects wrong-adapter source modes");
    saved = topology(); saved.modes[3].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    check(!chast::display_topology::valid(saved), "rejects invalid target mode type");
    saved = topology(); saved.paths[0].flags = 0;
    check(!chast::display_topology::valid(saved), "rejects inactive restore paths");
    check(!chast::is_usbmmidd_monitor(L"USB Mobile Monitor"), "friendly name alone cannot identify a virtual monitor");
    check(chast::run_display_recovery_guard(0, nullptr) == 2, "helper rejects absent inherited handles without display changes");
    wchar_t exe[] = L"guard", bad[] = L"18446744073709551616";
    wchar_t* args[] = {exe, bad, bad, bad, bad, bad};
    check(chast::run_display_recovery_guard(6, args) == 2, "helper rejects overflowing handle values");
    const HANDLE lease = CreateMutexW(nullptr, FALSE, L"Local\\chast-display-privacy-v1");
    check(lease && chast::virtual_display_privacy_busy(), "recovery lease is visible to other display operations");
    if (lease) CloseHandle(lease);
}
}  // namespace

int main() {
    lifecycle();
    topology_tests();
    return failures ? 1 : 0;
}
