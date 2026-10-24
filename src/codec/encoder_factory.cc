// Encoder backend selection: oneVPL first, FFmpeg's vendor encoders second.
//
// The public API lives in chast/encoder.h; this file is the only place that
// knows both backends exist. Auto keeps the long-standing oneVPL behaviour on
// Intel and gives NVIDIA/AMD adapters a hardware path through FFmpeg. A pinned
// backend never falls back, and no path here can reach a software encoder.

#include "chast/encoder.h"

#include <string>
#include <string_view>

#include "encoder_internal.h"

namespace chast {

const char* encoder_backend_name(EncoderBackend backend) {
    switch (backend) {
        case EncoderBackend::Vpl:
            return "vpl";
        case EncoderBackend::Ffmpeg:
            return "ffmpeg";
        case EncoderBackend::Auto:
        default:
            return "auto";
    }
}

bool parse_encoder_backend(std::string_view text, EncoderBackend& backend) {
    if (text == "auto") {
        backend = EncoderBackend::Auto;
        return true;
    }
    if (text == "vpl") {
        backend = EncoderBackend::Vpl;
        return true;
    }
    if (text == "ffmpeg") {
        backend = EncoderBackend::Ffmpeg;
        return true;
    }
    return false;
}

bool hardware_encoder_available(VideoCodec codec, ID3D11Device* device,
                                EncoderBackend backend) {
    if (backend == EncoderBackend::Vpl) {
        return vpl_encoder_available(codec, device);
    }
    if (backend == EncoderBackend::Ffmpeg) {
        return ffmpeg_encoder_available(codec, device);
    }
    return vpl_encoder_available(codec, device) ||
           ffmpeg_encoder_available(codec, device);
}

std::unique_ptr<IVideoEncoder> create_encoder(ID3D11Device* device,
                                              const EncoderConfig& config,
                                              std::string* error) {
    if (config.backend != EncoderBackend::Ffmpeg) {
        std::string vpl_error;
        if (auto encoder = create_vpl_encoder(device, config, &vpl_error)) {
            return encoder;
        }
        if (config.backend == EncoderBackend::Vpl) {
            if (error) {
                *error = vpl_error;
            }
            return nullptr;
        }
        std::string ffmpeg_error;
        if (auto encoder = create_ffmpeg_encoder(device, config, &ffmpeg_error)) {
            return encoder;
        }
        if (error) {
            *error = "oneVPL: " + vpl_error + "; FFmpeg: " + ffmpeg_error;
        }
        return nullptr;
    }

    std::string ffmpeg_error;
    if (auto encoder = create_ffmpeg_encoder(device, config, &ffmpeg_error)) {
        return encoder;
    }
    if (error) {
        *error = ffmpeg_error;
    }
    return nullptr;
}

}  // namespace chast
