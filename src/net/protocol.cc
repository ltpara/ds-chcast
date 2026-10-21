#include "chast/protocol.h"

#include <array>
#include <cstring>

namespace chast {

namespace {

constexpr char kFrameMagic[4] = {'C', 'F', 'R', 'M'};
constexpr char kControlMagic[4] = {'C', 'C', 'T', 'L'};
// A single receive buffer holding more than this without yielding a frame
// means the peer is not speaking the protocol.
constexpr size_t kMaxFrameBuffer = 64u * 1024u * 1024u;
constexpr size_t kMaxControlBuffer = 4u * 1024u * 1024u;

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
    std::memset(cursor, 0, 7);
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
    return true;
}

std::vector<uint8_t> make_hello() {
    std::vector<uint8_t> payload(4);
    uint8_t* cursor = payload.data();
    put_u16(cursor, kProtocolVersion);
    put_u16(cursor, 0);
    return make_control(ControlType::Hello, payload);
}

bool parse_hello(const std::vector<uint8_t>& payload, std::string* error) {
    Reader reader(payload.data(), payload.size());
    uint16_t version = 0;
    uint16_t reserved = 0;
    if (!reader.u16(version) || !reader.u16(reserved)) {
        set_error(error, "truncated Hello");
        return false;
    }
    if (version != kProtocolVersion) {
        set_error(error, "client speaks protocol version " + std::to_string(version));
        return false;
    }
    return true;
}

std::vector<uint8_t> make_hello_ack(const ServerInfo& info) {
    std::vector<uint8_t> payload(24 + info.encoder.size());
    uint8_t* cursor = payload.data();
    put_u8(cursor, codec_wire(info.codec));
    put_u8(cursor, 0);
    put_u16(cursor, info.width);
    put_u16(cursor, info.height);
    put_u16(cursor, info.framerate);
    put_u32(cursor, info.bitrate);
    put_u32(cursor, static_cast<uint32_t>(info.origin_x));
    put_u32(cursor, static_cast<uint32_t>(info.origin_y));
    put_u16(cursor, info.input_enabled ? kServerInfoInputEnabled : 0);
    put_u16(cursor, static_cast<uint16_t>(info.encoder.size()));
    std::memcpy(cursor, info.encoder.data(), info.encoder.size());
    return make_control(ControlType::HelloAck, payload);
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
    info.input_enabled = (flags & kServerInfoInputEnabled) != 0;
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
    return reader.string(info.encoder, name_size);
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
    put_u16(cursor, 0);
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
    uint16_t reserved = 0;
    if (!reader.u8(kind) || !reader.u8(action) || !reader.u16(buttons) || !reader.u32(x) ||
        !reader.u32(y) || !reader.u16(hid_usage) || !reader.u16(reserved)) {
        set_error(error, "truncated InputEvent");
        return false;
    }
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

}  // namespace chast
