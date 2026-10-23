#include "chast/clipboard.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <numeric>

#include "chast/protocol.h"
#include "chast/security.h"

namespace chast {

namespace {

constexpr size_t kMaxFormats = kMaxClipboardFormats;

bool valid_format_id(uint8_t raw, ClipboardFormat& format) {
    if (raw > static_cast<uint8_t>(ClipboardFormat::Dib)) {
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

bool valid_dib(const std::vector<uint8_t>& data, std::string* error) {
    if (data.size() < 12) {
        if (error) {
            *error = "DIB clipboard data is too short";
        }
        return false;
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
            if (error) {
                *error = "DIB clipboard data has an unknown header size";
            }
            return false;
    }
    if (data.size() < header_size) {
        if (error) {
            *error = "DIB clipboard data is shorter than its header";
        }
        return false;
    }
    int64_t width = 0;
    int64_t height = 0;
    if (header_size == 12) {
        width = static_cast<int64_t>(data[4] | (data[5] << 8));
        height = static_cast<int64_t>(data[6] | (data[7] << 8));
    } else {
        width = static_cast<int32_t>(read_u32(data.data() + 4));
        height = static_cast<int32_t>(read_u32(data.data() + 8));
    }
    if (width <= 0 || height == 0 || width > 32768 || height > 32768 || height < -32768) {
        if (error) {
            *error = "DIB clipboard data has an unusable size";
        }
        return false;
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
                if (!valid_clipboard_text(text)) {
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
    content.seq = 0;
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
