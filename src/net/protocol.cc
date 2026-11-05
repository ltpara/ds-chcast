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
    switch (codec) {
        case VideoCodec::H264:
            return 1;
        case VideoCodec::Hevc:
            return 2;
        case VideoCodec::Av1:
            return 3;
    }
    return 0;
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
    if (value == 3) {
        codec = VideoCodec::Av1;
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

// Defined further down with the other text helpers; the early makers need it.
std::string truncate_utf8_reason(const std::string& reason, size_t limit);

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
    if ((flags & ~0x3u) != 0) {
        set_error(error, "frame header carries unknown flags");
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
    uint8_t reserved = 0;
    if (!reader.u8(reserved) || reserved != 0) {
        set_error(error, "frame header has a malformed reserved byte");
        return false;
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
    const std::string device_name =
        truncate_utf8_reason(hello.device_name, kMaxDeviceNameLength);
    const size_t name_size = device_name.size();
    const size_t signature_size = hello.signature.size() > kMaxSignatureLength
                                      ? kMaxSignatureLength
                                      : hello.signature.size();
    const size_t totp_size = hello.totp.size() > kMaxTotpLength ? kMaxTotpLength
                                                                 : hello.totp.size();
    const size_t audit_size = hello.audit_ref.size() > kMaxAuditRefLength
                                  ? kMaxAuditRefLength
                                  : hello.audit_ref.size();
    std::vector<uint8_t> payload(6 + token_size + 2 + password_size + 2 + 2 + key_size +
                                 1 + 2 + name_size + kHelloNonceBytes + 8 + 1 + 2 +
                                 signature_size + 2 + totp_size + 2 + audit_size + 2);
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
        std::memcpy(cursor, device_name.data(), name_size);
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
        cursor += totp_size;
    }
    put_u16(cursor, static_cast<uint16_t>(audit_size));
    if (audit_size > 0) {
        std::memcpy(cursor, hello.audit_ref.data(), audit_size);
        cursor += audit_size;
    }
    put_u8(cursor, static_cast<uint8_t>(hello.mode));
    put_u8(cursor, hello.capabilities);
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
    if (hello.display != kDisplayAuto && hello.display != kDisplayAll &&
        hello.display >= kMaxDisplayCount) {
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
    uint16_t audit_size = 0;
    if (!reader.u16(audit_size) || audit_size > kMaxAuditRefLength ||
        !reader.string(hello.audit_ref, audit_size)) {
        set_error(error, "Hello carries a malformed audit reference");
        return false;
    }
    if (!valid_audit_ref(hello.audit_ref)) {
        set_error(error, "Hello carries an audit reference that is not lowercase hex");
        return false;
    }
    uint8_t mode = 0;
    if (!reader.u8(mode)) {
        set_error(error, "Hello carries no session mode");
        return false;
    }
    if (mode > static_cast<uint8_t>(SessionMode::PortForward)) {
        set_error(error, "Hello asks for session mode " + std::to_string(mode));
        return false;
    }
    hello.mode = static_cast<SessionMode>(mode);
    if (!reader.u8(hello.capabilities)) {
        set_error(error, "Hello carries no client capabilities");
        return false;
    }
    if ((hello.capabilities & ~kClientCapabilitiesKnown) != 0) {
        set_error(error, "Hello carries unknown client capabilities");
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
    const std::string text = truncate_utf8_reason(reason, kMaxAccessDenialReason);
    std::vector<uint8_t> payload(3 + text.size());
    uint8_t* cursor = payload.data();
    put_u8(cursor, static_cast<uint8_t>(code));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
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
    if (!reason.empty() && !valid_utf8(reason)) {
        set_error(error, "AccessDenied reason is not valid UTF-8");
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
    // the v13 permission mask, then the v41 session mode.
    size_t size = 24 + info.encoder.size() + 2 + 16 + extradata_size + 4 + 2 + 1;
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
                        (info.privacy_mode_enabled ? kServerInfoPrivacyModeEnabled : 0) |
                        (info.port_forward_enabled ? kServerInfoPortForwardEnabled : 0) |
                        (info.terminal_enabled ? kServerInfoTerminalEnabled : 0) |
                        (info.record_enabled ? kServerInfoRecordEnabled : 0) |
                        (info.camera_enabled ? kServerInfoCameraEnabled : 0) |
                        (info.merged ? kServerInfoMergedLayout : 0) |
                        (info.files_only ? kServerInfoFileTransferOnly : 0) |
                        (info.audio_datagram && info.audio.enabled ? kServerInfoAudioDatagram : 0) |
                        (info.control_only ? kServerInfoControlOnly : 0) |
                        (info.terminal_persist && info.terminal_enabled
                             ? kServerInfoTerminalPersist
                             : 0)));
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
    if (info.port_forward_enabled) {
        permissions |= kPermissionPortForward;
    }
    if (info.terminal_enabled) {
        permissions |= kPermissionTerminal;
    }
    if (info.record_enabled) {
        permissions |= kPermissionRecord;
    }
    if (info.camera_enabled) {
        permissions |= kPermissionCamera;
    }
    if (info.restart_enabled) {
        permissions |= kPermissionRestart;
    }
    if (info.block_input_enabled) {
        permissions |= kPermissionBlockInput;
    }
    if (info.os_login_enabled) {
        permissions |= kPermissionOsLogin;
    }
    if (info.session_select_enabled) {
        permissions |= kPermissionSessionSelect;
    }
    put_u16(cursor, permissions);
    put_u8(cursor, static_cast<uint8_t>(info.mode));
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
    if (reserved != 0) {
        set_error(error, "HelloAck carries a non-zero reserved byte");
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
    info.merged = (flags & kServerInfoMergedLayout) != 0;
    info.files_only = (flags & kServerInfoFileTransferOnly) != 0;
    info.control_only = (flags & kServerInfoControlOnly) != 0;
    info.terminal_persist = (flags & kServerInfoTerminalPersist) != 0;
    const bool audio_flag = (flags & kServerInfoAudioEnabled) != 0;
    info.audio_datagram = (flags & kServerInfoAudioDatagram) != 0;
    if (info.audio_datagram && !audio_flag) {
        set_error(error, "HelloAck announces datagram audio without audio");
        return false;
    }
    if (!codec_from_wire(codec, info.codec)) {
        set_error(error, "HelloAck names unknown codec " + std::to_string(codec));
        return false;
    }
    if ((info.files_only || info.control_only) && info.merged) {
        set_error(error, "HelloAck is control-only and merged at once");
        return false;
    }
    if ((info.files_only || info.control_only) && audio_flag) {
        set_error(error, "HelloAck announces audio in a control-only session");
        return false;
    }
    if ((info.files_only || info.control_only) && (info.width != 0 || info.height != 0)) {
        set_error(error, "HelloAck has a picture in a control-only session");
        return false;
    }
    if (!info.files_only && !info.control_only && (info.width == 0 || info.height == 0)) {
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
        if (!info.files_only && !info.control_only) {
            set_error(error, "HelloAck has an empty source picture");
            return false;
        }
    } else if (info.files_only || info.control_only) {
        set_error(error, "HelloAck has a source picture in a control-only session");
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
    uint8_t mode = 0;
    if (!reader.u8(mode)) {
        set_error(error, "HelloAck carries no session mode");
        return false;
    }
    if (mode > static_cast<uint8_t>(SessionMode::PortForward)) {
        set_error(error, "HelloAck names session mode " + std::to_string(mode));
        return false;
    }
    info.mode = static_cast<SessionMode>(mode);
    const bool control_mode = info.mode == SessionMode::Terminal ||
                              info.mode == SessionMode::Camera ||
                              info.mode == SessionMode::PortForward;
    if (info.files_only != (info.mode == SessionMode::FileTransfer)) {
        set_error(error, "HelloAck session mode disagrees with its file-transfer flag");
        return false;
    }
    if (info.control_only != control_mode) {
        set_error(error, "HelloAck session mode disagrees with its control-only flag");
        return false;
    }
    // Restart, block input, OS login and session select have no flag of their
    // own (their bits collide with the files-only and datagram-audio flags, or
    // have no flag left), so they live in the mask alone; every other
    // permission must still match its flag.
    if (permissions !=
        ((flags & kServerInfoPermissionFlags) |
         (permissions & (kPermissionRestart | kPermissionBlockInput |
                         kPermissionOsLogin | kPermissionSessionSelect)))) {
        set_error(error, "HelloAck permission mask disagrees with its flags");
        return false;
    }
    if (info.files_only && (permissions & ~kPermissionFileTransfer) != 0) {
        set_error(error, "HelloAck grants more than file transfer in a files-only session");
        return false;
    }
    if (control_mode) {
        const uint16_t mode_permission =
            info.mode == SessionMode::Terminal
                ? kPermissionTerminal
                : info.mode == SessionMode::Camera ? kPermissionCamera
                                                   : kPermissionPortForward;
        if ((permissions & ~mode_permission) != 0 ||
            (permissions & mode_permission) == 0) {
            set_error(error,
                      "HelloAck grants a permission the control-only session cannot use");
            return false;
        }
    }
    if (info.merged) {
        if (info.displays.empty()) {
            set_error(error, "HelloAck merged layout lists no displays");
            return false;
        }
        int64_t left = info.displays.front().left;
        int64_t top = info.displays.front().top;
        int64_t right = info.displays.front().right;
        int64_t bottom = info.displays.front().bottom;
        for (const DisplayDescription& display : info.displays) {
            left = std::min<int64_t>(left, display.left);
            top = std::min<int64_t>(top, display.top);
            right = std::max<int64_t>(right, display.right);
            bottom = std::max<int64_t>(bottom, display.bottom);
        }
        const int64_t union_width = right - left;
        const int64_t union_height = bottom - top;
        // The host pads the union up to even dimensions so the NV12 picture
        // can be encoded, so source may exceed the exact union by one pixel.
        if (info.origin_x != left || info.origin_y != top ||
            info.source_width < union_width || info.source_width - union_width > 1 ||
            info.source_height < union_height || info.source_height - union_height > 1) {
            set_error(error, "HelloAck merged geometry does not match its displays");
            return false;
        }
    }
    info.input_enabled = (permissions & kPermissionInput) != 0;
    info.clipboard_enabled = (permissions & kPermissionClipboard) != 0;
    info.file_transfer_enabled = (permissions & kPermissionFileTransfer) != 0;
    info.microphone_enabled = (permissions & kPermissionMicrophone) != 0;
    info.privacy_mode_enabled = (permissions & kPermissionPrivacyMode) != 0;
    info.port_forward_enabled = (permissions & kPermissionPortForward) != 0;
    info.terminal_enabled = (permissions & kPermissionTerminal) != 0;
    info.record_enabled = (permissions & kPermissionRecord) != 0;
    info.camera_enabled = (permissions & kPermissionCamera) != 0;
    info.restart_enabled = (permissions & kPermissionRestart) != 0;
    info.block_input_enabled = (permissions & kPermissionBlockInput) != 0;
    info.os_login_enabled = (permissions & kPermissionOsLogin) != 0;
    info.session_select_enabled = (permissions & kPermissionSessionSelect) != 0;
    if (info.terminal_persist && !info.terminal_enabled) {
        set_error(error, "HelloAck announces terminal persistence without terminals");
        return false;
    }
    if (!reader.at_end()) {
        set_error(error, "HelloAck carries trailing bytes");
        return false;
    }
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
        !reader.u64(stats.digest) || !reader.u64(stats.elapsed_ms) ||
        !reader.at_end()) {
        set_error(error, "malformed Stats");
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
    std::vector<uint8_t> payload(24);
    uint8_t* cursor = payload.data();
    uint8_t action = 0;
    uint16_t buttons = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint16_t hid_usage = 0;
    uint16_t pressure = 0;
    uint8_t contact = 0;
    uint8_t flags = 0;
    int16_t tilt_x = 0;
    int16_t tilt_y = 0;
    switch (event.kind) {
        case InputKind::Mouse:
            action = static_cast<uint8_t>(event.mouse.action);
            buttons = event.mouse.buttons;
            x = static_cast<uint32_t>(event.mouse.x);
            y = static_cast<uint32_t>(event.mouse.y);
            break;
        case InputKind::Key:
            action = static_cast<uint8_t>(event.key.action);
            hid_usage = event.key.hid_usage;
            break;
        case InputKind::Touch:
            action = static_cast<uint8_t>(event.touch.action);
            contact = event.touch.contact;
            x = static_cast<uint32_t>(event.touch.x);
            y = static_cast<uint32_t>(event.touch.y);
            pressure = event.touch.pressure;
            break;
        case InputKind::Pen:
            action = static_cast<uint8_t>(event.pen.action);
            x = static_cast<uint32_t>(event.pen.x);
            y = static_cast<uint32_t>(event.pen.y);
            pressure = event.pen.pressure;
            flags = event.pen.flags;
            tilt_x = event.pen.tilt_x;
            tilt_y = event.pen.tilt_y;
            break;
    }
    put_u8(cursor, static_cast<uint8_t>(event.kind));
    put_u8(cursor, action);
    put_u16(cursor, buttons);
    put_u32(cursor, x);
    put_u32(cursor, y);
    put_u16(cursor, hid_usage);
    put_u16(cursor, static_cast<uint16_t>(event.modifiers & kInputStateMask));
    put_u16(cursor, pressure);
    put_u8(cursor, contact);
    put_u8(cursor, flags);
    put_u16(cursor, static_cast<uint16_t>(tilt_x));
    put_u16(cursor, static_cast<uint16_t>(tilt_y));
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
    uint16_t pressure = 0;
    uint8_t contact = 0;
    uint8_t flags = 0;
    uint16_t tilt_x_raw = 0;
    uint16_t tilt_y_raw = 0;
    if (!reader.u8(kind) || !reader.u8(action) || !reader.u16(buttons) || !reader.u32(x) ||
        !reader.u32(y) || !reader.u16(hid_usage) || !reader.u16(modifiers) ||
        !reader.u16(pressure) || !reader.u8(contact) || !reader.u8(flags) ||
        !reader.u16(tilt_x_raw) || !reader.u16(tilt_y_raw)) {
        set_error(error, "truncated InputEvent");
        return false;
    }
    if (!reader.at_end()) {
        set_error(error, "InputEvent carries trailing bytes");
        return false;
    }
    if ((modifiers & ~kInputStateMask) != 0) {
        set_error(error, "InputEvent carries unknown input state bits");
        return false;
    }
    event.modifiers = modifiers;
    const int16_t tilt_x = static_cast<int16_t>(tilt_x_raw);
    const int16_t tilt_y = static_cast<int16_t>(tilt_y_raw);
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
            if (pressure != 0 || contact != 0 || flags != 0 || tilt_x != 0 || tilt_y != 0 ||
                hid_usage != 0) {
                set_error(error, "InputEvent mouse event carries non-mouse fields");
                return false;
            }
            event.kind = InputKind::Mouse;
            event.mouse.action = static_cast<MouseAction>(action);
            event.mouse.buttons = buttons;
            event.mouse.x = static_cast<int32_t>(x);
            event.mouse.y = static_cast<int32_t>(y);
            break;
        case static_cast<uint8_t>(InputKind::Key):
            if (action > static_cast<uint8_t>(KeyAction::Up)) {
                set_error(error, "InputEvent names unknown key action " + std::to_string(action));
                return false;
            }
            if (hid_usage == 0) {
                set_error(error, "InputEvent carries no HID usage");
                return false;
            }
            if (buttons != 0 || pressure != 0 || contact != 0 || flags != 0 || tilt_x != 0 ||
                tilt_y != 0) {
                set_error(error, "InputEvent key event carries non-key fields");
                return false;
            }
            event.kind = InputKind::Key;
            event.key.action = static_cast<KeyAction>(action);
            event.key.hid_usage = hid_usage;
            break;
        case static_cast<uint8_t>(InputKind::Touch):
            if (action > static_cast<uint8_t>(TouchAction::Cancel)) {
                set_error(error, "InputEvent names unknown touch action " +
                                     std::to_string(action));
                return false;
            }
            if (contact >= kMaxTouchContacts) {
                set_error(error, "InputEvent touch contact is out of range");
                return false;
            }
            if (pressure > 1024) {
                set_error(error, "InputEvent touch pressure is out of range");
                return false;
            }
            if (buttons != 0 || hid_usage != 0 || flags != 0 || tilt_x != 0 || tilt_y != 0) {
                set_error(error, "InputEvent touch event carries non-touch fields");
                return false;
            }
            event.kind = InputKind::Touch;
            event.touch.action = static_cast<TouchAction>(action);
            event.touch.contact = contact;
            event.touch.x = static_cast<int32_t>(x);
            event.touch.y = static_cast<int32_t>(y);
            event.touch.pressure = pressure;
            break;
        case static_cast<uint8_t>(InputKind::Pen):
            if (action > static_cast<uint8_t>(TouchAction::Cancel)) {
                set_error(error, "InputEvent names unknown pen action " + std::to_string(action));
                return false;
            }
            if ((flags & ~0x07u) != 0) {
                set_error(error, "InputEvent pen event carries unknown flags");
                return false;
            }
            if (pressure > 1024) {
                set_error(error, "InputEvent pen pressure is out of range");
                return false;
            }
            if (tilt_x < -90 || tilt_x > 90 || tilt_y < -90 || tilt_y > 90) {
                set_error(error, "InputEvent pen tilt is out of range");
                return false;
            }
            if (buttons != 0 || hid_usage != 0 || contact != 0) {
                set_error(error, "InputEvent pen event carries non-pen fields");
                return false;
            }
            event.kind = InputKind::Pen;
            event.pen.action = static_cast<TouchAction>(action);
            event.pen.x = static_cast<int32_t>(x);
            event.pen.y = static_cast<int32_t>(y);
            event.pen.pressure = pressure;
            event.pen.flags = flags;
            event.pen.tilt_x = tilt_x;
            event.pen.tilt_y = tilt_y;
            break;
        default:
            set_error(error, "InputEvent names unknown kind " + std::to_string(kind));
            return false;
    }
    return true;
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

bool valid_chat_text(const std::string& text) {
    return !text.empty() && text.size() <= kMaxChatBytes && valid_utf8(text);
}

std::vector<uint8_t> make_chat_message(const std::string& text) {
    if (!valid_chat_text(text)) {
        return {};
    }
    return make_control(ControlType::ChatMessage,
                        std::vector<uint8_t>(text.begin(), text.end()));
}

bool parse_chat_message(const std::vector<uint8_t>& payload, std::string& text,
                        std::string* error) {
    if (payload.empty()) {
        set_error(error, "ChatMessage carries no text");
        return false;
    }
    if (payload.size() > kMaxChatBytes) {
        set_error(error, "ChatMessage text is too long");
        return false;
    }
    text.assign(payload.begin(), payload.end());
    if (!valid_utf8(text)) {
        set_error(error, "ChatMessage text is not valid UTF-8");
        return false;
    }
    return true;
}

bool valid_text_input(const std::string& text) {
    return !text.empty() && text.size() <= kMaxTextInputBytes && valid_utf8(text);
}

std::vector<uint8_t> make_text_input(const std::string& text) {
    if (!valid_text_input(text)) {
        return {};
    }
    return make_control(ControlType::TextInput,
                        std::vector<uint8_t>(text.begin(), text.end()));
}

bool parse_text_input(const std::vector<uint8_t>& payload, std::string& text,
                      std::string* error) {
    if (payload.empty()) {
        set_error(error, "TextInput carries no text");
        return false;
    }
    if (payload.size() > kMaxTextInputBytes) {
        set_error(error, "TextInput text is too long");
        return false;
    }
    text.assign(payload.begin(), payload.end());
    if (!valid_utf8(text)) {
        set_error(error, "TextInput text is not valid UTF-8");
        return false;
    }
    return true;
}

namespace {

bool valid_port_forward_host(const std::string& host) {
    return !host.empty() && host.size() <= kMaxPortForwardHostBytes &&
           host.find('\0') == std::string::npos && valid_utf8(host);
}

bool valid_port_forward_code(PortForwardCode code) {
    return code == PortForwardCode::Ok || code == PortForwardCode::Refused ||
           code == PortForwardCode::InvalidTarget || code == PortForwardCode::Busy ||
           code == PortForwardCode::Cancelled;
}

std::string truncate_port_forward_reason(const std::string& reason) {
    if (reason.size() <= kMaxPortForwardReasonBytes) {
        return valid_utf8(reason) ? reason : std::string();
    }
    size_t cut = kMaxPortForwardReasonBytes;
    while (cut > 0 && (static_cast<unsigned char>(reason[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    std::string text = reason.substr(0, cut);
    return valid_utf8(text) ? text : std::string();
}

std::vector<uint8_t> make_port_forward_reason_message(ControlType type, uint32_t forward_id,
                                                      PortForwardCode code,
                                                      const std::string& reason) {
    if (!valid_port_forward_code(code)) {
        return {};
    }
    const std::string text = truncate_port_forward_reason(reason);
    std::vector<uint8_t> payload(4 + 1 + 2 + text.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, forward_id);
    put_u8(cursor, static_cast<uint8_t>(code));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(type, payload);
}

bool parse_port_forward_reason_message(const std::vector<uint8_t>& payload, uint32_t& forward_id,
                                       PortForwardCode& code, std::string& reason,
                                       std::string* error, const char* name) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u32(forward_id) || !reader.u8(raw_code) || !reader.u16(reason_size)) {
        set_error(error, std::string("truncated ") + name);
        return false;
    }
    code = static_cast<PortForwardCode>(raw_code);
    if (!valid_port_forward_code(code)) {
        set_error(error, std::string(name) + " carries an unknown code");
        return false;
    }
    if (reason_size > kMaxPortForwardReasonBytes || !reader.string(reason, reason_size) ||
        !reader.at_end()) {
        set_error(error, std::string(name) + " carries a malformed reason");
        return false;
    }
    if (!reason.empty() && !valid_utf8(reason)) {
        set_error(error, std::string(name) + " reason is not valid UTF-8");
        return false;
    }
    return true;
}

}  // namespace

std::vector<uint8_t> make_port_forward_request(const PortForwardRequest& request) {
    if (request.forward_id == 0 || !valid_port_forward_host(request.host) ||
        request.port == 0) {
        return {};
    }
    std::vector<uint8_t> payload(4 + 2 + 2 + request.host.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, request.forward_id);
    put_u16(cursor, request.port);
    put_u16(cursor, static_cast<uint16_t>(request.host.size()));
    std::memcpy(cursor, request.host.data(), request.host.size());
    return make_control(ControlType::PortForwardRequest, payload);
}

bool parse_port_forward_request(const std::vector<uint8_t>& payload,
                                PortForwardRequest& request, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint16_t host_size = 0;
    if (!reader.u32(request.forward_id) || !reader.u16(request.port) ||
        !reader.u16(host_size)) {
        set_error(error, "truncated PortForwardRequest");
        return false;
    }
    if (request.forward_id == 0) {
        set_error(error, "PortForwardRequest carries forward id 0");
        return false;
    }
    if (request.port == 0) {
        set_error(error, "PortForwardRequest carries port 0");
        return false;
    }
    if (host_size == 0 || host_size > kMaxPortForwardHostBytes ||
        !reader.string(request.host, host_size) || !reader.at_end()) {
        set_error(error, "PortForwardRequest carries a malformed host");
        return false;
    }
    if (!valid_port_forward_host(request.host)) {
        set_error(error, "PortForwardRequest host is not valid UTF-8");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_port_forward_result(const PortForwardResult& result) {
    return make_port_forward_reason_message(ControlType::PortForwardResult, result.forward_id,
                                            result.code, result.reason);
}

bool parse_port_forward_result(const std::vector<uint8_t>& payload,
                               PortForwardResult& result, std::string* error) {
    return parse_port_forward_reason_message(payload, result.forward_id, result.code,
                                             result.reason, error, "PortForwardResult");
}

std::vector<uint8_t> make_port_forward_stop(const PortForwardStop& stop) {
    return make_port_forward_reason_message(ControlType::PortForwardStop, stop.forward_id,
                                            stop.code, stop.reason);
}

bool parse_port_forward_stop(const std::vector<uint8_t>& payload, PortForwardStop& stop,
                             std::string* error) {
    return parse_port_forward_reason_message(payload, stop.forward_id, stop.code,
                                             stop.reason, error, "PortForwardStop");
}

std::vector<uint8_t> make_port_forward_stream_header(uint32_t forward_id,
                                                      uint32_t connection_id) {
    if (forward_id == 0 || connection_id == 0) {
        return {};
    }
    std::vector<uint8_t> header(kPortForwardStreamHeaderSize, 0);
    std::memcpy(header.data(), kPortForwardStreamMagic, 4);
    header[4] = kPortForwardStreamVersion;
    uint8_t* cursor = header.data() + 8;
    put_u32(cursor, forward_id);
    put_u32(cursor, connection_id);
    return header;
}

bool decode_port_forward_stream_header(const uint8_t* data, size_t size,
                                       PortForwardStreamHeader& header, std::string* error) {
    if (!data || size < kPortForwardStreamHeaderSize) {
        set_error(error, "truncated CPFW header");
        return false;
    }
    if (std::memcmp(data, kPortForwardStreamMagic, 4) != 0) {
        set_error(error, "CPFW header has the wrong magic");
        return false;
    }
    if (data[4] != kPortForwardStreamVersion) {
        set_error(error, "CPFW header has an unknown version");
        return false;
    }
    if (data[5] != 0 || data[6] != 0 || data[7] != 0) {
        set_error(error, "CPFW header has non-zero reserved bytes");
        return false;
    }
    Reader reader(data + 8, kPortForwardStreamHeaderSize - 8);
    if (!reader.u32(header.forward_id) || !reader.u32(header.connection_id)) {
        set_error(error, "truncated CPFW header");
        return false;
    }
    if (header.forward_id == 0 || header.connection_id == 0) {
        set_error(error, "CPFW header carries a zero id");
        return false;
    }
    return true;
}

namespace {

bool valid_terminal_code(TerminalCode code) {
    return code == TerminalCode::Ok || code == TerminalCode::Refused ||
           code == TerminalCode::Busy || code == TerminalCode::Failed ||
           code == TerminalCode::Cancelled || code == TerminalCode::NotFound;
}

bool valid_terminal_flags(uint8_t flags) {
    const uint8_t known = kTerminalRequestPersist | kTerminalRequestAttach;
    if ((flags & ~known) != 0) {
        return false;
    }
    return (flags & known) != known;
}

std::string truncate_terminal_reason(const std::string& reason) {
    if (reason.size() <= kMaxTerminalReasonBytes) {
        return valid_utf8(reason) ? reason : std::string();
    }
    size_t cut = kMaxTerminalReasonBytes;
    while (cut > 0 && (static_cast<unsigned char>(reason[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    std::string text = reason.substr(0, cut);
    return valid_utf8(text) ? text : std::string();
}

std::vector<uint8_t> make_terminal_reason_message(ControlType type, uint32_t terminal_id,
                                                  TerminalCode code,
                                                  const std::string& reason) {
    if (!valid_terminal_code(code)) {
        return {};
    }
    const std::string text = truncate_terminal_reason(reason);
    std::vector<uint8_t> payload(4 + 1 + 2 + text.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, terminal_id);
    put_u8(cursor, static_cast<uint8_t>(code));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(type, payload);
}

bool parse_terminal_reason_message(const std::vector<uint8_t>& payload, uint32_t& terminal_id,
                                   TerminalCode& code, std::string& reason, std::string* error,
                                   const char* name) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u32(terminal_id) || !reader.u8(raw_code) || !reader.u16(reason_size)) {
        set_error(error, std::string("truncated ") + name);
        return false;
    }
    code = static_cast<TerminalCode>(raw_code);
    if (!valid_terminal_code(code)) {
        set_error(error, std::string(name) + " carries an unknown code");
        return false;
    }
    if (reason_size > kMaxTerminalReasonBytes || !reader.string(reason, reason_size) ||
        !reader.at_end()) {
        set_error(error, std::string(name) + " carries a malformed reason");
        return false;
    }
    if (!reason.empty() && !valid_utf8(reason)) {
        set_error(error, std::string(name) + " reason is not valid UTF-8");
        return false;
    }
    return true;
}

}  // namespace

bool valid_terminal_size(uint16_t columns, uint16_t rows) {
    return columns >= 1 && columns <= kMaxTerminalColumns && rows >= 1 &&
           rows <= kMaxTerminalRows;
}

std::vector<uint8_t> make_terminal_request(const TerminalRequest& request) {
    if (request.terminal_id == 0 || !valid_terminal_size(request.columns, request.rows) ||
        !valid_terminal_flags(request.flags)) {
        return {};
    }
    if ((request.flags & kTerminalRequestAttach) != 0) {
        if (request.service_id == 0) {
            return {};
        }
    } else if (request.service_id != 0) {
        return {};
    }
    std::vector<uint8_t> payload(4 + 2 + 2 + 1 + 4);
    uint8_t* cursor = payload.data();
    put_u32(cursor, request.terminal_id);
    put_u16(cursor, request.columns);
    put_u16(cursor, request.rows);
    put_u8(cursor, request.flags);
    put_u32(cursor, request.service_id);
    return make_control(ControlType::TerminalRequest, payload);
}

bool parse_terminal_request(const std::vector<uint8_t>& payload,
                            TerminalRequest& request, std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u32(request.terminal_id) || !reader.u16(request.columns) ||
        !reader.u16(request.rows) || !reader.u8(request.flags) ||
        !reader.u32(request.service_id) || !reader.at_end()) {
        set_error(error, "malformed TerminalRequest");
        return false;
    }
    if (request.terminal_id == 0) {
        set_error(error, "TerminalRequest carries terminal id 0");
        return false;
    }
    if (!valid_terminal_size(request.columns, request.rows)) {
        set_error(error, "TerminalRequest carries a size out of range");
        return false;
    }
    if (!valid_terminal_flags(request.flags)) {
        set_error(error, "TerminalRequest carries unknown or conflicting flags");
        return false;
    }
    if ((request.flags & kTerminalRequestAttach) != 0) {
        if (request.service_id == 0) {
            set_error(error, "TerminalRequest attaches without a service id");
            return false;
        }
    } else if (request.service_id != 0) {
        set_error(error, "TerminalRequest carries a service id without attaching");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_terminal_result(const TerminalResult& result) {
    if (result.terminal_id == 0 || !valid_terminal_code(result.code)) {
        return {};
    }
    if (result.code != TerminalCode::Ok && result.service_id != 0) {
        return {};
    }
    const std::string text = truncate_terminal_reason(result.reason);
    std::vector<uint8_t> payload(4 + 4 + 1 + 2 + text.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, result.terminal_id);
    put_u32(cursor, result.service_id);
    put_u8(cursor, static_cast<uint8_t>(result.code));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(ControlType::TerminalResult, payload);
}

bool parse_terminal_result(const std::vector<uint8_t>& payload,
                           TerminalResult& result, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u32(result.terminal_id) || !reader.u32(result.service_id) ||
        !reader.u8(raw_code) || !reader.u16(reason_size)) {
        set_error(error, "truncated TerminalResult");
        return false;
    }
    result.code = static_cast<TerminalCode>(raw_code);
    if (!valid_terminal_code(result.code)) {
        set_error(error, "TerminalResult carries an unknown code");
        return false;
    }
    if (result.terminal_id == 0 || (result.code != TerminalCode::Ok &&
                                    result.service_id != 0)) {
        set_error(error, "TerminalResult carries an inconsistent service id");
        return false;
    }
    if (reason_size > kMaxTerminalReasonBytes || !reader.string(result.reason, reason_size) ||
        !reader.at_end()) {
        set_error(error, "TerminalResult carries a malformed reason");
        return false;
    }
    if (!result.reason.empty() && !valid_utf8(result.reason)) {
        set_error(error, "TerminalResult reason is not valid UTF-8");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_terminal_stop(const TerminalStop& stop) {
    return make_terminal_reason_message(ControlType::TerminalStop, stop.terminal_id, stop.code,
                                        stop.reason);
}

bool parse_terminal_stop(const std::vector<uint8_t>& payload, TerminalStop& stop,
                         std::string* error) {
    return parse_terminal_reason_message(payload, stop.terminal_id, stop.code, stop.reason,
                                         error, "TerminalStop");
}

std::vector<uint8_t> make_terminal_resize(uint32_t terminal_id, uint16_t columns,
                                          uint16_t rows) {
    if (terminal_id == 0 || !valid_terminal_size(columns, rows)) {
        return {};
    }
    std::vector<uint8_t> payload(4 + 2 + 2);
    uint8_t* cursor = payload.data();
    put_u32(cursor, terminal_id);
    put_u16(cursor, columns);
    put_u16(cursor, rows);
    return make_control(ControlType::TerminalResize, payload);
}

bool parse_terminal_resize(const std::vector<uint8_t>& payload, TerminalSize& size,
                           std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u32(size.terminal_id) || !reader.u16(size.columns) ||
        !reader.u16(size.rows) || !reader.at_end()) {
        set_error(error, "malformed TerminalResize");
        return false;
    }
    if (size.terminal_id == 0) {
        set_error(error, "TerminalResize carries terminal id 0");
        return false;
    }
    if (!valid_terminal_size(size.columns, size.rows)) {
        set_error(error, "TerminalResize carries a size out of range");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_terminal_stream_header(uint32_t terminal_id) {
    if (terminal_id == 0) {
        return {};
    }
    std::vector<uint8_t> header(kTerminalStreamHeaderSize, 0);
    std::memcpy(header.data(), kTerminalStreamMagic, 4);
    header[4] = kTerminalStreamVersion;
    uint8_t* cursor = header.data() + 8;
    put_u32(cursor, terminal_id);
    return header;
}

bool decode_terminal_stream_header(const uint8_t* data, size_t size,
                                   TerminalStreamHeader& header, std::string* error) {
    if (!data || size < kTerminalStreamHeaderSize) {
        set_error(error, "truncated CTTY header");
        return false;
    }
    if (std::memcmp(data, kTerminalStreamMagic, 4) != 0) {
        set_error(error, "CTTY header has the wrong magic");
        return false;
    }
    if (data[4] != kTerminalStreamVersion) {
        set_error(error, "CTTY header has an unknown version");
        return false;
    }
    if (data[5] != 0 || data[6] != 0 || data[7] != 0) {
        set_error(error, "CTTY header has non-zero reserved bytes");
        return false;
    }
    Reader reader(data + 8, kTerminalStreamHeaderSize - 8);
    uint32_t reserved = 0;
    if (!reader.u32(header.terminal_id) || !reader.u32(reserved)) {
        set_error(error, "truncated CTTY header");
        return false;
    }
    if (reserved != 0) {
        set_error(error, "CTTY header has non-zero reserved bytes");
        return false;
    }
    if (header.terminal_id == 0) {
        set_error(error, "CTTY header carries terminal id 0");
        return false;
    }
    return true;
}

namespace {

bool valid_camera_code(CameraCode code) {
    return code == CameraCode::Ok || code == CameraCode::Refused ||
           code == CameraCode::NotFound || code == CameraCode::Busy ||
           code == CameraCode::Failed;
}

std::string truncate_camera_reason(const std::string& reason) {
    if (reason.size() <= kMaxCameraReasonBytes) {
        return valid_utf8(reason) ? reason : std::string();
    }
    size_t cut = kMaxCameraReasonBytes;
    while (cut > 0 && (static_cast<unsigned char>(reason[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    std::string text = reason.substr(0, cut);
    return valid_utf8(text) ? text : std::string();
}

}  // namespace

std::vector<uint8_t> make_camera_list_request() {
    return make_control(ControlType::ListCameras, {});
}

bool parse_camera_list_request(const std::vector<uint8_t>& payload, std::string* error) {
    if (!payload.empty()) {
        set_error(error, "ListCameras carries a payload");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_camera_list(const CameraList& list) {
    if (!valid_camera_code(list.code)) {
        return {};
    }
    const std::string reason = truncate_camera_reason(list.reason);
    std::vector<const CameraDescription*> cameras;
    if (list.code == CameraCode::Ok) {
        if (list.cameras.size() > kMaxCameras) {
            return {};
        }
        for (const CameraDescription& camera : list.cameras) {
            if (camera.id == 0 || camera.name.size() > kMaxCameraNameBytes ||
                !valid_utf8(camera.name)) {
                return {};
            }
            for (const CameraDescription* seen : cameras) {
                if (seen->id == camera.id) {
                    return {};
                }
            }
            cameras.push_back(&camera);
        }
    }
    size_t size = 1 + 2 + reason.size() + 2;
    for (const CameraDescription* camera : cameras) {
        size += 2 + 1 + 2 + camera->name.size();
    }
    std::vector<uint8_t> payload(size);
    uint8_t* cursor = payload.data();
    put_u8(cursor, static_cast<uint8_t>(list.code));
    put_u16(cursor, static_cast<uint16_t>(reason.size()));
    if (!reason.empty()) {
        std::memcpy(cursor, reason.data(), reason.size());
        cursor += reason.size();
    }
    put_u16(cursor, static_cast<uint16_t>(cameras.size()));
    for (const CameraDescription* camera : cameras) {
        put_u16(cursor, camera->id);
        put_u8(cursor, camera->is_default ? 0x1 : 0);
        put_u16(cursor, static_cast<uint16_t>(camera->name.size()));
        if (!camera->name.empty()) {
            std::memcpy(cursor, camera->name.data(), camera->name.size());
            cursor += camera->name.size();
        }
    }
    return make_control(ControlType::CameraList, payload);
}

bool parse_camera_list(const std::vector<uint8_t>& payload, CameraList& list,
                       std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u8(raw_code) || !reader.u16(reason_size)) {
        set_error(error, "truncated CameraList");
        return false;
    }
    list.code = static_cast<CameraCode>(raw_code);
    if (!valid_camera_code(list.code)) {
        set_error(error, "CameraList carries an unknown code");
        return false;
    }
    if (reason_size > kMaxCameraReasonBytes || !reader.string(list.reason, reason_size)) {
        set_error(error, "CameraList carries a malformed reason");
        return false;
    }
    if (!list.reason.empty() && !valid_utf8(list.reason)) {
        set_error(error, "CameraList reason is not valid UTF-8");
        return false;
    }
    uint16_t count = 0;
    if (!reader.u16(count)) {
        set_error(error, "CameraList carries no camera count");
        return false;
    }
    if (count > kMaxCameras) {
        set_error(error, "CameraList lists " + std::to_string(count) + " cameras");
        return false;
    }
    if (list.code != CameraCode::Ok && count != 0) {
        set_error(error, "a non-Ok CameraList carries cameras");
        return false;
    }
    list.cameras.clear();
    list.cameras.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        CameraDescription camera;
        uint8_t flags = 0;
        uint16_t name_size = 0;
        if (!reader.u16(camera.id) || !reader.u8(flags) || !reader.u16(name_size)) {
            set_error(error, "truncated CameraList entry " + std::to_string(i));
            return false;
        }
        if (camera.id == 0) {
            set_error(error, "CameraList entry carries id 0");
            return false;
        }
        if ((flags & ~0x1u) != 0) {
            set_error(error, "CameraList entry " + std::to_string(i) + " has unknown flags");
            return false;
        }
        camera.is_default = (flags & 0x1u) != 0;
        if (name_size > kMaxCameraNameBytes || !reader.string(camera.name, name_size)) {
            set_error(error, "CameraList entry " + std::to_string(i) + " has a malformed name");
            return false;
        }
        if (!camera.name.empty() && !valid_utf8(camera.name)) {
            set_error(error, "CameraList entry " + std::to_string(i) +
                                 " name is not valid UTF-8");
            return false;
        }
        for (const CameraDescription& seen : list.cameras) {
            if (seen.id == camera.id) {
                set_error(error, "CameraList repeats camera id " + std::to_string(camera.id));
                return false;
            }
        }
        list.cameras.push_back(std::move(camera));
    }
    if (!reader.at_end()) {
        set_error(error, "CameraList carries trailing bytes");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_camera_start(uint16_t camera_id) {
    if (camera_id == 0) {
        return {};
    }
    std::vector<uint8_t> payload(2);
    uint8_t* cursor = payload.data();
    put_u16(cursor, camera_id);
    return make_control(ControlType::StartCamera, payload);
}

bool parse_camera_start(const std::vector<uint8_t>& payload, uint16_t& camera_id,
                        std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u16(camera_id) || !reader.at_end()) {
        set_error(error, "malformed StartCamera");
        return false;
    }
    if (camera_id == 0) {
        set_error(error, "StartCamera carries camera id 0");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_camera_result(const CameraResult& result) {
    if (!valid_camera_code(result.code)) {
        return {};
    }
    const std::string reason = truncate_camera_reason(result.reason);
    std::vector<uint8_t> payload(2 + 1 + 2 + reason.size());
    uint8_t* cursor = payload.data();
    put_u16(cursor, result.camera_id);
    put_u8(cursor, static_cast<uint8_t>(result.code));
    put_u16(cursor, static_cast<uint16_t>(reason.size()));
    if (!reason.empty()) {
        std::memcpy(cursor, reason.data(), reason.size());
    }
    return make_control(ControlType::CameraResult, payload);
}

bool parse_camera_result(const std::vector<uint8_t>& payload, CameraResult& result,
                         std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u16(result.camera_id) || !reader.u8(raw_code) ||
        !reader.u16(reason_size)) {
        set_error(error, "truncated CameraResult");
        return false;
    }
    result.code = static_cast<CameraCode>(raw_code);
    if (!valid_camera_code(result.code)) {
        set_error(error, "CameraResult carries an unknown code");
        return false;
    }
    if (reason_size > kMaxCameraReasonBytes || !reader.string(result.reason, reason_size) ||
        !reader.at_end()) {
        set_error(error, "CameraResult carries a malformed reason");
        return false;
    }
    if (!result.reason.empty() && !valid_utf8(result.reason)) {
        set_error(error, "CameraResult reason is not valid UTF-8");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_camera_stop(uint16_t camera_id) {
    if (camera_id == 0) {
        return {};
    }
    std::vector<uint8_t> payload(2);
    uint8_t* cursor = payload.data();
    put_u16(cursor, camera_id);
    return make_control(ControlType::StopCamera, payload);
}

bool parse_camera_stop(const std::vector<uint8_t>& payload, uint16_t& camera_id,
                       std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u16(camera_id) || !reader.at_end()) {
        set_error(error, "malformed StopCamera");
        return false;
    }
    if (camera_id == 0) {
        set_error(error, "StopCamera carries camera id 0");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_camera_stream_header(uint16_t camera_id) {
    if (camera_id == 0) {
        return {};
    }
    std::vector<uint8_t> header(kCameraStreamHeaderSize, 0);
    std::memcpy(header.data(), kCameraStreamMagic, 4);
    header[4] = kCameraStreamVersion;
    uint8_t* cursor = header.data() + 8;
    put_u16(cursor, camera_id);
    return header;
}

bool decode_camera_stream_header(const uint8_t* data, size_t size,
                                 CameraStreamHeader& header, std::string* error) {
    if (!data || size < kCameraStreamHeaderSize) {
        set_error(error, "truncated CCAM header");
        return false;
    }
    if (std::memcmp(data, kCameraStreamMagic, 4) != 0) {
        set_error(error, "CCAM header has the wrong magic");
        return false;
    }
    if (data[4] != kCameraStreamVersion) {
        set_error(error, "CCAM header has an unknown version");
        return false;
    }
    if (data[5] != 0 || data[6] != 0 || data[7] != 0) {
        set_error(error, "CCAM header has non-zero reserved bytes");
        return false;
    }
    Reader reader(data + 8, kCameraStreamHeaderSize - 8);
    uint16_t reserved1 = 0;
    uint32_t reserved2 = 0;
    if (!reader.u16(header.camera_id) || !reader.u16(reserved1) ||
        !reader.u32(reserved2)) {
        set_error(error, "truncated CCAM header");
        return false;
    }
    if (reserved1 != 0 || reserved2 != 0) {
        set_error(error, "CCAM header has non-zero reserved bytes");
        return false;
    }
    if (header.camera_id == 0) {
        set_error(error, "CCAM header carries camera id 0");
        return false;
    }
    return true;
}

namespace {

bool valid_voice_code(VoiceCallCode code) {
    return code == VoiceCallCode::Ok || code == VoiceCallCode::Refused ||
           code == VoiceCallCode::Busy || code == VoiceCallCode::Failed;
}

std::string truncate_voice_reason(const std::string& reason) {
    if (reason.size() <= kMaxVoiceCallReasonBytes) {
        return valid_utf8(reason) ? reason : std::string();
    }
    size_t cut = kMaxVoiceCallReasonBytes;
    while (cut > 0 && (static_cast<unsigned char>(reason[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    std::string text = reason.substr(0, cut);
    return valid_utf8(text) ? text : std::string();
}

// The host microphone is always 48 kHz Opus in 20 ms frames, one or two
// channels. Anything else would describe a stream the viewer cannot decode
// with the microphone decoder.
bool valid_voice_info(const AudioInfo& info) {
    return info.codec == AudioCodec::Opus && info.sample_rate == 48000 &&
           (info.channels == 1 || info.channels == 2) &&
           info.frame_samples == kOpusFrameSamples && !info.extradata.empty() &&
           info.extradata.size() <= kMaxAudioExtradata;
}

}  // namespace

std::vector<uint8_t> make_voice_call_request(const VoiceCallRequest& request) {
    if (request.request_id == 0) {
        return {};
    }
    std::vector<uint8_t> payload(1 + 8);
    uint8_t* cursor = payload.data();
    put_u8(cursor, request.connect ? 1 : 0);
    put_u64(cursor, request.request_id);
    return make_control(ControlType::VoiceCallRequest, payload);
}

bool parse_voice_call_request(const std::vector<uint8_t>& payload,
                              VoiceCallRequest& request, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t flags = 0;
    if (!reader.u8(flags) || !reader.u64(request.request_id) || !reader.at_end()) {
        set_error(error, "malformed VoiceCallRequest");
        return false;
    }
    if ((flags & ~0x1u) != 0) {
        set_error(error, "VoiceCallRequest carries unknown flags");
        return false;
    }
    if (request.request_id == 0) {
        set_error(error, "VoiceCallRequest carries request id 0");
        return false;
    }
    request.connect = (flags & 0x1u) != 0;
    return true;
}

std::vector<uint8_t> make_voice_call_response(const VoiceCallResponse& response) {
    if (response.request_id == 0 || !valid_voice_code(response.code)) {
        return {};
    }
    const bool ok = response.code == VoiceCallCode::Ok;
    if (ok && !valid_voice_info(response.audio)) {
        return {};
    }
    const std::string reason = truncate_voice_reason(response.reason);
    size_t size = 8 + 1 + 2 + reason.size();
    if (ok) {
        size += 1 + 4 + 2 + 2 + 2 + response.audio.extradata.size();
    }
    std::vector<uint8_t> payload(size);
    uint8_t* cursor = payload.data();
    put_u64(cursor, response.request_id);
    put_u8(cursor, static_cast<uint8_t>(response.code));
    put_u16(cursor, static_cast<uint16_t>(reason.size()));
    if (!reason.empty()) {
        std::memcpy(cursor, reason.data(), reason.size());
        cursor += reason.size();
    }
    if (ok) {
        put_u8(cursor, audio_codec_wire(response.audio.codec));
        put_u32(cursor, response.audio.sample_rate);
        put_u16(cursor, response.audio.channels);
        put_u16(cursor, response.audio.frame_samples);
        put_u16(cursor, static_cast<uint16_t>(response.audio.extradata.size()));
        std::memcpy(cursor, response.audio.extradata.data(), response.audio.extradata.size());
    }
    return make_control(ControlType::VoiceCallResponse, payload);
}

bool parse_voice_call_response(const std::vector<uint8_t>& payload,
                               VoiceCallResponse& response, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u64(response.request_id) || !reader.u8(raw_code) ||
        !reader.u16(reason_size)) {
        set_error(error, "truncated VoiceCallResponse");
        return false;
    }
    response.code = static_cast<VoiceCallCode>(raw_code);
    if (!valid_voice_code(response.code)) {
        set_error(error, "VoiceCallResponse carries an unknown code");
        return false;
    }
    if (response.request_id == 0) {
        set_error(error, "VoiceCallResponse carries request id 0");
        return false;
    }
    if (reason_size > kMaxVoiceCallReasonBytes ||
        !reader.string(response.reason, reason_size)) {
        set_error(error, "VoiceCallResponse carries a malformed reason");
        return false;
    }
    if (!response.reason.empty() && !valid_utf8(response.reason)) {
        set_error(error, "VoiceCallResponse reason is not valid UTF-8");
        return false;
    }
    response.audio = AudioInfo{};
    if (response.code == VoiceCallCode::Ok) {
        uint8_t codec = 0;
        uint16_t extradata_size = 0;
        if (!reader.u8(codec) || !reader.u32(response.audio.sample_rate) ||
            !reader.u16(response.audio.channels) ||
            !reader.u16(response.audio.frame_samples) ||
            !reader.u16(extradata_size)) {
            set_error(error, "truncated VoiceCallResponse format");
            return false;
        }
        if (!audio_codec_from_wire(codec, response.audio.codec) ||
            extradata_size == 0 || extradata_size > kMaxAudioExtradata ||
            !reader.bytes(response.audio.extradata, extradata_size)) {
            set_error(error, "VoiceCallResponse carries a malformed format");
            return false;
        }
        response.audio.enabled = true;
        response.audio.bitrate = 0;
        if (!valid_voice_info(response.audio)) {
            set_error(error, "VoiceCallResponse format is not a voice stream");
            return false;
        }
    }
    if (!reader.at_end()) {
        set_error(error, "VoiceCallResponse carries trailing bytes");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_voice_audio(const AudioHeader& header,
                                      const uint8_t* payload, size_t size) {
    if (!payload || size == 0 || size > kMaxAudioPayload ||
        header.codec != AudioCodec::Opus || !valid_audio_rate(header.sample_rate) ||
        (header.channels != 1 && header.channels != 2) ||
        header.frame_samples == 0 || header.frame_samples > 8192) {
        return {};
    }
    AudioHeader wire = header;
    wire.payload_size = static_cast<uint32_t>(size);
    wire.payload_crc32 = crc32(payload, size);
    std::vector<uint8_t> message(kAudioHeaderSize + size);
    encode_audio_header(wire, message.data());
    std::memcpy(message.data() + kAudioHeaderSize, payload, size);
    return make_control(ControlType::VoiceAudio, message);
}

bool parse_voice_audio(const std::vector<uint8_t>& payload, AudioHeader& header,
                       const uint8_t** audio, size_t& audio_size, std::string* error) {
    if (payload.size() <= kAudioHeaderSize) {
        set_error(error, "VoiceAudio carries no encoded packet");
        return false;
    }
    if (!decode_audio_header(payload.data(), kAudioHeaderSize, header, error)) {
        return false;
    }
    if (header.codec != AudioCodec::Opus) {
        set_error(error, "VoiceAudio is not Opus");
        return false;
    }
    audio_size = payload.size() - kAudioHeaderSize;
    if (header.payload_size != audio_size) {
        set_error(error, "VoiceAudio length does not match its header");
        return false;
    }
    const uint8_t* data = payload.data() + kAudioHeaderSize;
    if (header.payload_crc32 != crc32(data, audio_size)) {
        set_error(error, "VoiceAudio payload CRC does not match");
        return false;
    }
    *audio = data;
    return true;
}

namespace {

bool valid_host_action(HostAction action) {
    return action == HostAction::Restart || action == HostAction::LockScreen ||
           action == HostAction::BlockInput;
}

bool valid_host_action_code(HostActionCode code) {
    return code == HostActionCode::Ok || code == HostActionCode::Denied ||
           code == HostActionCode::Unsupported || code == HostActionCode::Busy ||
           code == HostActionCode::Failed;
}

// Only BlockInput carries a value; the other actions must send zero so a
// malformed request cannot be mistaken for a state toggle.
bool valid_host_action_value(HostAction action, uint8_t value) {
    return action == HostAction::BlockInput ? value <= 1 : value == 0;
}

}  // namespace

std::vector<uint8_t> make_host_action_request(const HostControlRequest& request) {
    if (!valid_host_action(request.action)) {
        return {};
    }
    if (request.action != HostAction::BlockInput && request.block_input) {
        return {};
    }
    std::vector<uint8_t> payload(2);
    payload[0] = static_cast<uint8_t>(request.action);
    payload[1] = request.block_input ? 1 : 0;
    return make_control(ControlType::HostActionRequest, payload);
}

bool parse_host_action_request(const std::vector<uint8_t>& payload,
                               HostControlRequest& request, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_action = 0;
    uint8_t raw_value = 0;
    if (!reader.u8(raw_action) || !reader.u8(raw_value) || !reader.at_end()) {
        set_error(error, "malformed HostActionRequest");
        return false;
    }
    request.action = static_cast<HostAction>(raw_action);
    if (!valid_host_action(request.action)) {
        set_error(error, "HostActionRequest carries an unknown action");
        return false;
    }
    if (!valid_host_action_value(request.action, raw_value)) {
        set_error(error, "HostActionRequest carries an invalid value");
        return false;
    }
    request.block_input = raw_value != 0;
    return true;
}

std::vector<uint8_t> make_host_action_result(const HostControlResult& result) {
    if (!valid_host_action(result.action) || !valid_host_action_code(result.code)) {
        return {};
    }
    if (result.action != HostAction::BlockInput && result.block_input) {
        return {};
    }
    const std::string text = truncate_utf8_reason(result.reason, kMaxHostActionReason);
    std::vector<uint8_t> payload(5 + text.size());
    uint8_t* cursor = payload.data();
    put_u8(cursor, static_cast<uint8_t>(result.action));
    put_u8(cursor, result.block_input ? 1 : 0);
    put_u8(cursor, static_cast<uint8_t>(result.code));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(ControlType::HostActionResult, payload);
}

bool parse_host_action_result(const std::vector<uint8_t>& payload,
                              HostControlResult& result, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_action = 0;
    uint8_t raw_state = 0;
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u8(raw_action) || !reader.u8(raw_state) || !reader.u8(raw_code) ||
        !reader.u16(reason_size) || !reader.string(result.reason, reason_size) ||
        !reader.at_end()) {
        set_error(error, "malformed HostActionResult");
        return false;
    }
    result.action = static_cast<HostAction>(raw_action);
    if (!valid_host_action(result.action)) {
        set_error(error, "HostActionResult carries an unknown action");
        return false;
    }
    if (!valid_host_action_value(result.action, raw_state)) {
        set_error(error, "HostActionResult carries an invalid state");
        return false;
    }
    result.block_input = raw_state != 0;
    result.code = static_cast<HostActionCode>(raw_code);
    if (!valid_host_action_code(result.code)) {
        set_error(error, "HostActionResult carries an unknown code");
        return false;
    }
    if (result.reason.size() > kMaxHostActionReason) {
        set_error(error, "HostActionResult reason is too long");
        return false;
    }
    if (!result.reason.empty() && !valid_utf8(result.reason)) {
        set_error(error, "HostActionResult carries a malformed reason");
        return false;
    }
    return true;
}

namespace {

bool valid_os_login_code(OsLoginCode code) {
    return code == OsLoginCode::Ok || code == OsLoginCode::Denied ||
           code == OsLoginCode::Unsupported || code == OsLoginCode::Busy ||
           code == OsLoginCode::Failed;
}

// The injector turns '\n' and '\t' into Return and Tab, so any control
// character in a password would split it before it reaches the logon box.
bool valid_os_login_password(const std::string& password) {
    if (password.empty() || password.size() > kMaxOsLoginPasswordBytes ||
        !valid_utf8(password)) {
        return false;
    }
    for (const unsigned char ch : password) {
        if (ch < 0x20 || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<uint8_t> make_os_login_request(const std::string& password) {
    if (!valid_os_login_password(password)) {
        return {};
    }
    std::vector<uint8_t> payload(password.begin(), password.end());
    return make_control(ControlType::OsLoginRequest, payload);
}

bool parse_os_login_request(const std::vector<uint8_t>& payload,
                            std::string& password, std::string* error) {
    std::string text(payload.begin(), payload.end());
    if (!valid_os_login_password(text)) {
        set_error(error, "malformed OsLoginRequest");
        return false;
    }
    password = std::move(text);
    return true;
}

std::vector<uint8_t> make_os_login_result(OsLoginCode code, const std::string& reason) {
    if (!valid_os_login_code(code)) {
        return {};
    }
    const std::string text = truncate_utf8_reason(reason, kMaxOsLoginReason);
    std::vector<uint8_t> payload(3 + text.size());
    uint8_t* cursor = payload.data();
    put_u8(cursor, static_cast<uint8_t>(code));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(ControlType::OsLoginResult, payload);
}

bool parse_os_login_result(const std::vector<uint8_t>& payload, OsLoginCode& code,
                           std::string& reason, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    if (!reader.u8(raw_code) || !reader.u16(reason_size) ||
        !reader.string(reason, reason_size) || !reader.at_end()) {
        set_error(error, "malformed OsLoginResult");
        return false;
    }
    code = static_cast<OsLoginCode>(raw_code);
    if (!valid_os_login_code(code)) {
        set_error(error, "OsLoginResult carries an unknown code");
        return false;
    }
    if (reason.size() > kMaxOsLoginReason) {
        set_error(error, "OsLoginResult reason is too long");
        return false;
    }
    if (!reason.empty() && !valid_utf8(reason)) {
        set_error(error, "OsLoginResult carries a malformed reason");
        return false;
    }
    return true;
}

namespace {

bool valid_session_id(uint32_t id) {
    return id != 0 && id != 0xFFFFFFFFu;
}

bool valid_session_list_code(SessionListCode code) {
    return code == SessionListCode::Ok || code == SessionListCode::Denied ||
           code == SessionListCode::Unsupported || code == SessionListCode::Failed;
}

bool valid_session_select_code(SessionSelectCode code) {
    return code == SessionSelectCode::Ok || code == SessionSelectCode::NotFound ||
           code == SessionSelectCode::Denied || code == SessionSelectCode::Unsupported ||
           code == SessionSelectCode::Busy || code == SessionSelectCode::Failed;
}

bool valid_session_flags(uint8_t flags) {
    return (flags & ~3u) == 0;
}

}  // namespace

std::vector<uint8_t> make_session_list_request() {
    return make_control(ControlType::SessionListRequest, {});
}

bool parse_session_list_request(const std::vector<uint8_t>& payload, std::string* error) {
    if (!payload.empty()) {
        set_error(error, "SessionListRequest carries a payload");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_session_list_result(const SessionListResult& result) {
    if (!valid_session_list_code(result.code)) {
        return {};
    }
    if (result.code != SessionListCode::Ok && !result.sessions.empty()) {
        return {};
    }
    if (result.sessions.size() > kMaxRemoteSessions) {
        return {};
    }
    const std::string reason = truncate_utf8_reason(result.reason, kMaxSessionReason);
    std::vector<uint8_t> payload(3 + reason.size() + 2);
    uint8_t* cursor = payload.data();
    put_u8(cursor, static_cast<uint8_t>(result.code));
    put_u16(cursor, static_cast<uint16_t>(reason.size()));
    if (!reason.empty()) {
        std::memcpy(cursor, reason.data(), reason.size());
        cursor += reason.size();
    }
    put_u16(cursor, static_cast<uint16_t>(result.sessions.size()));
    std::vector<uint32_t> ids;
    ids.reserve(result.sessions.size());
    for (const RemoteSession& session : result.sessions) {
        if (!valid_session_id(session.id) || session.user.size() > kMaxSessionUserBytes ||
            !valid_utf8(session.user)) {
            return {};
        }
        for (const uint32_t id : ids) {
            if (id == session.id) {
                return {};
            }
        }
        ids.push_back(session.id);
        const size_t entry = 4 + 1 + 2 + session.user.size();
        payload.resize(payload.size() + entry);
        cursor = payload.data() + payload.size() - entry;
        put_u32(cursor, session.id);
        put_u8(cursor, static_cast<uint8_t>((session.active ? 1u : 0u) |
                                            (session.current ? 2u : 0u)));
        put_u16(cursor, static_cast<uint16_t>(session.user.size()));
        std::memcpy(cursor, session.user.data(), session.user.size());
    }
    return make_control(ControlType::SessionListResult, payload);
}

bool parse_session_list_result(const std::vector<uint8_t>& payload,
                               SessionListResult& result, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    uint16_t count = 0;
    result = SessionListResult{};
    if (!reader.u8(raw_code) || !reader.u16(reason_size) ||
        !reader.string(result.reason, reason_size) || !reader.u16(count)) {
        set_error(error, "malformed SessionListResult");
        return false;
    }
    result.code = static_cast<SessionListCode>(raw_code);
    if (!valid_session_list_code(result.code)) {
        set_error(error, "SessionListResult carries an unknown code");
        return false;
    }
    if (result.reason.size() > kMaxSessionReason) {
        set_error(error, "SessionListResult reason is too long");
        return false;
    }
    if (!result.reason.empty() && !valid_utf8(result.reason)) {
        set_error(error, "SessionListResult carries a malformed reason");
        return false;
    }
    if (count > kMaxRemoteSessions) {
        set_error(error, "SessionListResult lists too many sessions");
        return false;
    }
    if (result.code != SessionListCode::Ok && count != 0) {
        set_error(error, "SessionListResult lists sessions in a non-Ok answer");
        return false;
    }
    std::vector<uint32_t> ids;
    for (uint16_t i = 0; i < count; ++i) {
        RemoteSession session;
        uint8_t flags = 0;
        uint16_t user_size = 0;
        if (!reader.u32(session.id) || !reader.u8(flags) || !reader.u16(user_size) ||
            !reader.string(session.user, user_size)) {
            set_error(error, "truncated SessionListResult entry");
            return false;
        }
        if (!valid_session_id(session.id)) {
            set_error(error, "SessionListResult carries an invalid session id");
            return false;
        }
        if (!valid_session_flags(flags)) {
            set_error(error, "SessionListResult carries unknown session flags");
            return false;
        }
        if (session.user.size() > kMaxSessionUserBytes) {
            set_error(error, "SessionListResult user name is too long");
            return false;
        }
        if (!session.user.empty() && !valid_utf8(session.user)) {
            set_error(error, "SessionListResult carries a malformed user name");
            return false;
        }
        for (const uint32_t id : ids) {
            if (id == session.id) {
                set_error(error, "SessionListResult repeats a session id");
                return false;
            }
        }
        ids.push_back(session.id);
        session.active = (flags & 1u) != 0;
        session.current = (flags & 2u) != 0;
        result.sessions.push_back(std::move(session));
    }
    if (!reader.at_end()) {
        set_error(error, "SessionListResult carries trailing bytes");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_session_select_request(uint32_t session_id) {
    if (!valid_session_id(session_id)) {
        return {};
    }
    std::vector<uint8_t> payload(4);
    uint8_t* cursor = payload.data();
    put_u32(cursor, session_id);
    return make_control(ControlType::SessionSelectRequest, payload);
}

bool parse_session_select_request(const std::vector<uint8_t>& payload,
                                  uint32_t& session_id, std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u32(session_id) || !reader.at_end()) {
        set_error(error, "malformed SessionSelectRequest");
        return false;
    }
    if (!valid_session_id(session_id)) {
        set_error(error, "SessionSelectRequest carries an invalid session id");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_session_select_result(const SessionSelectResult& result) {
    if (!valid_session_id(result.session_id) || !valid_session_select_code(result.code)) {
        return {};
    }
    const std::string reason = truncate_utf8_reason(result.reason, kMaxSessionReason);
    std::vector<uint8_t> payload(7 + reason.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, result.session_id);
    put_u8(cursor, static_cast<uint8_t>(result.code));
    put_u16(cursor, static_cast<uint16_t>(reason.size()));
    if (!reason.empty()) {
        std::memcpy(cursor, reason.data(), reason.size());
    }
    return make_control(ControlType::SessionSelectResult, payload);
}

bool parse_session_select_result(const std::vector<uint8_t>& payload,
                                 SessionSelectResult& result, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    result = SessionSelectResult{};
    if (!reader.u32(result.session_id) || !reader.u8(raw_code) ||
        !reader.u16(reason_size) || !reader.string(result.reason, reason_size) ||
        !reader.at_end()) {
        set_error(error, "malformed SessionSelectResult");
        return false;
    }
    if (!valid_session_id(result.session_id)) {
        set_error(error, "SessionSelectResult carries an invalid session id");
        return false;
    }
    result.code = static_cast<SessionSelectCode>(raw_code);
    if (!valid_session_select_code(result.code)) {
        set_error(error, "SessionSelectResult carries an unknown code");
        return false;
    }
    if (result.reason.size() > kMaxSessionReason) {
        set_error(error, "SessionSelectResult reason is too long");
        return false;
    }
    if (!result.reason.empty() && !valid_utf8(result.reason)) {
        set_error(error, "SessionSelectResult carries a malformed reason");
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

std::vector<uint8_t> make_set_display_follow(uint8_t flags) {
    if ((flags & ~kDisplayFollowAll) != 0) {
        return {};
    }
    std::vector<uint8_t> payload(1);
    uint8_t* cursor = payload.data();
    put_u8(cursor, flags);
    return make_control(ControlType::SetDisplayFollow, payload);
}

bool parse_set_display_follow(const std::vector<uint8_t>& payload, uint8_t& flags,
                              std::string* error) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u8(flags) || !reader.at_end()) {
        set_error(error, "malformed SetDisplayFollow");
        return false;
    }
    if ((flags & ~kDisplayFollowAll) != 0) {
        set_error(error, "SetDisplayFollow carries unknown flags");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_display_follow_state(const DisplayFollowState& state) {
    const uint8_t flags = static_cast<uint8_t>(state.flags & kDisplayFollowAll);
    // A refusal never claims a mode is on: the flags describe what runs.
    if (state.code != DisplayFollowCode::Ok && flags != 0) {
        return {};
    }
    const std::string text = truncate_utf8_reason(state.reason, kMaxDisplayFollowReason);
    std::vector<uint8_t> payload(4 + text.size());
    uint8_t* cursor = payload.data();
    put_u8(cursor, flags);
    put_u8(cursor, static_cast<uint8_t>(state.code));
    put_u16(cursor, static_cast<uint16_t>(text.size()));
    if (!text.empty()) {
        std::memcpy(cursor, text.data(), text.size());
    }
    return make_control(ControlType::DisplayFollowState, payload);
}

bool parse_display_follow_state(const std::vector<uint8_t>& payload,
                                DisplayFollowState& state, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t flags = 0;
    uint8_t raw_code = 0;
    uint16_t reason_size = 0;
    std::string reason;
    if (!reader.u8(flags) || !reader.u8(raw_code) || !reader.u16(reason_size) ||
        !reader.string(reason, reason_size) || !reader.at_end()) {
        set_error(error, "malformed DisplayFollowState");
        return false;
    }
    if ((flags & ~kDisplayFollowAll) != 0) {
        set_error(error, "DisplayFollowState carries unknown flags");
        return false;
    }
    if (raw_code > static_cast<uint8_t>(DisplayFollowCode::Unsupported)) {
        set_error(error, "unknown display follow code " + std::to_string(raw_code));
        return false;
    }
    if (reason.size() > kMaxDisplayFollowReason) {
        set_error(error, "DisplayFollowState reason is too long");
        return false;
    }
    if (!reason.empty() && !valid_utf8(reason)) {
        set_error(error, "DisplayFollowState carries a malformed reason");
        return false;
    }
    state.flags = flags;
    state.code = static_cast<DisplayFollowCode>(raw_code);
    state.reason = std::move(reason);
    return true;
}

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
    if (raw_codec > static_cast<uint8_t>(StreamCodecPreference::Av1)) {
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

// A relative directory is a suggestion for the receiver, but it still has to
// be unambiguous: no absolute roots, no drive letters, no "." or ".." and no
// empty component. Returns false with the reason on anything suspicious.
bool parse_file_relative_path(const std::string& path, std::string* error) {
    if (path.empty()) {
        return true;
    }
    if (path.size() > kMaxFileRelativePathBytes) {
        set_error(error, "FileOffer relative path is too long");
        return false;
    }
    if (!valid_utf8(path)) {
        set_error(error, "FileOffer relative path is not UTF-8");
        return false;
    }
    if (path.front() == '/' || path.back() == '/' || path.find('\\') != std::string::npos ||
        path.find(':') != std::string::npos) {
        set_error(error, "FileOffer relative path is not a plain relative directory");
        return false;
    }
    size_t components = 0;
    size_t start = 0;
    for (;;) {
        const size_t slash = path.find('/', start);
        const size_t length = slash == std::string::npos ? path.size() - start : slash - start;
        if (length == 0 || (length == 1 && path[start] == '.') ||
            (length == 2 && path[start] == '.' && path[start + 1] == '.')) {
            set_error(error, "FileOffer relative path has an unusable component");
            return false;
        }
        if (++components > kMaxFileRelativeComponents) {
            set_error(error, "FileOffer relative path is nested too deeply");
            return false;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

}  // namespace

std::string make_offer_relative_path(const std::string& top, const std::string& relative) {
    size_t start = 0;
    while (start < relative.size() &&
           (relative[start] == '\\' || relative[start] == '/')) {
        ++start;
    }
    if (start == relative.size() || (start + 1 == relative.size() && relative[start] == '.')) {
        return top;
    }
    std::string joined = top;
    if (!joined.empty()) {
        joined.push_back('/');
    }
    for (size_t i = start; i < relative.size(); ++i) {
        joined.push_back(relative[i] == '\\' ? '/' : relative[i]);
    }
    return joined;
}

std::vector<uint8_t> make_file_offer(const FileOffer& offer) {
    if (offer.transfer_id == 0 || offer.name.empty() || !valid_utf8(offer.name) ||
        offer.size > kMaxFileTransferBytes) {
        // The parser refuses these, so sending them would only stall the
        // transfer until the peer's timeout.
        return {};
    }
    const std::string name = truncate_utf8_reason(offer.name, kMaxFileNameBytes);
    std::string relative = offer.relative_path;
    if (relative.size() > kMaxFileRelativePathBytes) {
        // The parser refuses over-long paths; clearing keeps the length field
        // honest instead of truncating a UTF-8 byte sequence in half.
        relative.clear();
    }
    std::vector<uint8_t> payload(4 + 8 + 1 + 32 + 2 + name.size() + 2 + relative.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, offer.transfer_id);
    put_u64(cursor, offer.size);
    put_u8(cursor, static_cast<uint8_t>((offer.resume ? kFileOfferFlagResume : 0) |
                                        (offer.clipboard ? kFileOfferFlagClipboard : 0)));
    std::memcpy(cursor, offer.sha256.data(), offer.sha256.size());
    cursor += offer.sha256.size();
    put_u16(cursor, static_cast<uint16_t>(name.size()));
    if (!name.empty()) {
        std::memcpy(cursor, name.data(), name.size());
        cursor += name.size();
    }
    put_u16(cursor, static_cast<uint16_t>(relative.size()));
    if (!relative.empty()) {
        std::memcpy(cursor, relative.data(), relative.size());
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
    if (offer.transfer_id == 0) {
        set_error(error, "FileOffer carries no transfer id");
        return false;
    }
    offer.resume = (flags & kFileOfferFlagResume) != 0;
    offer.clipboard = (flags & kFileOfferFlagClipboard) != 0;
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
    offer.relative_path.clear();
    if (!reader.at_end()) {
        // v22 appends the directory. A missing tail is a v21-style root
        // offer; peers still have to agree on the version in the handshake.
        uint16_t relative_size = 0;
        if (!reader.u16(relative_size) ||
            !reader.string(offer.relative_path, relative_size) || !reader.at_end()) {
            set_error(error, "FileOffer relative path overruns the message");
            return false;
        }
        if (!parse_file_relative_path(offer.relative_path, error)) {
            return false;
        }
    }
    if (offer.name.empty() || !valid_utf8(offer.name)) {
        set_error(error, "FileOffer carries an unusable name");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_file_answer(const FileAnswer& answer) {
    if (answer.transfer_id == 0) {
        return {};
    }
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
    if (answer.transfer_id == 0) {
        set_error(error, "FileAnswer carries no transfer id");
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
    if (result.transfer_id == 0) {
        return {};
    }
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
    if (result.transfer_id == 0) {
        set_error(error, "FileResult carries no transfer id");
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
    if (cancel.transfer_id == 0) {
        return {};
    }
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
    if (cancel.transfer_id == 0) {
        set_error(error, "FileCancel carries no transfer id");
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

namespace {

bool known_file_code(uint8_t code) {
    return code <= static_cast<uint8_t>(FileCode::Cancelled);
}

bool valid_list_entry_name(const std::string& name) {
    return !name.empty() && name.size() <= kMaxFileNameBytes && valid_utf8(name) &&
           name.find('\0') == std::string::npos && name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos;
}

}  // namespace

bool valid_file_browser_path(const std::string& path) {
    if (path.empty()) {
        return true;
    }
    if (path.size() > kMaxFileBrowserPathBytes || !valid_utf8(path) ||
        path.find('\0') != std::string::npos) {
        return false;
    }
    if (path.front() == '/' || path.back() == '/' ||
        path.find('\\') != std::string::npos || path.find(':') != std::string::npos) {
        return false;
    }
    size_t components = 0;
    size_t start = 0;
    for (;;) {
        const size_t slash = path.find('/', start);
        const size_t length = slash == std::string::npos ? path.size() - start : slash - start;
        if (length == 0 || length > kMaxFileNameBytes ||
            (length == 1 && path[start] == '.') ||
            (length == 2 && path[start] == '.' && path[start + 1] == '.')) {
            return false;
        }
        if (++components > kMaxFileRelativeComponents) {
            return false;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

bool valid_file_browser_name(const std::string& name) {
    return !name.empty() && name.size() <= kMaxFileNameBytes && valid_utf8(name) &&
           name.find('\0') == std::string::npos && name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos && name.find(':') == std::string::npos &&
           name != "." && name != "..";
}

std::vector<uint8_t> make_file_list_request(uint32_t request_id,
                                            const std::string& directory) {
    if (request_id == 0 || !valid_file_browser_path(directory)) {
        return {};
    }
    std::vector<uint8_t> payload(4 + 2 + directory.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, request_id);
    put_u16(cursor, static_cast<uint16_t>(directory.size()));
    if (!directory.empty()) {
        std::memcpy(cursor, directory.data(), directory.size());
    }
    return make_control(ControlType::FileListRequest, payload);
}

bool parse_file_list_request(const std::vector<uint8_t>& payload, uint32_t& request_id,
                             std::string& directory, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint16_t size = 0;
    if (!reader.u32(request_id) || !reader.u16(size) ||
        !reader.string(directory, size) || !reader.at_end()) {
        set_error(error, "malformed FileListRequest");
        return false;
    }
    if (request_id == 0) {
        set_error(error, "FileListRequest has a zero request id");
        return false;
    }
    if (!valid_file_browser_path(directory)) {
        set_error(error, "FileListRequest path is not a plain relative directory");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_file_list_result(const FileListResult& result) {
    if (result.request_id == 0 ||
        !known_file_code(static_cast<uint8_t>(result.code)) ||
        result.entries.size() > kMaxFileListEntries) {
        return {};
    }
    if (result.code != FileCode::Ok && (!result.entries.empty() || result.truncated)) {
        return {};
    }
    size_t entries_size = 0;
    for (const FileListEntry& entry : result.entries) {
        if (!valid_list_entry_name(entry.name)) {
            return {};
        }
        entries_size += 2 + entry.name.size() + 8 + 8 + 1;
    }
    const std::string reason = truncate_utf8_reason(result.reason, kMaxFileBrowserReasonBytes);
    std::vector<uint8_t> payload(4 + 1 + 1 + 2 + reason.size() + 2 + entries_size);
    uint8_t* cursor = payload.data();
    put_u32(cursor, result.request_id);
    put_u8(cursor, static_cast<uint8_t>(result.code));
    put_u8(cursor, result.truncated ? kFileListFlagTruncated : 0);
    put_u16(cursor, static_cast<uint16_t>(reason.size()));
    if (!reason.empty()) {
        std::memcpy(cursor, reason.data(), reason.size());
        cursor += reason.size();
    }
    put_u16(cursor, static_cast<uint16_t>(result.entries.size()));
    for (const FileListEntry& entry : result.entries) {
        put_u16(cursor, static_cast<uint16_t>(entry.name.size()));
        std::memcpy(cursor, entry.name.data(), entry.name.size());
        cursor += entry.name.size();
        put_u64(cursor, entry.size);
        put_u64(cursor, entry.modified_ms);
        put_u8(cursor, entry.directory ? kFileEntryFlagDirectory : 0);
    }
    return make_control(ControlType::FileListResult, payload);
}

bool parse_file_list_result(const std::vector<uint8_t>& payload, FileListResult& result,
                            std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t code = 0;
    uint8_t flags = 0;
    uint16_t reason_size = 0;
    uint16_t count = 0;
    if (!reader.u32(result.request_id) || !reader.u8(code) || !reader.u8(flags) ||
        !reader.u16(reason_size) || !reader.string(result.reason, reason_size) ||
        !reader.u16(count)) {
        set_error(error, "truncated FileListResult");
        return false;
    }
    if (result.request_id == 0) {
        set_error(error, "FileListResult has a zero request id");
        return false;
    }
    if (!known_file_code(code) || (flags & ~kFileListFlagTruncated) != 0) {
        set_error(error, "FileListResult carries an unknown code or flags");
        return false;
    }
    result.code = static_cast<FileCode>(code);
    result.truncated = (flags & kFileListFlagTruncated) != 0;
    if (reason_size > kMaxFileBrowserReasonBytes) {
        set_error(error, "FileListResult reason is too long");
        return false;
    }
    if (!result.reason.empty() && !valid_utf8(result.reason)) {
        set_error(error, "FileListResult reason is not UTF-8");
        return false;
    }
    if (count > kMaxFileListEntries) {
        set_error(error, "FileListResult carries too many entries");
        return false;
    }
    if (result.code != FileCode::Ok && (count != 0 || result.truncated)) {
        set_error(error, "a refused FileListResult carries entries");
        return false;
    }
    result.entries.clear();
    result.entries.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t name_size = 0;
        FileListEntry entry;
        uint8_t entry_flags = 0;
        if (!reader.u16(name_size) || !reader.string(entry.name, name_size) ||
            !reader.u64(entry.size) || !reader.u64(entry.modified_ms) ||
            !reader.u8(entry_flags)) {
            set_error(error, "FileListResult entry overruns the message");
            return false;
        }
        if (!valid_list_entry_name(entry.name) || (entry_flags & ~kFileEntryFlagDirectory) != 0) {
            set_error(error, "FileListResult carries an unusable entry");
            return false;
        }
        entry.directory = (entry_flags & kFileEntryFlagDirectory) != 0;
        result.entries.push_back(std::move(entry));
    }
    if (!reader.at_end()) {
        set_error(error, "FileListResult carries trailing bytes");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_file_download_request(uint32_t request_id,
                                                const std::string& path) {
    if (request_id == 0 || path.empty() || !valid_file_browser_path(path)) {
        return {};
    }
    std::vector<uint8_t> payload(4 + 2 + path.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, request_id);
    put_u16(cursor, static_cast<uint16_t>(path.size()));
    std::memcpy(cursor, path.data(), path.size());
    return make_control(ControlType::FileDownloadRequest, payload);
}

bool parse_file_download_request(const std::vector<uint8_t>& payload,
                                 uint32_t& request_id, std::string& path,
                                 std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint16_t size = 0;
    if (!reader.u32(request_id) || !reader.u16(size) || !reader.string(path, size) ||
        !reader.at_end()) {
        set_error(error, "malformed FileDownloadRequest");
        return false;
    }
    if (request_id == 0) {
        set_error(error, "FileDownloadRequest has a zero request id");
        return false;
    }
    if (path.empty() || !valid_file_browser_path(path)) {
        set_error(error, "FileDownloadRequest path is not a plain relative file");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_file_download_result(const FileDownloadResult& result) {
    if (result.request_id == 0 ||
        !known_file_code(static_cast<uint8_t>(result.code)) ||
        (result.code == FileCode::Ok) != (result.transfer_id != 0)) {
        return {};
    }
    const std::string reason = truncate_utf8_reason(result.reason, kMaxFileBrowserReasonBytes);
    std::vector<uint8_t> payload(4 + 4 + 1 + 2 + reason.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, result.request_id);
    put_u32(cursor, result.transfer_id);
    put_u8(cursor, static_cast<uint8_t>(result.code));
    put_u16(cursor, static_cast<uint16_t>(reason.size()));
    if (!reason.empty()) {
        std::memcpy(cursor, reason.data(), reason.size());
    }
    return make_control(ControlType::FileDownloadResult, payload);
}

bool parse_file_download_result(const std::vector<uint8_t>& payload,
                                FileDownloadResult& result, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t code = 0;
    uint16_t reason_size = 0;
    if (!reader.u32(result.request_id) || !reader.u32(result.transfer_id) ||
        !reader.u8(code) || !reader.u16(reason_size) ||
        !reader.string(result.reason, reason_size) || !reader.at_end()) {
        set_error(error, "malformed FileDownloadResult");
        return false;
    }
    if (result.request_id == 0) {
        set_error(error, "FileDownloadResult has a zero request id");
        return false;
    }
    if (!known_file_code(code)) {
        set_error(error, "FileDownloadResult carries an unknown code");
        return false;
    }
    result.code = static_cast<FileCode>(code);
    if ((result.code == FileCode::Ok) != (result.transfer_id != 0)) {
        set_error(error, "FileDownloadResult code and transfer id disagree");
        return false;
    }
    if (reason_size > kMaxFileBrowserReasonBytes) {
        set_error(error, "FileDownloadResult reason is too long");
        return false;
    }
    if (!result.reason.empty() && !valid_utf8(result.reason)) {
        set_error(error, "FileDownloadResult reason is not UTF-8");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_file_op_request(uint32_t request_id, FileOperation op,
                                          const std::string& path,
                                          const std::string& new_name) {
    const uint8_t raw_op = static_cast<uint8_t>(op);
    if (request_id == 0 || raw_op > static_cast<uint8_t>(FileOperation::Remove) ||
        path.empty() || !valid_file_browser_path(path)) {
        return {};
    }
    if (op == FileOperation::Rename ? !valid_file_browser_name(new_name)
                                    : !new_name.empty()) {
        return {};
    }
    std::vector<uint8_t> payload(4 + 1 + 2 + path.size() + 2 + new_name.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, request_id);
    put_u8(cursor, raw_op);
    put_u16(cursor, static_cast<uint16_t>(path.size()));
    std::memcpy(cursor, path.data(), path.size());
    cursor += path.size();
    put_u16(cursor, static_cast<uint16_t>(new_name.size()));
    if (!new_name.empty()) {
        std::memcpy(cursor, new_name.data(), new_name.size());
    }
    return make_control(ControlType::FileOpRequest, payload);
}

bool parse_file_op_request(const std::vector<uint8_t>& payload, uint32_t& request_id,
                           FileOperation& op, std::string& path, std::string& new_name,
                           std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t raw_op = 0;
    uint16_t path_size = 0;
    uint16_t name_size = 0;
    if (!reader.u32(request_id) || !reader.u8(raw_op) || !reader.u16(path_size) ||
        !reader.string(path, path_size) || !reader.u16(name_size) ||
        !reader.string(new_name, name_size) || !reader.at_end()) {
        set_error(error, "malformed FileOpRequest");
        return false;
    }
    if (request_id == 0) {
        set_error(error, "FileOpRequest has a zero request id");
        return false;
    }
    if (raw_op > static_cast<uint8_t>(FileOperation::Remove)) {
        set_error(error, "FileOpRequest carries an unknown operation");
        return false;
    }
    op = static_cast<FileOperation>(raw_op);
    if (path.empty() || !valid_file_browser_path(path)) {
        set_error(error, "FileOpRequest path is not a plain relative path");
        return false;
    }
    if (op == FileOperation::Rename ? !valid_file_browser_name(new_name)
                                    : !new_name.empty()) {
        set_error(error, "FileOpRequest new name does not match its operation");
        return false;
    }
    return true;
}

std::vector<uint8_t> make_file_op_result(const FileOpResult& result) {
    if (result.request_id == 0 ||
        !known_file_code(static_cast<uint8_t>(result.code))) {
        return {};
    }
    const std::string reason = truncate_utf8_reason(result.reason, kMaxFileBrowserReasonBytes);
    std::vector<uint8_t> payload(4 + 1 + 2 + reason.size());
    uint8_t* cursor = payload.data();
    put_u32(cursor, result.request_id);
    put_u8(cursor, static_cast<uint8_t>(result.code));
    put_u16(cursor, static_cast<uint16_t>(reason.size()));
    if (!reason.empty()) {
        std::memcpy(cursor, reason.data(), reason.size());
    }
    return make_control(ControlType::FileOpResult, payload);
}

bool parse_file_op_result(const std::vector<uint8_t>& payload, FileOpResult& result,
                          std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint8_t code = 0;
    uint16_t reason_size = 0;
    if (!reader.u32(result.request_id) || !reader.u8(code) ||
        !reader.u16(reason_size) || !reader.string(result.reason, reason_size) ||
        !reader.at_end()) {
        set_error(error, "malformed FileOpResult");
        return false;
    }
    if (result.request_id == 0) {
        set_error(error, "FileOpResult has a zero request id");
        return false;
    }
    if (!known_file_code(code)) {
        set_error(error, "FileOpResult carries an unknown code");
        return false;
    }
    result.code = static_cast<FileCode>(code);
    if (reason_size > kMaxFileBrowserReasonBytes) {
        set_error(error, "FileOpResult reason is too long");
        return false;
    }
    if (!result.reason.empty() && !valid_utf8(result.reason)) {
        set_error(error, "FileOpResult reason is not UTF-8");
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
