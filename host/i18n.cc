#include "i18n.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace chast::host {
namespace {

bool g_english = false;

std::wstring widen_ascii(const std::string& text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (const char ch : text) {
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    }
    return wide;
}

std::wstring read_language_setting() {
    wchar_t* appdata = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&appdata, &length, L"APPDATA") != 0 || appdata == nullptr) {
        return {};
    }
    std::wstring path = appdata;
    std::free(appdata);
    path += L"\\chast\\ui.json";
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
        return {};
    }
    std::string json;
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        json.append(buffer, read);
    }
    std::fclose(file);
    // The viewer writes a single {"language":"en"} object; a corrupt or partial
    // file simply keeps Chinese, matching the viewer's own fallback.
    const size_t key = json.find("\"language\"");
    if (key == std::string::npos) {
        return {};
    }
    const size_t colon = json.find(':', key);
    if (colon == std::string::npos) {
        return {};
    }
    const size_t quote = json.find('"', colon + 1);
    if (quote == std::string::npos) {
        return {};
    }
    const size_t end = json.find('"', quote + 1);
    if (end == std::string::npos) {
        return {};
    }
    return widen_ascii(json.substr(quote + 1, end - quote - 1));
}

}  // namespace

void load_ui_language() {
    std::wstring language;
    wchar_t* override = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&override, &length, L"CHAST_UI_LANGUAGE") == 0 && override != nullptr) {
        language = override;
        std::free(override);
    }
    if (language.empty()) {
        language = read_language_setting();
    }
    g_english = language == L"en";
}

const wchar_t* tr(const wchar_t* source) {
    if (!g_english || source == nullptr) {
        return source;
    }
    const auto& table = english_strings();
    const auto found = table.find(source);
    return found == table.end() ? source : found->second.c_str();
}

bool english_ui() {
    return g_english;
}

}  // namespace chast::host
