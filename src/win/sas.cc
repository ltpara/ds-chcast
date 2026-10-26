#include "chast/sas.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>

namespace chast {

namespace {

using SendSasFn = void(WINAPI*)(BOOL);

// One process-wide module handle: sas.dll stays loaded for the lifetime of
// the worker, so every request after the first skips the loader.
HMODULE sas_module() {
    static HMODULE module = LoadLibraryW(L"sas.dll");
    return module;
}

SendSasFn send_sas_entry() {
    const HMODULE module = sas_module();
    if (module == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<SendSasFn>(GetProcAddress(module, "SendSAS"));
}

}  // namespace

bool sas_available() {
    return send_sas_entry() != nullptr;
}

bool send_secure_attention(std::string* error) {
    const SendSasFn send = send_sas_entry();
    if (send == nullptr) {
        if (error != nullptr) {
            *error = "sas.dll does not expose SendSAS";
        }
        return false;
    }
    send(FALSE);
    return true;
}

}  // namespace chast
