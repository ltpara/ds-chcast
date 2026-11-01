#pragma once

// PNG writing for captures and screenshots. WIC does the encoding, so the
// caller only supplies top-down BGRA pixels; the alpha channel is forced
// opaque because the GPU surfaces this reads back do not carry meaningful
// transparency.

#include <cstdint>
#include <string>

namespace chast {

// Writes `height` rows of `width` BGRA pixels to `path`, creating the parent
// directory when needed. `stride` is the byte length of one row. Returns false
// and fills `error` when the file cannot be created or the encoder rejects the
// pixels.
bool write_png_bgra(const std::string& path, const uint8_t* pixels, uint32_t width,
                    uint32_t height, uint32_t stride, std::string* error = nullptr);

}  // namespace chast
