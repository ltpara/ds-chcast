#pragma once
#include "chast/decoder.h"

namespace chast {
std::unique_ptr<IVideoDecoder> create_vpx_decoder(const DecoderConfig& config, std::string* error);
}
