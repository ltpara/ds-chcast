#include "chast/protocol.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace chast {

bool valid_utf8(const std::string& text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = bytes[i];
        if (lead < 0x80) {
            if (lead == 0) {
                return false;
            }
            ++i;
            continue;
        }
        size_t tail = 0;
        uint32_t code_point = 0;
        if ((lead & 0xE0) == 0xC0) {
            tail = 1;
            code_point = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            tail = 2;
            code_point = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            tail = 3;
            code_point = lead & 0x07u;
        } else {
            return false;
        }
        if (i + tail >= text.size()) {
            return false;
        }
        for (size_t j = 1; j <= tail; ++j) {
            const unsigned char next = bytes[i + j];
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (next & 0x3Fu);
        }
        if ((tail == 1 && code_point < 0x80) || (tail == 2 && code_point < 0x800) ||
            (tail == 3 && code_point < 0x10000)) {
            return false;  // overlong encoding
        }
        if ((code_point >= 0xD800 && code_point <= 0xDFFF) || code_point > 0x10FFFF) {
            return false;
        }
        i += tail + 1;
    }
    return true;
}

namespace {

constexpr char kFrameMagic[4] = {'C', 'F', 'R', 'M'};
constexpr char kAudioMagic[4] = {'C', 'A', 'U', 'D'};
constexpr char kControlMagic[4] = {'C', 'C', 'T', 'L'};
// A single receive buffer holding more than this without yielding a frame
// means the peer is not speaking the protocol.
constexpr size_t kMaxFrameBuffer = 64u * 1024u * 1024u;
constexpr size_t kMaxAudioBuffer = 4u * 1024u * 1024u;
constexpr size_t kMaxControlBuffer = 4u * 1024u * 1024u;
constexpr size_t kMaxDisplays = kMaxDisplayCount;

// HelloAck display entry: name_size u16, name, left/top/right/bottom i32,
// rotation u16, flags u16 (bit 0: primary).
constexpr size_t kDisplayFixedSize = 2 + 16 + 2 + 2;

void put_u8(uint8_t*& out, uint8_t value) {
    *out++ = value;
}

void put_u16(uint8_t*& out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out += 2;
}

void put_u32(uint8_t*& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
    out += 4;
}

void put_u64(uint8_t*& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
    out += 8;
}

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : cursor_(data), end_(data + size) {}

    bool u8(uint8_t& value) {
        if (cursor_ == end_) {
            return false;
        }
        value = *cursor_++;
        return true;
    }

    bool u16(uint16_t& value) {
        if (remaining() < 2) {
            return false;
        }
        value = static_cast<uint16_t>(cursor_[0] | (cursor_[1] << 8));
        cursor_ += 2;
        return true;
    }

    bool u32(uint32_t& value) {
        if (remaining() < 4) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(cursor_[i]) << (8 * i);
        }
        cursor_ += 4;
        return true;
    }

    bool u64(uint64_t& value) {
        if (remaining() < 8) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(cursor_[i]) << (8 * i);
        }
        cursor_ += 8;
        return true;
    }

    bool string(std::string& value, size_t size) {
        if (remaining() < size) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(cursor_), size);
        cursor_ += size;
        return true;
    }

    bool bytes(std::vector<uint8_t>& value, size_t size) {
        if (remaining() < size) {
            return false;
        }
        value.assign(cursor_, cursor_ + size);
        cursor_ += size;
        return true;
    }

    bool raw(uint8_t* value, size_t size) {
        if (remaining() < size) {
            return false;
        }
        std::memcpy(value, cursor_, size);
        cursor_ += size;
        return true;
    }

    bool at_end() const { return cursor_ == end_; }
    size_t remaining() const { return static_cast<size_t>(end_ - cursor_); }

private:
    const uint8_t* cursor_;
    const uint8_t* end_;
};

const std::array<uint32_t, 256>& crc_table() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> generated{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            generated[i] = value;
        }
        return generated;
    }();
    return table;
}

uint8_t codec_wire(VideoCodec codec) {
    return codec == VideoCodec::H264 ? 1 : 2;
}

bool codec_from_wire(uint8_t value, VideoCodec& codec) {
    if (value == 1) {
        codec = VideoCodec::H264;
        return true;
    }
    if (value == 2) {
        codec = VideoCodec::Hevc;
        return true;
    }
    return false;
}

uint8_t audio_codec_wire(AudioCodec codec) {
    switch (codec) {
        case AudioCodec::AAC:
            return 1;
        case AudioCodec::Opus:
            return 2;
    }
    return 0;
}

bool audio_codec_from_wire(uint8_t value, AudioCodec& codec) {
    if (value == 1) {
        codec = AudioCodec::AAC;
        return true;
    }
    if (value == 2) {
        codec = AudioCodec::Opus;
        return true;
    }
    return false;
}

bool valid_audio_rate(uint32_t rate) {
    return rate >= 8000 && rate <= 96000;
}

void set_error(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t size) {
    const std::array<uint32_t, 256>& table = crc_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void encode_frame_header(const FrameHeader& header, uint8_t out[kFrameHeaderSize]) {
    uint8_t* cursor = out;
    std::memcpy(cursor, kFrameMagic, sizeof(kFrameMagic));
    cursor += sizeof(kFrameMagic);
    put_u16(cursor, kProtocolVersion);
    uint16_t flags = 0;
    if (header.keyframe) {
        flags |= 0x1;
    }
    if (header.idr) {
        flags |= 0x2;
    }
    put_u16(cursor, flags);
    put_u64(cursor, header.seq);
    put_u64(cursor, header.pts_ms);
    put_u32(cursor, header.payload_size);
    put_u32(cursor, header.payload_crc32);
    put_u32(cursor, header.width);
    put_u32(cursor, header.height);
    put_u8(cursor, codec_wire(header.codec));
    const uint64_t capture_ms = header.capture_ms & 0xFFFFFFFFFFFFull;
    for (int i = 0; i < 6; ++i) {
        put_u8(cursor, static_cast<uint8_t>(capture_ms >> (8 * i)));
    }
    put_u8(cursor, 0);
}

bool decode_frame_header(const uint8_t* data, size_t size, FrameHeader& header,
                         std::string* error) {
    if (size < kFrameHeaderSize) {
        set_error(error, "short frame header");
        return false;
    }
    if (std::memcmp(data, kFrameMagic, sizeof(kFrameMagic)) != 0) {
        set_error(error, "bad frame magic");
        return false;
    }
    Reader reader(data + sizeof(kFrameMagic), size - sizeof(kFrameMagic));
    uint16_t version = 0;
    uint16_t flags = 0;
    uint8_t codec = 0;
    if (!reader.u16(version) || !reader.u16(flags) || !reader.u64(header.seq) ||
        !reader.u64(header.pts_ms) || !reader.u32(header.payload_size) ||
        !reader.u32(header.payload_crc32) || !reader.u32(header.width) ||
        !reader.u32(header.height) || !reader.u8(codec)) {
        set_error(error, "truncated frame header");
        return false;
    }
    if (version != kProtocolVersion) {
        set_error(error, "unsupported frame protocol version " + std::to_string(version));
        return false;
    }
    if (!codec_from_wire(codec, header.codec)) {
        set_error(error, "unknown frame codec " + std::to_string(codec));
        return false;
    }
    if (header.payload_size == 0 || header.payload_size > kMaxFramePayload) {
        set_error(error, "frame payload size " + std::to_string(header.payload_size) +
                             " outside the protocol limit");
        return false;
    }
    if (header.width == 0 || header.height == 0) {
        set_error(error, "frame header has an empty picture");
        return false;
    }
    header.keyframe = (flags & 0x1) != 0;
    header.idr = (flags & 0x2) != 0;
    header.capture_ms = 0;
    for (int i = 0; i < 6; ++i) {
        uint8_t byte = 0;
        if (!reader.u8(byte)) {
            set_error(error, "truncated frame capture time");
            return false;
        }
        header.capture_ms |= static_cast<uint64_t>(byte) << (8 * i);
    }
    return true;
}

void encode_audio_header(const AudioHeader& header, uint8_t out[kAudioHeaderSize]) {
    uint8_t* cursor = out;
    std::memcpy(cursor, kAudioMagic, sizeof(kAudioMagic));
    cursor += sizeof(kAudioMagic);
    put_u16(cursor, kProtocolVersion);
    put_u16(cursor, 0);
    put_u64(cursor, header.seq);
    put_u64(cursor, header.pts_ms);
    const uint64_t capture_ms = header.capture_ms & 0xFFFFFFFFFFFFull;
    put_u64(cursor, capture_ms);
    put_u32(cursor, header.payload_size);
    put_u32(cursor, header.payload_crc32);
    put_u32(cursor, header.sample_rate);
    put_u16(cursor, header.frame_samples);
    put_u8(cursor, static_cast<uint8_t>(header.channels));
    put_u8(cursor, audio_codec_wire(header.codec));
}

bool decode_audio_header(const uint8_t* data, size_t size, AudioHeader& header,
                         std::string* error) {
    if (size < kAudioHeaderSize) {
        set_error(error, "short audio header");
        return false;
    }
    if (std::memcmp(data, kAudioMagic, sizeof(kAudioMagic)) != 0) {
        set_error(error, "bad audio magic");
        return false;
    }
    Reader reader(data + sizeof(kAudioMagic), size - sizeof(kAudioMagic));
    uint16_t version = 0;
    uint16_t flags = 0;
    uint8_t channels = 0;
    uint8_t codec = 0;
    if (!reader.u16(version) || !reader.u16(flags) || !reader.u64(header.seq) ||
        !reader.u64(header.pts_ms) || !reader.u64(header.capture_ms) ||
        !reader.u32(header.payload_size) || !reader.u32(header.payload_crc32) ||
        !reader.u32(header.sample_rate) || !reader.u16(header.frame_samples) ||
        !reader.u8(channels) || !reader.u8(codec)) {
        set_error(error, "truncated audio header");
        return false;
    }
    header.channels = channels;
    if (version != kProtocolVersion) {
        set_error(error, "unsupported audio protocol version " + std::to_string(version));
        return false;
    }
    if (flags != 0) {
        set_error(error, "audio header carries unknown flags");
        return false;
    }
    if (!audio_codec_from_wire(codec, header.codec)) {
        set_error(error, "unknown audio codec " + std::to_string(codec));
        return false;
    }
    header.capture_ms &= 0xFFFFFFFFFFFFull;
    if (header.payload_size == 0 || header.payload_size > kMaxAudioPayload) {
        set_error(error, "audio payload size " + std::to_string(header.payload_size) +
                             " outside the protocol limit");
        return false;
    }
    if (!valid_audio_rate(header.sample_rate)) {
        set_error(error, "audio sample rate " + std::to_string(header.sample_rate) +
                             " is not supported");
        return false;
    }
    if (header.channels != 1 && header.channels != 2) {
        set_error(error, "audio header has " + std::to_string(header.channels) +
                             " channels");
        return false;
    }
    if (header.frame_samples == 0 || header.frame_samples > 8192) {
        set_error(error, "audio frame length " + std::to_string(header.frame_samples) +
                             " is outside the protocol limit");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_microphone_stream_header(const AudioInfo& info) {
    if (info.codec != AudioCodec::Opus || info.sample_rate != 48000 ||
        (info.channels != 1 && info.channels != 2) ||
        info.frame_samples != kOpusFrameSamples ||
        info.extradata.empty() || info.extradata.size() > kMaxAudioExtradata) {
        return {};
    }
    std::vector<uint8_t> header(kMicrophoneStreamHeaderSize + info.extradata.size());
    uint8_t* cursor = header.data();
    std::memcpy(cursor, kMicrophoneStreamMagic, sizeof(kMicrophoneStreamMagic));
    cursor += sizeof(kMicrophoneStreamMagic);
    put_u8(cursor, kMicrophoneStreamVersion);
    put_u8(cursor, audio_codec_wire(info.codec));
    put_u16(cursor, 0);
    put_u32(cursor, info.sample_rate);
    put_u16(cursor, info.channels);
    put_u16(cursor, static_cast<uint16_t>(info.frame_samples));
    put_u16(cursor, static_cast<uint16_t>(info.extradata.size()));
    put_u16(cursor, 0);
    std::memcpy(cursor, info.extradata.data(), info.extradata.size());
    return header;
}

size_t microphone_stream_header_length(const uint8_t* data, size_t size) {
    if (size < kMicrophoneStreamHeaderSize ||
        std::memcmp(data, kMicrophoneStreamMagic, sizeof(kMicrophoneStreamMagic)) != 0) {
        return 0;
    }
    const uint16_t extradata_size = static_cast<uint16_t>(
        data[16] | (static_cast<uint16_t>(data[17]) << 8));
    if (extradata_size == 0 || extradata_size > kMaxAudioExtradata) {
        return 0;
    }
    return kMicrophoneStreamHeaderSize + extradata_size;
}

bool decode_microphone_stream_header(const uint8_t* data, size_t size, AudioInfo& info,
                                     size_t& consumed, std::string* error) {
    consumed = 0;
    if (size < kMicrophoneStreamHeaderSize) {
        set_error(error, "short microphone stream header");
        return false;
    }
    if (std::memcmp(data, kMicrophoneStreamMagic, sizeof(kMicrophoneStreamMagic)) != 0) {
        set_error(error, "bad microphone stream magic");
        return false;
    }
    Reader reader(data + sizeof(kMicrophoneStreamMagic),
                  kMicrophoneStreamHeaderSize - sizeof(kMicrophoneStreamMagic));
    uint8_t version = 0;
    uint8_t codec = 0;
    uint16_t reserved = 0;
    uint16_t tail_reserved = 0;
    uint16_t channels = 0;
    uint16_t frame_samples = 0;
    uint16_t extradata_size = 0;
    if (!reader.u8(version) || !reader.u8(codec) || !reader.u16(reserved) ||
        !reader.u32(info.sample_rate) || !reader.u16(channels) ||
        !reader.u16(frame_samples) || !reader.u16(extradata_size) ||
        !reader.u16(tail_reserved)) {
        set_error(error, "truncated microphone stream header");
        return false;
    }
    if (version != kMicrophoneStreamVersion) {
        set_error(error, "unsupported microphone stream version " +
                             std::to_string(version));
        return false;
    }
    if (!audio_codec_from_wire(codec, info.codec) || info.codec != AudioCodec::Opus) {
        set_error(error, "microphone stream names unsupported codec " +
                             std::to_string(codec));
        return false;
    }
    if (reserved != 0 || tail_reserved != 0) {
        set_error(error, "microphone stream header carries non-zero reserved fields");
        return false;
    }
    if (info.sample_rate != 48000) {
        set_error(error, "microphone stream sample rate " +
                             std::to_string(info.sample_rate) + " is not supported");
        return false;
    }
    if (channels != 1 && channels != 2) {
        set_error(error, "microphone stream has " + std::to_string(channels) +
                             " channels");
        return false;
    }
    if (frame_samples != kOpusFrameSamples) {
        set_error(error, "microphone stream frame length " +
                             std::to_string(frame_samples) + " is not supported");
        return false;
    }
    if (extradata_size == 0 || extradata_size > kMaxAudioExtradata) {
        set_error(error, "microphone stream config size " +
                             std::to_string(extradata_size) + " is outside the limit");
        return false;
    }
    if (size < kMicrophoneStreamHeaderSize + extradata_size) {
        set_error(error, "microphone stream config is truncated");
        return false;
    }
    info.enabled = true;
    info.channels = channels;
    info.frame_samples = frame_samples;
    info.bitrate = 0;
    info.extradata.assign(data + kMicrophoneStreamHeaderSize,
                          data + kMicrophoneStreamHeaderSize + extradata_size);
    consumed = kMicrophoneStreamHeaderSize + extradata_size;
    return true;
}

std::vector<uint8_t> make_hello(const ClientHello& hello) {
    const size_t token_size =
        hello.access_token.size() > kMaxAccessTokenLength ? kMaxAccessTokenLength
                                                          : hello.access_token.size();
    const size_t password_size =
        hello.password.size() > kMaxPasswordLength ? kMaxPasswordLength
                                                   : hello.password.size();
    const size_t key_size = hello.device_key.size() > kMaxDeviceKeyLength
                                ? kMaxDeviceKeyLength
                                : hello.device_key.size();
    const size_t name_size = hello.device_name.size() > kMaxDeviceNameLength
                                 ? kMaxDeviceNameLength
                                 : hello.device_name.size();
    const size_t signature_size = hello.signature.size() > kMaxSignatureLength
                                      ? kMaxSignatureLength
                                      : hello.signature.size();
    const size_t totp_size = hello.totp.size() > kMaxTotpLength ? kMaxTotpLength
                                                                 : hello.totp.size();
    std::vector<uint8_t> payload(6 + token_size + 2 + password_size + 2 + 2 + key_size +
                                 1 + 2 + name_size + kHelloNonceBytes + 8 + 1 + 2 +
                                 signature_size + 2 + totp_size);
    uint8_t* cursor = payload.data();
    put_u16(cursor, kProtocolVersion);
    put_u16(cursor, hello.display);
    put_u16(cursor, static_cast<uint16_t>(token_size));
    std::memcpy(cursor, hello.access_token.data(), token_size);
    cursor += token_size;
    put_u16(cursor, static_cast<uint16_t>(password_size));
    std::memcpy(cursor, hello.password.data(), password_size);
    cursor += password_size;
    put_u16(cursor, hello.requested_permissions);
    put_u16(cursor, static_cast<uint16_t>(key_size));
    if (key_size > 0) {
        std::memcpy(cursor, hello.device_key.data(), key_size);
        cursor += key_size;
    }
    put_u8(cursor, hello.device_platform);
    put_u16(cursor, static_cast<uint16_t>(name_size));
    if (name_size > 0) {
        std::memcpy(cursor, hello.device_name.data(), name_size);
        cursor += name_size;
    }
    std::memcpy(cursor, hello.nonce.data(), kHelloNonceBytes);
    cursor += kHelloNonceBytes;
    put_u64(cursor, hello.unix_seconds);
    put_u8(cursor, hello.auth_flags);
    put_u16(cursor, static_cast<uint16_t>(signature_size));
    if (signature_size > 0) {
        std::memcpy(cursor, hello.signature.data(), signature_size);
        cursor += signature_size;
    }
    put_u16(cursor, static_cast<uint16_t>(totp_size));
    if (totp_size > 0) {
        std::memcpy(cursor, hello.totp.data(), totp_size);
    }
    return make_control(ControlType::Hello, payload);
}

bool parse_hello(const std::vector<uint8_t>& payload, ClientHello& hello,
                 std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint16_t version = 0;
    uint16_t token_size = 0;
    if (!reader.u16(version) || !reader.u16(hello.display) || !reader.u16(token_size)) {
        set_error(error, "truncated Hello");
        return false;
    }
    if (version != kProtocolVersion) {
        set_error(error, "client speaks protocol version " + std::to_string(version));
        return false;
    }
    if (hello.display != kDisplayAuto && hello.display >= kMaxDisplayCount) {
        set_error(error, "Hello asks for display " + std::to_string(hello.display) +
                             " outside the protocol limit");
        return false;
    }
    if (token_size > kMaxAccessTokenLength ||
        !reader.string(hello.access_token, token_size)) {
        set_error(error, "Hello carries a malformed access token");
        return false;
    }
    uint16_t password_size = 0;
    if (!reader.u16(password_size) || password_size > kMaxPasswordLength ||
        !reader.string(hello.password, password_size)) {
        set_error(error, "Hello carries a malformed password");
        return false;
    }
    if (!reader.u16(hello.requested_permissions)) {
        set_error(error, "Hello carries no permission request");
        return false;
    }
    if ((hello.requested_permissions & ~kPermissionAll) != 0) {
        set_error(error, "Hello requests unknown permissions");
        return false;
    }
    uint16_t key_size = 0;
    if (!reader.u16(key_size) || key_size > kMaxDeviceKeyLength ||
        !reader.bytes(hello.device_key, key_size)) {
        set_error(error, "Hello carries a malformed device key");
        return false;
    }
    if (!reader.u8(hello.device_platform)) {
        set_error(error, "Hello carries no device platform");
        return false;
    }
    uint16_t name_size = 0;
    if (!reader.u16(name_size) || name_size > kMaxDeviceNameLength ||
        !reader.string(hello.device_name, name_size)) {
        set_error(error, "Hello carries a malformed device name");
        return false;
    }
    if (!hello.device_name.empty() && !valid_utf8(hello.device_name)) {
        set_error(error, "Hello carries an invalid device name");
        return false;
    }
    std::vector<uint8_t> nonce;
    if (!reader.bytes(nonce, kHelloNonceBytes)) {
        set_error(error, "Hello carries no client nonce");
        return false;
    }
    std::memcpy(hello.nonce.data(), nonce.data(), kHelloNonceBytes);
    if (!reader.u64(hello.unix_seconds) || !reader.u8(hello.auth_flags)) {
        set_error(error, "Hello carries no authentication stamp");
        return false;
    }
    if ((hello.auth_flags & ~kAuthFlagsKnown) != 0) {
        set_error(error, "Hello carries unknown authentication flags");
        return false;
    }
    uint16_t signature_size = 0;
    if (!reader.u16(signature_size) || signature_size > kMaxSignatureLength ||
        !reader.bytes(hello.signature, signature_size)) {
        set_error(error, "Hello carries a malformed signature");
        return false;
    }
    uint16_t totp_size = 0;
    if (!reader.u16(totp_size) || totp_size > kMaxTotpLength ||
        !reader.string(hello.totp, totp_size)) {
        set_error(error, "Hello carries a malformed TOTP code");
        return false;
    }
    if (!reader.at_end()) {
        set_error(error, "Hello carries trailing bytes");
        return false;
    }
    if ((hello.auth_flags & kAuthFlagSigned) != 0 && hello.signature.empty()) {
        set_error(error, "Hello claims a signature it does not carry");
        return false;
    }
    if ((hello.auth_flags & kAuthFlagTrustDevice) != 0 &&
        (hello.auth_flags & kAuthFlagSigned) == 0) {
        set_error(error, "Hello asks for trust without a signature");
        return false;
    }
    if (!hello.totp.empty()) {
        bool digits = hello.totp.size() == 6;
        for (char c : hello.totp) {
            digits = digits && c >= '0' && c <= '9';
        }
        if (!digits) {
            set_error(error, "Hello carries a malformed TOTP code");
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> make_access_denied(AccessDenial code, const std::string& reason) {
    const size_t reason_size =
        reason.size() > kMaxAccessDenialReason ? kMaxAccessDenialReason : reason.size();
    std::vector<uint8_t> payload(3 + reason_size);
    uint8_t* cursor = payload.data();
    put_u8(cursor, static_cast<uint8_t>(code));
    put_u16(cursor, static_cast<uint16_t>(reason_size));
    std::memcpy(cursor, reason.data(), reason_size);
    return make_control(ControlType::AccessDenied, payload);
}

bool parse_access_denied(const std::vector<uint8_t>& payload, AccessDenial& code,
                         std::string& reason, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u8(raw_code) || !reader.u16(reason_size)) {
        set_error(error, "truncated AccessDenied");
        return false;
    }
    if (raw_code < static_cast<uint8_t>(AccessDenial::InvalidToken) ||
        raw_code > static_cast<uint8_t>(AccessDenial::AuthTimeout)) {
        set_error(error, "unknown access denial code " + std::to_string(raw_code));
        return false;
    }
    if (reason_size > kMaxAccessDenialReason || reader.remaining() != reason_size) {
        set_error(error, "AccessDenied carries a malformed reason");
        return false;
    }
    if (!reader.string(reason, reason_size)) {
        set_error(error, "truncated AccessDenied reason");
        return false;
    }
    code = static_cast<AccessDenial>(raw_code);
    return true;
}

namespace {

// HelloAck and StreamInfo share their payload byte for byte.
std::vector<uint8_t> build_server_info(const ServerInfo& info) {
    size_t display_count = info.displays.size();
    if (display_count > kMaxDisplays) {
        display_count = kMaxDisplays;
    }
    size_t extradata_size = info.audio.extradata.size();
    if (extradata_size > kMaxAudioExtradata) {
        extradata_size = kMaxAudioExtradata;
    }
    // 24 fixed bytes, the encoder name, the display count, the displays, the
    // 16-byte audio description and its AudioSpecificConfig, the captured
    // desktop size (the encoded size in the fixed block may be scaled down),
    // then the v13 permission mask.
    size_t size = 24 + info.encoder.size() + 2 + 16 + extradata_size + 4 + 2;
    for (size_t i = 0; i < display_count; ++i) {
        size += kDisplayFixedSize + info.displays[i].name.size();
    }
    std::vector<uint8_t> payload(size);
    uint8_t* cursor = payload.data();
    put_u8(cursor, codec_wire(info.codec));
    put_u8(cursor, 0);
    put_u16(cursor, info.width);
    put_u16(cursor, info.height);
    put_u16(cursor, info.framerate);
    put_u32(cursor, info.bitrate);
    put_u32(cursor, static_cast<uint32_t>(info.origin_x));
    put_u32(cursor, static_cast<uint32_t>(info.origin_y));
    put_u16(cursor, static_cast<uint16_t>(
                        (info.input_enabled ? kServerInfoInputEnabled : 0) |
                        (info.clipboard_enabled ? kServerInfoClipboardEnabled : 0) |
                        (info.audio.enabled ? kServerInfoAudioEnabled : 0) |
                        (info.file_transfer_enabled ? kServerInfoFileTransferEnabled : 0) |
                        (info.microphone_enabled ? kServerInfoMicrophoneEnabled : 0) |
                        (info.privacy_mode_enabled ? kServerInfoPrivacyModeEnabled : 0)));
    put_u16(cursor, static_cast<uint16_t>(info.encoder.size()));
    std::memcpy(cursor, info.encoder.data(), info.encoder.size());
    cursor += info.encoder.size();
    put_u16(cursor, static_cast<uint16_t>(display_count));
    for (size_t i = 0; i < display_count; ++i) {
        const DisplayDescription& display = info.displays[i];
        put_u16(cursor, static_cast<uint16_t>(display.name.size()));
        std::memcpy(cursor, display.name.data(), display.name.size());
        cursor += display.name.size();
        put_u32(cursor, static_cast<uint32_t>(display.left));
        put_u32(cursor, static_cast<uint32_t>(display.top));
        put_u32(cursor, static_cast<uint32_t>(display.right));
        put_u32(cursor, static_cast<uint32_t>(display.bottom));
        put_u16(cursor, display.rotation_degrees);
        put_u16(cursor, display.primary ? 1 : 0);
    }
    put_u8(cursor, info.audio.enabled ? 1 : 0);
    put_u8(cursor, audio_codec_wire(info.audio.codec));
    put_u16(cursor, info.audio.channels);
    put_u16(cursor, info.audio.frame_samples);
    put_u32(cursor, info.audio.sample_rate);
    put_u32(cursor, info.audio.bitrate);
    put_u16(cursor, static_cast<uint16_t>(extradata_size));
    if (extradata_size > 0) {
        std::memcpy(cursor, info.audio.extradata.data(), extradata_size);
        cursor += extradata_size;
    }
    put_u16(cursor, info.source_width);
    put_u16(cursor, info.source_height);
    uint16_t permissions = 0;
    if (info.input_enabled) {
        permissions |= kPermissionInput;
    }
    if (info.clipboard_enabled) {
        permissions |= kPermissionClipboard;
    }
    if (info.audio.enabled) {
        permissions |= kPermissionAudio;
    }
    if (info.file_transfer_enabled) {
        permissions |= kPermissionFileTransfer;
    }
    if (info.microphone_enabled) {
        permissions |= kPermissionMicrophone;
    }
    if (info.privacy_mode_enabled) {
        permissions |= kPermissionPrivacyMode;
    }
    put_u16(cursor, permissions);
    return payload;
}

}  // namespace

std::vector<uint8_t> make_hello_ack(const ServerInfo& info) {
    return make_control(ControlType::HelloAck, build_server_info(info));
}

std::vector<uint8_t> make_stream_info(const ServerInfo& info) {
    return make_control(ControlType::StreamInfo, build_server_info(info));
}

bool parse_hello_ack(const std::vector<uint8_t>& payload, ServerInfo& info, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t codec = 0;
    uint8_t reserved = 0;
    uint32_t origin_x = 0;
    uint32_t origin_y = 0;
    uint16_t flags = 0;
    uint16_t name_size = 0;
    if (!reader.u8(codec) || !reader.u8(reserved) || !reader.u16(info.width) ||
        !reader.u16(info.height) || !reader.u16(info.framerate) || !reader.u32(info.bitrate) ||
        !reader.u32(origin_x) || !reader.u32(origin_y) || !reader.u16(flags) ||
        !reader.u16(name_size)) {
        set_error(error, "truncated HelloAck");
        return false;
    }
    info.origin_x = static_cast<int32_t>(origin_x);
    info.origin_y = static_cast<int32_t>(origin_y);
    if ((flags & ~kServerInfoKnownFlags) != 0) {
        set_error(error, "HelloAck carries unknown flags");
        return false;
    }
    info.input_enabled = (flags & kServerInfoInputEnabled) != 0;
    info.clipboard_enabled = (flags & kServerInfoClipboardEnabled) != 0;
    const bool audio_flag = (flags & kServerInfoAudioEnabled) != 0;
    if (!codec_from_wire(codec, info.codec)) {
        set_error(error, "HelloAck names unknown codec " + std::to_string(codec));
        return false;
    }
    if (info.width == 0 || info.height == 0) {
        set_error(error, "HelloAck has an empty picture");
        return false;
    }
    if (name_size > reader.remaining()) {
        set_error(error, "HelloAck encoder name overruns the message");
        return false;
    }
    if (!reader.string(info.encoder, name_size)) {
        set_error(error, "HelloAck encoder name is truncated");
        return false;
    }
    uint16_t display_count = 0;
    if (!reader.u16(display_count)) {
        set_error(error, "HelloAck carries no display list");
        return false;
    }
    if (display_count > kMaxDisplays) {
        set_error(error, "HelloAck lists " + std::to_string(display_count) + " displays");
        return false;
    }
    info.displays.clear();
    info.displays.reserve(display_count);
    for (uint16_t i = 0; i < display_count; ++i) {
        DisplayDescription display;
        uint16_t display_name_size = 0;
        uint32_t left = 0;
        uint32_t top = 0;
        uint32_t right = 0;
        uint32_t bottom = 0;
        uint16_t display_flags = 0;
        if (!reader.u16(display_name_size)) {
            set_error(error, "truncated HelloAck display " + std::to_string(i));
            return false;
        }
        // After the name come four i32, the rotation and the flags.
        constexpr size_t kAfterName = 20;
        if (reader.remaining() < kAfterName ||
            display_name_size > reader.remaining() - kAfterName ||
            !reader.string(display.name, display_name_size) || !reader.u32(left) ||
            !reader.u32(top) || !reader.u32(right) || !reader.u32(bottom) ||
            !reader.u16(display.rotation_degrees) || !reader.u16(display_flags)) {
            set_error(error, "truncated HelloAck display " + std::to_string(i));
            return false;
        }
        display.left = static_cast<int32_t>(left);
        display.top = static_cast<int32_t>(top);
        display.right = static_cast<int32_t>(right);
        display.bottom = static_cast<int32_t>(bottom);
        display.primary = (display_flags & 0x1u) != 0;
        if ((display_flags & ~0x1u) != 0) {
            set_error(error, "HelloAck display " + std::to_string(i) + " has unknown flags");
            return false;
        }
        if (display.rotation_degrees != 0 && display.rotation_degrees != 90 &&
            display.rotation_degrees != 180 && display.rotation_degrees != 270) {
            set_error(error, "HelloAck display " + std::to_string(i) +
                                 " has an unknown rotation");
            return false;
        }
        if (display.right <= display.left || display.bottom <= display.top) {
            set_error(error, "HelloAck display " + std::to_string(i) + " has an empty rect");
            return false;
        }
        info.displays.push_back(std::move(display));
    }
    uint8_t audio_enabled = 0;
    uint8_t audio_codec = 0;
    uint16_t extradata_size = 0;
    info.audio = AudioInfo{};
    if (!reader.u8(audio_enabled) || !reader.u8(audio_codec) ||
        !reader.u16(info.audio.channels) || !reader.u16(info.audio.frame_samples) ||
        !reader.u32(info.audio.sample_rate) || !reader.u32(info.audio.bitrate) ||
        !reader.u16(extradata_size)) {
        set_error(error, "HelloAck carries no audio description");
        return false;
    }
    if ((audio_enabled != 0) != audio_flag) {
        set_error(error, "HelloAck audio flag disagrees with its audio block");
        return false;
    }
    info.audio.enabled = audio_flag;
    if (!audio_codec_from_wire(audio_codec, info.audio.codec)) {
        set_error(error, "HelloAck names unknown audio codec " + std::to_string(audio_codec));
        return false;
    }
    if (info.audio.channels != 1 && info.audio.channels != 2) {
        set_error(error, "HelloAck audio has " +
                             std::to_string(info.audio.channels) + " channels");
        return false;
    }
    if (!valid_audio_rate(info.audio.sample_rate)) {
        set_error(error, "HelloAck audio sample rate " +
                             std::to_string(info.audio.sample_rate) + " is not supported");
        return false;
    }
    const uint32_t expected_frame_samples =
        info.audio.codec == AudioCodec::Opus ? kOpusFrameSamples : kAudioFrameSamples;
    if (info.audio.frame_samples != expected_frame_samples) {
        set_error(error, "HelloAck audio frame length " +
                             std::to_string(info.audio.frame_samples) + " is not supported");
        return false;
    }
    if (info.audio.enabled && info.audio.codec == AudioCodec::Opus &&
        info.audio.sample_rate != 48000) {
        set_error(error, "HelloAck Opus audio is not at 48 kHz");
        return false;
    }
    if (extradata_size > kMaxAudioExtradata || extradata_size > reader.remaining()) {
        set_error(error, "HelloAck audio config overruns the message");
        return false;
    }
    if (info.audio.bitrate > 2000000u) {
        set_error(error, "HelloAck audio bitrate is outside the protocol limit");
        return false;
    }
    if (extradata_size > 0 && !reader.bytes(info.audio.extradata, extradata_size)) {
        set_error(error, "HelloAck audio config is truncated");
        return false;
    }
    if (info.audio.enabled && info.audio.extradata.empty()) {
        set_error(error, "HelloAck announced audio without its decoder config");
        return false;
    }
    if (!reader.u16(info.source_width) || !reader.u16(info.source_height)) {
        set_error(error, "HelloAck carries no source size");
        return false;
    }
    if (info.source_width == 0 || info.source_height == 0) {
        set_error(error, "HelloAck has an empty source picture");
        return false;
    }
    uint16_t permissions = 0;
    if (!reader.u16(permissions)) {
        set_error(error, "HelloAck carries no permissions");
        return false;
    }
    if ((permissions & ~kPermissionAll) != 0) {
        set_error(error, "HelloAck grants unknown permissions");
        return false;
    }
    if (permissions != (flags & kServerInfoKnownFlags)) {
        set_error(error, "HelloAck permission mask disagrees with its flags");
        return false;
    }
    info.input_enabled = (permissions & kPermissionInput) != 0;
    info.clipboard_enabled = (permissions & kPermissionClipboard) != 0;
    info.file_transfer_enabled = (permissions & kPermissionFileTransfer) != 0;
    info.microphone_enabled = (permissions & kPermissionMicrophone) != 0;
    info.privacy_mode_enabled = (permissions & kPermissionPrivacyMode) != 0;
    return true;
}

std::vector<uint8_t> make_stats(const ClientStats& stats) {
    std::vector<uint8_t> payload(48);
    uint8_t* cursor = payload.data();
    put_u64(cursor, stats.received);
    put_u64(cursor, stats.decoded);
    put_u64(cursor, stats.crc_errors);
    put_u64(cursor, stats.decode_errors);
    put_u64(cursor, stats.digest);
    put_u64(cursor, stats.elapsed_ms);
    return make_control(ControlType::Stats, payload);
}

bool parse_stats(const std::vector<uint8_t>& payload, ClientStats& stats, std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u64(stats.received) || !reader.u64(stats.decoded) ||
        !reader.u64(stats.crc_errors) || !reader.u64(stats.decode_errors) ||
        !reader.u64(stats.digest) || !reader.u64(stats.elapsed_ms)) {
        set_error(error, "truncated Stats");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_latency_probe(uint64_t client_us) {
    std::vector<uint8_t> payload(8);
    uint8_t* cursor = payload.data();
    put_u64(cursor, client_us);
    return make_control(ControlType::LatencyProbe, payload);
}

bool parse_latency_probe(const std::vector<uint8_t>& payload, uint64_t& client_us,
                         std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u64(client_us) || !reader.at_end()) {
        set_error(error, "malformed LatencyProbe");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_latency_reply(uint64_t client_us, uint64_t host_recv_us,
                                        uint64_t host_send_us) {
    std::vector<uint8_t> payload(24);
    uint8_t* cursor = payload.data();
    put_u64(cursor, client_us);
    put_u64(cursor, host_recv_us);
    put_u64(cursor, host_send_us);
    return make_control(ControlType::LatencyReply, payload);
}

bool parse_latency_reply(const std::vector<uint8_t>& payload, uint64_t& client_us,
                         uint64_t& host_recv_us, uint64_t& host_send_us, std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u64(client_us) || !reader.u64(host_recv_us) || !reader.u64(host_send_us) ||
        !reader.at_end()) {
        set_error(error, "malformed LatencyReply");
        return false;
    }
    if (host_send_us < host_recv_us) {
        set_error(error, "LatencyReply sends before it receives");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_input_event(const InputEvent& event) {
    std::vector<uint8_t> payload(16);
    uint8_t* cursor = payload.data();
    put_u8(cursor, static_cast<uint8_t>(event.kind));
    const bool mouse = event.kind == InputKind::Mouse;
    put_u8(cursor, mouse ? static_cast<uint8_t>(event.mouse.action)
                         : static_cast<uint8_t>(event.key.action));
    put_u16(cursor, mouse ? event.mouse.buttons : 0);
    put_u32(cursor, static_cast<uint32_t>(mouse ? event.mouse.x : 0));
    put_u32(cursor, static_cast<uint32_t>(mouse ? event.mouse.y : 0));
    put_u16(cursor, mouse ? 0 : event.key.hid_usage);
    put_u16(cursor, static_cast<uint16_t>(event.modifiers & kInputStateMask));
    return make_control(ControlType::InputEvent, payload);
}

bool parse_input_event(const std::vector<uint8_t>& payload, InputEvent& event, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t kind = 0;
    uint8_t action = 0;
    uint16_t buttons = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint16_t hid_usage = 0;
    uint16_t modifiers = 0;
    if (!reader.u8(kind) || !reader.u8(action) || !reader.u16(buttons) || !reader.u32(x) ||
        !reader.u32(y) || !reader.u16(hid_usage) || !reader.u16(modifiers)) {
        set_error(error, "truncated InputEvent");
        return false;
    }
    if ((modifiers & ~kInputStateMask) != 0) {
        set_error(error, "InputEvent carries unknown input state bits");
        return false;
    }
    event.modifiers = modifiers;
    switch (kind) {
        case static_cast<uint8_t>(InputKind::Mouse):
            if (action > static_cast<uint8_t>(MouseAction::WheelHorizontal)) {
                set_error(error, "InputEvent names unknown mouse action " +
                                     std::to_string(action));
                return false;
            }
            if (buttons & ~0x1Fu) {
                set_error(error, "InputEvent carries unknown mouse buttons");
                return false;
            }
            event.kind = InputKind::Mouse;
            event.mouse.action = static_cast<MouseAction>(action);
            event.mouse.buttons = buttons;
            event.mouse.x = static_cast<int32_t>(x);
            event.mouse.y = static_cast<int32_t>(y);
            return true;
        case static_cast<uint8_t>(InputKind::Key):
            if (action > static_cast<uint8_t>(KeyAction::Up)) {
                set_error(error, "InputEvent names unknown key action " + std::to_string(action));
                return false;
            }
            if (hid_usage == 0) {
                set_error(error, "InputEvent carries no HID usage");
                return false;
            }
            event.kind = InputKind::Key;
            event.key.action = static_cast<KeyAction>(action);
            event.key.hid_usage = hid_usage;
            return true;
        default:
            set_error(error, "InputEvent names unknown kind " + std::to_string(kind));
            return false;
    }
}

bool valid_clipboard_text(const std::string& text) {
    return !text.empty() && text.size() <= kMaxClipboardBytes && valid_utf8(text);
}

std::vector<uint8_t> make_clipboard_update(uint64_t seq, const std::string& text) {
    if (!valid_clipboard_text(text)) {
        return {};
    }
    std::vector<uint8_t> payload(8 + text.size());
    uint8_t* cursor = payload.data();
    put_u64(cursor, seq);
    std::memcpy(cursor, text.data(), text.size());
    return make_control(ControlType::ClipboardUpdate, payload);
}

bool parse_clipboard_update(const std::vector<uint8_t>& payload, ClipboardUpdate& update,
                            std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u64(update.seq)) {
        set_error(error, "truncated ClipboardUpdate");
        return false;
    }
    if (reader.remaining() == 0) {
        set_error(error, "ClipboardUpdate carries no text");
        return false;
    }
    if (reader.remaining() > kMaxClipboardBytes) {
        set_error(error, "ClipboardUpdate text is too long");
        return false;
    }
    reader.string(update.text, reader.remaining());
    if (!valid_clipboard_text(update.text)) {
        set_error(error, "ClipboardUpdate carries invalid clipboard text");
        return false;
    }
    return true;
}

// CursorUpdate: flags u8, reserved u8, x/y u32, hotspot u16 x2, shape id u64,
// width/height u16 x2, then width*height*4 premultiplied BGRA bytes or none.
constexpr size_t kCursorFixedSize = 1 + 1 + 4 + 4 + 2 + 2 + 8 + 2 + 2;

std::vector<uint8_t> make_cursor_update(const CursorUpdate& update) {
    if (update.width > kMaxCursorSize || update.height > kMaxCursorSize) {
        return {};
    }
    const size_t pixels = static_cast<size_t>(update.width) * update.height;
    // A sprite that does not fill the declared size is dropped rather than
    // truncated: half an image is worse than none, and the viewer keeps the
    // previous shape either way.
    const bool has_sprite = !update.sprite.empty();
    if (has_sprite && update.sprite.size() != pixels * 4u) {
        return {};
    }
    if (has_sprite && update.shape_id == 0) {
        return {};
    }
    std::vector<uint8_t> payload(kCursorFixedSize + (has_sprite ? pixels * 4u : 0u));
    uint8_t* cursor = payload.data();
    put_u8(cursor, update.visible ? 1 : 0);
    put_u8(cursor, has_sprite ? 1 : 0);
    put_u32(cursor, static_cast<uint32_t>(update.x));
    put_u32(cursor, static_cast<uint32_t>(update.y));
    put_u16(cursor, update.hotspot_x);
    put_u16(cursor, update.hotspot_y);
    put_u64(cursor, update.shape_id);
    put_u16(cursor, update.width);
    put_u16(cursor, update.height);
    if (has_sprite) {
        std::memcpy(cursor, update.sprite.data(), pixels * 4u);
    }
    return make_control(ControlType::CursorUpdate, payload);
}

bool parse_cursor_update(const std::vector<uint8_t>& payload, CursorUpdate& update,
                         std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t flags = 0;
    uint8_t shape_flags = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint16_t hotspot_x = 0;
    uint16_t hotspot_y = 0;
    uint64_t shape_id = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    update = CursorUpdate{};
    if (!reader.u8(flags) || !reader.u8(shape_flags) || !reader.u32(x) || !reader.u32(y) ||
        !reader.u16(hotspot_x) || !reader.u16(hotspot_y) || !reader.u64(shape_id) ||
        !reader.u16(width) || !reader.u16(height)) {
        set_error(error, "truncated CursorUpdate");
        return false;
    }
    if (width > kMaxCursorSize || height > kMaxCursorSize) {
        set_error(error, "CursorUpdate sprite is " + std::to_string(width) + "x" +
                             std::to_string(height));
        return false;
    }
    if ((flags & ~1u) != 0 || (shape_flags & ~1u) != 0) {
        set_error(error, "CursorUpdate has unknown flags");
        return false;
    }
    update.visible = (flags & 1u) != 0;
    update.x = static_cast<int32_t>(x);
    update.y = static_cast<int32_t>(y);
    update.hotspot_x = hotspot_x;
    update.hotspot_y = hotspot_y;
    update.shape_id = shape_id;
    update.width = width;
    update.height = height;
    const size_t pixels = static_cast<size_t>(width) * height;
    if ((shape_flags & 1u) != 0) {
        if (pixels == 0 || !reader.bytes(update.sprite, pixels * 4u)) {
            set_error(error, "CursorUpdate sprite is truncated");
            return false;
        }
    } else if (update.shape_id != 0 && pixels == 0) {
        set_error(error, "CursorUpdate names a shape without a size");
        return false;
    }
    if (!reader.at_end()) {
        set_error(error, "CursorUpdate carries trailing bytes");
        return false;
    }
    // A hotspot outside the sprite means the two sides disagree on the
    // geometry; refuse instead of drawing off-target.
    if (width != 0 && height != 0 &&
        (hotspot_x >= width || hotspot_y >= height)) {
        set_error(error, "CursorUpdate hotspot lies outside the sprite");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_permission_update(uint16_t permissions) {
    std::vector<uint8_t> payload(2);
    uint8_t* cursor = payload.data();
    put_u16(cursor, permissions);
    return make_control(ControlType::PermissionUpdate, payload);
}

bool parse_permission_update(const std::vector<uint8_t>& payload,
                             uint16_t& permissions, std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u16(permissions) || !reader.at_end()) {
        set_error(error, "malformed PermissionUpdate");
        return false;
    }
    if ((permissions & ~kPermissionAll) != 0) {
        set_error(error, "PermissionUpdate grants unknown permissions");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_switch_display(uint16_t display_index) {
    std::vector<uint8_t> payload(2);
    uint8_t* cursor = payload.data();
    put_u16(cursor, display_index);
    return make_control(ControlType::SwitchDisplay, payload);
}

bool parse_switch_display(const std::vector<uint8_t>& payload, uint16_t& display_index,
                          std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u16(display_index) || !reader.at_end()) {
        set_error(error, "malformed SwitchDisplay");
        return false;
    }
    // The host checks the index against its display list at the moment it
    // handles the request: the list can change while the session runs.
    return true;
}

std::vector<uint8_t> make_display_switch_result(uint16_t display_index, DisplaySwitchCode code,
                                                const std::string& reason) {
    std::string text = reason;
    if (text.size() > kMaxDisplaySwitchReason) {
        size_t cut = kMaxDisplaySwitchReason;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        text.resize(cut);
    }
    if (!valid_utf8(text)) {
        text.clear();
    }
    std::vector<uint8_t> payload(5 + text.size());
    uint8_t* cursor = payload.data();
    put_u16(cursor, display_index);
    put_u8(cursor, static_cast<uint8_t>(code));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(ControlType::DisplaySwitchResult, payload);
}

bool parse_display_switch_result(const std::vector<uint8_t>& payload, uint16_t& display_index,
                                 DisplaySwitchCode& code, std::string& reason,
                                 std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u16(display_index) || !reader.u8(raw_code) || !reader.u16(reason_size) ||
        !reader.string(reason, reason_size) || !reader.at_end()) {
        set_error(error, "malformed DisplaySwitchResult");
        return false;
    }
    if (raw_code > static_cast<uint8_t>(DisplaySwitchCode::Busy)) {
        set_error(error, "unknown display switch result code " + std::to_string(raw_code));
        return false;
    }
    if (reason.size() > kMaxDisplaySwitchReason) {
        set_error(error, "DisplaySwitchResult reason is too long");
        return false;
    }
    if (!reason.empty() && !valid_utf8(reason)) {
        set_error(error, "DisplaySwitchResult carries a malformed reason");
        return false;
    }
    code = static_cast<DisplaySwitchCode>(raw_code);
    return true;
}

namespace {

std::string truncate_utf8_reason(const std::string& reason, size_t limit) {
    if (reason.size() <= limit) {
        return valid_utf8(reason) ? reason : std::string();
    }
    size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(reason[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    std::string text = reason.substr(0, cut);
    if (!valid_utf8(text)) {
        text.clear();
    }
    return text;
}

bool parse_mode_code(uint8_t raw_code, DisplayModeCode& code, std::string* error) {
    if (raw_code > static_cast<uint8_t>(DisplayModeCode::Busy)) {
        set_error(error, "unknown display mode result code " + std::to_string(raw_code));
        return false;
    }
    code = static_cast<DisplayModeCode>(raw_code);
    return true;
}

}  // namespace

std::vector<uint8_t> make_list_display_modes(uint16_t display_index) {
    std::vector<uint8_t> payload(2);
    uint8_t* cursor = payload.data();
    put_u16(cursor, display_index);
    return make_control(ControlType::ListDisplayModes, payload);
}

bool parse_list_display_modes(const std::vector<uint8_t>& payload, uint16_t& display_index,
                              std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u16(display_index) || !reader.at_end()) {
        set_error(error, "malformed ListDisplayModes");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_display_modes(const DisplayModeList& list) {
    const std::string text = truncate_utf8_reason(list.reason, kMaxDisplayModeReason);
    const size_t count = list.code == DisplayModeCode::Ok
                             ? std::min<size_t>(list.modes.size(), kMaxDisplayModes)
                             : 0;
    std::vector<uint8_t> payload(5 + text.size() + 2 + count * 8);
    uint8_t* cursor = payload.data();
    put_u16(cursor, list.display);
    put_u8(cursor, static_cast<uint8_t>(list.code));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
        cursor += text.size();
    }
    put_u16(cursor, static_cast<uint16_t>(count));
    for (size_t i = 0; i < count; ++i) {
        const DisplayModeInfo& mode = list.modes[i];
        put_u16(cursor, mode.width);
        put_u16(cursor, mode.height);
        put_u16(cursor, mode.refresh_hz);
        put_u16(cursor, mode.current ? 1 : 0);
    }
    return make_control(ControlType::DisplayModes, payload);
}

bool parse_display_modes(const std::vector<uint8_t>& payload, DisplayModeList& list,
                         std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u16(list.display) || !reader.u8(raw_code) || !reader.u16(reason_size) ||
        !reader.string(list.reason, reason_size)) {
        set_error(error, "truncated DisplayModes");
        return false;
    }
    if (!parse_mode_code(raw_code, list.code, error)) {
        return false;
    }
    if (list.reason.size() > kMaxDisplayModeReason) {
        set_error(error, "DisplayModes reason is too long");
        return false;
    }
    if (!list.reason.empty() && !valid_utf8(list.reason)) {
        set_error(error, "DisplayModes carries a malformed reason");
        return false;
    }
    uint16_t count = 0;
    if (!reader.u16(count)) {
        set_error(error, "DisplayModes carries no mode count");
        return false;
    }
    if (count > kMaxDisplayModes) {
        set_error(error, "DisplayModes lists " + std::to_string(count) + " modes");
        return false;
    }
    if (list.code != DisplayModeCode::Ok && count != 0) {
        set_error(error, "a refused DisplayModes lists modes");
        return false;
    }
    list.modes.clear();
    list.modes.reserve(count);
    bool current_seen = false;
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t refresh = 0;
        uint16_t flags = 0;
        if (!reader.u16(width) || !reader.u16(height) || !reader.u16(refresh) ||
            !reader.u16(flags)) {
            set_error(error, "truncated DisplayModes entry " + std::to_string(i));
            return false;
        }
        if ((flags & ~1u) != 0) {
            set_error(error, "DisplayModes entry carries unknown flags");
            return false;
        }
        if (width == 0 || height == 0) {
            set_error(error, "DisplayModes entry has an empty picture");
            return false;
        }
        DisplayModeInfo mode;
        mode.width = width;
        mode.height = height;
        mode.refresh_hz = refresh;
        mode.current = (flags & 1u) != 0;
        if (mode.current) {
            if (current_seen) {
                set_error(error, "DisplayModes marks more than one current mode");
                return false;
            }
            current_seen = true;
        }
        list.modes.push_back(mode);
    }
    if (!reader.at_end()) {
        set_error(error, "DisplayModes has trailing bytes");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_set_display_mode(uint16_t display_index, uint16_t width,
                                           uint16_t height, uint16_t refresh_hz) {
    std::vector<uint8_t> payload(8);
    uint8_t* cursor = payload.data();
    put_u16(cursor, display_index);
    put_u16(cursor, width);
    put_u16(cursor, height);
    put_u16(cursor, refresh_hz);
    return make_control(ControlType::SetDisplayMode, payload);
}

bool parse_set_display_mode(const std::vector<uint8_t>& payload, uint16_t& display_index,
                            uint16_t& width, uint16_t& height, uint16_t& refresh_hz,
                            std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u16(display_index) || !reader.u16(width) || !reader.u16(height) ||
        !reader.u16(refresh_hz) || !reader.at_end()) {
        set_error(error, "malformed SetDisplayMode");
        return false;
    }
    if (width == 0 || height == 0) {
        set_error(error, "SetDisplayMode names an empty picture");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_display_mode_result(uint16_t display_index, uint16_t width,
                                              uint16_t height, uint16_t refresh_hz,
                                              DisplayModeCode code, const std::string& reason) {
    const std::string text = truncate_utf8_reason(reason, kMaxDisplayModeReason);
    std::vector<uint8_t> payload(11 + text.size());
    uint8_t* cursor = payload.data();
    put_u16(cursor, display_index);
    put_u16(cursor, width);
    put_u16(cursor, height);
    put_u16(cursor, refresh_hz);
    put_u8(cursor, static_cast<uint8_t>(code));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(ControlType::DisplayModeResult, payload);
}

bool parse_display_mode_result(const std::vector<uint8_t>& payload, uint16_t& display_index,
                               uint16_t& width, uint16_t& height, uint16_t& refresh_hz,
                               DisplayModeCode& code, std::string& reason, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u16(display_index) || !reader.u16(width) || !reader.u16(height) ||
        !reader.u16(refresh_hz) || !reader.u8(raw_code) || !reader.u16(reason_size) ||
        !reader.string(reason, reason_size) || !reader.at_end()) {
        set_error(error, "malformed DisplayModeResult");
        return false;
    }
    if (!parse_mode_code(raw_code, code, error)) {
        return false;
    }
    if (reason.size() > kMaxDisplayModeReason) {
        set_error(error, "DisplayModeResult reason is too long");
        return false;
    }
    if (!reason.empty() && !valid_utf8(reason)) {
        set_error(error, "DisplayModeResult carries a malformed reason");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_set_privacy_mode(bool enabled) {
    std::vector<uint8_t> payload(1);
    payload[0] = enabled ? 1 : 0;
    return make_control(ControlType::SetPrivacyMode, payload);
}

bool parse_set_privacy_mode(const std::vector<uint8_t>& payload, bool& enabled,
                            std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw = 0;
    if (!reader.u8(raw) || !reader.at_end() || raw > 1) {
        set_error(error, "malformed SetPrivacyMode");
        return false;
    }
    enabled = raw != 0;
    return true;
}

std::vector<uint8_t> make_privacy_mode_state(bool enabled, PrivacyModeCode code,
                                             const std::string& reason) {
    const std::string text = truncate_utf8_reason(reason, kMaxPrivacyModeReason);
    std::vector<uint8_t> payload(4 + text.size());
    uint8_t* cursor = payload.data();
    put_u8(cursor, enabled ? 1 : 0);
    put_u8(cursor, static_cast<uint8_t>(code));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(ControlType::PrivacyModeState, payload);
}

bool parse_privacy_mode_state(const std::vector<uint8_t>& payload, bool& enabled,
                              PrivacyModeCode& code, std::string& reason,
                              std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_enabled = 0;
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u8(raw_enabled) || !reader.u8(raw_code) || !reader.u16(reason_size) ||
        !reader.string(reason, reason_size) || !reader.at_end()) {
        set_error(error, "malformed PrivacyModeState");
        return false;
    }
    if (raw_enabled > 1) {
        set_error(error, "PrivacyModeState carries an invalid enabled flag");
        return false;
    }
    if (raw_code > static_cast<uint8_t>(PrivacyModeCode::Failed)) {
        set_error(error, "unknown privacy mode code " + std::to_string(raw_code));
        return false;
    }
    if (reason.size() > kMaxPrivacyModeReason) {
        set_error(error, "PrivacyModeState reason is too long");
        return false;
    }
    if (!reason.empty() && !valid_utf8(reason)) {
        set_error(error, "PrivacyModeState carries a malformed reason");
        return false;
    }
    if (raw_code != static_cast<uint8_t>(PrivacyModeCode::Ok) && raw_enabled != 0) {
        set_error(error, "PrivacyModeState reports a failure as enabled");
        return false;
    }
    enabled = raw_enabled != 0;
    code = static_cast<PrivacyModeCode>(raw_code);
    return true;
}

std::vector<uint8_t> make_desktop_state(DesktopKind kind) {
    std::vector<uint8_t> payload(1);
    payload[0] = static_cast<uint8_t>(kind);
    return make_control(ControlType::DesktopState, payload);
}

bool parse_desktop_state(const std::vector<uint8_t>& payload, DesktopKind& kind,
                         std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw = 0;
    if (!reader.u8(raw) || !reader.at_end() ||
        raw > static_cast<uint8_t>(DesktopKind::Unavailable)) {
        set_error(error, "malformed DesktopState");
        return false;
    }
    kind = static_cast<DesktopKind>(raw);
    return true;
}

std::vector<uint8_t> make_send_sas() {
    return make_control(ControlType::SendSas, {});
}

bool parse_send_sas(const std::vector<uint8_t>& payload, std::string* error) {
    if (!payload.empty()) {
        set_error(error, "SendSas must carry no payload");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_sas_result(SasCode code, const std::string& reason) {
    const std::string text = truncate_utf8_reason(reason, kMaxSasReason);
    std::vector<uint8_t> payload(3 + text.size());
    uint8_t* cursor = payload.data();
    put_u8(cursor, static_cast<uint8_t>(code));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(ControlType::SasResult, payload);
}

bool parse_sas_result(const std::vector<uint8_t>& payload, SasCode& code,
                      std::string& reason, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u8(raw_code) || !reader.u16(reason_size) ||
        !reader.string(reason, reason_size) || !reader.at_end()) {
        set_error(error, "malformed SasResult");
        return false;
    }
    if (raw_code > static_cast<uint8_t>(SasCode::Failed)) {
        set_error(error, "unknown SAS result code " + std::to_string(raw_code));
        return false;
    }
    if (reason.size() > kMaxSasReason) {
        set_error(error, "SasResult reason is too long");
        return false;
    }
    if (!reason.empty() && !valid_utf8(reason)) {
        set_error(error, "SasResult carries a malformed reason");
        return false;
    }
    code = static_cast<SasCode>(raw_code);
    return true;
}

std::vector<uint8_t> make_set_stream_options(const StreamOptions& options) {
    std::vector<uint8_t> payload(4);
    uint8_t* cursor = payload.data();
    put_u8(cursor, static_cast<uint8_t>(options.codec));
    put_u16(cursor, options.max_fps);
    put_u8(cursor, static_cast<uint8_t>(options.quality));
    return make_control(ControlType::SetStreamOptions, payload);
}

bool parse_set_stream_options(const std::vector<uint8_t>& payload, StreamOptions& options,
                              std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_codec = 0;
    uint8_t raw_quality = 0;
    if (!reader.u8(raw_codec) || !reader.u16(options.max_fps) ||
        !reader.u8(raw_quality) || !reader.at_end()) {
        set_error(error, "malformed SetStreamOptions");
        return false;
    }
    if (raw_codec > static_cast<uint8_t>(StreamCodecPreference::Hevc)) {
        set_error(error, "unknown stream codec preference " + std::to_string(raw_codec));
        return false;
    }
    if (options.max_fps > kMaxStreamFps) {
        set_error(error, "SetStreamOptions frame cap " + std::to_string(options.max_fps) +
                             " is outside 0.." + std::to_string(kMaxStreamFps));
        return false;
    }
    if (raw_quality > static_cast<uint8_t>(StreamQuality::Smooth)) {
        set_error(error, "unknown stream quality " + std::to_string(raw_quality));
        return false;
    }
    options.codec = static_cast<StreamCodecPreference>(raw_codec);
    options.quality = static_cast<StreamQuality>(raw_quality);
    return true;
}

std::vector<uint8_t> make_stream_options_result(const StreamOptionsResult& result) {
    const std::string text = truncate_utf8_reason(result.reason, kMaxStreamOptionReason);
    std::vector<uint8_t> payload(7 + text.size());
    uint8_t* cursor = payload.data();
    put_u8(cursor, static_cast<uint8_t>(result.code));
    put_u8(cursor, codec_wire(result.codec));
    put_u16(cursor, result.fps);
    put_u8(cursor, static_cast<uint8_t>(result.quality));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(ControlType::StreamOptionsResult, payload);
}

bool parse_stream_options_result(const std::vector<uint8_t>& payload,
                                 StreamOptionsResult& result, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint8_t raw_codec = 0;
    uint8_t raw_quality = 0;
    uint16_t reason_size = 0;
    if (!reader.u8(raw_code) || !reader.u8(raw_codec) || !reader.u16(result.fps) ||
        !reader.u8(raw_quality) || !reader.u16(reason_size) ||
        !reader.string(result.reason, reason_size) || !reader.at_end()) {
        set_error(error, "malformed StreamOptionsResult");
        return false;
    }
    if (raw_code > static_cast<uint8_t>(StreamOptionsCode::Busy)) {
        set_error(error, "unknown stream options code " + std::to_string(raw_code));
        return false;
    }
    if (!codec_from_wire(raw_codec, result.codec)) {
        set_error(error, "unknown stream options codec " + std::to_string(raw_codec));
        return false;
    }
    if (raw_quality > static_cast<uint8_t>(StreamQuality::Smooth)) {
        set_error(error, "unknown stream quality " + std::to_string(raw_quality));
        return false;
    }
    if (result.fps > kMaxStreamFps) {
        set_error(error, "stream options frame rate " + std::to_string(result.fps) +
                             " is outside 0.." + std::to_string(kMaxStreamFps));
        return false;
    }
    if (result.reason.size() > kMaxStreamOptionReason) {
        set_error(error, "StreamOptionsResult reason is too long");
        return false;
    }
    if (!result.reason.empty() && !valid_utf8(result.reason)) {
        set_error(error, "StreamOptionsResult carries a malformed reason");
        return false;
    }
    result.code = static_cast<StreamOptionsCode>(raw_code);
    result.quality = static_cast<StreamQuality>(raw_quality);
    return true;
}

namespace {

bool parse_file_code(uint8_t raw, FileCode& code, std::string* error) {
    if (raw > static_cast<uint8_t>(FileCode::Cancelled)) {
        set_error(error, "unknown file result code " + std::to_string(raw));
        return false;
    }
    code = static_cast<FileCode>(raw);
    return true;
}

bool parse_file_reason(const std::string& reason, size_t limit, const char* what,
                       std::string* error) {
    if (reason.size() > limit) {
        set_error(error, std::string(what) + " reason is too long");
        return false;
    }
    if (!reason.empty() && !valid_utf8(reason)) {
        set_error(error, std::string(what) + " carries a malformed reason");
        return false;
    }
    return true;
}

}  // namespace

std::vector<uint8_t> make_file_offer(const FileOffer& offer) {
    const std::string name = truncate_utf8_reason(offer.name, kMaxFileNameBytes);
    std::vector<uint8_t> payload(4 + 8 + 1 + 32 + 2 + name.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, offer.transfer_id);
    put_u64(cursor, offer.size);
    put_u8(cursor, offer.resume ? kFileOfferFlagResume : 0);
    std::memcpy(cursor, offer.sha256.data(), offer.sha256.size());
    cursor += offer.sha256.size();
    put_u16(cursor, static_cast<uint16_t>(name.size()));
    if (!name.empty()) {
        std::memcpy(cursor, name.data(), name.size());
    }
    return make_control(ControlType::FileOffer, payload);
}

bool parse_file_offer(const std::vector<uint8_t>& payload, FileOffer& offer, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t flags = 0;
    uint16_t name_size = 0;
    if (!reader.u32(offer.transfer_id) || !reader.u64(offer.size) || !reader.u8(flags) ||
        !reader.raw(offer.sha256.data(), offer.sha256.size()) || !reader.u16(name_size)) {
        set_error(error, "truncated FileOffer");
        return false;
    }
    if ((flags & ~kFileOfferFlagsKnown) != 0) {
        set_error(error, "FileOffer carries unknown flags");
        return false;
    }
    offer.resume = (flags & kFileOfferFlagResume) != 0;
    if (offer.size > kMaxFileTransferBytes) {
        set_error(error, "FileOffer names a file past the protocol limit");
        return false;
    }
    if (name_size > kMaxFileNameBytes) {
        set_error(error, "FileOffer name is too long");
        return false;
    }
    if (!reader.string(offer.name, name_size)) {
        set_error(error, "FileOffer name overruns the message");
        return false;
    }
    if (!reader.at_end()) {
        set_error(error, "FileOffer has trailing bytes");
        return false;
    }
    if (offer.name.empty() || !valid_utf8(offer.name)) {
        set_error(error, "FileOffer carries an unusable name");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_file_answer(const FileAnswer& answer) {
    const std::string text = truncate_utf8_reason(answer.reason, kMaxFileReasonBytes);
    std::vector<uint8_t> payload(4 + 1 + 8 + 2 + text.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, answer.transfer_id);
    put_u8(cursor, static_cast<uint8_t>(answer.code));
    put_u64(cursor, answer.code == FileCode::Ok ? answer.resume_offset : 0);
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(ControlType::FileAnswer, payload);
}

bool parse_file_answer(const std::vector<uint8_t>& payload, FileAnswer& answer,
                       std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u32(answer.transfer_id) || !reader.u8(raw_code) ||
        !reader.u64(answer.resume_offset) || !reader.u16(reason_size) ||
        !reader.string(answer.reason, reason_size) || !reader.at_end()) {
        set_error(error, "malformed FileAnswer");
        return false;
    }
    if (!parse_file_code(raw_code, answer.code, error)) {
        return false;
    }
    if (answer.code != FileCode::Ok) {
        if (answer.resume_offset != 0) {
            set_error(error, "a refused FileAnswer carries a resume offset");
            return false;
        }
    } else if (answer.resume_offset > kMaxFileTransferBytes) {
        set_error(error, "FileAnswer resume offset is past the protocol limit");
        return false;
    }
    return parse_file_reason(answer.reason, kMaxFileReasonBytes, "FileAnswer", error);
}

std::vector<uint8_t> make_file_result(const FileResult& result) {
    const std::string text = truncate_utf8_reason(result.reason, kMaxFileReasonBytes);
    std::vector<uint8_t> payload(4 + 1 + 8 + 2 + text.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, result.transfer_id);
    put_u8(cursor, static_cast<uint8_t>(result.code));
    put_u64(cursor, result.received);
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(ControlType::FileResult, payload);
}

bool parse_file_result(const std::vector<uint8_t>& payload, FileResult& result,
                       std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u32(result.transfer_id) || !reader.u8(raw_code) ||
        !reader.u64(result.received) || !reader.u16(reason_size) ||
        !reader.string(result.reason, reason_size) || !reader.at_end()) {
        set_error(error, "malformed FileResult");
        return false;
    }
    if (!parse_file_code(raw_code, result.code, error)) {
        return false;
    }
    if (result.code != FileCode::Ok && result.code != FileCode::DigestMismatch &&
        result.code != FileCode::IoError && result.code != FileCode::Cancelled) {
        set_error(error, "FileResult carries a code it cannot report");
        return false;
    }
    if (result.received > kMaxFileTransferBytes) {
        set_error(error, "FileResult received count is past the protocol limit");
        return false;
    }
    return parse_file_reason(result.reason, kMaxFileReasonBytes, "FileResult", error);
}

std::vector<uint8_t> make_file_cancel(const FileCancel& cancel) {
    const std::string text = truncate_utf8_reason(cancel.reason, kMaxFileReasonBytes);
    std::vector<uint8_t> payload(4 + 1 + 2 + text.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, cancel.transfer_id);
    put_u8(cursor, static_cast<uint8_t>(cancel.code));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(ControlType::FileCancel, payload);
}

bool parse_file_cancel(const std::vector<uint8_t>& payload, FileCancel& cancel,
                       std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u32(cancel.transfer_id) || !reader.u8(raw_code) ||
        !reader.u16(reason_size) || !reader.string(cancel.reason, reason_size) ||
        !reader.at_end()) {
        set_error(error, "malformed FileCancel");
        return false;
    }
    if (!parse_file_code(raw_code, cancel.code, error)) {
        return false;
    }
    if (cancel.code != FileCode::Cancelled) {
        set_error(error, "FileCancel carries a code other than Cancelled");
        return false;
    }
    return parse_file_reason(cancel.reason, kMaxFileReasonBytes, "FileCancel", error);
}

void encode_file_stream_header(const FileStreamHeader& header,
                               uint8_t out[kFileStreamHeaderSize]) {
    uint8_t* cursor = out;
    std::memcpy(cursor, kFileStreamMagic, sizeof(kFileStreamMagic));
    cursor += sizeof(kFileStreamMagic);
    put_u8(cursor, kFileStreamVersion);
    put_u8(cursor, 0);
    put_u8(cursor, 0);
    put_u8(cursor, 0);
    put_u32(cursor, header.transfer_id);
    put_u64(cursor, header.offset);
}

bool decode_file_stream_header(const uint8_t* data, size_t size, FileStreamHeader& header,
                               std::string* error) {
    if (size != kFileStreamHeaderSize) {
        set_error(error, "truncated CFIL header");
        return false;
    }
    if (std::memcmp(data, kFileStreamMagic, sizeof(kFileStreamMagic)) != 0) {
        set_error(error, "CFIL stream has bad magic");
        return false;
    }
    if (data[4] != kFileStreamVersion) {
        set_error(error, "CFIL stream version " + std::to_string(data[4]) +
                             " is not supported");
        return false;
    }
    if (data[5] != 0 || data[6] != 0 || data[7] != 0) {
        set_error(error, "CFIL header reserved bytes are not zero");
        return false;
    }
    Reader reader(data + 8, size - 8);
    if (!reader.u32(header.transfer_id) || !reader.u64(header.offset) || !reader.at_end()) {
        set_error(error, "malformed CFIL header");
        return false;
    }
    if (header.offset > kMaxFileTransferBytes) {
        set_error(error, "CFIL offset is past the protocol limit");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_control(ControlType type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> message(kControlHeaderSize + payload.size());
    uint8_t* cursor = message.data();
    std::memcpy(cursor, kControlMagic, sizeof(kControlMagic));
    cursor += sizeof(kControlMagic);
    put_u16(cursor, kProtocolVersion);
    put_u16(cursor, static_cast<uint16_t>(type));
    put_u32(cursor, static_cast<uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(cursor, payload.data(), payload.size());
    }
    return message;
}
bool ControlParser::push(const uint8_t* data, size_t size,
                         std::vector<std::pair<ControlType, std::vector<uint8_t>>>& messages,
                         std::string* error) {
    if (!data || size == 0) {
        return true;
    }
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    } else if (offset_ > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<ptrdiff_t>(offset_));
        offset_ = 0;
    }
    buffer_.insert(buffer_.end(), data, data + size);
    if (buffer_.size() > kMaxControlBuffer) {
        set_error(error, "control stream buffer overflow");
        return false;
    }

    size_t consumed = 0;
    while (buffer_.size() - consumed >= kControlHeaderSize) {
        const uint8_t* header = buffer_.data() + consumed;
        if (std::memcmp(header, kControlMagic, sizeof(kControlMagic)) != 0) {
            set_error(error, "bad control magic");
            return false;
        }
        Reader reader(header + sizeof(kControlMagic), kControlHeaderSize - sizeof(kControlMagic));
        uint16_t version = 0;
        uint16_t raw_type = 0;
        uint32_t length = 0;
        reader.u16(version);
        reader.u16(raw_type);
        reader.u32(length);
        if (version != kProtocolVersion) {
            set_error(error, "control protocol version " + std::to_string(version));
            return false;
        }
        if (length > kMaxControlPayload) {
            set_error(error, "control message too large");
            return false;
        }
        if (buffer_.size() - consumed < kControlHeaderSize + length) {
            break;
        }
        const uint8_t* payload = header + kControlHeaderSize;
        messages.emplace_back(static_cast<ControlType>(raw_type),
                              std::vector<uint8_t>(payload, payload + length));
        consumed += kControlHeaderSize + length;
    }
    if (consumed > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<ptrdiff_t>(consumed));
    }
    return true;
}

void ControlParser::reset() {
    buffer_.clear();
    offset_ = 0;
}

bool FrameParser::push(const uint8_t* data, size_t size, std::string* error) {
    if (!data || size == 0) {
        return true;
    }
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    } else if (offset_ > 0 && offset_ >= 64u * 1024u) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<ptrdiff_t>(offset_));
        offset_ = 0;
    }
    buffer_.insert(buffer_.end(), data, data + size);
    if (buffered() > kMaxFrameBuffer) {
        set_error(error, "video stream buffer overflow");
        return false;
    }
    return true;
}

FrameParser::Status FrameParser::next(FrameHeader& header, const uint8_t** payload,
                                      std::string* error) {
    const size_t available = buffered();
    if (available < kFrameHeaderSize) {
        return Status::Incomplete;
    }
    const uint8_t* front = buffer_.data() + offset_;
    if (!decode_frame_header(front, available, header, error)) {
        return Status::Error;
    }
    const size_t total = kFrameHeaderSize + header.payload_size;
    if (available < total) {
        return Status::Incomplete;
    }
    *payload = front + kFrameHeaderSize;
    current_payload_ = header.payload_size;
    return Status::Frame;
}

void FrameParser::consume() {
    offset_ += kFrameHeaderSize + current_payload_;
    current_payload_ = 0;
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    }
}

void FrameParser::reset() {
    buffer_.clear();
    offset_ = 0;
    current_payload_ = 0;
}

bool AudioParser::push(const uint8_t* data, size_t size, std::string* error) {
    if (!data || size == 0) {
        return true;
    }
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    } else if (offset_ > 0 && offset_ >= 64u * 1024u) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<ptrdiff_t>(offset_));
        offset_ = 0;
    }
    buffer_.insert(buffer_.end(), data, data + size);
    if (buffered() > kMaxAudioBuffer) {
        set_error(error, "audio stream buffer overflow");
        return false;
    }
    return true;
}

AudioParser::Status AudioParser::next(AudioHeader& header, const uint8_t** payload,
                                      std::string* error) {
    const size_t available = buffered();
    if (available < kAudioHeaderSize) {
        return Status::Incomplete;
    }
    const uint8_t* front = buffer_.data() + offset_;
    if (!decode_audio_header(front, available, header, error)) {
        return Status::Error;
    }
    const size_t total = kAudioHeaderSize + header.payload_size;
    if (available < total) {
        return Status::Incomplete;
    }
    *payload = front + kAudioHeaderSize;
    current_payload_ = header.payload_size;
    return Status::Packet;
}

void AudioParser::consume() {
    offset_ += kAudioHeaderSize + current_payload_;
    current_payload_ = 0;
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    }
}

void AudioParser::reset() {
    buffer_.clear();
    offset_ = 0;
    current_payload_ = 0;
}

}  // namespace chast
