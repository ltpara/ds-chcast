#include "chast/clipboard.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <numeric>
#include <string_view>

#include <zstd.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xmllite.h>
#endif

#include "chast/protocol.h"
#include "chast/security.h"

namespace chast {

namespace {

constexpr size_t kMaxFormats = kMaxClipboardFormats;

bool valid_format_id(uint8_t raw, ClipboardFormat& format) {
    if (raw > static_cast<uint8_t>(ClipboardFormat::Svg)) {
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
        case ClipboardFormat::Png:
            return kMaxClipboardPngBytes;
        case ClipboardFormat::Svg:
            return kMaxClipboardSvgBytes;
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

bool svg_is_space(wchar_t value) {
    return value == L' ' || value == L'\t' || value == L'\r' || value == L'\n';
}

bool svg_local_fragment(std::wstring_view value) {
    while (!value.empty() && svg_is_space(value.front())) value.remove_prefix(1);
    while (!value.empty() && svg_is_space(value.back())) value.remove_suffix(1);
    if (value.size() < 2 || value.front() != L'#') return false;
    for (const wchar_t ch : value.substr(1)) {
        const bool valid = (ch >= L'a' && ch <= L'z') ||
                           (ch >= L'A' && ch <= L'Z') ||
                           (ch >= L'0' && ch <= L'9') || ch == L'_' ||
                           ch == L'-' || ch == L'.' || ch == L':';
        if (!valid) return false;
    }
    return true;
}

bool svg_attribute_is_safe(std::wstring_view name, std::wstring_view value) {
    std::wstring lower_name(name);
    for (wchar_t& ch : lower_name) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
    }
    if (lower_name == L"style" || lower_name == L"base" ||
        (lower_name.size() >= 2 && lower_name[0] == L'o' && lower_name[1] == L'n')) {
        return false;
    }
    if (lower_name == L"href") return svg_local_fragment(value);
    if (value.find(L'\\') != std::wstring_view::npos) return false;

    std::wstring lower_value(value);
    for (wchar_t& ch : lower_value) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
    }
    size_t cursor = 0;
    while ((cursor = lower_value.find(L"url", cursor)) != std::wstring::npos) {
        size_t open = cursor + 3;
        while (open < lower_value.size() && svg_is_space(lower_value[open])) ++open;
        if (open == lower_value.size() || lower_value[open] != L'(') {
            return false;
        }
        const size_t close = lower_value.find(L')', open + 1);
        if (close == std::wstring::npos) return false;
        std::wstring_view target(value.data() + open + 1, close - open - 1);
        while (!target.empty() && svg_is_space(target.front())) target.remove_prefix(1);
        while (!target.empty() && svg_is_space(target.back())) target.remove_suffix(1);
        if (target.size() >= 2 &&
            ((target.front() == L'\'' && target.back() == L'\'') ||
             (target.front() == L'"' && target.back() == L'"'))) {
            target = target.substr(1, target.size() - 2);
        }
        if (!svg_local_fragment(target)) return false;
        cursor = close + 1;
    }
    return lower_value.find(L"javascript:") == std::wstring::npos;
}

bool valid_svg(const std::vector<uint8_t>& data, std::string* error) {
    const auto fail = [error](const char* reason) {
        if (error) *error = reason;
        return false;
    };
    if (data.empty() || data.size() > kMaxClipboardSvgBytes ||
        std::find(data.begin(), data.end(), uint8_t{0}) != data.end()) {
        return fail("SVG clipboard data is empty, oversized, or contains a NUL");
    }
    const std::string utf8(data.begin(), data.end());
    if (!valid_utf8(utf8)) return fail("SVG clipboard data is not valid UTF-8");

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!memory) return fail("SVG parser input allocation failed");
    void* target = GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        return fail("SVG parser input could not be locked");
    }
    std::memcpy(target, data.data(), data.size());
    GlobalUnlock(memory);

    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(memory, TRUE, &stream);
    if (FAILED(hr)) {
        GlobalFree(memory);
        return fail("SVG parser input stream could not be created");
    }
    IXmlReader* reader = nullptr;
    hr = CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(&reader), nullptr);
    if (FAILED(hr) || reader == nullptr) {
        stream->Release();
        return fail("SVG XML reader could not be created");
    }
    const auto release = [&] {
        reader->Release();
        stream->Release();
    };
    if (FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(reader->SetInput(stream))) {
        release();
        return fail("SVG XML reader could not be configured");
    }

    static constexpr std::wstring_view kSvgNamespace = L"http://www.w3.org/2000/svg";
    static constexpr std::wstring_view kXmlNamespace = L"http://www.w3.org/XML/1998/namespace";
    static constexpr std::wstring_view kXmlnsNamespace = L"http://www.w3.org/2000/xmlns/";
    static constexpr std::wstring_view kXlinkNamespace = L"http://www.w3.org/1999/xlink";
    static constexpr std::wstring_view kSafeElements[] = {
        L"svg", L"g", L"defs", L"symbol", L"use", L"path", L"rect", L"circle",
        L"ellipse", L"line", L"polyline", L"polygon", L"text", L"tspan", L"textPath",
        L"title", L"desc", L"metadata", L"a", L"image", L"switch", L"view", L"clipPath",
        L"mask", L"linearGradient", L"radialGradient", L"stop", L"pattern", L"marker",
        L"filter", L"feBlend", L"feColorMatrix", L"feComponentTransfer", L"feFuncA",
        L"feFuncB", L"feFuncG", L"feFuncR", L"feGaussianBlur", L"feOffset", L"feFlood",
        L"feComposite", L"feMerge", L"feMergeNode", L"feMorphology", L"feTurbulence",
        L"feDisplacementMap", L"feDiffuseLighting", L"feSpecularLighting", L"feDistantLight",
        L"fePointLight", L"feSpotLight", L"feConvolveMatrix", L"feTile", L"feDropShadow",
    };
    bool root_seen = false;
    uint32_t nodes = 0;
    XmlNodeType node = XmlNodeType_None;
    while ((hr = reader->Read(&node)) == S_OK) {
        if (++nodes > 200000) {
            release();
            return fail("SVG XML has too many nodes");
        }
        if (node == XmlNodeType_DocumentType || node == XmlNodeType_ProcessingInstruction) {
            release();
            return fail("SVG XML contains a prohibited document feature");
        }
        if (node != XmlNodeType_Element) continue;

        const WCHAR* local_ptr = nullptr;
        UINT local_length = 0;
        const WCHAR* namespace_ptr = nullptr;
        UINT namespace_length = 0;
        if (FAILED(reader->GetLocalName(&local_ptr, &local_length)) ||
            FAILED(reader->GetNamespaceUri(&namespace_ptr, &namespace_length))) {
            release();
            return fail("SVG XML contains an unreadable element");
        }
        const std::wstring_view local(local_ptr, local_length);
        const std::wstring_view element_namespace(namespace_ptr ? namespace_ptr : L"",
                                                  namespace_length);
        UINT depth = 0;
        reader->GetDepth(&depth);
        if (depth > 64 || element_namespace != kSvgNamespace ||
            std::find(std::begin(kSafeElements), std::end(kSafeElements), local) ==
                std::end(kSafeElements) || (depth == 0 && (root_seen || local != L"svg"))) {
            release();
            return fail("SVG XML contains an unsupported or active element");
        }
        if (depth == 0) root_seen = true;

        UINT attribute_count = 0;
        if (FAILED(reader->GetAttributeCount(&attribute_count)) || attribute_count > 256) {
            release();
            return fail("SVG XML has too many attributes");
        }
        if (attribute_count > 0) {
            hr = reader->MoveToFirstAttribute();
            while (hr == S_OK) {
                const WCHAR* attribute_ptr = nullptr;
                UINT attribute_length = 0;
                const WCHAR* attribute_namespace_ptr = nullptr;
                UINT attribute_namespace_length = 0;
                const WCHAR* value_ptr = nullptr;
                UINT value_length = 0;
                if (FAILED(reader->GetLocalName(&attribute_ptr, &attribute_length)) ||
                    FAILED(reader->GetNamespaceUri(&attribute_namespace_ptr,
                                                   &attribute_namespace_length)) ||
                    FAILED(reader->GetValue(&value_ptr, &value_length))) {
                    release();
                    return fail("SVG XML contains an unreadable attribute");
                }
                const std::wstring_view attribute(attribute_ptr, attribute_length);
                const std::wstring_view attribute_namespace(
                    attribute_namespace_ptr ? attribute_namespace_ptr : L"",
                    attribute_namespace_length);
                const std::wstring_view value(value_ptr ? value_ptr : L"", value_length);
                const bool allowed_namespace = attribute_namespace.empty() ||
                    attribute_namespace == kXmlNamespace ||
                    attribute_namespace == kXmlnsNamespace ||
                    attribute_namespace == kXlinkNamespace;
                if (!allowed_namespace ||
                    !svg_attribute_is_safe(attribute, value)) {
                    release();
                    return fail("SVG XML contains an active or external attribute");
                }
                hr = reader->MoveToNextAttribute();
            }
            if (FAILED(hr) || FAILED(reader->MoveToElement())) {
                release();
                return fail("SVG XML attributes are malformed");
            }
        }
    }
    release();
    return hr == S_FALSE && root_seen ? true : fail("SVG XML is not a complete SVG document");
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

uint32_t read_be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

const std::array<uint32_t, 256>& png_crc_table() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t n = 0; n < values.size(); ++n) {
            uint32_t value = n;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1) != 0 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
            }
            values[n] = value;
        }
        return values;
    }();
    return table;
}

uint32_t png_crc_update(uint32_t crc, const uint8_t* data, size_t size) {
    const std::array<uint32_t, 256>& table = png_crc_table();
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

bool valid_ihdr(const uint8_t* data, uint32_t& width, uint32_t& height) {
    width = read_be32(data);
    height = read_be32(data + 4);
    const uint8_t bit_depth = data[8];
    const uint8_t color_type = data[9];
    bool depth_ok = false;
    switch (color_type) {
        case 0:  // grayscale
            depth_ok = bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
                       bit_depth == 8 || bit_depth == 16;
            break;
        case 3:  // indexed
            depth_ok = bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
                       bit_depth == 8;
            break;
        case 2:  // truecolor
        case 4:  // grayscale + alpha
        case 6:  // truecolor + alpha
            depth_ok = bit_depth == 8 || bit_depth == 16;
            break;
        default:
            return false;
    }
    return depth_ok && data[10] == 0 && data[11] == 0 &&
           (data[12] == 0 || data[12] == 1);
}

bool chunk_type(const uint8_t* type, const char (&value)[5]) {
    return std::memcmp(type, value, 4) == 0;
}

// Validate the complete PNG framing before WIC sees it. In particular, chunk
// lengths and CRCs bound every read and the IHDR cannot request more pixels
// than the decoder is willing to allocate.
bool valid_png(const std::vector<uint8_t>& data, std::string* error) {
    const auto fail = [error](const char* message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (data.size() < 8 || std::memcmp(data.data(), kSignature, sizeof(kSignature)) != 0) {
        return fail("PNG clipboard data has a bad signature");
    }
    size_t cursor = sizeof(kSignature);
    bool first = true;
    bool saw_ihdr = false;
    bool saw_idat = false;
    bool saw_iend = false;
    uint32_t width = 0;
    uint32_t height = 0;
    while (cursor < data.size()) {
        if (data.size() - cursor < 12) {
            return fail("PNG clipboard data has a truncated chunk");
        }
        const uint32_t length = read_be32(data.data() + cursor);
        if (length > data.size() - cursor - 12) {
            return fail("PNG clipboard data has a chunk past its end");
        }
        const uint8_t* type = data.data() + cursor + 4;
        const uint8_t* payload = type + 4;
        const uint32_t stored_crc = read_be32(payload + length);
        for (int i = 0; i < 4; ++i) {
            if (!((type[i] >= 'A' && type[i] <= 'Z') ||
                  (type[i] >= 'a' && type[i] <= 'z'))) {
                return fail("PNG clipboard data has an invalid chunk type");
            }
        }
        uint32_t crc = png_crc_update(0xFFFFFFFFu, type, 4);
        crc = png_crc_update(crc, payload, length) ^ 0xFFFFFFFFu;
        if (crc != stored_crc) {
            return fail("PNG clipboard data has a bad chunk CRC");
        }

        const bool is_ihdr = chunk_type(type, "IHDR");
        const bool is_plte = chunk_type(type, "PLTE");
        const bool is_idat = chunk_type(type, "IDAT");
        const bool is_iend = chunk_type(type, "IEND");
        if (first && !is_ihdr) {
            return fail("PNG clipboard data does not start with IHDR");
        }
        if (is_ihdr) {
            if (!first || saw_ihdr || length != 13 ||
                !valid_ihdr(payload, width, height)) {
                return fail("PNG clipboard data has an invalid IHDR");
            }
            const uint64_t pixels = static_cast<uint64_t>(width) * height;
            if (width > kMaxClipboardPngDimension || height > kMaxClipboardPngDimension ||
                pixels == 0 || pixels > kMaxClipboardPngPixels) {
                return fail("PNG clipboard dimensions exceed the decode limit");
            }
            saw_ihdr = true;
        } else if (!saw_ihdr) {
            return fail("PNG clipboard data has a chunk before IHDR");
        } else if (is_idat) {
            saw_idat = true;
        } else if (is_iend) {
            if (length != 0 || !saw_idat) {
                return fail("PNG clipboard data has an invalid IEND");
            }
            saw_iend = true;
        } else if (!is_plte && (type[0] >= 'A' && type[0] <= 'Z')) {
            return fail("PNG clipboard data has an unknown critical chunk");
        }

        cursor += 12 + static_cast<size_t>(length);
        first = false;
        if (is_iend) {
            if (cursor != data.size()) {
                return fail("PNG clipboard data has bytes after IEND");
            }
            break;
        }
    }
    if (!saw_ihdr || !saw_idat || !saw_iend || cursor != data.size()) {
        return fail("PNG clipboard data is incomplete");
    }
    return true;
}

// Paste targets trust the DIB header and allocate/read according to it, so a
// header that promises more pixels than the blob carries turns into an
// out-of-bounds read on the host. Derive the exact palette and pixel byte
// counts from the header instead of only checking that the header itself fits.
bool valid_dib_impl(const std::vector<uint8_t>& data, std::string* error) {
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

bool valid_clipboard_dib(const std::vector<uint8_t>& data, std::string* error) {
    return valid_dib_impl(data, error);
}

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
                if (!valid_dib_impl(entry.data, error)) {
                    return false;
                }
                break;
            case ClipboardFormat::Png:
                if (!valid_png(entry.data, error)) {
                    return false;
                }
                break;
            case ClipboardFormat::Svg:
                if (!valid_svg(entry.data, error)) {
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
        std::vector<uint8_t> compressed;
        if (entry.data.size() >= kClipboardCompressionMinBytes) {
            std::vector<uint8_t> candidate(ZSTD_compressBound(entry.data.size()));
            const size_t produced = ZSTD_compress(candidate.data(), candidate.size(),
                                                  entry.data.data(), entry.data.size(), 1);
            if (!ZSTD_isError(produced) && produced < entry.data.size()) {
                candidate.resize(produced);
                compressed = std::move(candidate);
            }
        }
        const bool is_compressed = !compressed.empty();
        const std::vector<uint8_t>& stored = is_compressed ? compressed : entry.data;
        const uint32_t stored_length = static_cast<uint32_t>(stored.size());
        const uint32_t raw_length = static_cast<uint32_t>(entry.data.size());
        stream.push_back(static_cast<uint8_t>(entry.format));
        stream.push_back(is_compressed ? kClipboardEntryFlagCompressed : 0);
        stream.push_back(0);
        stream.push_back(0);
        for (int i = 0; i < 4; ++i) {
            stream.push_back(static_cast<uint8_t>(stored_length >> (8 * i)));
        }
        for (int i = 0; i < 4; ++i) {
            stream.push_back(static_cast<uint8_t>(raw_length >> (8 * i)));
        }
        stream.insert(stream.end(), stored.begin(), stored.end());
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
        const uint8_t flags = data[cursor + 1];
        if (data[cursor + 2] != 0 || data[cursor + 3] != 0) {
            if (error) {
                *error = "clipboard stream entry reserved bytes are not zero";
            }
            return false;
        }
        if ((flags & ~kClipboardEntryFlagCompressed) != 0) {
            if (error) {
                *error = "clipboard stream entry has unknown flags";
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
        const uint32_t stored_length = read_u32(data + cursor + 4);
        const uint32_t raw_length = read_u32(data + cursor + 8);
        cursor += kClipboardEntryHeaderSize;
        if (raw_length > format_limit(format)) {
            if (error) {
                *error = "clipboard stream entry " + std::to_string(i) + " is past its limit";
            }
            return false;
        }
        const bool compressed = (flags & kClipboardEntryFlagCompressed) != 0;
        if (compressed) {
            // The sender only compresses when the payload shrinks; accepting a
            // larger "compressed" block would let a peer expand a small stream
            // into the per-format cap for no reason.
            if (raw_length == 0 || stored_length >= raw_length) {
                if (error) {
                    *error = "clipboard stream entry " + std::to_string(i) +
                             " is not actually compressed";
                }
                return false;
            }
        } else if (stored_length != raw_length) {
            if (error) {
                *error = "clipboard stream entry " + std::to_string(i) +
                         " stores a different length than it declares";
            }
            return false;
        }
        if (stored_length > size - cursor) {
            if (error) {
                *error = "clipboard stream entry " + std::to_string(i) + " overruns";
            }
            return false;
        }
        ClipboardFormatData entry;
        entry.format = format;
        if (compressed) {
            entry.data.resize(raw_length);
            const size_t produced = ZSTD_decompress(entry.data.data(), entry.data.size(),
                                                    data + cursor, stored_length);
            if (ZSTD_isError(produced) || produced != raw_length) {
                if (error) {
                    *error = "clipboard stream entry " + std::to_string(i) +
                             " does not decompress to its declared length";
                }
                return false;
            }
        } else {
            entry.data.assign(data + cursor, data + cursor + stored_length);
        }
        cursor += stored_length;
        seen += raw_length;
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
