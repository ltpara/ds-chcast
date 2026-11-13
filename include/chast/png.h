#pragma once

// PNG encoding/decoding for captures, screenshots and clipboard images. WIC
// does the codec work, so the caller only supplies or receives top-down BGRA
// pixels. Encoded capture images force alpha opaque because GPU readback has no
// meaningful transparency; decoded clipboard images retain their alpha.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chast {

// Writes `height` rows of `width` BGRA pixels to `path`, creating the parent
// directory when needed. `stride` is the byte length of one row. Returns false
// and fills `error` when the file cannot be created or the encoder rejects the
// pixels.
bool write_png_bgra(const std::string& path, const uint8_t* pixels, uint32_t width,
                    uint32_t height, uint32_t stride, std::string* error = nullptr);

// The same encoding into a memory buffer, for a picture that travels over the
// network instead of straight to disk. The result is a complete PNG file.
bool encode_png_bgra(const uint8_t* pixels, uint32_t width, uint32_t height,
                     uint32_t stride, std::vector<uint8_t>& png,
                     std::string* error = nullptr);

// Converts a CF_DIB payload (a BMP without BITMAPFILEHEADER) to PNG through
// WIC. The temporary BMP is built in memory, so an oversized clipboard DIB
// never has to be written to disk before it can travel as PNG.
bool encode_dib_png(const std::vector<uint8_t>& dib, std::vector<uint8_t>& png,
                    std::string* error = nullptr);

// Decodes a complete PNG file from memory to top-down BGRA. Width, height and
// pixel count are bounded before allocation so an untrusted clipboard item
// cannot use a tiny compressed stream to request an unbounded image buffer.
bool decode_png_bgra(const uint8_t* data, size_t size, uint32_t& width,
                     uint32_t& height, std::vector<uint8_t>& pixels,
                     std::string* error = nullptr);

// Writes already-encoded bytes to `path`, creating the parent directory when
// needed. Used by a receiver that saved the encoder step for the peer.
bool write_png_file(const std::string& path, const uint8_t* data, size_t size,
                    std::string* error = nullptr);

}  // namespace chast
