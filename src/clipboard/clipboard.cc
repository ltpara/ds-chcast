#include "chast/clipboard.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <functional>
#include <numeric>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "chast/protocol.h"
#include "chast/security.h"

namespace chast {

namespace {

constexpr size_t kMaxFormats = kMaxClipboardFormats;

bool valid_format_id(uint8_t raw, ClipboardFormat& format) {
    if (raw > static_cast<uint8_t>(ClipboardFormat::FileList)) {
        return false;
    }
    format = static_cast<ClipboardFormat>(raw);
    return true;
}

uint32_t format_limit(ClipboardFormat format) {
    switch (format) {
        case ClipboardFormat::Text:
            return kMaxClipboardTextBytes;
        case ClipboardFormat::Html:
        case ClipboardFormat::Rtf:
            return kMaxClipboardMarkupBytes;
        case ClipboardFormat::Dib:
            return kMaxClipboardImageBytes;
        case ClipboardFormat::FileList:
            return kMaxClipboardFileListBytes;
    }
    return 0;
}

bool valid_markup(const std::vector<uint8_t>& data, ClipboardFormat format,
                  std::string* error) {
    if (data.empty()) {
        return true;
    }
    if (std::find(data.begin(), data.end(), uint8_t{0}) != data.end()) {
        if (error) {
            *error = format == ClipboardFormat::Html ? "HTML clipboard data has a NUL"
                                                     : "RTF clipboard data has a NUL";
        }
        return false;
    }
    const std::string text(data.begin(), data.end());
    // CF_HTML is UTF-8; RTF is ASCII, which is a subset, so the same check
    // catches a mislabelled binary blob without rejecting legitimate escapes.
    if (!valid_utf8(text)) {
        if (error) {
            *error = "clipboard markup is not valid UTF-8";
        }
        return false;
    }
    if (format == ClipboardFormat::Rtf && data.size() < 5) {
        if (error) {
            *error = "RTF clipboard data is too short";
        }
        return false;
    }
    if (format == ClipboardFormat::Rtf && std::memcmp(data.data(), "{\\rtf", 5) != 0) {
        if (error) {
            *error = "RTF clipboard data does not start with {\\rtf";
        }
        return false;
    }
    return true;
}

uint32_t read_u32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t read_u64(const uint8_t* data) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (8 * i);
    }
    return value;
}

// Paste targets trust the DIB header and allocate/read according to it, so a
// header that promises more pixels than the blob carries turns into an
// out-of-bounds read on the host. Derive the exact palette and pixel byte
// counts from the header instead of only checking that the header itself fits.
bool valid_dib(const std::vector<uint8_t>& data, std::string* error) {
    constexpr uint64_t kMaxDimension = 8192;
    const auto fail = [error](const char* message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (data.size() < 12) {
        return fail("DIB clipboard data is too short");
    }
    const uint32_t header_size = read_u32(data.data());
    switch (header_size) {
        case 12:   // BITMAPCOREHEADER
        case 40:   // BITMAPINFOHEADER
        case 52:   // BITMAPV2INFOHEADER
        case 56:   // BITMAPV3INFOHEADER
        case 64:   // OS/2 2.x
        case 108:  // BITMAPV4HEADER
        case 124:  // BITMAPV5HEADER
            break;
        default:
            return fail("DIB clipboard data has an unknown header size");
    }
    if (data.size() < header_size) {
        return fail("DIB clipboard data is shorter than its header");
    }

    uint64_t width = 0;
    uint64_t height = 0;
    uint32_t planes = 1;
    uint32_t bit_count = 0;
    uint32_t compression = 0;  // BI_RGB; the core header has no such field
    uint32_t size_image = 0;
    uint32_t colors_used = 0;
    uint64_t palette_entry_bytes = 4;
    if (header_size == 12) {
        width = data[4] | (data[5] << 8);
        height = data[6] | (data[7] << 8);
        planes = data[8] | (data[9] << 8);
        bit_count = data[10] | (data[11] << 8);
        palette_entry_bytes = 3;  // RGBTRIPLE
    } else {
        width = read_u32(data.data() + 4);
        const int32_t signed_height = static_cast<int32_t>(read_u32(data.data() + 8));
        height = signed_height < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(signed_height))
                                   : static_cast<uint64_t>(signed_height);
        planes = data[12] | (data[13] << 8);
        bit_count = data[14] | (data[15] << 8);
        compression = read_u32(data.data() + 16);
        size_image = read_u32(data.data() + 20);
        colors_used = read_u32(data.data() + 32);
    }

    if (width == 0 || width > kMaxDimension || height == 0 || height > kMaxDimension ||
        planes != 1) {
        return fail("DIB clipboard data has an unusable size");
    }
    const bool plain_bit_count = bit_count == 1 || bit_count == 4 || bit_count == 8 ||
                                 bit_count == 16 || bit_count == 24 || bit_count == 32;
    switch (compression) {
        case 0:  // BI_RGB
            if (!plain_bit_count) {
                return fail("DIB clipboard data has an unusable bit count");
            }
            break;
        case 1:  // BI_RLE8
            if (bit_count != 8) {
                return fail("DIB clipboard data has an unusable bit count");
            }
            break;
        case 2:  // BI_RLE4
            if (bit_count != 4) {
                return fail("DIB clipboard data has an unusable bit count");
            }
            break;
        case 3:  // BI_BITFIELDS
            if (bit_count != 16 && bit_count != 32) {
                return fail("DIB clipboard data has an unusable bit count");
            }
            break;
        case 4:  // BI_JPEG
        case 5:  // BI_PNG
            break;
        default:
            return fail("DIB clipboard data uses an unsupported compression");
    }

    // The 40-byte header stores the three channel masks after the header; the
    // V2 and later headers carry them inside the header itself. The OS/2 2.x
    // header also has no mask fields, so it keeps the trailing layout.
    uint64_t mask_bytes = 0;
    if (compression == 3 && (header_size == 40 || header_size == 64)) {
        mask_bytes = 12;
    }
    uint64_t palette_bytes = 0;
    if (compression == 0 || compression == 1 || compression == 2 || compression == 3) {
        uint64_t entries = colors_used;
        if (entries == 0 && bit_count <= 8 && bit_count > 0) {
            entries = uint64_t{1} << bit_count;
        }
        if (entries > 256) {
            return fail("DIB clipboard data has an unusable color table");
        }
        palette_bytes = entries * palette_entry_bytes;
    }
    const uint64_t pixel_offset = uint64_t{header_size} + mask_bytes + palette_bytes;
    if (pixel_offset > data.size()) {
        return fail("DIB clipboard data is shorter than its color table");
    }

    if (compression == 0 || compression == 3) {
        const uint64_t stride = ((width * bit_count + 31) / 32) * 4;
        const uint64_t pixels = stride * height;
        if (pixels == 0 || pixel_offset + pixels > data.size()) {
            return fail("DIB clipboard data is shorter than its pixel data");
        }
    } else if (size_image == 0 || size_image > data.size() - pixel_offset) {
        // RLE and JPEG/PNG payloads are stored as-is; biSizeImage is the only
        // trustworthy bound, so a missing or oversized value is refused.
        return fail("DIB clipboard data is shorter than its compressed payload");
    }
    return true;
}

}  // namespace

const ClipboardFormatData* ClipboardContent::find(ClipboardFormat format) const {
    for (const ClipboardFormatData& entry : formats) {
        if (entry.format == format) {
            return &entry;
        }
    }
    return nullptr;
}

std::string ClipboardContent::text() const {
    const ClipboardFormatData* entry = find(ClipboardFormat::Text);
    if (!entry || entry->data.empty()) {
        return {};
    }
    return std::string(entry->data.begin(), entry->data.end());
}

uint64_t clipboard_content_hash(const ClipboardContent& content) {
    std::vector<const ClipboardFormatData*> entries;
    entries.reserve(content.formats.size());
    for (const ClipboardFormatData& entry : content.formats) {
        entries.push_back(&entry);
    }
    std::sort(entries.begin(), entries.end(),
              [](const ClipboardFormatData* a, const ClipboardFormatData* b) {
                  return static_cast<uint8_t>(a->format) < static_cast<uint8_t>(b->format);
              });
    Sha256 hash;
    if (!hash.ready()) {
        return 0;
    }
    for (const ClipboardFormatData* entry : entries) {
        const uint8_t format = static_cast<uint8_t>(entry->format);
        const uint32_t length = static_cast<uint32_t>(entry->data.size());
        const uint8_t prefix[5] = {
            format,
            static_cast<uint8_t>(length),
            static_cast<uint8_t>(length >> 8),
            static_cast<uint8_t>(length >> 16),
            static_cast<uint8_t>(length >> 24),
        };
        if (!hash.update(prefix, sizeof(prefix)) ||
            !hash.update(entry->data.data(), entry->data.size())) {
            return 0;
        }
    }
    std::array<uint8_t, 32> digest{};
    if (!hash.finish(digest)) {
        return 0;
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(digest[i]) << (8 * i);
    }
    return value;
}

namespace {

constexpr size_t kFileEntryHeaderSize = 16;
constexpr size_t kClipboardFileNameBytes = 255;
constexpr size_t kClipboardRelPathBytes = 1024;
constexpr uint16_t kFileEntryHasRelPath = 1;
constexpr uint16_t kFileEntryDirectory = 2;

void append_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void append_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

uint16_t read_u16_at(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint16_t>(data[offset]) |
           static_cast<uint16_t>(data[offset + 1] << 8);
}

uint64_t read_u64_at(const std::vector<uint8_t>& data, size_t offset) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[offset + i]) << (8 * i);
    }
    return value;
}

bool has_nul(const std::string& text) {
    return std::find(text.begin(), text.end(), '\0') != text.end();
}

bool valid_file_entry(const ClipboardFileEntry& file) {
    if (!valid_clipboard_file_name(file.name) || !valid_clipboard_rel_path(file.rel_path) ||
        (file.directory && file.size != 0)) return false;
    if (!file.rel_path.empty()) {
        const auto slash = file.rel_path.rfind('/');
        if (file.rel_path.substr(slash == std::string::npos ? 0 : slash + 1) != file.name) return false;
    }
    return true;
}

}  // namespace

bool valid_clipboard_file_name(const std::string& name) {
    if (name.empty() || name.size() > kClipboardFileNameBytes) {
        return false;
    }
    if (name == "." || name == "..") {
        return false;
    }
    if (name.find_first_of("/\\:") != std::string::npos || has_nul(name)) {
        return false;
    }
    return valid_utf8(name);
}

bool valid_clipboard_rel_path(const std::string& path) {
    if (path.empty()) {
        return true;
    }
    if (path.size() > kClipboardRelPathBytes || path.front() == '/' ||
        path.find_first_of("\\:") != std::string::npos || has_nul(path)) {
        return false;
    }
    if (!valid_utf8(path)) {
        return false;
    }
    size_t start = 0;
    for (;;) {
        const size_t slash = path.find('/', start);
        const std::string segment =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (slash == std::string::npos) {
            return true;
        }
        start = slash + 1;
    }
}

std::vector<uint8_t> encode_clipboard_file_list(const std::vector<ClipboardFileEntry>& files) {
    if (files.empty() || files.size() > kMaxClipboardFileEntries) {
        return {};
    }
    std::vector<uint8_t> out;
    out.reserve(kMaxClipboardFileListBytes / 4);
    append_u16(out, static_cast<uint16_t>(files.size()));
    for (const ClipboardFileEntry& file : files) {
        if (!valid_file_entry(file)) {
            return {};
        }
        if (out.size() + kFileEntryHeaderSize + file.name.size() +
                file.rel_path.size() >
            kMaxClipboardFileListBytes) {
            return {};
        }
        append_u16(out, static_cast<uint16_t>(file.name.size()));
        append_u16(out, (file.rel_path.empty() ? 0 : kFileEntryHasRelPath) |
                       (file.directory ? kFileEntryDirectory : 0));
        append_u16(out, static_cast<uint16_t>(file.rel_path.size()));
        append_u16(out, 0);
        append_u64(out, file.size);
        out.insert(out.end(), file.name.begin(), file.name.end());
        out.insert(out.end(), file.rel_path.begin(), file.rel_path.end());
    }
    return out;
}

bool decode_clipboard_file_list(const std::vector<uint8_t>& payload,
                                std::vector<ClipboardFileEntry>& files,
                                std::string* error) {
    files.clear();
    if (payload.size() < 2 || payload.size() > kMaxClipboardFileListBytes) {
        if (error) {
            *error = "clipboard file list is truncated";
        }
        return false;
    }
    const uint16_t count = read_u16_at(payload, 0);
    if (count == 0 || count > kMaxClipboardFileEntries) {
        if (error) {
            *error = "clipboard file list carries an unusable entry count";
        }
        return false;
    }
    size_t offset = 2;
    files.reserve(count);
    for (uint16_t index = 0; index < count; ++index) {
        if (offset + kFileEntryHeaderSize > payload.size()) {
            if (error) {
                *error = "clipboard file list is truncated";
            }
            files.clear();
            return false;
        }
        ClipboardFileEntry file;
        const uint16_t name_length = read_u16_at(payload, offset);
        const uint16_t flags = read_u16_at(payload, offset + 2);
        const uint16_t rel_length = read_u16_at(payload, offset + 4);
        const uint16_t reserved = read_u16_at(payload, offset + 6);
        file.size = read_u64_at(payload, offset + 8);
        file.directory = (flags & kFileEntryDirectory) != 0;
        offset += kFileEntryHeaderSize;
        if (reserved != 0 || (flags & ~(kFileEntryHasRelPath | kFileEntryDirectory)) != 0 ||
            (rel_length != 0) != ((flags & kFileEntryHasRelPath) != 0) ||
            offset + name_length + rel_length > payload.size()) {
            if (error) {
                *error = "clipboard file list entry is malformed";
            }
            files.clear();
            return false;
        }
        file.name.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                         payload.begin() + static_cast<std::ptrdiff_t>(offset + name_length));
        offset += name_length;
        file.rel_path.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                             payload.begin() +
                                 static_cast<std::ptrdiff_t>(offset + rel_length));
        offset += rel_length;
        if (!valid_file_entry(file)) {
            if (error) {
                *error = "clipboard file list names an unusable path";
            }
            files.clear();
            return false;
        }
        files.push_back(std::move(file));
    }
    if (offset != payload.size()) {
        if (error) {
            *error = "clipboard file list has trailing bytes";
        }
        files.clear();
        return false;
    }
    return true;
}

uint64_t clipboard_file_identity_hash(const std::vector<ClipboardFileEntry>& files) {
    Sha256 hash;
    if (!hash.ready()) {
        return 0;
    }
    for (const ClipboardFileEntry& file : files) {
        const uint8_t type = file.directory ? 1 : 0;
        if (!hash.update(&type, sizeof(type))) return 0;
        const uint32_t length = static_cast<uint32_t>(file.name.size());
        const uint8_t prefix[4] = {
            static_cast<uint8_t>(length),
            static_cast<uint8_t>(length >> 8),
            static_cast<uint8_t>(length >> 16),
            static_cast<uint8_t>(length >> 24),
        };
        const uint32_t folder_length = static_cast<uint32_t>(file.rel_path.size());
        const uint8_t folder_prefix[4] = {
            static_cast<uint8_t>(folder_length),
            static_cast<uint8_t>(folder_length >> 8),
            static_cast<uint8_t>(folder_length >> 16),
            static_cast<uint8_t>(folder_length >> 24),
        };
        uint8_t size_bytes[8];
        for (int i = 0; i < 8; ++i) {
            size_bytes[i] = static_cast<uint8_t>(file.size >> (8 * i));
        }
        if (!hash.update(prefix, sizeof(prefix)) ||
            !hash.update(file.name.data(), file.name.size()) ||
            !hash.update(folder_prefix, sizeof(folder_prefix)) ||
            !hash.update(file.rel_path.data(), file.rel_path.size()) ||
            !hash.update(size_bytes, sizeof(size_bytes))) {
            return 0;
        }
    }
    std::array<uint8_t, 32> digest{};
    if (!hash.finish(digest)) {
        return 0;
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(digest[i]) << (8 * i);
    }
    return value;
}

bool collect_clipboard_files(const std::vector<std::string>& top_level_paths,
                             std::vector<ClipboardFileEntry>& entries,
                             std::vector<std::string>& paths, std::string* error) {
    entries.clear();
    paths.clear();
    auto fail = [&](const char* reason) {
        entries.clear();
        paths.clear();
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    };
    constexpr size_t kMaxWalkDepth = 16;
    struct Collected {
        ClipboardFileEntry entry;
        std::string path;  // UTF-8 path of the bytes on this machine
    };
    std::vector<Collected> collected;
    bool refused = false;
    auto path_from_utf8 = [](const std::string& utf8) {
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    };
    auto join_utf8 = [](const std::string& base, const std::string& name) {
        if (base.empty()) {
            return name;
        }
        if (base.back() == '/' || base.back() == '\\') {
            return base + name;
        }
        return base + "/" + name;
    };
    auto add_entry = [&](const std::filesystem::path& physical,
                         const std::string& utf8_path, const std::string& rel_dir, bool directory) {
        std::error_code file_error;
        const uint64_t size = directory ? 0 : std::filesystem::file_size(physical, file_error);
        if (file_error) { refused = true; return; }
        const auto u8name = physical.filename().u8string();
        ClipboardFileEntry entry;
        entry.name.assign(u8name.begin(), u8name.end());
        entry.size = size;
        entry.directory = directory;
        if (!rel_dir.empty()) entry.rel_path = rel_dir + "/" + entry.name;
        if (!valid_file_entry(entry) || collected.size() == kMaxClipboardFileEntries) {
            refused = true;
            return;
        }
        collected.push_back({std::move(entry), utf8_path});
    };
    auto is_reparse = [&](const std::filesystem::path& path, const std::filesystem::file_status& status) {
#ifdef _WIN32
        (void)status;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) { refused = true; return true; }
        return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
        return std::filesystem::is_symlink(status);
#endif
    };
    // Emit only leaf directories: nonempty ancestors are reconstructed from
    // rel_path. This keeps large shallow trees within the existing list cap.
    std::function<void(const std::filesystem::path&, const std::string&, size_t)> collect;
    collect = [&](const std::filesystem::path& physical, const std::string& rel_dir, size_t depth) {
        if (refused) return;
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(physical, ec);
        if (ec) { refused = true; return; }
        if (is_reparse(physical, status)) return;
        if (depth > kMaxWalkDepth) { refused = true; return; }
        const auto u8path = physical.u8string();
        const std::string path(u8path.begin(), u8path.end());
        if (std::filesystem::is_regular_file(status)) {
            add_entry(physical, path, rel_dir, false);
            return;
        }
        if (!std::filesystem::is_directory(status)) return;
        const auto u8name = physical.filename().u8string();
        const std::string name(u8name.begin(), u8name.end());
        const auto next_dir = join_utf8(rel_dir, name);
        if (!valid_clipboard_file_name(name) || !valid_clipboard_rel_path(next_dir)) {
            refused = true;
            return;
        }
        const size_t before = collected.size();
        std::filesystem::directory_iterator it(physical, ec), end;
        if (ec) { refused = true; return; }
        for (; it != end && !refused; it.increment(ec)) {
            if (ec) { refused = true; break; }
            collect(it->path(), next_dir, depth + 1);
        }
        if (ec) refused = true;
        if (!refused && collected.size() == before) add_entry(physical, path, rel_dir, true);
    };
    for (const auto& path : top_level_paths) {
        collect(path_from_utf8(path), {}, 0);
        if (refused) break;
    }
    if (refused) {
        return fail("the copied tree is unreadable or exceeds the clipboard list limits");
    }
    if (collected.empty()) {
        return fail("the clipboard holds no usable files or directories");
    }
    // A stable order is what lets the receiver hash its staged copy and match
    // the announced list even though the filesystem enumerated differently.
    std::stable_sort(collected.begin(), collected.end(),
                     [](const Collected& left, const Collected& right) {
                         if (left.entry.rel_path != right.entry.rel_path) {
                             return left.entry.rel_path < right.entry.rel_path;
                         }
                         if (left.entry.name != right.entry.name) {
                             return left.entry.name < right.entry.name;
                         }
                         return left.path < right.path;
                     });
    entries.reserve(collected.size());
    paths.reserve(collected.size());
    for (Collected& item : collected) {
        entries.push_back(std::move(item.entry));
        paths.push_back(std::move(item.path));
    }
    return true;
}

bool valid_clipboard_content(const ClipboardContent& content, std::string* error) {
    if (content.formats.empty()) {
        if (error) {
            *error = "clipboard content has no formats";
        }
        return false;
    }
    if (content.formats.size() > kMaxFormats) {
        if (error) {
            *error = "clipboard content carries too many formats";
        }
        return false;
    }
    uint64_t total = 0;
    int previous = -1;
    for (const ClipboardFormatData& entry : content.formats) {
        ClipboardFormat format = ClipboardFormat::Text;
        if (!valid_format_id(static_cast<uint8_t>(entry.format), format)) {
            if (error) {
                *error = "clipboard content carries an unknown format";
            }
            return false;
        }
        if (static_cast<int>(format) <= previous) {
            if (error) {
                *error = "clipboard formats are not strictly ascending";
            }
            return false;
        }
        previous = static_cast<int>(format);
        if (entry.data.size() > format_limit(format)) {
            if (error) {
                *error = "clipboard format " + std::to_string(static_cast<int>(format)) +
                         " is past its limit";
            }
            return false;
        }
        total += entry.data.size();
        if (total > kMaxClipboardTotalBytes) {
            if (error) {
                *error = "clipboard content is past the total limit";
            }
            return false;
        }
        switch (format) {
            case ClipboardFormat::Text: {
                const std::string text(entry.data.begin(), entry.data.end());
                if (text.empty()) {
                    if (error) {
                        *error = "clipboard text is empty";
                    }
                    return false;
                }
                // The stream carries text outside the legacy ClipboardUpdate
                // message, so only the format rules apply here; the length was
                // bounded by kMaxClipboardTextBytes above.
                if (text.find('\0') != std::string::npos || !valid_utf8(text)) {
                    if (error) {
                        *error = "clipboard text is not valid UTF-8";
                    }
                    return false;
                }
                break;
            }
            case ClipboardFormat::Html:
            case ClipboardFormat::Rtf:
                if (!valid_markup(entry.data, format, error)) {
                    return false;
                }
                break;
            case ClipboardFormat::Dib:
                if (!valid_dib(entry.data, error)) {
                    return false;
                }
                break;
            case ClipboardFormat::FileList: {
                std::vector<ClipboardFileEntry> files;
                if (!decode_clipboard_file_list(entry.data, files, error)) {
                    return false;
                }
                break;
            }
        }
    }
    return true;
}

std::vector<uint8_t> encode_clipboard_stream(const ClipboardContent& content) {
    if (!valid_clipboard_content(content, nullptr)) {
        return {};
    }
    uint64_t total = 0;
    for (const ClipboardFormatData& entry : content.formats) {
        total += entry.data.size();
    }
    const uint64_t hash = clipboard_content_hash(content);
    if (hash == 0) {
        return {};
    }
    std::vector<uint8_t> stream(kClipboardStreamHeaderSize);
    size_t cursor = 0;
    std::memcpy(stream.data(), kClipboardStreamMagic, sizeof(kClipboardStreamMagic));
    cursor += sizeof(kClipboardStreamMagic);
    stream[cursor++] = kClipboardStreamVersion;
    stream[cursor++] = static_cast<uint8_t>(content.formats.size());
    stream[cursor++] = 0;
    stream[cursor++] = 0;
    for (int i = 0; i < 8; ++i) {
        stream[cursor++] = static_cast<uint8_t>(hash >> (8 * i));
    }
    for (int i = 0; i < 8; ++i) {
        stream[cursor++] = static_cast<uint8_t>(total >> (8 * i));
    }
    for (int i = 0; i < 8; ++i) {
        stream[cursor++] = static_cast<uint8_t>(content.seq >> (8 * i));
    }
    for (const ClipboardFormatData& entry : content.formats) {
        const uint32_t length = static_cast<uint32_t>(entry.data.size());
        stream.push_back(static_cast<uint8_t>(entry.format));
        stream.push_back(0);
        stream.push_back(0);
        stream.push_back(0);
        for (int i = 0; i < 4; ++i) {
            stream.push_back(static_cast<uint8_t>(length >> (8 * i)));
        }
        stream.insert(stream.end(), entry.data.begin(), entry.data.end());
    }
    return stream;
}

bool decode_clipboard_stream(const uint8_t* data, size_t size, ClipboardContent& content,
                             std::string* error) {
    content = ClipboardContent{};
    if (!data || size < kClipboardStreamHeaderSize) {
        if (error) {
            *error = "clipboard stream is truncated";
        }
        return false;
    }
    if (std::memcmp(data, kClipboardStreamMagic, sizeof(kClipboardStreamMagic)) != 0) {
        if (error) {
            *error = "clipboard stream has bad magic";
        }
        return false;
    }
    if (data[4] != kClipboardStreamVersion) {
        if (error) {
            *error = "clipboard stream version " + std::to_string(data[4]) +
                     " is not supported";
        }
        return false;
    }
    const uint8_t count = data[5];
    if (data[6] != 0 || data[7] != 0) {
        if (error) {
            *error = "clipboard stream reserved bytes are not zero";
        }
        return false;
    }
    if (count == 0 || count > kMaxFormats) {
        if (error) {
            *error = "clipboard stream carries " + std::to_string(count) + " formats";
        }
        return false;
    }
    const uint64_t declared_hash = read_u64(data + 8);
    const uint64_t total = read_u64(data + 16);
    if (total > kMaxClipboardTotalBytes) {
        if (error) {
            *error = "clipboard stream is past the total limit";
        }
        return false;
    }
    content.seq = read_u64(data + 24);
    content.formats.clear();
    content.formats.reserve(count);
    size_t cursor = kClipboardStreamHeaderSize;
    uint64_t seen = 0;
    int previous = -1;
    for (uint8_t i = 0; i < count; ++i) {
        if (size - cursor < kClipboardEntryHeaderSize) {
            if (error) {
                *error = "clipboard stream entry " + std::to_string(i) + " is truncated";
            }
            return false;
        }
        ClipboardFormat format = ClipboardFormat::Text;
        if (!valid_format_id(data[cursor], format)) {
            if (error) {
                *error = "clipboard stream carries an unknown format";
            }
            return false;
        }
        if (data[cursor + 1] != 0 || data[cursor + 2] != 0 || data[cursor + 3] != 0) {
            if (error) {
                *error = "clipboard stream entry reserved bytes are not zero";
            }
            return false;
        }
        if (static_cast<int>(format) <= previous) {
            if (error) {
                *error = "clipboard stream formats are not strictly ascending";
            }
            return false;
        }
        previous = static_cast<int>(format);
        const uint32_t length = read_u32(data + cursor + 4);
        cursor += kClipboardEntryHeaderSize;
        if (length > format_limit(format)) {
            if (error) {
                *error = "clipboard stream entry " + std::to_string(i) + " is past its limit";
            }
            return false;
        }
        if (length > size - cursor) {
            if (error) {
                *error = "clipboard stream entry " + std::to_string(i) + " overruns";
            }
            return false;
        }
        ClipboardFormatData entry;
        entry.format = format;
        entry.data.assign(data + cursor, data + cursor + length);
        cursor += length;
        seen += length;
        content.formats.push_back(std::move(entry));
    }
    if (cursor != size) {
        if (error) {
            *error = "clipboard stream has trailing bytes";
        }
        return false;
    }
    if (seen != total) {
        if (error) {
            *error = "clipboard stream total length is wrong";
        }
        return false;
    }
    if (!valid_clipboard_content(content, error)) {
        return false;
    }
    content.hash = clipboard_content_hash(content);
    if (content.hash == 0 || content.hash != declared_hash) {
        if (error) {
            *error = "clipboard stream hash does not match its content";
        }
        return false;
    }
    return true;
}

}  // namespace chast
