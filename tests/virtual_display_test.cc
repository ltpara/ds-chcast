// Pure-logic checks for virtual display detection and mode selection. Live
// probes are read-only, package checks run in a temporary directory and no
// driver or real %ProgramData% file is touched.

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "chast/virtual_display.h"

namespace {

using chast::VirtualDisplayAction;
using chast::VirtualDisplayMode;
using chast::VirtualDisplayPolicy;
using chast::VirtualDisplayState;
using chast::VirtualDisplayStatus;

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

void test_platform() {
    std::string reason;
    const bool supported = chast::virtual_display_platform_supported(&reason);
    check(supported || !reason.empty(), "an unsupported platform explains why");
    if (supported) {
        check(reason.empty(), "a supported platform has no complaint");
    }
    check(chast::virtual_display_platform_supported(nullptr) == supported,
          "the support check does not need a reason buffer");
}

void test_classification() {
    chast::VirtualDisplayProbe probe;
    probe.platform_supported = false;
    probe.platform_reason = "usbmmidd ships an x64 driver only";
    chast::VirtualDisplayStatus status = chast::classify_virtual_display_probe(probe);
    check(status.state == chast::VirtualDisplayState::Unsupported,
          "an unsupported platform is reported as unsupported");
    check(status.provider == chast::VirtualDisplayProviderKind::None,
          "an unsupported platform selects no provider");
    check(status.detail == probe.platform_reason, "the platform reason reaches the status");

    probe = {};
    probe.platform_supported = true;
    probe.platform_reason.clear();
    probe.scan_error = "display devices could not be enumerated";
    status = chast::classify_virtual_display_probe(probe);
    check(status.state == chast::VirtualDisplayState::Error,
          "a failed scan is an error, not a missing driver");
    check(status.detail == probe.scan_error, "the scan error reaches the status");

    probe = {};
    probe.platform_supported = true;
    probe.conflicting_driver = "Root\\MttVDD\\0000";
    status = chast::classify_virtual_display_probe(probe);
    check(status.state == chast::VirtualDisplayState::Error,
          "a conflicting IddCx driver is an error");
    check(status.provider == chast::VirtualDisplayProviderKind::None,
          "a conflict does not select usbmmidd");
    check(contains(status.detail, "MttVDD"), "the conflict names the other driver");

    probe = {};
    probe.platform_supported = true;
    status = chast::classify_virtual_display_probe(probe);
    check(status.provider == chast::VirtualDisplayProviderKind::Usbmmidd,
          "a supported machine without a driver selects usbmmidd for installation");
    check(status.state == chast::VirtualDisplayState::NotInstalled,
          "a supported machine without a driver reports not installed");
    check(status.max_monitors == chast::kMaxVirtualDisplayMonitors,
          "the driver limit is exposed before installation");

    probe.service_registered = true;
    status = chast::classify_virtual_display_probe(probe);
    check(status.state == chast::VirtualDisplayState::Installed,
          "a registered driver service counts as installed");

    probe.adapter_present = true;
    probe.active_monitors = 2;
    status = chast::classify_virtual_display_probe(probe);
    check(status.state == chast::VirtualDisplayState::Active,
          "present monitors are reported as active");
    check(status.active_monitors == 2, "the active monitor count is reported");

    probe.active_monitors = 9;
    status = chast::classify_virtual_display_probe(probe);
    check(status.active_monitors == chast::kMaxVirtualDisplayMonitors,
          "an impossible count is clamped to the driver limit");
}

void test_mode_selection() {
    using chast::VirtualDisplayMode;
    const std::vector<VirtualDisplayMode> modes = {
        {1280, 720, 60}, {1920, 1080, 60}, {1920, 1080, 120}, {2560, 1080, 60},
        {1920, 1200, 60}, {1280, 1024, 60},
    };
    VirtualDisplayMode mode = chast::select_virtual_display_mode(modes, 1920, 1080);
    check(mode.width == 1920 && mode.height == 1080 && mode.refresh_hz == 120,
          "an exact resolution wins and keeps the highest refresh rate");
    mode = chast::select_virtual_display_mode(modes, 1600, 1000);
    check(mode.width == 1920 && mode.height == 1200,
          "the closest aspect ratio wins when the resolution is missing");
    mode = chast::select_virtual_display_mode(modes, 1366, 768);
    check(mode.width == 1280 && mode.height == 720,
          "a matching aspect ratio prefers the closest area");
    mode = chast::select_virtual_display_mode({}, 1920, 1080);
    check(mode.width == 1920 && mode.height == 1080 && mode.refresh_hz == 60,
          "an empty mode list returns the 1920x1080 fallback");
    mode = chast::select_virtual_display_mode(modes, 0, 0);
    check(mode.width == modes.front().width && mode.height == modes.front().height,
          "an unknown request keeps the first supported mode");
    check(chast::select_virtual_display_mode({{0, 0, 0}, {1280, 720, 60}}, 1024, 768).width ==
              1280,
          "a zero-sized mode is skipped");
}

void test_names() {
    check(std::string(chast::virtual_display_provider_name(
              chast::VirtualDisplayProviderKind::Usbmmidd)) == "usbmmidd",
          "usbmmidd is spelled for logs and settings");
    check(std::string(chast::virtual_display_provider_name(
              chast::VirtualDisplayProviderKind::None)) == "none",
          "the empty provider is spelled none");
    check(std::string(chast::virtual_display_state_name(
              chast::VirtualDisplayState::NotInstalled)) == "not_installed",
          "the not-installed state is spelled for logs");
}

void test_default_modes() {
    const std::vector<chast::VirtualDisplayMode>& modes = chast::usbmmidd_default_modes();
    bool has_1080p = false;
    bool all_sane = true;
    for (const chast::VirtualDisplayMode& mode : modes) {
        has_1080p = has_1080p || (mode.width == 1920 && mode.height == 1080);
        all_sane = all_sane && mode.width >= 640 && mode.height >= 480 && mode.refresh_hz >= 60;
    }
    check(!modes.empty() && modes.size() <= 16,
          "the documented default mode list is present without duplicates");
    check(has_1080p, "the driver default list contains 1920x1080");
    check(all_sane, "every default mode has a usable size and refresh rate");
}

void test_provider_factory() {
    check(chast::make_virtual_display_provider(chast::VirtualDisplayProviderKind::None, L"") ==
              nullptr,
          "no provider is built for none");
    check(chast::make_virtual_display_provider(chast::VirtualDisplayProviderKind::Vdd, L"") ==
              nullptr,
          "the unimplemented VDD provider is not built");
    std::error_code ec;
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path(ec) / L"chast-vdisplay-missing-package";
    std::filesystem::remove_all(missing, ec);
    std::unique_ptr<chast::VirtualDisplayProvider> provider =
        chast::make_virtual_display_provider(chast::VirtualDisplayProviderKind::Usbmmidd,
                                             missing.wstring());
    check(provider != nullptr &&
              provider->kind() == chast::VirtualDisplayProviderKind::Usbmmidd,
          "the usbmmidd provider is built before the driver is installed");
    if (provider == nullptr) {
        return;
    }
    std::string error;
    check(!provider->install(&error) && !error.empty(),
          "installation refuses a missing driver package");
    check(!provider->set_monitor_count(5, &error) && contains(error, "at most"),
          "the monitor count is bounded even before installation");
    check(!provider->select_mode({0, 0, 60}, &error) && contains(error, "empty"),
          "an empty mode is rejected before a display is touched");
    check(!provider->modes().empty(), "the provider exposes at least one mode");

    const chast::VirtualDisplayStatus status = provider->status();
    check(!status.detail.empty(), "the live status explains itself");
    check(status.state == chast::VirtualDisplayState::Unsupported ||
              status.state == chast::VirtualDisplayState::Error ||
              status.provider == chast::VirtualDisplayProviderKind::Usbmmidd,
          "the live scan classifies consistently");

    const chast::VirtualDisplayProbe probe = chast::probe_virtual_display();
    check(probe.platform_supported || !probe.platform_reason.empty(),
          "a failed platform check carries a reason");
    if (probe.platform_supported && probe.scan_error.empty() &&
        probe.conflicting_driver.empty()) {
        const chast::VirtualDisplayProviderKind detected =
            chast::detect_virtual_display_provider();
        check(detected == chast::VirtualDisplayProviderKind::None ||
                  detected == chast::VirtualDisplayProviderKind::Usbmmidd,
              "detection never reports the unimplemented VDD provider");
    }
}

class FakeProvider final : public chast::VirtualDisplayProvider {
  public:
    VirtualDisplayStatus current;
    std::vector<VirtualDisplayMode> supported = chast::usbmmidd_default_modes();
    bool count_result = true;
    bool mode_result = true;
    bool report_monitors = true;
    int count_calls = 0;
    int mode_calls = 0;
    uint32_t last_count = 0;
    VirtualDisplayMode last_mode;

    chast::VirtualDisplayProviderKind kind() const override {
        return chast::VirtualDisplayProviderKind::Usbmmidd;
    }
    VirtualDisplayStatus status() const override { return current; }
    std::vector<VirtualDisplayMode> modes() const override { return supported; }
    bool install(std::string*) override { return false; }
    bool uninstall(std::string*) override { return false; }
    bool set_monitor_count(uint32_t count, std::string* error) override {
        ++count_calls;
        last_count = count;
        if (!count_result) {
            if (error != nullptr) {
                *error = "deviceinstaller refused";
            }
            return false;
        }
        if (report_monitors) {
            current.state = VirtualDisplayState::Active;
            current.active_monitors = count;
        }
        return true;
    }
    bool select_mode(const VirtualDisplayMode& mode, std::string* error) override {
        ++mode_calls;
        last_mode = mode;
        if (!mode_result) {
            if (error != nullptr) {
                *error = "mode rejected";
            }
            return false;
        }
        return true;
    }
};

VirtualDisplayStatus installed_status(uint32_t active) {
    VirtualDisplayStatus status;
    status.provider = chast::VirtualDisplayProviderKind::Usbmmidd;
    status.state = active > 0 ? VirtualDisplayState::Active : VirtualDisplayState::Installed;
    status.active_monitors = active;
    status.max_monitors = chast::kMaxVirtualDisplayMonitors;
    return status;
}

void test_policy_plan() {
    const std::vector<VirtualDisplayMode>& modes = chast::usbmmidd_default_modes();
    chast::VirtualDisplayPlan plan =
        chast::plan_virtual_display(VirtualDisplayPolicy::Off, 1, "1920x1080", modes,
                                    installed_status(0), false);
    check(plan.action == VirtualDisplayAction::None && contains(plan.reason, "off"),
          "off plans nothing");

    VirtualDisplayStatus unsupported;
    unsupported.state = VirtualDisplayState::Unsupported;
    unsupported.detail = "x64 only";
    plan = chast::plan_virtual_display(VirtualDisplayPolicy::On, 1, "1920x1080", modes,
                                       unsupported, false);
    check(plan.action == VirtualDisplayAction::Skip && contains(plan.reason, "x64 only"),
          "an unsupported platform is skipped with its reason");

    VirtualDisplayStatus conflict;
    conflict.state = VirtualDisplayState::Error;
    conflict.detail = "another virtual display driver is present: MttVDD";
    plan = chast::plan_virtual_display(VirtualDisplayPolicy::On, 1, "1920x1080", modes, conflict,
                                       false);
    check(plan.action == VirtualDisplayAction::Skip && contains(plan.reason, "MttVDD"),
          "a conflicting driver is skipped instead of coexisting");

    VirtualDisplayStatus missing;
    missing.state = VirtualDisplayState::NotInstalled;
    missing.provider = chast::VirtualDisplayProviderKind::Usbmmidd;
    plan = chast::plan_virtual_display(VirtualDisplayPolicy::Auto, 1, "1920x1080", modes, missing,
                                       false);
    check(plan.action == VirtualDisplayAction::Skip &&
              contains(plan.reason, "--virtual-display"),
          "a missing driver points at the installer instead of failing");

    plan = chast::plan_virtual_display(VirtualDisplayPolicy::Auto, 1, "1920x1080", modes,
                                       installed_status(0), true);
    check(plan.action == VirtualDisplayAction::Skip && contains(plan.reason, "already active"),
          "auto leaves a machine with a working display alone");

    plan = chast::plan_virtual_display(VirtualDisplayPolicy::Auto, 2, "2560x1440", modes,
                                       installed_status(0), false);
    check(plan.action == VirtualDisplayAction::Ensure && plan.target_monitors == 2,
          "auto enables the requested count on a headless machine");
    check(plan.apply_mode && plan.mode.width == 2560 && plan.mode.height == 1440,
          "the preferred mode is selected from the supported list");
    check(contains(plan.reason, "2 virtual monitor(s)") && contains(plan.reason, "2560x1440"),
          "the plan explains the count and mode it will apply");

    plan = chast::plan_virtual_display(VirtualDisplayPolicy::On, 1, "1920x1080", modes,
                                       installed_status(0), true);
    check(plan.action == VirtualDisplayAction::Ensure,
          "on enables a virtual display even when a physical one is active");

    plan = chast::plan_virtual_display(VirtualDisplayPolicy::On, 9, "1920x1080", modes,
                                       installed_status(0), false);
    check(plan.target_monitors == chast::kMaxVirtualDisplayMonitors,
          "a plan never exceeds the driver limit");
    plan = chast::plan_virtual_display(VirtualDisplayPolicy::On, 0, "1920x1080", modes,
                                       installed_status(0), false);
    check(plan.target_monitors == 1, "a zero request falls back to one monitor");

    plan = chast::plan_virtual_display(VirtualDisplayPolicy::On, 1, "1920", modes,
                                       installed_status(0), false);
    check(plan.action == VirtualDisplayAction::Ensure && !plan.apply_mode,
          "an invalid resolution still enables the display without a mode change");

    plan = chast::plan_virtual_display(VirtualDisplayPolicy::On, 1, "1920x1080", {}, 
                                       installed_status(0), false);
    check(plan.apply_mode && plan.mode.width == 1920 && plan.mode.height == 1080,
          "an empty mode list falls back to 1920x1080");
}

void test_prepare() {
    FakeProvider provider;
    provider.current = installed_status(0);

    chast::VirtualDisplayPrepareResult result =
        chast::prepare_virtual_display(VirtualDisplayPolicy::Off, 1, "1920x1080", &provider,
                                       false);
    check(result.plan.action == VirtualDisplayAction::None && provider.count_calls == 0 &&
              contains(result.detail, "off"),
          "off never calls the provider");

    result = chast::prepare_virtual_display(VirtualDisplayPolicy::Auto, 1, "1920x1080", nullptr,
                                            false);
    check(contains(result.detail, "no virtual display provider"),
          "a missing provider is reported without failing");

    result = chast::prepare_virtual_display(VirtualDisplayPolicy::Auto, 1, "1920x1080", &provider,
                                            true);
    check(provider.count_calls == 0 && provider.mode_calls == 0 &&
              contains(result.detail, "already active"),
          "auto with a working display touches nothing");

    result = chast::prepare_virtual_display(VirtualDisplayPolicy::Auto, 2, "2560x1440", &provider,
                                            false);
    check(result.monitors_applied && result.mode_applied,
          "a headless auto start enables monitors and applies the mode");
    check(provider.last_count == 2 && provider.last_mode.width == 2560 &&
              provider.last_mode.height == 1440,
          "the provider receives the planned count and mode");
    check(result.status.active_monitors == 2 && contains(result.detail, "ready: 2"),
          "the result reports the monitors that are actually active");

    FakeProvider already;
    already.current = installed_status(1);
    result = chast::prepare_virtual_display(VirtualDisplayPolicy::Auto, 1, "1920x1080", &already,
                                            false);
    check(already.count_calls == 0 && already.mode_calls == 1 && result.mode_applied,
          "an already active display keeps its count and still gets the mode");

    FakeProvider failing;
    failing.current = installed_status(0);
    failing.count_result = false;
    result = chast::prepare_virtual_display(VirtualDisplayPolicy::On, 2, "1920x1080", &failing,
                                            false);
    check(!result.monitors_applied && failing.mode_calls == 0 &&
              contains(result.detail, "deviceinstaller refused"),
          "a failed enable is reported and no mode is applied");

    FakeProvider mode_failing;
    mode_failing.current = installed_status(0);
    mode_failing.mode_result = false;
    result = chast::prepare_virtual_display(VirtualDisplayPolicy::On, 1, "1920x1080",
                                            &mode_failing, false);
    check(result.monitors_applied && !result.mode_applied &&
              contains(result.detail, "preferred mode"),
          "a mode failure keeps the enabled display and explains the mode error");

    FakeProvider no_mode;
    no_mode.current = installed_status(0);
    result = chast::prepare_virtual_display(VirtualDisplayPolicy::On, 1, "bad", &no_mode, false);
    check(result.monitors_applied && no_mode.mode_calls == 0 &&
              !contains(result.detail, " at "),
          "an invalid resolution skips the mode call");

    FakeProvider stuck;
    stuck.current = installed_status(0);
    stuck.report_monitors = false;
    result = chast::prepare_virtual_display(VirtualDisplayPolicy::On, 1, "1920x1080", &stuck,
                                            false);
    check(result.monitors_applied && result.status.active_monitors == 0 &&
              contains(result.detail, "no active virtual monitor"),
          "a provider that reports no monitor afterwards is not called ready");
}

// ----- PLAT-01b: driver package, install record and command line -----

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::error_code ec;
        path = std::filesystem::temp_directory_path(ec) /
               ("chast-vdisplay-test-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_bytes(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool set_protected_dacl(const std::filesystem::path& path, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor,
                                                             nullptr) == FALSE) {
        return false;
    }
    BOOL present = FALSE;
    PACL acl = nullptr;
    BOOL defaulted = FALSE;
    const bool ok =
        GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) != FALSE &&
        SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
    LocalFree(descriptor);
    return ok;
}

// Package fixtures live under %TEMP%, which inherits whatever DACL the
// machine gives it; a sandboxed desktop can add another writable account and
// the product then rightly refuses the directory. Give the fixture an
// explicit private DACL: SYSTEM, Administrators and the owner, which is the
// current user.
bool harden_package_dir(const std::filesystem::path& path) {
    return set_protected_dacl(path,
                              L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;CO)");
}

void test_manifest_parsing() {
    std::vector<chast::VirtualDisplayManifestEntry> entries;
    std::string error;
    const std::string digest(64, 'a');
    const std::string text = "# generated by tools/fetch-usbmmidd.ps1\r\n" + digest +
                             "  usbmmidd.inf\r\n" + digest + " *x64/usbmmidd.dll\r\n\r\n";
    check(chast::parse_virtual_display_manifest(text, &entries, &error) && entries.size() == 2,
          "a sha256sum-style manifest with comments and CRLF parses");
    check(!entries.empty() && entries[1].path == "x64/usbmmidd.dll",
          "the binary-mode marker is stripped from the path");
    entries.clear();
    check(!chast::parse_virtual_display_manifest(digest + "  ../outside.dll\n", &entries, &error) &&
              !error.empty(),
          "a manifest path may not escape the package");
    entries.clear();
    check(!chast::parse_virtual_display_manifest(digest + "  C:/outside.dll\n", &entries, &error),
          "an absolute manifest path is rejected");
    entries.clear();
    check(!chast::parse_virtual_display_manifest("not-a-hash  usbmmidd.inf\n", &entries, &error),
          "a malformed digest is rejected");
    entries.clear();
    check(!chast::parse_virtual_display_manifest(
              digest + "  usbmmidd.inf\n" + digest + "  usbmmidd.inf\n", &entries, &error),
          "a duplicate manifest entry is rejected");
    entries.clear();
    check(!chast::parse_virtual_display_manifest("", &entries, &error),
          "an empty manifest is rejected");
}

void test_sha256() {
    TempDir temp;
    const std::filesystem::path file = temp.path / L"abc.txt";
    write_bytes(file, "abc");
    std::string hex;
    std::string error;
    check(chast::sha256_file_hex(file.wstring(), &hex, &error) &&
              hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 matches the NIST abc vector");
    hex.clear();
    check(!chast::sha256_file_hex((temp.path / L"missing.bin").wstring(), &hex, &error) &&
              !error.empty(),
          "a missing file reports a hashing error");
}

void test_package_verification() {
    std::string reason;
    if (!chast::virtual_display_platform_supported(&reason)) {
        check(!reason.empty(), "the package test is skipped with a platform reason");
        return;
    }
    TempDir temp;
    check(harden_package_dir(temp.path), "the package fixture gets a private DACL");
    const std::vector<std::string> required = {
        "deviceinstaller64.exe", "usbmmidd.inf",    "usbmmidd.cat",
        "x64/usbmmidd.dll",      "win32/usbmmidd.dll", "license.txt",
    };
    std::string detail;
    check(chast::verify_virtual_display_package(temp.path.wstring(), &detail) ==
              chast::VirtualDisplayPackageStatus::Missing,
          "an empty package directory is missing files");
    for (const std::string& name : required) {
        write_bytes(temp.path / std::filesystem::path(name), "content of " + name);
    }
    detail.clear();
    check(chast::verify_virtual_display_package(temp.path.wstring(), &detail) ==
              chast::VirtualDisplayPackageStatus::Missing,
          "a package without manifest.sha256 is rejected");
    std::string manifest;
    for (const std::string& name : required) {
        std::string digest;
        std::string error;
        const bool hashed = chast::sha256_file_hex(
            (temp.path / std::filesystem::path(name)).wstring(), &digest, &error);
        const std::string what = "hashing " + name + " succeeds";
        check(hashed, what.c_str());
        manifest += digest + "  " + name + "\n";
    }
    write_bytes(temp.path / "manifest.sha256", manifest);
    detail.clear();
    check(chast::verify_virtual_display_package(temp.path.wstring(), &detail) ==
              chast::VirtualDisplayPackageStatus::SignatureMismatch,
          "a package whose catalog is unsigned is rejected");
    std::string broken = manifest;
    broken.replace(0, 64, std::string(64, '0'));
    write_bytes(temp.path / "manifest.sha256", broken);
    detail.clear();
    check(chast::verify_virtual_display_package(temp.path.wstring(), &detail) ==
              chast::VirtualDisplayPackageStatus::HashMismatch,
          "a file that does not match the manifest is rejected");
    write_bytes(temp.path / "manifest.sha256", manifest);
    write_bytes(temp.path / "SOURCE.txt",
                "URL: https://example.invalid/usbmmidd_v2.zip\n"
                "VERSION: 2.0.0.1\n"
                "SHA256: " +
                    std::string(64, 'b') + "\n");
    std::string version;
    std::string package_sha;
    detail.clear();
    check(chast::verify_virtual_display_package(temp.path.wstring(), &detail, &version,
                                                &package_sha) ==
                  chast::VirtualDisplayPackageStatus::SignatureMismatch &&
              version == "2.0.0.1" && package_sha == std::string(64, 'b'),
          "SOURCE.txt contributes the version and package hash before the signature check");
}

void test_install_record() {
    chast::VirtualDisplayInstallRecord record;
    record.package_version = "2.0.0.1";
    record.package_sha256 = std::string(64, 'a');
    record.installed_at = chast::virtual_display_timestamp_utc();
    record.owned = false;
    record.last_error = "line\nwith \"quotes\" and \\slash";
    const std::string json = chast::virtual_display_install_json(record);
    chast::VirtualDisplayInstallRecord parsed;
    std::string error;
    check(chast::parse_virtual_display_install_json(json, &parsed, &error),
          "an install record round-trips through JSON");
    check(parsed.provider == record.provider &&
              parsed.package_version == record.package_version &&
              parsed.package_sha256 == record.package_sha256 &&
              parsed.installed_at == record.installed_at && parsed.owned == record.owned &&
              parsed.last_error == record.last_error,
          "every install-record field survives the round trip");
    check(!chast::parse_virtual_display_install_json("{\"provider\":\"usbmmidd\"}", &parsed,
                                                     &error) &&
              !error.empty(),
          "a partial install record is rejected");
    check(!chast::parse_virtual_display_install_json("not json", &parsed, &error),
          "a non-object install record is rejected");

    const std::string stamp = chast::virtual_display_timestamp_utc();
    check(stamp.size() == 20 && stamp[4] == '-' && stamp[7] == '-' && stamp[10] == 'T' &&
              stamp[19] == 'Z',
          "the install timestamp is a UTC ISO-8601 second");
}

std::filesystem::path system_binary(const wchar_t* name) {
    wchar_t windows[MAX_PATH]{};
    const UINT size = GetWindowsDirectoryW(windows, MAX_PATH);
    std::filesystem::path path(std::wstring(windows, size));
    path /= L"System32";
    path /= name;
    return path;
}

// Finds an installed driver catalog whose signer is the WHQL publisher, the
// identity the package check requires. The scan is bounded so a machine with a
// large DriverStore cannot stall the test.
std::filesystem::path find_hardware_publisher_catalog() {
    const std::filesystem::path repository = system_binary(L"DriverStore") / L"FileRepository";
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        repository, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    int verified = 0;
    while (!ec && it != end && verified < 64) {
        std::error_code type_error;
        if (it->is_regular_file(type_error) &&
            _wcsicmp(it->path().extension().c_str(), L".cat") == 0) {
            ++verified;
            std::string error;
            if (chast::verify_catalog_signature(it->path().wstring(), &error)) {
                return it->path();
            }
        }
        it.increment(ec);
    }
    return {};
}

bool build_signed_package(const std::filesystem::path& package) {
    // The official package ships deviceinstaller64.exe unsigned; the WHQL
    // catalog carries the signature the installer trusts.
    const std::filesystem::path catalog = find_hardware_publisher_catalog();
    if (catalog.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(package, ec);
    if (CopyFileW(catalog.c_str(), (package / L"usbmmidd.cat").c_str(), TRUE) == FALSE) {
        return false;
    }
    const std::vector<std::string> files = {
        "deviceinstaller64.exe", "usbmmidd.inf",       "usbmmidd.cat",
        "x64/usbmmidd.dll",      "win32/usbmmidd.dll", "license.txt",
    };
    for (const std::string& name : files) {
        if (name != "usbmmidd.cat") {
            write_bytes(package / std::filesystem::path(name), "content of " + name);
        }
    }
    std::string manifest;
    for (const std::string& name : files) {
        std::string digest;
        std::string error;
        if (!chast::sha256_file_hex((package / std::filesystem::path(name)).wstring(), &digest,
                                    &error)) {
            return false;
        }
        manifest += digest + "  " + name + "\n";
    }
    write_bytes(package / "manifest.sha256", manifest);
    return true;
}

void test_package_guard() {
    std::string reason;
    if (!chast::virtual_display_platform_supported(&reason)) {
        check(!reason.empty(), "the guard test is skipped with a platform reason");
        return;
    }
    TempDir temp;
    const std::filesystem::path package = temp.path / L"package";
    if (!build_signed_package(package)) {
        check(true, "no WHQL catalog is installed; the guard test is skipped");
        return;
    }
    check(harden_package_dir(package), "the guarded package gets a private DACL");
    chast::VirtualDisplayPackageGuard guard;
    std::string detail;
    const chast::VirtualDisplayPackageStatus status = chast::verify_virtual_display_package(
        package.wstring(), &detail, nullptr, nullptr, &guard);
    check(status == chast::VirtualDisplayPackageStatus::Ok,
          "a package with a WHQL catalog verifies even though its installer is unsigned");
    if (status != chast::VirtualDisplayPackageStatus::Ok) {
        std::printf("     detail: %s\n", detail.c_str());
        return;
    }
    check(guard.active(), "verification holds the package open");
    const std::filesystem::path installer = package / L"deviceinstaller64.exe";
    HANDLE write = CreateFileW(installer.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(write == INVALID_HANDLE_VALUE, "a guarded installer cannot be opened for writing");
    if (write != INVALID_HANDLE_VALUE) {
        CloseHandle(write);
    }
    check(DeleteFileW(installer.c_str()) == FALSE, "a guarded installer cannot be deleted");
    guard.reset();
    check(!guard.active(), "resetting the guard releases the package");
    write = CreateFileW(installer.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    check(write != INVALID_HANDLE_VALUE, "the installer is writable once the guard is released");
    if (write != INVALID_HANDLE_VALUE) {
        CloseHandle(write);
    }
}

void test_package_reparse() {
    std::string reason;
    if (!chast::virtual_display_platform_supported(&reason)) {
        check(!reason.empty(), "the reparse test is skipped with a platform reason");
        return;
    }
    TempDir temp;
    const std::filesystem::path package = temp.path / L"package";
    if (!build_signed_package(package)) {
        check(true, "no WHQL catalog is installed; the reparse test is skipped");
        return;
    }
    check(harden_package_dir(package), "the linked package gets a private DACL");
    const std::filesystem::path link = temp.path / L"package-link";
    if (CreateSymbolicLinkW(link.c_str(), package.c_str(),
                            SYMBOLIC_LINK_FLAG_DIRECTORY |
                                SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE) {
        check(true, "directory links are unavailable; the reparse check is skipped");
        return;
    }
    std::string detail;
    check(chast::verify_virtual_display_package(link.wstring(), &detail) ==
              chast::VirtualDisplayPackageStatus::UntrustedAcl &&
              contains(detail, "reparse"),
          "a package reached through a reparse point is rejected");
}

class ScopedProgramData {
  public:
    explicit ScopedProgramData(const std::filesystem::path& path) {
        wchar_t buffer[32768]{};
        const DWORD size = GetEnvironmentVariableW(
            L"ProgramData", buffer, static_cast<DWORD>(std::size(buffer)));
        if (size > 0 && size < std::size(buffer)) {
            previous_.assign(buffer, size);
        }
        SetEnvironmentVariableW(L"ProgramData", path.c_str());
    }
    ~ScopedProgramData() {
        SetEnvironmentVariableW(L"ProgramData",
                                previous_.empty() ? nullptr : previous_.c_str());
    }
    ScopedProgramData(const ScopedProgramData&) = delete;
    ScopedProgramData& operator=(const ScopedProgramData&) = delete;

  private:
    std::wstring previous_;
};

bool set_users_write_acl(const std::filesystem::path& path) {
    return set_protected_dacl(path, L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GW;;;BU)");
}

bool set_legacy_record_acl(const std::filesystem::path& path) {
    return set_protected_dacl(path,
                              L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;CO)(A;;GR;;;BU)");
}

void test_state_directory_security() {
    TempDir temp;
    ScopedProgramData program_data(temp.path);
    std::string error;
    if (!chast::harden_virtual_display_state_directory(&error)) {
        check(!error.empty(), "state-directory hardening explains a failure");
        return;
    }
    check(chast::virtual_display_state_directory_trusted(),
          "a hardened state directory is trusted");
    chast::VirtualDisplayInstallRecord record;
    record.package_version = "2.0.0.1";
    record.package_sha256 = std::string(64, 'c');
    record.installed_at = chast::virtual_display_timestamp_utc();
    record.owned = true;
    record.last_error.clear();
    check(chast::write_install_record(record, &error),
          "the protected install record is written");
    chast::VirtualDisplayInstallRecord parsed;
    check(chast::read_install_record(&parsed) &&
              parsed.package_version == record.package_version && parsed.owned == record.owned,
          "the protected install record reads back");
    record.owned = false;
    check(chast::write_install_record(record, &error) && chast::read_install_record(&parsed) &&
              !parsed.owned,
          "a second write replaces the record atomically");
    size_t leftovers = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(temp.path / L"chast", ec)) {
        if (entry.path().filename().wstring().find(L".tmp-") != std::wstring::npos) {
            ++leftovers;
        }
    }
    check(leftovers == 0, "no temporary record file is left behind");

    const std::filesystem::path state = temp.path / L"chast" / L"virtual-display.json";
    DeleteFileW(state.c_str());
    write_bytes(state, chast::virtual_display_install_json(record));
    check(set_users_write_acl(state), "a planted record gets a Users-write DACL");
    check(!chast::read_install_record(&parsed), "the planted record is not trusted");
    check(chast::harden_virtual_display_state_directory(&error),
          "hardening succeeds over a planted record");
    check(GetFileAttributesW(state.c_str()) == INVALID_FILE_ATTRIBUTES,
          "hardening discards a record that other accounts could change");

    write_bytes(state, chast::virtual_display_install_json(record));
    check(set_legacy_record_acl(state), "a legacy record gets a CREATOR OWNER ACL");
    check(!chast::read_install_record(&parsed),
          "a legacy record is not trusted until it is migrated");
    check(chast::harden_virtual_display_state_directory(&error),
          "hardening migrates a legacy record");
    check(chast::read_install_record(&parsed) && !parsed.owned,
          "a legacy record written by Administrators survives migration");

    const std::filesystem::path raw = temp.path / L"raw-program-data";
    std::filesystem::create_directories(raw / L"chast", ec);
    write_bytes(raw / L"chast" / L"virtual-display.json",
                chast::virtual_display_install_json(record));
    ScopedProgramData raw_data(raw);
    check(!chast::virtual_display_state_directory_trusted(),
          "a plain ProgramData directory is not trusted");
    check(!chast::read_install_record(&parsed),
          "a record in an unprotected directory is ignored");
    std::string refused;
    check(!chast::write_install_record(record, &refused),
          "a record is not written into an unprotected directory");
}

void test_default_package_dir() {
    const std::wstring dir = chast::default_virtual_display_package_dir();
    const std::wstring suffix = L"\\drivers\\usbmmidd_v2";
    check(dir.size() > suffix.size() &&
              dir.compare(dir.size() - suffix.size(), suffix.size(), suffix) == 0,
          "the default package directory is drivers\\usbmmidd_v2 next to the executable");
}

void test_vdisplay_cli() {
    chast::VdisplayCommand command = chast::parse_vdisplay_command(
        {L"install", L"--package", L"C:\\chast\\drivers\\usbmmidd_v2", L"--count", L"3"});
    check(command.action == chast::VdisplayAction::Install && command.count == 3 &&
              command.package_dir == L"C:\\chast\\drivers\\usbmmidd_v2" && command.error.empty(),
          "install takes a package directory and a monitor count");
    command = chast::parse_vdisplay_command(
        {L"install", L"--package", L"C:\\pkg", L"--log", L"C:\\pkg\\vdisplay.log"});
    check(command.count == 1 && command.log_path == L"C:\\pkg\\vdisplay.log",
          "install defaults to one monitor and accepts --log");
    command =
        chast::parse_vdisplay_command({L"install", L"--package", L"C:\\pkg", L"--count", L"0"});
    check(!command.error.empty(), "install rejects zero monitors");
    command =
        chast::parse_vdisplay_command({L"install", L"--package", L"C:\\pkg", L"--count", L"5"});
    check(!command.error.empty(), "install rejects more than four monitors");
    command = chast::parse_vdisplay_command({L"install"});
    check(!command.error.empty(), "install requires --package");
    command = chast::parse_vdisplay_command({L"set-count", L"2"});
    check(command.action == chast::VdisplayAction::SetCount && command.count == 2,
          "set-count accepts a positional count");
    command = chast::parse_vdisplay_command({L"set-count", L"--count", L"0"});
    check(command.count == 0 && command.error.empty(), "set-count accepts zero and --count");
    command = chast::parse_vdisplay_command({L"set-count", L"5"});
    check(!command.error.empty(), "set-count rejects more than four");
    command = chast::parse_vdisplay_command({L"set-count"});
    check(!command.error.empty(), "set-count requires a count");
    command = chast::parse_vdisplay_command({L"uninstall", L"--package", L"C:\\pkg"});
    check(!command.error.empty(), "uninstall takes no package directory");
    command = chast::parse_vdisplay_command({L"status", L"--log", L"C:\\temp\\vdisplay.log"});
    check(command.action == chast::VdisplayAction::Status && command.error.empty(),
          "status takes only an optional log path");
    command = chast::parse_vdisplay_command({L"bogus"});
    check(!command.error.empty(), "an unknown action is a usage error");
}

void test_vdisplay_status_command() {
    std::string reason;
    if (!chast::virtual_display_platform_supported(&reason)) {
        return;
    }
    TempDir temp;
    const std::filesystem::path log = temp.path / L"vdisplay.log";
    const int code = chast::run_vdisplay_command({L"status", L"--log", log.wstring()});
    check(code == chast::kVdisplayExitOk, "status exits 0 without elevation");
    check(contains(read_all(log), "status state="), "status appends one readable log line");
    const int usage = chast::run_vdisplay_command({L"bogus", L"--log", log.wstring()});
    check(usage == chast::kVdisplayExitUsage, "an unknown action exits 2");
    check(contains(read_all(log), "usage: unknown vdisplay action"),
          "a usage error still reaches the requested log file");
}

}  // namespace

// Acceptance helper: verifies a real driver package without installing it, so
// the package checks can run on a machine where installing a display driver is
// not allowed. Not part of the normal self-test.
int verify_package_main(const wchar_t* path) {
    chast::VirtualDisplayPackageGuard guard;
    std::string detail;
    std::string version;
    std::string package_sha;
    const chast::VirtualDisplayPackageStatus status = chast::verify_virtual_display_package(
        path, &detail, &version, &package_sha, &guard);
    std::printf("status=%d\n", static_cast<int>(status));
    std::printf("detail=%s\n", detail.c_str());
    std::printf("version=%s\n", version.c_str());
    std::printf("sha256=%s\n", package_sha.c_str());
    std::printf("guard_active=%s\n", guard.active() ? "true" : "false");
    return status == chast::VirtualDisplayPackageStatus::Ok ? 0 : 1;
}

int main() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr && argc == 3 && _wcsicmp(argv[1], L"--verify-package") == 0) {
        const int code = verify_package_main(argv[2]);
        LocalFree(argv);
        return code;
    }
    if (argv != nullptr) {
        LocalFree(argv);
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_platform();
    test_classification();
    test_mode_selection();
    test_names();
    test_default_modes();
    test_provider_factory();
    test_manifest_parsing();
    test_sha256();
    test_package_verification();
    test_package_guard();
    test_package_reparse();
    test_install_record();
    test_state_directory_security();
    test_default_package_dir();
    test_vdisplay_cli();
    test_vdisplay_status_command();
    test_policy_plan();
    test_prepare();
    if (failures != 0) {
        std::printf("virtual display checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("virtual display checks passed\n");
    return 0;
}
