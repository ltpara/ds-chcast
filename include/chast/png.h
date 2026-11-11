#pragma once

// PNG writing for captures and screenshots. WIC does the encoding, so the
// caller only supplies top-down BGRA pixels; the alpha channel is forced
// opaque because the GPU surfaces this reads back do not carry meaningful
// transparency.

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

// Writes already-encoded bytes to `path`, creating the parent directory when
// needed. Used by a receiver that saved the encoder step for the peer.
bool write_png_file(const std::string& path, const uint8_t* data, size_t size,
                    std::string* error = nullptr);

}  // namespace chast
