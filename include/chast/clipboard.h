#pragma once

// Multi-format clipboard content, its wire container, and the Win32 backend.
//
// A copied item carries whichever of text, HTML, RTF and DIB the source
// application published. One CCLB stream carries the whole set; the receiver
// writes every format back to its own clipboard in one ownership change, so a
// paste target can pick the richest one it understands.
//
// The content model and container codec are platform independent; the
// read_clipboard()/write_clipboard() functions at the bottom are the Win32
// side.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chast {

enum class ClipboardFormat : uint8_t {
    Text = 0,  // UTF-8, no NUL
    Html = 1,  // CF_HTML in UTF-8, header included
    Rtf = 2,   // "Rich Text Format", ASCII/UTF-8
    Dib = 3,   // CF_DIB bytes: BITMAPINFOHEADER + palette + pixels
};

inline constexpr uint8_t kMaxClipboardFormats = 4;
inline constexpr uint32_t kMaxClipboardTextBytes = 256u * 1024u;
inline constexpr uint32_t kMaxClipboardMarkupBytes = 4u * 1024u * 1024u;
inline constexpr uint32_t kMaxClipboardImageBytes = 32u * 1024u * 1024u;
inline constexpr uint32_t kMaxClipboardTotalBytes = 40u * 1024u * 1024u;

inline constexpr char kClipboardStreamMagic[4] = {'C', 'C', 'L', 'B'};
inline constexpr uint8_t kClipboardStreamVersion = 1;
// magic (4) + version (1) + format count (1) + reserved (2) + hash (8) +
// total payload length (8).
inline constexpr size_t kClipboardStreamHeaderSize = 24;
// Per entry: format (1) + reserved (1) + reserved (2) + length (4).
inline constexpr size_t kClipboardEntryHeaderSize = 8;

struct ClipboardFormatData {
    ClipboardFormat format = ClipboardFormat::Text;
    std::vector<uint8_t> data;
};

struct ClipboardContent {
    // Diagnostic counter from the sender. The stream already preserves order.
    uint64_t seq = 0;
    // Filled in by encode/decode; 0 in a freshly read local clipboard.
    uint64_t hash = 0;
    std::vector<ClipboardFormatData> formats;

    bool empty() const { return formats.empty(); }
    const ClipboardFormatData* find(ClipboardFormat format) const;
    // The Text payload as a string, empty when the item has no text.
    std::string text() const;
};

// SHA-256 of the canonical format sequence (ascending format id, then the
// u32 length and bytes of each entry), truncated to its first 8 bytes. Used to
// suppress echo loops, not as a security check.
uint64_t clipboard_content_hash(const ClipboardContent& content);

// Structural and limit checks shared by the encoder and both receivers:
// at most four formats, strictly ascending ids, per-format caps, total at or
// below kMaxClipboardTotalBytes, Text valid UTF-8 without NUL, HTML valid
// UTF-8, RTF starting with "{\rtf" and DIB carrying a known header size.
bool valid_clipboard_content(const ClipboardContent& content, std::string* error);

// Full CCLB stream bytes, magic included. Returns an empty vector when the
// content is invalid; entries are written in ascending format order and the
// hash is recomputed from the bytes.
std::vector<uint8_t> encode_clipboard_stream(const ClipboardContent& content);

// Validates the magic, version, hash and every entry before filling
// `content`. seq and hash are set from the stream. Returns false with a
// reason for truncated, oversized or malformed input.
bool decode_clipboard_stream(const uint8_t* data, size_t size, ClipboardContent& content,
                             std::string* error);

// ----- Win32 clipboard -----

// Reads the formats above that the clipboard currently offers into `content`
// (hash left 0). Returns false with `error` when the clipboard cannot be
// opened or holds none of the supported formats; a form with no supported
// format is not an error for the caller, it just yields an empty content.
bool read_clipboard(ClipboardContent& content, std::string* error);

// Replaces the clipboard with `content` in one ownership change. Formats the
// system does not accept are skipped; Text is written last as the fallback.
bool write_clipboard(const ClipboardContent& content, std::string* error);

// Text-only convenience wrappers. read returns false unless the clipboard
// holds non-empty text within kMaxClipboardTextBytes.
bool read_clipboard_text(std::string& text);
bool write_clipboard_text(const std::string& text);

}  // namespace chast
