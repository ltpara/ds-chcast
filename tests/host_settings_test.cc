// Pure-logic checks for the host settings file: no GPU, no network.

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "chast/host_settings.h"

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
    std::filesystem::path dir = std::filesystem::path(buffer) / L"chast-settings-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool same(const chast::HostSettings& a, const chast::HostSettings& b) {
    return a.bind_address == b.bind_address && a.port == b.port &&
           a.allow_input == b.allow_input && a.require_approval == b.require_approval &&
           a.token_mode == b.token_mode && a.access_token == b.access_token &&
           a.codec == b.codec && a.bitrate_kbps == b.bitrate_kbps &&
           a.framerate == b.framerate && a.quality == b.quality && a.autostart == b.autostart &&
           a.display == b.display;
}

void test_round_trip(const std::filesystem::path& dir) {
    const std::string path = (dir / L"round-trip" / L"host.ini").string();
    chast::HostSettings settings;
    settings.bind_address = "192.168.1.5";
    settings.port = 6100;
    settings.allow_input = false;
    settings.require_approval = false;
    settings.token_mode = chast::HostTokenMode::Reusable;
    settings.access_token = "ABC234XY";
    settings.codec = chast::VideoCodec::Hevc;
    settings.bitrate_kbps = 12000;
    settings.framerate = 60;
    settings.quality = 0.5f;
    settings.autostart = true;
    settings.display = 1;

    std::string error;
    check(chast::save_host_settings(path, settings, &error), "settings save");
    check(error.empty(), "save reports no error");

    const chast::HostSettingsLoad loaded = chast::load_host_settings(path);
    check(loaded.existed, "saved settings are found again");
    check(loaded.warning.empty(), "a saved file loads without warnings");
    check(same(loaded.settings, settings), "every setting round-trips");
}

void test_missing_file(const std::filesystem::path& dir) {
    const chast::HostSettingsLoad loaded =
        chast::load_host_settings((dir / L"absent.ini").string());
    check(!loaded.existed, "a missing file is not reported as existing");
    check(loaded.warning.empty(), "a missing file is not a warning");
    check(loaded.settings.port == 4433 && loaded.settings.bind_address == "0.0.0.0",
          "missing file yields the documented defaults");
    check(loaded.settings.allow_input && loaded.settings.require_approval,
          "a fresh host accepts input behind an approval");
    check(loaded.settings.token_mode == chast::HostTokenMode::OneTime,
          "a fresh host asks for a one-time token");
}

void test_invalid_values(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "invalid.ini";
    write_text(path,
               "# hand edited\n"
               "bind=\n"
               "port=70000\n"
               "input=maybe\n"
               "approve=off\n"
               "token_mode=forever\n"
               "token=" + std::string(200, 'x') + "\n"
               "codec=vp9\n"
               "bitrate_kbps=fast\n"
               "framerate=0\n"
               "quality=2\n"
               "display=99\n"
               "unknown_key=whatever\n"
               "this line has no equals\n");
    const chast::HostSettingsLoad loaded = chast::load_host_settings(path.string());
    const chast::HostSettings& settings = loaded.settings;
    check(loaded.existed, "an edited file is read");
    check(settings.bind_address == "0.0.0.0", "an empty bind falls back");
    check(settings.port == 4433, "an out-of-range port falls back");
    check(settings.allow_input, "an unparsable boolean keeps the default");
    check(!settings.require_approval, "a valid boolean is still applied");
    check(settings.token_mode == chast::HostTokenMode::OneTime, "an unknown token mode falls back");
    check(settings.access_token.empty(), "an oversized token is dropped");
    check(settings.codec == chast::VideoCodec::H264, "an unknown codec falls back");
    check(settings.bitrate_kbps == 0, "a non-numeric bitrate falls back");
    check(settings.framerate == 30, "a zero framerate falls back");
    check(settings.quality == 0.67f, "an out-of-range quality falls back");
    check(settings.display == chast::kDisplayAuto, "an out-of-range display falls back");

    for (const char* key : {"port", "input", "token_mode", "token", "codec", "bitrate_kbps",
                            "framerate", "quality", "display"}) {
        check(loaded.warning.find(key) != std::string::npos,
              (std::string("the warning names ") + key).c_str());
    }
    check(loaded.warning.find("unknown_key") == std::string::npos,
          "unknown keys are accepted silently");
    check(loaded.warning.find("without '='") != std::string::npos,
          "a line without '=' is reported");
}

void test_tokens() {
    const std::string token = chast::generate_access_token();
    check(token.size() == 8, "a generated token is eight characters");
    bool alphabet = true;
    for (char c : token) {
        alphabet = alphabet && (c == '2' || c == '3' || c == '4' || c == '5' || c == '6' ||
                                c == '7' || c == '8' || c == '9' || (c >= 'A' && c <= 'Z')) &&
                   c != 'I' && c != 'O';
    }
    check(alphabet, "a generated token avoids look-alike characters");
    bool differ = false;
    for (int i = 0; i < 8 && !differ; ++i) {
        differ = chast::generate_access_token() != token;
    }
    check(differ, "generated tokens are not constant");
    check(chast::generate_access_token(16).size() == 16, "the token length is honoured");
}

void test_codec_names() {
    chast::VideoCodec codec = chast::VideoCodec::H264;
    check(chast::parse_codec_setting("HEVC", codec) && codec == chast::VideoCodec::Hevc,
          "HEVC is accepted in any case");
    check(chast::parse_codec_setting("h265", codec) && codec == chast::VideoCodec::Hevc,
          "h265 is an accepted alias");
    check(chast::parse_codec_setting("avc", codec) && codec == chast::VideoCodec::H264,
          "avc is an accepted alias");
    check(!chast::parse_codec_setting("vp9", codec), "an unknown codec is rejected");
    check(std::string(chast::codec_setting_name(chast::VideoCodec::Hevc)) == "hevc",
          "HEVC is written as hevc");
}

void test_unicode_path(const std::filesystem::path& dir) {
    // Windows profiles are often non-ASCII; the config path goes through
    // UTF-8, so a Chinese folder name has to survive both directions.
    const std::filesystem::path folder = dir / L"主机设置";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::string path = (folder / L"host.ini").string();
    chast::HostSettings settings;
    settings.port = 5000;
    settings.access_token = "ABC234XY";
    std::string error;
    check(chast::save_host_settings(path, settings, &error), "a non-ASCII path saves");
    const chast::HostSettingsLoad loaded = chast::load_host_settings(path);
    check(loaded.existed && loaded.settings.port == 5000 && loaded.warning.empty(),
          "a non-ASCII path loads back");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::filesystem::path dir = temp_dir();
    test_round_trip(dir);
    test_missing_file(dir);
    test_invalid_values(dir);
    test_tokens();
    test_codec_names();
    test_unicode_path(dir);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (failures != 0) {
        std::printf("host settings checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("host settings checks passed\n");
    return 0;
}
