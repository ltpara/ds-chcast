#pragma once
#include "chast/host_settings.h"

namespace chast::host {
enum class ServiceSettingsPage { Security, Connection };
// Apply only fields edited on the selected page. Unrelated worker changes,
// including rotated tokens and refreshed trusted-device timestamps, survive.
bool merge_service_settings(const HostSettings& original, const HostSettings& edited,
                            const HostSettings& current, ServiceSettingsPage page,
                            HostSettings& merged, std::string* error);
struct ServiceSettingsResult {
    bool saved = false;
    bool running = false;
    std::string error;
};
// Administrator-only machine configuration transaction. Stops and waits for
// the existing worker before reading its final settings, then saves/restarts.
ServiceSettingsResult save_service_settings(const HostSettings& original,
                                            const HostSettings& edited,
                                            ServiceSettingsPage page);
bool service_settings_admin();
}
