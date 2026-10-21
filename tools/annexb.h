// Annex B access-unit splitting shared by the decoder harness and the QUIC
// loopback verifier. Both must cut the elementary stream at exactly the units
// the encoder emitted (one AUD-prefixed access unit per packet), otherwise the
// per-frame content digests cannot be compared.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "chast/encoder.h"

namespace chast_tools {

struct NalUnit {
    size_t start = 0;    // first byte of the start code
    size_t payload = 0;  // first byte after the start code
};

// Every start code (3- or 4-byte) in the buffer, in order. The plane bytes of
// a compressed stream cannot contain 00 00 01 by construction (emulation
// prevention), so a plain scan is safe.
inline std::vector<NalUnit> scan_nals(const std::vector<uint8_t>& data) {
    std::vector<NalUnit> nals;
    for (size_t i = 0; i + 2 < data.size();) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            NalUnit nal;
            nal.start = (i > 0 && data[i - 1] == 0) ? i - 1 : i;
            nal.payload = i + 3;
            nals.push_back(nal);
            i = nal.payload;
        } else {
            ++i;
        }
    }
    return nals;
}

inline unsigned nal_type(const std::vector<uint8_t>& data, const NalUnit& nal,
                         chast::VideoCodec codec) {
    if (nal.payload >= data.size()) {
        return 0;
    }
    if (codec == chast::VideoCodec::H264) {
        return data[nal.payload] & 0x1F;
    }
    return (data[nal.payload] >> 1) & 0x3F;
}

inline bool is_aud(unsigned type, chast::VideoCodec codec) {
    return codec == chast::VideoCodec::H264 ? type == 9 : type == 35;
}

inline bool is_sequence_header(unsigned type, chast::VideoCodec codec) {
    return codec == chast::VideoCodec::H264 ? type == 7 : type == 33;
}

inline bool is_idr(unsigned type, chast::VideoCodec codec) {
    return codec == chast::VideoCodec::H264 ? type == 5 : (type == 19 || type == 20);
}

struct AccessUnit {
    std::vector<uint8_t> bytes;
    bool keyframe = false;
};

// Splits at AUDs; when the stream carries no AUD every NAL becomes its own
// unit, which is still decodable because each starts with its own slice.
inline std::vector<AccessUnit> split_access_units(const std::vector<uint8_t>& data,
                                                  chast::VideoCodec codec) {
    std::vector<AccessUnit> units;
    const std::vector<NalUnit> nals = scan_nals(data);
    if (nals.empty()) {
        return units;
    }
    bool has_aud = false;
    for (const NalUnit& nal : nals) {
        if (is_aud(nal_type(data, nal, codec), codec)) {
            has_aud = true;
            break;
        }
    }

    size_t unit_start = nals.front().start;
    bool unit_key = false;
    for (size_t i = 0; i < nals.size(); ++i) {
        const NalUnit& nal = nals[i];
        const unsigned type = nal_type(data, nal, codec);
        const bool boundary = has_aud ? is_aud(type, codec) : i != 0;
        if (boundary && i != 0) {
            AccessUnit unit;
            unit.bytes.assign(data.begin() + unit_start, data.begin() + nal.start);
            unit.keyframe = unit_key;
            if (!unit.bytes.empty()) {
                units.push_back(std::move(unit));
            }
            unit_start = nal.start;
            unit_key = false;
        }
        if (is_sequence_header(type, codec) || is_idr(type, codec)) {
            unit_key = true;
        }
    }
    AccessUnit unit;
    unit.bytes.assign(data.begin() + unit_start, data.end());
    unit.keyframe = unit_key;
    if (!unit.bytes.empty()) {
        units.push_back(std::move(unit));
    }
    return units;
}

inline const char* codec_name(chast::VideoCodec codec) {
    return codec == chast::VideoCodec::H264 ? "H.264" : "HEVC";
}

}  // namespace chast_tools
