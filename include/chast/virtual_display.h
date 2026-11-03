#pragma once

// Virtual display support for headless hosts.
//
// A machine with no monitor gives the capture API nothing to read. This
// module detects an installed provider, reports whether a virtual monitor is
// active, and reserves the install/toggle interface for the elevated slices.
// Detection and classification are split so the decisions can be tested on a
// machine without a driver.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace chast {

// When the worker makes sure a virtual display exists before capturing.
// Off: only the operator enables one from the tray. Auto: create one when the
// machine has no active display (headless). On: always create one.
enum class VirtualDisplayPolicy {
    Off,
    Auto,
    On,
};

enum class VirtualDisplayProviderKind {
    None,
    Usbmmidd,
    Vdd,
};

enum class VirtualDisplayState {
    Unsupported,
    NotInstalled,
    Installed,
    Active,
    Error,
};

struct VirtualDisplayMode {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refresh_hz = 60;
};

struct VirtualDisplayStatus {
    VirtualDisplayProviderKind provider = VirtualDisplayProviderKind::None;
    VirtualDisplayState state = VirtualDisplayState::Unsupported;
    uint32_t active_monitors = 0;
    uint32_t max_monitors = 0;
    // UTF-8 diagnostic for the tray, logs and the status file.
    std::string detail;
};

inline constexpr uint32_t kMaxVirtualDisplayMonitors = 4;

// Facts gathered from the machine, kept separate from the decision so a
// hypothetical machine can be classified in tests.
struct VirtualDisplayProbe {
    bool platform_supported = false;
    // Why platform_supported is false; empty when it is true.
    std::string platform_reason;
    // A present display device carries the usbmmidd hardware id or service.
    bool adapter_present = false;
    // The usbmmidd UMDF service key exists, which means the driver is
    // installed even when no adapter is present right now.
    bool service_registered = false;
    uint32_t active_monitors = 0;
    // Non-empty when the scan itself failed; the classifier then reports an
    // error instead of pretending the driver is missing.
    std::string scan_error;
    // Non-empty when another IddCx virtual display driver is present.
    // Installing a second one would fight over the display topology, so the
    // caller reports the conflict instead of attempting coexistence.
    std::string conflicting_driver;
};

VirtualDisplayStatus classify_virtual_display_probe(const VirtualDisplayProbe& probe);

// Read-only scan of the machine: no driver is opened or changed.
VirtualDisplayProbe probe_virtual_display();
VirtualDisplayProviderKind detect_virtual_display_provider();

// Exact resolution first, then the nearest aspect ratio, then the nearest
// area. An empty list yields the 1920x1080 fallback.
VirtualDisplayMode select_virtual_display_mode(const std::vector<VirtualDisplayMode>& modes,
                                               uint32_t width, uint32_t height);

// usbmmidd's documented default modes, also the fallback while the driver is
// not active and its live list cannot be read.
const std::vector<VirtualDisplayMode>& usbmmidd_default_modes();

const char* virtual_display_provider_name(VirtualDisplayProviderKind kind);
const char* virtual_display_state_name(VirtualDisplayState state);

// WIDTHxHEIGHT, normalized to lowercase without spaces. Width must be
// 640..7680 and height 480..4320; anything else is rejected so a typo cannot
// ask the driver for an absurd mode.
bool parse_virtual_display_resolution(const std::string& input, std::string& normalized);

enum class VirtualDisplayAction {
    // Nothing to do: the policy is off, or a prior check already ruled the
    // machine out (unsupported, conflicting driver, no active display).
    None,
    // The policy wants a virtual display but the machine cannot provide one
    // without an explicit install or operator action.
    Skip,
    // Make the provider match target_monitors and apply the preferred mode.
    Ensure,
};

struct VirtualDisplayPlan {
    VirtualDisplayAction action = VirtualDisplayAction::None;
    uint32_t target_monitors = 0;
    VirtualDisplayMode mode{};
    bool apply_mode = false;
    // Why the plan was skipped, or what it is about to do. UTF-8 for logs.
    std::string reason;
};

// Decides what the worker should do before capturing. Pure: the probe facts
// and the mode list come from the caller so the headless and installed cases
// can be exercised without a driver.
VirtualDisplayPlan plan_virtual_display(VirtualDisplayPolicy policy,
                                        uint32_t requested_monitors,
                                        const std::string& requested_resolution,
                                        const std::vector<VirtualDisplayMode>& modes,
                                        const VirtualDisplayStatus& status,
                                        bool active_display_present);

struct VirtualDisplayPrepareResult {
    VirtualDisplayPlan plan;
    // Status after any change; the status before the call when nothing ran.
    VirtualDisplayStatus status;
    bool monitors_applied = false;
    bool mode_applied = false;
    // One UTF-8 line for the worker log and the tray detail.
    std::string detail;
};

class VirtualDisplayProvider {
  public:
    virtual ~VirtualDisplayProvider() = default;
    virtual VirtualDisplayProviderKind kind() const = 0;
    virtual VirtualDisplayStatus status() const = 0;
    virtual std::vector<VirtualDisplayMode> modes() const = 0;
    virtual bool install(std::string* error) = 0;
    virtual bool uninstall(std::string* error) = 0;
    virtual bool set_monitor_count(uint32_t count, std::string* error) = 0;
    virtual bool select_mode(const VirtualDisplayMode& mode, std::string* error) = 0;
};

// Executes the plan through the provider. Never fails the session: a missing
// driver or a provider error is reported in detail and the caller continues.
VirtualDisplayPrepareResult prepare_virtual_display(
    VirtualDisplayPolicy policy, uint32_t requested_monitors,
    const std::string& requested_resolution, VirtualDisplayProvider* provider,
    bool active_display_present);

// Only Usbmmidd is implemented; None and Vdd return nullptr. The provider is
// usable before the driver is installed: status() reports NotInstalled and the
// elevated actions verify the packaged driver before changing the machine.
std::unique_ptr<VirtualDisplayProvider> make_virtual_display_provider(
    VirtualDisplayProviderKind kind, const std::wstring& package_dir);

// <executable directory>\drivers\usbmmidd_v2, the layout the installer writes.
std::wstring default_virtual_display_package_dir();

// ----- driver package and install record (PLAT-01b) -----

enum class VirtualDisplayPackageStatus {
    Ok,
    Unsupported,
    Missing,
    HashMismatch,
    SignatureMismatch,
};

struct VirtualDisplayManifestEntry {
    std::string sha256;
    // Relative path normalized to '/' separators.
    std::string path;
};

// Parses a sha256sum-style manifest: "<64 hex>  <relative path>" per line,
// with optional '#' comments and blank lines. Paths must stay inside the
// package directory.
bool parse_virtual_display_manifest(const std::string& text,
                                    std::vector<VirtualDisplayManifestEntry>* entries,
                                    std::string* error);

bool sha256_file_hex(const std::wstring& path, std::string* hex, std::string* error);

// The package must contain deviceinstaller64.exe, usbmmidd.inf, usbmmidd.cat,
// x64/usbmmidd.dll, Win32/usbmmidd.dll, License.txt and manifest.sha256.
// Every manifest entry is hashed, and deviceinstaller64.exe must carry a
// valid Authenticode signature. The driver catalog is left to Plug and Play,
// which refuses an invalid driver package during installation.
VirtualDisplayPackageStatus verify_virtual_display_package(
    const std::wstring& package_dir, std::string* detail,
    std::string* package_version = nullptr, std::string* package_sha256 = nullptr);

bool verify_authenticode(const std::wstring& path, std::string* error);

// %ProgramData%\chast\virtual-display.json. The elevated CLI writes it; the
// tray and worker only read it.
struct VirtualDisplayInstallRecord {
    std::string provider = "usbmmidd";
    std::string package_version;
    std::string package_sha256;
    std::string installed_at;
    bool owned = true;
    std::string last_error;
};

std::string virtual_display_install_json(const VirtualDisplayInstallRecord& record);
bool parse_virtual_display_install_json(const std::string& text,
                                        VirtualDisplayInstallRecord* record,
                                        std::string* error);
std::string virtual_display_timestamp_utc();

enum class VdisplayAction {
    None,
    Install,
    Uninstall,
    SetCount,
    Status,
};

struct VdisplayCommand {
    VdisplayAction action = VdisplayAction::None;
    std::wstring package_dir;
    uint32_t count = 1;
    std::wstring log_path;
    std::string error;
};

// Parses the arguments after "--vdisplay". install needs --package and an
// optional --count 1..4 (default 1); set-count takes 0..4; status needs
// neither. --log overrides the default %ProgramData%\chast\vdisplay.log.
VdisplayCommand parse_vdisplay_command(const std::vector<std::wstring>& args);

// Runs a parsed command, logs one line per step and returns the documented
// exit code. install/uninstall/set-count require an elevated token and never
// run automatically from the service.
int run_vdisplay_command(const std::vector<std::wstring>& args);

inline constexpr int kVdisplayExitOk = 0;
inline constexpr int kVdisplayExitUsage = 2;
inline constexpr int kVdisplayExitPackage = 3;
inline constexpr int kVdisplayExitSignature = 4;
inline constexpr int kVdisplayExitInstall = 5;
inline constexpr int kVdisplayExitNoDevice = 6;
inline constexpr int kVdisplayExitUninstall = 7;
inline constexpr int kVdisplayExitUnsupported = 8;
inline constexpr int kVdisplayExitCount = 9;

bool virtual_display_platform_supported(std::string* reason);

}  // namespace chast
