#pragma once

// Backend entry points behind chast/encoder.h. encoder_factory.cc owns the
// public create/availability functions and the Auto policy; each backend only
// knows its own runtime.

#include <memory>
#include <string>

#include "chast/encoder.h"

namespace chast {

std::unique_ptr<IVideoEncoder> create_vpl_encoder(ID3D11Device* device,
                                                  const EncoderConfig& config,
                                                  std::string* error);
bool vpl_encoder_available(VideoCodec codec, ID3D11Device* device);

std::unique_ptr<IVideoEncoder> create_ffmpeg_encoder(ID3D11Device* device,
                                                     const EncoderConfig& config,
                                                     std::string* error);
bool ffmpeg_encoder_available(VideoCodec codec, ID3D11Device* device);

}  // namespace chast
