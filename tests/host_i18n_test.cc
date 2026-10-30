// Host tray language selection: the environment override, the viewer's shared
// ui.json and the English table itself. No window, GPU or network is touched.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "i18n.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

std::filesystem::path temp_dir() {
    wchar_t buffer[MAX_PATH]{};
    GetTempPathW(MAX_PATH, buffer);
    std::filesystem::path dir = std::filesystem::path(buffer) / L"chast-i18n-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / L"chast", ec);
    return dir;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::wstring env_value(const wchar_t* name) {
    wchar_t* value = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::wstring copy = value;
    std::free(value);
    return copy;
}

std::string narrow_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

bool has_cjk(const std::wstring& text) {
    for (const wchar_t ch : text) {
        if ((ch >= 0x3000 && ch <= 0x303F) || (ch >= 0x3400 && ch <= 0x9FFF) ||
            (ch >= 0xF900 && ch <= 0xFAFF) || (ch >= 0xFF00 && ch <= 0xFFEF)) {
            return true;
        }
    }
    return false;
}

bool is_chinese(const wchar_t* source, const char* what) {
    const bool same = std::wstring(chast::host::tr(source)) == source;
    check(same, what);
    return same;
}

bool is_english(const wchar_t* source, const wchar_t* wanted, const char* what) {
    const bool same = std::wstring(chast::host::tr(source)) == wanted;
    check(same, what);
    if (!same) {
        std::printf("     got  %s\n     want %s\n",
                    narrow_utf8(chast::host::tr(source)).c_str(),
                    narrow_utf8(wanted).c_str());
    }
    return same;
}

void test_environment_override() {
    _wputenv_s(L"CHAST_UI_LANGUAGE", L"en");
    chast::host::load_ui_language();
    is_english(L"允许", L"Allow", "override: 允许 renders in English");
    is_english(L"键盘与鼠标", L"Keyboard and mouse", "override: 键盘与鼠标 renders in English");
    is_english(L" · 设备 ID ", L" · Device ID ", "override: a spaced fragment keeps its spaces");
    // A string without a translation must survive instead of blanking a control.
    is_english(L"no such host string", L"no such host string",
               "override: unknown source falls back to Chinese");
    check(chast::host::tr(nullptr) == nullptr, "override: tr(nullptr) stays null");

    _wputenv_s(L"CHAST_UI_LANGUAGE", L"zh");
    chast::host::load_ui_language();
    is_chinese(L"允许", "zh: source is returned unchanged");

    // Any value other than "en" keeps Chinese, including an empty override that
    // lets the file decide; the file is tested separately.
    _wputenv_s(L"CHAST_UI_LANGUAGE", L"fr");
    chast::host::load_ui_language();
    is_chinese(L"允许", "unsupported language: Chinese");
}

void test_shared_ui_json() {
    const std::filesystem::path root = temp_dir();
    const std::filesystem::path json = root / L"chast" / L"ui.json";
    const std::wstring saved_appdata = env_value(L"APPDATA");

    _wputenv_s(L"CHAST_UI_LANGUAGE", L"");
    _wputenv_s(L"APPDATA", root.c_str());

    chast::host::load_ui_language();
    is_chinese(L"允许", "ui.json missing: Chinese");

    write_text(json, "{\"language\":\"en\"}");
    chast::host::load_ui_language();
    is_english(L"允许", L"Allow", "ui.json en: English");

    write_text(json, "{\"language\":\"zh\"}");
    chast::host::load_ui_language();
    is_chinese(L"允许", "ui.json zh: Chinese");

    // Partial or unrelated JSON must not leave the UI half-translated.
    write_text(json, "{\"language\":\"e");
    chast::host::load_ui_language();
    is_chinese(L"允许", "truncated ui.json: Chinese");

    write_text(json, "{\"scale\":1.0}");
    chast::host::load_ui_language();
    is_chinese(L"允许", "ui.json without language: Chinese");

    // The explicit override outranks the file, so tests and portable runs are
    // not at the mercy of a developer's saved preference.
    write_text(json, "{\"language\":\"zh\"}");
    _wputenv_s(L"CHAST_UI_LANGUAGE", L"en");
    chast::host::load_ui_language();
    is_english(L"允许", L"Allow", "override wins over ui.json");

    _wputenv_s(L"CHAST_UI_LANGUAGE", L"");
    _wputenv_s(L"APPDATA", saved_appdata.c_str());
}

void test_table() {
    const auto& table = chast::host::english_strings();
    check(!table.empty(), "table is not empty");
    check(table.size() >= 321, "table still covers every wrapped host string");

    int empty_keys = 0;
    int cjk_values = 0;
    int empty_values = 0;
    for (const auto& [key, value] : table) {
        if (key.empty()) {
            ++empty_keys;
        }
        if (value.empty()) {
            // Two fragments (' 的请求', ' 的连接') are deliberately empty in
            // English; an empty value is a choice, never an accident.
            ++empty_values;
            continue;
        }
        if (has_cjk(value)) {
            ++cjk_values;
            std::printf("     CJK left in %s -> %s\n", narrow_utf8(key).c_str(),
                        narrow_utf8(value).c_str());
        }
    }
    check(empty_keys == 0, "every key is non-empty");
    check(cjk_values == 0, "no English value contains CJK characters");
    check(empty_values == 2, "only the two deliberate empty fragments have empty values");
}

}  // namespace

int main() {
    test_environment_override();
    test_shared_ui_json();
    test_table();
    if (failures != 0) {
        std::printf("\n%d host i18n check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall host i18n checks passed\n");
    return 0;
}
