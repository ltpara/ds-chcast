#include "display_layout.h"

#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <vector>

namespace chast {
namespace {

struct Entry {
    std::string peer;
    uint32_t display = 0;
    RECT bounds{};
};

bool valid_bounds(const RECT& bounds) {
    const long long width = static_cast<long long>(bounds.right) - bounds.left;
    const long long height = static_cast<long long>(bounds.bottom) - bounds.top;
    return width >= kMinDisplayWindowWidth && height >= kMinDisplayWindowHeight &&
           width <= 16384 && height <= 16384 &&
           bounds.left >= -32768 && bounds.left <= 32768 &&
           bounds.top >= -32768 && bounds.top <= 32768;
}

std::string encode_peer(const std::string& peer) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(peer.size() * 2);
    for (const char raw : peer) {
        const unsigned char value = static_cast<unsigned char>(raw);
        encoded.push_back(kHex[value >> 4]);
        encoded.push_back(kHex[value & 0x0f]);
    }
    return encoded;
}

bool parse_int(const std::string& text, long long* value) {
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, *value);
    return result.ec == std::errc() && result.ptr == end;
}

std::vector<Entry> read_entries(const std::filesystem::path& file) {
    std::vector<Entry> entries;
    if (file.empty()) return entries;
    std::error_code code;
    if (!std::filesystem::is_regular_file(file, code)) return entries;
    std::ifstream input(file, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() > 2048) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> fields;
        size_t start = 0;
        while (true) {
            const size_t tab = line.find('\t', start);
            if (tab == std::string::npos) {
                fields.push_back(line.substr(start));
                break;
            }
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
            if (fields.size() > 6) break;
        }
        if (fields.size() != 6 || fields[0].empty() || fields[0].size() > 512) {
            continue;
        }
        long long display = 0, left = 0, top = 0, width = 0, height = 0;
        if (!parse_int(fields[1], &display) || display < 0 ||
            display > UINT32_MAX ||
            !parse_int(fields[2], &left) || !parse_int(fields[3], &top) ||
            !parse_int(fields[4], &width) || !parse_int(fields[5], &height)) {
            continue;
        }
        if (left < INT32_MIN || left > INT32_MAX || top < INT32_MIN ||
            top > INT32_MAX || width < INT32_MIN || width > INT32_MAX ||
            height < INT32_MIN || height > INT32_MAX) {
            continue;
        }
        Entry entry;
        entry.peer = fields[0];
        entry.display = static_cast<uint32_t>(display);
        entry.bounds = RECT{static_cast<LONG>(left), static_cast<LONG>(top),
                            static_cast<LONG>(left + width),
                            static_cast<LONG>(top + height)};
        if (!valid_bounds(entry.bounds)) continue;
        entries.push_back(std::move(entry));
        if (entries.size() >= kMaxDisplayLayoutEntries) break;
    }
    return entries;
}

void write_entries(const std::filesystem::path& file,
                   const std::vector<Entry>& entries) {
    if (file.empty()) return;
    std::error_code code;
    std::filesystem::create_directories(file.parent_path(), code);
    if (code) return;
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output) return;
    for (const Entry& entry : entries) {
        const long long width =
            static_cast<long long>(entry.bounds.right) - entry.bounds.left;
        const long long height =
            static_cast<long long>(entry.bounds.bottom) - entry.bounds.top;
        output << entry.peer << '\t' << entry.display << '\t'
               << entry.bounds.left << '\t' << entry.bounds.top << '\t'
               << width << '\t' << height << '\n';
    }
}

}  // namespace

std::filesystem::path DisplayLayoutPath() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE,
                                    nullptr, &raw))) {
        return {};
    }
    const std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return path / L"chast" / L"display-windows.tsv";
}

std::optional<RECT> LoadDisplayBounds(const std::filesystem::path& file,
                                      const std::string& peer_id,
                                      uint32_t display_index) {
    const std::string peer = encode_peer(peer_id);
    std::optional<RECT> found;
    for (const Entry& entry : read_entries(file)) {
        if (entry.peer == peer && entry.display == display_index) {
            found = entry.bounds;
        }
    }
    if (found && !DisplayBoundsVisible(*found)) return std::nullopt;
    return found;
}

void SaveDisplayBounds(const std::filesystem::path& file,
                       const std::string& peer_id, uint32_t display_index,
                       const RECT& bounds) {
    if (file.empty() || !valid_bounds(bounds) || !DisplayBoundsVisible(bounds)) {
        return;
    }
    const std::string peer = encode_peer(peer_id);
    std::vector<Entry> entries = read_entries(file);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const Entry& entry) {
                                     return entry.peer == peer &&
                                            entry.display == display_index;
                                 }),
                  entries.end());
    while (entries.size() >= kMaxDisplayLayoutEntries) {
        entries.erase(entries.begin());
    }
    entries.push_back(Entry{peer, display_index, bounds});
    write_entries(file, entries);
}

bool DisplayBoundsVisible(const RECT& bounds) {
    return MonitorFromRect(&bounds, MONITOR_DEFAULTTONULL) != nullptr;
}

}  // namespace chast
