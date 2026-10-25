// Pure-logic checks for the wire format: no GPU, no network.

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "chast/clipboard.h"
#include "chast/identity.h"
#include "chast/input.h"
#include "chast/protocol.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// The parsers take the payload; the makers return the whole control message.
std::vector<uint8_t> payload_of(const std::vector<uint8_t>& message) {
    return std::vector<uint8_t>(message.begin() + chast::kControlHeaderSize, message.end());
}

void test_hello() {
    std::string error;
    auto parses_hello = [&](const std::vector<uint8_t>& payload,
                            chast::ClientHello& hello) {
        return chast::parse_hello(payload, hello, &error);
    };
    auto rejects_hello = [&](const std::vector<uint8_t>& payload, const char* what) {
        chast::ClientHello hello;
        check(!parses_hello(payload, hello), what);
    };

    chast::ClientHello sent;
    chast::ClientHello got;
    check(parses_hello(payload_of(chast::make_hello(sent)), got), "Hello round-trips");
    check(got.display == chast::kDisplayAuto, "Hello defaults to the server's display");
    check(got.access_token.empty(), "Hello without a token carries an empty one");
    check(got.password.empty(), "Hello without a password carries an empty one");
    check(got.requested_permissions == chast::kPermissionAll,
          "Hello requests every permission by default");

    sent.display = 3;
    check(parses_hello(payload_of(chast::make_hello(sent)), got) && got.display == 3,
          "Hello keeps an explicit display index");

    sent.display = chast::kDisplayAll;
    check(parses_hello(payload_of(chast::make_hello(sent)), got) &&
              got.display == chast::kDisplayAll,
          "Hello accepts the merged display sentinel");

    sent.access_token = "correct horse battery staple";
    sent.password = "hunter2";
    sent.requested_permissions = chast::kPermissionInput | chast::kPermissionAudio;
    sent.device_key.assign(72, 0x11);
    sent.device_platform = static_cast<uint8_t>(chast::DevicePlatform::Windows);
    sent.device_name = "viewer-pc";
    for (size_t i = 0; i < sent.nonce.size(); ++i) {
        sent.nonce[i] = static_cast<uint8_t>(i * 7);
    }
    sent.unix_seconds = 1700000000;
    sent.auth_flags = chast::kAuthFlagSigned;
    sent.signature.assign(64, 0x22);
    sent.totp = "123456";
    check(parses_hello(payload_of(chast::make_hello(sent)), got) &&
              got.access_token == sent.access_token &&
              got.password == sent.password &&
              got.requested_permissions == sent.requested_permissions &&
              got.device_key == sent.device_key &&
              got.device_platform == sent.device_platform &&
              got.device_name == sent.device_name &&
              got.nonce == sent.nonce &&
              got.unix_seconds == sent.unix_seconds &&
              got.auth_flags == sent.auth_flags &&
              got.signature == sent.signature && got.totp == sent.totp,
          "Hello round-trips the v13 authentication fields");

    sent.access_token.clear();
    sent.password.clear();
    sent.device_key.clear();
    sent.signature.clear();
    sent.auth_flags = 0;
    sent.totp.clear();
    sent.device_name.clear();
    sent.unix_seconds = 0;

    rejects_hello(std::vector<uint8_t>(4), "short Hello is rejected");
    std::vector<uint8_t> bad_display = payload_of(chast::make_hello(sent));
    bad_display[2] = static_cast<uint8_t>(chast::kMaxDisplayCount);
    bad_display[3] = 0;
    rejects_hello(bad_display, "Hello with a display index past the limit is rejected");

    std::vector<uint8_t> old_version = payload_of(chast::make_hello(sent));
    old_version[0] = static_cast<uint8_t>(chast::kProtocolVersion - 1);
    old_version[1] = 0;
    rejects_hello(old_version, "Hello from another protocol version is rejected");

    std::vector<uint8_t> long_token = payload_of(chast::make_hello(sent));
    long_token[4] = static_cast<uint8_t>(chast::kMaxAccessTokenLength + 1);
    long_token[5] = 0;
    rejects_hello(long_token, "Hello with an oversized token length is rejected");

    std::vector<uint8_t> truncated_token = payload_of(chast::make_hello(sent));
    truncated_token[4] = 3;  // claims three bytes that belong to later fields
    truncated_token[5] = 0;
    rejects_hello(truncated_token,
                  "Hello whose token length disagrees with the payload is rejected");

    std::vector<uint8_t> long_password = payload_of(chast::make_hello(sent));
    long_password[6] = static_cast<uint8_t>(chast::kMaxPasswordLength + 1);
    long_password[7] = 0;
    rejects_hello(long_password, "Hello with an oversized password is rejected");

    std::vector<uint8_t> unknown_permissions = payload_of(chast::make_hello(sent));
    unknown_permissions[8] = 0x40;  // version, display, token length, password length
    unknown_permissions[9] = 0;
    rejects_hello(unknown_permissions, "Hello requesting unknown permissions is rejected");

    std::vector<uint8_t> trailing = payload_of(chast::make_hello(sent));
    trailing.push_back(0xFF);
    rejects_hello(trailing, "Hello with trailing bytes is rejected");

    chast::ClientHello unsigned_claim = sent;
    unsigned_claim.auth_flags = chast::kAuthFlagSigned;
    rejects_hello(payload_of(chast::make_hello(unsigned_claim)),
                  "Hello that claims a signature but carries none is rejected");
    chast::ClientHello trust_claim = sent;
    trust_claim.auth_flags = chast::kAuthFlagTrustDevice;
    trust_claim.device_key.assign(72, 0x11);
    rejects_hello(payload_of(chast::make_hello(trust_claim)),
                  "Hello that asks for trust without a signature is rejected");
    chast::ClientHello bad_totp = sent;
    bad_totp.totp = "12ab56";
    rejects_hello(payload_of(chast::make_hello(bad_totp)),
                  "Hello with a non-numeric TOTP code is rejected");
}

void test_access_denied() {
    chast::AccessDenial code = chast::AccessDenial::InvalidToken;
    std::string reason;
    std::string error;
    check(chast::parse_access_denied(
              payload_of(chast::make_access_denied(chast::AccessDenial::TokenUsed,
                                                   "the token was already used")),
              code, reason, &error) &&
              code == chast::AccessDenial::TokenUsed &&
              reason == "the token was already used",
          "AccessDenied round-trips its code and reason");

    check(chast::parse_access_denied(
              payload_of(chast::make_access_denied(chast::AccessDenial::Declined, "")), code,
              reason, &error) &&
              code == chast::AccessDenial::Declined && reason.empty(),
          "AccessDenied may carry an empty reason");

    std::vector<uint8_t> unknown_code =
        payload_of(chast::make_access_denied(chast::AccessDenial::Declined, "no"));
    unknown_code[0] = 0;
    check(!chast::parse_access_denied(unknown_code, code, reason, &error),
          "AccessDenied with an unknown code is rejected");

    std::vector<uint8_t> long_reason = payload_of(
        chast::make_access_denied(chast::AccessDenial::Declined, "short"));
    long_reason[1] = static_cast<uint8_t>(chast::kMaxAccessDenialReason + 1);
    long_reason[2] = static_cast<uint8_t>((chast::kMaxAccessDenialReason + 1) >> 8);
    check(!chast::parse_access_denied(long_reason, code, reason, &error),
          "AccessDenied whose reason length disagrees with the payload is rejected");

    check(!chast::parse_access_denied(std::vector<uint8_t>(2), code, reason, &error),
          "short AccessDenied is rejected");
}

void test_hello_ack() {
    chast::ServerInfo sent;
    sent.codec = chast::VideoCodec::Hevc;
    sent.width = 2560;
    sent.height = 1440;
    sent.source_width = 2560;
    sent.source_height = 1440;
    sent.framerate = 60;
    sent.bitrate = 12000000;
    sent.origin_x = -1920;
    sent.origin_y = -120;
    sent.input_enabled = true;
    sent.clipboard_enabled = true;
    sent.file_transfer_enabled = true;
    sent.microphone_enabled = true;
    sent.privacy_mode_enabled = true;
    sent.encoder = "mfxhw64 (VDENC HEVC)";
    chast::DisplayDescription primary;
    primary.name = "\\\\.\\DISPLAY1";
    primary.right = 2560;
    primary.bottom = 1440;
    primary.primary = true;
    chast::DisplayDescription portrait;
    portrait.name = "\\\\.\\DISPLAY2";
    portrait.left = -1080;
    portrait.bottom = 1920;
    portrait.rotation_degrees = 90;
    sent.displays = {primary, portrait};
    sent.audio.enabled = true;
    sent.audio.codec = chast::AudioCodec::AAC;
    sent.audio.sample_rate = 48000;
    sent.audio.channels = 2;
    sent.audio.bitrate = 160000;
    sent.audio.frame_samples = chast::kAudioFrameSamples;
    // AudioSpecificConfig for AAC-LC 48 kHz stereo.
    sent.audio.extradata = {0x11, 0x90};

    chast::ServerInfo got;
    std::string error;
    const bool parsed = chast::parse_hello_ack(payload_of(chast::make_hello_ack(sent)), got, &error);
    check(parsed, "HelloAck round-trips");
    if (!parsed) {
        std::printf("     %s\n", error.c_str());
        return;
    }
    check(got.codec == sent.codec, "HelloAck keeps the codec");
    check(got.width == sent.width && got.height == sent.height, "HelloAck keeps the size");
    check(got.source_width == 2560 && got.source_height == 1440,
          "HelloAck keeps the captured source size");
    check(got.framerate == sent.framerate && got.bitrate == sent.bitrate,
          "HelloAck keeps the rate control");
    check(got.origin_x == sent.origin_x && got.origin_y == sent.origin_y,
          "HelloAck keeps negative virtual-desktop origins");
    check(got.input_enabled, "HelloAck keeps the input flag");
    check(got.clipboard_enabled, "HelloAck keeps the clipboard flag");
    check(got.file_transfer_enabled, "HelloAck keeps the file transfer flag");
    check(got.microphone_enabled, "HelloAck keeps the microphone flag");
    check(got.privacy_mode_enabled, "HelloAck keeps the privacy mode flag");
    check(got.input_enabled && got.clipboard_enabled && got.audio.enabled &&
              got.file_transfer_enabled && got.microphone_enabled &&
              got.privacy_mode_enabled,
          "HelloAck mask grants every permission");
    check(got.encoder == sent.encoder, "HelloAck keeps the encoder name");
    check(got.displays.size() == 2, "HelloAck keeps the display count");
    if (got.displays.size() == 2) {
        check(got.displays[0].name == primary.name && got.displays[0].primary &&
                  got.displays[0].right == 2560 && got.displays[0].rotation_degrees == 0,
              "HelloAck keeps the primary display");
        check(got.displays[1].left == -1080 && got.displays[1].top == 0 &&
                  got.displays[1].rotation_degrees == 90 && !got.displays[1].primary,
              "HelloAck keeps a rotated display with a negative origin");
    }
    check(got.audio.enabled && got.audio.codec == chast::AudioCodec::AAC &&
              got.audio.sample_rate == sent.audio.sample_rate &&
              got.audio.channels == sent.audio.channels &&
              got.audio.bitrate == sent.audio.bitrate &&
              got.audio.frame_samples == sent.audio.frame_samples &&
              got.audio.extradata == sent.audio.extradata,
          "HelloAck keeps the audio description and decoder config");

    chast::ServerInfo opus = sent;
    opus.audio.codec = chast::AudioCodec::Opus;
    opus.audio.frame_samples = chast::kOpusFrameSamples;
    opus.audio.bitrate = 128000;
    opus.audio.extradata.assign(19, 0);
    std::memcpy(opus.audio.extradata.data(), "OpusHead", 8);
    opus.audio.extradata[8] = 1;
    opus.audio.extradata[9] = 2;
    opus.audio.extradata[10] = 0x38;
    opus.audio.extradata[11] = 0x01;  // pre-skip 312, for a 6.5 ms lead
    opus.audio.extradata[14] = 0x80;  // input rate 48000
    opus.audio.extradata[15] = 0xBB;
    chast::ServerInfo opus_got;
    check(chast::parse_hello_ack(payload_of(chast::make_hello_ack(opus)), opus_got, &error) &&
              opus_got.audio.codec == chast::AudioCodec::Opus &&
              opus_got.audio.frame_samples == chast::kOpusFrameSamples &&
              opus_got.audio.extradata == opus.audio.extradata &&
              opus_got.microphone_enabled,
          "HelloAck round-trips an Opus description with the microphone permission");

    auto ack_payload = [&] { return payload_of(chast::make_hello_ack(sent)); };
    const size_t display_count_at = 24 + sent.encoder.size();
    const size_t first_display_at = display_count_at + 2;
    size_t display_bytes = 0;
    for (const chast::DisplayDescription& display : sent.displays) {
        display_bytes += 22 + display.name.size();
    }
    const size_t audio_at = first_display_at + display_bytes;
    auto rejects_ack = [&](std::vector<uint8_t> payload, const char* what) {
        chast::ServerInfo dropped;
        check(!chast::parse_hello_ack(payload, dropped, &error), what);
    };
    std::vector<uint8_t> too_many = ack_payload();
    too_many[display_count_at] = 65;
    too_many[display_count_at + 1] = 0;
    rejects_ack(too_many, "a HelloAck with too many displays is rejected");
    std::vector<uint8_t> bad_rotation = ack_payload();
    bad_rotation[first_display_at + 2 + primary.name.size() + 16] = 45;
    rejects_ack(bad_rotation, "a display with an unknown rotation is rejected");
    std::vector<uint8_t> empty_rect = ack_payload();
    const size_t right_at = first_display_at + 2 + primary.name.size() + 8;
    empty_rect[right_at] = 0;
    empty_rect[right_at + 1] = 0;
    empty_rect[right_at + 2] = 0;
    empty_rect[right_at + 3] = 0;
    rejects_ack(empty_rect, "a display with an empty rect is rejected");
    std::vector<uint8_t> unknown_flags = ack_payload();
    // codec, reserved, size, rate, bitrate, origin, then the flags word.
    constexpr size_t kFlagsAt = 20;
    unknown_flags[kFlagsAt] = 0x80;
    unknown_flags[kFlagsAt + 1] = 0;
    rejects_ack(std::move(unknown_flags), "a HelloAck with unknown flags is rejected");

    std::vector<uint8_t> no_audio = ack_payload();
    no_audio.resize(audio_at);
    rejects_ack(std::move(no_audio), "a HelloAck without the audio description is rejected");
    std::vector<uint8_t> audio_flag_only = ack_payload();
    audio_flag_only[kFlagsAt] = 0x03;  // input and clipboard, no audio
    rejects_ack(std::move(audio_flag_only),
                "a HelloAck whose audio flag disagrees with its block is rejected");
    std::vector<uint8_t> audio_block_only = ack_payload();
    audio_block_only[audio_at] = 0;
    rejects_ack(std::move(audio_block_only),
                "a HelloAck that announces audio without the flag is rejected");
    std::vector<uint8_t> unknown_audio_codec = ack_payload();
    unknown_audio_codec[audio_at + 1] = 3;
    rejects_ack(std::move(unknown_audio_codec),
                "a HelloAck with an unknown audio codec is rejected");
    std::vector<uint8_t> no_extradata = ack_payload();
    no_extradata[audio_at + 14] = 0;
    no_extradata[audio_at + 15] = 0;
    rejects_ack(std::move(no_extradata),
                "a HelloAck that announces audio without its config is rejected");
    std::vector<uint8_t> long_extradata = ack_payload();
    long_extradata[audio_at + 14] = static_cast<uint8_t>(chast::kMaxAudioExtradata + 1);
    long_extradata[audio_at + 15] = 0;
    rejects_ack(std::move(long_extradata),
                "a HelloAck with an oversized audio config is rejected");
    std::vector<uint8_t> bad_audio_frame = ack_payload();
    bad_audio_frame[audio_at + 4] = 0;
    bad_audio_frame[audio_at + 5] = 0;
    rejects_ack(std::move(bad_audio_frame),
                "a HelloAck with an unsupported audio frame length is rejected");
    std::vector<uint8_t> bad_audio_rate = ack_payload();
    bad_audio_rate[audio_at + 6] = 0;
    bad_audio_rate[audio_at + 7] = 0;
    bad_audio_rate[audio_at + 8] = 0;
    bad_audio_rate[audio_at + 9] = 0;
    rejects_ack(std::move(bad_audio_rate), "a HelloAck with a bad audio rate is rejected");

    std::vector<uint8_t> no_source = ack_payload();
    no_source.resize(no_source.size() - 4);
    rejects_ack(std::move(no_source), "a HelloAck without the source size is rejected");
    std::vector<uint8_t> empty_source = ack_payload();
    empty_source[empty_source.size() - 4] = 0;
    empty_source[empty_source.size() - 3] = 0;
    empty_source[empty_source.size() - 2] = 0;
    empty_source[empty_source.size() - 1] = 0;
    rejects_ack(std::move(empty_source), "a HelloAck with an empty source picture is rejected");

    std::vector<uint8_t> mask_mismatch = ack_payload();
    mask_mismatch[mask_mismatch.size() - 2] = 0x03;  // flags still say audio too
    mask_mismatch[mask_mismatch.size() - 1] = 0;
    rejects_ack(std::move(mask_mismatch),
                "a HelloAck whose permission mask disagrees with its flags is rejected");
    std::vector<uint8_t> unknown_permissions = ack_payload();
    unknown_permissions[unknown_permissions.size() - 2] = 0x40;
    unknown_permissions[unknown_permissions.size() - 1] = 0;
    rejects_ack(std::move(unknown_permissions),
                "a HelloAck granting unknown permissions is rejected");

    sent.input_enabled = false;
    sent.clipboard_enabled = false;
    sent.file_transfer_enabled = false;
    sent.microphone_enabled = false;
    sent.privacy_mode_enabled = false;
    sent.audio.enabled = false;
    sent.audio.extradata.clear();
    chast::ServerInfo disabled;
    check(chast::parse_hello_ack(payload_of(chast::make_hello_ack(sent)), disabled, &error) &&
              !disabled.input_enabled && !disabled.clipboard_enabled &&
              !disabled.file_transfer_enabled && !disabled.audio.enabled &&
              !disabled.microphone_enabled && !disabled.privacy_mode_enabled,
          "HelloAck reports every permission disabled");
}

void test_merged_hello_ack() {
    auto merged_info = [] {
        chast::ServerInfo info;
        info.codec = chast::VideoCodec::H264;
        info.merged = true;
        info.width = 3840;
        info.height = 1080;
        info.source_width = 3840;
        info.source_height = 1080;
        info.origin_x = -1920;
        info.origin_y = 0;
        info.framerate = 30;
        info.bitrate = 8000000;
        info.input_enabled = true;
        info.encoder = "mfxhw64";
        chast::DisplayDescription left;
        left.name = "\\\\.\\DISPLAY1";
        left.left = -1920;
        left.right = 0;
        left.bottom = 1080;
        left.primary = true;
        chast::DisplayDescription right;
        right.name = "\\\\.\\DISPLAY2";
        right.right = 1920;
        right.bottom = 1080;
        info.displays = {left, right};
        return info;
    };

    std::string error;
    chast::ServerInfo sent = merged_info();
    chast::ServerInfo got;
    const bool parsed =
        chast::parse_hello_ack(payload_of(chast::make_hello_ack(sent)), got, &error);
    check(parsed, "a merged HelloAck round-trips");
    if (!parsed) {
        std::printf("     %s\n", error.c_str());
        return;
    }
    check(got.merged, "a merged HelloAck keeps the merged flag");
    check(got.origin_x == -1920 && got.origin_y == 0 &&
              got.source_width == 3840 && got.source_height == 1080,
          "a merged HelloAck keeps the union geometry");
    check(got.displays.size() == 2, "a merged HelloAck lists its member displays");
    check(got.input_enabled, "the merged flag does not disturb the permission mask");

    chast::ServerInfo padded = merged_info();
    padded.source_width = 3841;  // odd union padded up for NV12
    padded.width = 3842;
    chast::ServerInfo padded_got;
    check(chast::parse_hello_ack(payload_of(chast::make_hello_ack(padded)), padded_got, &error) &&
              padded_got.source_width == 3841,
          "a merged HelloAck accepts one pixel of even-size padding");

    auto rejects = [&](const chast::ServerInfo& info, const char* what) {
        chast::ServerInfo dropped;
        check(!chast::parse_hello_ack(payload_of(chast::make_hello_ack(info)), dropped, &error),
              what);
    };
    chast::ServerInfo wrong_origin = merged_info();
    wrong_origin.origin_x = -1919;
    rejects(wrong_origin, "a merged HelloAck whose origin misses the union is rejected");
    chast::ServerInfo wrong_size = merged_info();
    wrong_size.source_width = 3842;
    rejects(wrong_size, "a merged HelloAck whose source misses the union is rejected");
    chast::ServerInfo empty = merged_info();
    empty.displays.clear();
    rejects(empty, "a merged HelloAck with no member displays is rejected");
}

void test_permission_update() {
    uint16_t permissions = chast::kPermissionAll;
    std::string error;
    check((chast::kPermissionAll & chast::kPermissionFileTransfer) != 0 &&
              (chast::kPermissionAll & chast::kPermissionClipboard) != 0,
          "the permission mask includes clipboard and file transfer");
    check(chast::parse_permission_update(
              payload_of(chast::make_permission_update(chast::kPermissionInput)),
              permissions, &error) &&
              permissions == chast::kPermissionInput,
          "PermissionUpdate round-trips a reduced mask");
    check(chast::parse_permission_update(
              payload_of(chast::make_permission_update(chast::kPermissionAll)),
              permissions, &error) &&
              permissions == chast::kPermissionAll,
          "PermissionUpdate round-trips every permission");

    std::vector<uint8_t> unknown = payload_of(chast::make_permission_update(0));
    unknown[0] = 0x40;
    check(!chast::parse_permission_update(unknown, permissions, &error),
          "PermissionUpdate with unknown bits is rejected");
    std::vector<uint8_t> trailing = payload_of(chast::make_permission_update(0));
    trailing.push_back(0);
    check(!chast::parse_permission_update(trailing, permissions, &error),
          "PermissionUpdate with trailing bytes is rejected");
    check(!chast::parse_permission_update(std::vector<uint8_t>(1), permissions, &error),
          "short PermissionUpdate is rejected");
}

void test_sas() {
    std::string error;
    check(chast::parse_send_sas(payload_of(chast::make_send_sas()), &error),
          "SendSas round-trips an empty request");
    check(payload_of(chast::make_send_sas()).empty(), "SendSas has no payload");
    std::vector<uint8_t> trailing = payload_of(chast::make_send_sas());
    trailing.push_back(0);
    check(!chast::parse_send_sas(trailing, &error),
          "SendSas with trailing bytes is rejected");

    chast::SasCode code = chast::SasCode::Failed;
    std::string reason;
    check(chast::parse_sas_result(
              payload_of(chast::make_sas_result(chast::SasCode::Ok, {})), code, reason,
              &error) &&
              code == chast::SasCode::Ok && reason.empty(),
          "SasResult round-trips a success");
    check(chast::parse_sas_result(
              payload_of(chast::make_sas_result(
                  chast::SasCode::Unsupported,
                  "Ctrl+Alt+Del needs the Windows service")),
              code, reason, &error) &&
              code == chast::SasCode::Unsupported &&
              reason == "Ctrl+Alt+Del needs the Windows service",
          "SasResult round-trips a refusal with its reason");
    std::vector<uint8_t> unknown_code =
        payload_of(chast::make_sas_result(chast::SasCode::Denied, {}));
    unknown_code[0] = 4;
    check(!chast::parse_sas_result(unknown_code, code, reason, &error),
          "SasResult with an unknown code is rejected");
    const std::string long_reason(400, 'x');
    check(chast::parse_sas_result(
              payload_of(chast::make_sas_result(chast::SasCode::Failed, long_reason)),
              code, reason, &error) &&
              reason.size() == chast::kMaxSasReason,
          "SasResult truncates an oversized reason to the protocol limit");
    std::vector<uint8_t> bad_utf8 =
        payload_of(chast::make_sas_result(chast::SasCode::Failed, {}));
    bad_utf8[1] = 1;
    bad_utf8[2] = 0;
    bad_utf8.push_back(0xFF);
    check(!chast::parse_sas_result(bad_utf8, code, reason, &error),
          "SasResult with a malformed reason is rejected");
    check(!chast::parse_sas_result(std::vector<uint8_t>(), code, reason, &error),
          "empty SasResult is rejected");
}

void test_stream_options() {
    std::string error;
    chast::StreamOptions options;
    check(chast::parse_set_stream_options(
              payload_of(chast::make_set_stream_options(options)), options, &error) &&
              options.codec == chast::StreamCodecPreference::Auto && options.max_fps == 0 &&
              options.quality == chast::StreamQuality::High,
          "SetStreamOptions round-trips the defaults");

    chast::StreamOptions asked;
    asked.codec = chast::StreamCodecPreference::Hevc;
    asked.max_fps = 60;
    asked.quality = chast::StreamQuality::Smooth;
    check(chast::parse_set_stream_options(
              payload_of(chast::make_set_stream_options(asked)), options, &error) &&
              options.codec == chast::StreamCodecPreference::Hevc &&
              options.max_fps == 60 &&
              options.quality == chast::StreamQuality::Smooth,
          "SetStreamOptions round-trips explicit preferences");

    asked.max_fps = chast::kMaxStreamFps;
    check(chast::parse_set_stream_options(
              payload_of(chast::make_set_stream_options(asked)), options, &error) &&
              options.max_fps == chast::kMaxStreamFps,
          "SetStreamOptions accepts the top of the frame-cap range");
    asked.max_fps = chast::kMaxStreamFps + 1;
    check(!chast::parse_set_stream_options(
              payload_of(chast::make_set_stream_options(asked)), options, &error),
          "SetStreamOptions rejects a frame cap past the range");

    std::vector<uint8_t> unknown_codec =
        payload_of(chast::make_set_stream_options(chast::StreamOptions{}));
    unknown_codec[0] = 3;
    check(!chast::parse_set_stream_options(unknown_codec, options, &error),
          "SetStreamOptions with an unknown codec is rejected");
    std::vector<uint8_t> unknown_quality =
        payload_of(chast::make_set_stream_options(chast::StreamOptions{}));
    unknown_quality[3] = 3;
    check(!chast::parse_set_stream_options(unknown_quality, options, &error),
          "SetStreamOptions with an unknown quality is rejected");
    std::vector<uint8_t> trailing =
        payload_of(chast::make_set_stream_options(chast::StreamOptions{}));
    trailing.push_back(0);
    check(!chast::parse_set_stream_options(trailing, options, &error),
          "SetStreamOptions with trailing bytes is rejected");
    check(!chast::parse_set_stream_options(std::vector<uint8_t>(3), options, &error),
          "short SetStreamOptions is rejected");

    chast::StreamOptionsResult result;
    chast::StreamOptionsResult sent;
    sent.code = chast::StreamOptionsCode::Ok;
    sent.codec = chast::VideoCodec::Hevc;
    sent.fps = 30;
    sent.quality = chast::StreamQuality::Balanced;
    check(chast::parse_stream_options_result(
              payload_of(chast::make_stream_options_result(sent)), result, &error) &&
              result.code == chast::StreamOptionsCode::Ok &&
              result.codec == chast::VideoCodec::Hevc && result.fps == 30 &&
              result.quality == chast::StreamQuality::Balanced && result.reason.empty(),
          "StreamOptionsResult round-trips an acceptance");

    sent.code = chast::StreamOptionsCode::Unsupported;
    sent.reason = "no HEVC encoder is available";
    check(chast::parse_stream_options_result(
              payload_of(chast::make_stream_options_result(sent)), result, &error) &&
              result.code == chast::StreamOptionsCode::Unsupported &&
              result.reason == "no HEVC encoder is available",
          "StreamOptionsResult round-trips a refusal with its reason");

    std::vector<uint8_t> unknown_result =
        payload_of(chast::make_stream_options_result(sent));
    unknown_result[0] = 4;
    check(!chast::parse_stream_options_result(unknown_result, result, &error),
          "StreamOptionsResult with an unknown code is rejected");
    std::vector<uint8_t> unknown_result_codec =
        payload_of(chast::make_stream_options_result(sent));
    unknown_result_codec[1] = 3;
    check(!chast::parse_stream_options_result(unknown_result_codec, result, &error),
          "StreamOptionsResult with an unknown codec is rejected");
    std::vector<uint8_t> unknown_result_quality =
        payload_of(chast::make_stream_options_result(sent));
    unknown_result_quality[4] = 3;
    check(!chast::parse_stream_options_result(unknown_result_quality, result, &error),
          "StreamOptionsResult with an unknown quality is rejected");
    std::vector<uint8_t> high_fps =
        payload_of(chast::make_stream_options_result(sent));
    high_fps[2] = static_cast<uint8_t>((chast::kMaxStreamFps + 1) & 0xFF);
    high_fps[3] = static_cast<uint8_t>((chast::kMaxStreamFps + 1) >> 8);
    check(!chast::parse_stream_options_result(high_fps, result, &error),
          "StreamOptionsResult with an out-of-range frame rate is rejected");

    sent.reason = std::string(400, 'x');
    check(chast::parse_stream_options_result(
              payload_of(chast::make_stream_options_result(sent)), result, &error) &&
              result.reason.size() == chast::kMaxStreamOptionReason,
          "StreamOptionsResult truncates an oversized reason to the protocol limit");

    std::vector<uint8_t> bad_utf8 =
        payload_of(chast::make_stream_options_result(chast::StreamOptionsResult{}));
    bad_utf8[5] = 1;
    bad_utf8[6] = 0;
    bad_utf8.push_back(0xFF);
    check(!chast::parse_stream_options_result(bad_utf8, result, &error),
          "StreamOptionsResult with a malformed reason is rejected");
    check(!chast::parse_stream_options_result(std::vector<uint8_t>(), result, &error),
          "empty StreamOptionsResult is rejected");
}

void test_privacy_mode() {
    std::string error;
    bool enabled = false;
    check(chast::parse_set_privacy_mode(
              payload_of(chast::make_set_privacy_mode(true)), enabled, &error) &&
              enabled,
          "SetPrivacyMode round-trips an enable request");
    check(chast::parse_set_privacy_mode(
              payload_of(chast::make_set_privacy_mode(false)), enabled, &error) &&
              !enabled,
          "SetPrivacyMode round-trips a disable request");
    std::vector<uint8_t> bad_flag = payload_of(chast::make_set_privacy_mode(true));
    bad_flag[0] = 2;
    check(!chast::parse_set_privacy_mode(bad_flag, enabled, &error),
          "SetPrivacyMode with an invalid flag is rejected");
    std::vector<uint8_t> trailing = payload_of(chast::make_set_privacy_mode(true));
    trailing.push_back(0);
    check(!chast::parse_set_privacy_mode(trailing, enabled, &error),
          "SetPrivacyMode with trailing bytes is rejected");
    check(!chast::parse_set_privacy_mode(std::vector<uint8_t>(), enabled, &error),
          "empty SetPrivacyMode is rejected");

    chast::PrivacyModeCode code = chast::PrivacyModeCode::Failed;
    std::string reason;
    check(chast::parse_privacy_mode_state(
              payload_of(chast::make_privacy_mode_state(true, chast::PrivacyModeCode::Ok, {})),
              enabled, code, reason, &error) &&
              enabled && code == chast::PrivacyModeCode::Ok && reason.empty(),
          "PrivacyModeState round-trips an enabled state");
    check(chast::parse_privacy_mode_state(
              payload_of(chast::make_privacy_mode_state(false, chast::PrivacyModeCode::Denied,
                                                         "permission revoked")),
              enabled, code, reason, &error) &&
              !enabled && code == chast::PrivacyModeCode::Denied &&
              reason == "permission revoked",
          "PrivacyModeState round-trips a refusal with its reason");
    std::vector<uint8_t> inconsistent =
        payload_of(chast::make_privacy_mode_state(false, chast::PrivacyModeCode::Failed, {}));
    inconsistent[0] = 1;
    check(!chast::parse_privacy_mode_state(inconsistent, enabled, code, reason, &error),
          "PrivacyModeState that reports a failure as enabled is rejected");
    std::vector<uint8_t> unknown_code =
        payload_of(chast::make_privacy_mode_state(false, chast::PrivacyModeCode::Denied, {}));
    unknown_code[1] = 4;
    check(!chast::parse_privacy_mode_state(unknown_code, enabled, code, reason, &error),
          "PrivacyModeState with an unknown code is rejected");
    const std::string long_reason(400, 'x');
    check(chast::parse_privacy_mode_state(
              payload_of(chast::make_privacy_mode_state(
                  false, chast::PrivacyModeCode::Failed, long_reason)),
              enabled, code, reason, &error) &&
              reason.size() == chast::kMaxPrivacyModeReason,
          "PrivacyModeState truncates an oversized reason to the protocol limit");
    std::vector<uint8_t> bad_utf8 =
        payload_of(chast::make_privacy_mode_state(false, chast::PrivacyModeCode::Failed, {}));
    bad_utf8[2] = 1;
    bad_utf8[3] = 0;
    bad_utf8.push_back(0xFF);
    check(!chast::parse_privacy_mode_state(bad_utf8, enabled, code, reason, &error),
          "PrivacyModeState with a malformed reason is rejected");

    chast::DesktopKind kind = chast::DesktopKind::Unavailable;
    check(chast::parse_desktop_state(payload_of(chast::make_desktop_state(
                                         chast::DesktopKind::Secure)),
                                     kind, &error) &&
              kind == chast::DesktopKind::Secure,
          "DesktopState round-trips a secure desktop");
    std::vector<uint8_t> unknown_kind =
        payload_of(chast::make_desktop_state(chast::DesktopKind::Default));
    unknown_kind[0] = 3;
    check(!chast::parse_desktop_state(unknown_kind, kind, &error),
          "DesktopState with an unknown kind is rejected");
    check(!chast::parse_desktop_state(std::vector<uint8_t>(2), kind, &error),
          "DesktopState with trailing bytes is rejected");
}

void test_display_switch() {
    std::string error;
    uint16_t display = chast::kDisplayAuto;
    check(chast::parse_switch_display(payload_of(chast::make_switch_display(3)), display,
                                      &error) &&
              display == 3,
          "SwitchDisplay round-trips an explicit index");
    check(chast::parse_switch_display(
              payload_of(chast::make_switch_display(chast::kDisplayAuto)), display, &error) &&
              display == chast::kDisplayAuto,
          "SwitchDisplay round-trips the host-default index");
    std::vector<uint8_t> short_switch = payload_of(chast::make_switch_display(1));
    short_switch.pop_back();
    check(!chast::parse_switch_display(short_switch, display, &error),
          "short SwitchDisplay is rejected");
    std::vector<uint8_t> trailing_switch = payload_of(chast::make_switch_display(1));
    trailing_switch.push_back(0);
    check(!chast::parse_switch_display(trailing_switch, display, &error),
          "SwitchDisplay with trailing bytes is rejected");

    uint16_t result_index = 0;
    chast::DisplaySwitchCode code = chast::DisplaySwitchCode::Busy;
    std::string reason;
    check(chast::parse_display_switch_result(
              payload_of(chast::make_display_switch_result(
                  2, chast::DisplaySwitchCode::Ok, std::string())),
              result_index, code, reason, &error) &&
              result_index == 2 && code == chast::DisplaySwitchCode::Ok && reason.empty(),
          "DisplaySwitchResult round-trips Ok without a reason");
    const std::string chinese = "该显示器已被拔出";
    check(chast::parse_display_switch_result(
              payload_of(chast::make_display_switch_result(
                  5, chast::DisplaySwitchCode::NotFound, chinese)),
              result_index, code, reason, &error) &&
              result_index == 5 && code == chast::DisplaySwitchCode::NotFound &&
              reason == chinese,
          "DisplaySwitchResult round-trips a UTF-8 reason");

    // A reason longer than the cap is cut between characters, never inside
    // one, so the result still parses.
    std::string long_reason(250, 'a');
    for (int i = 0; i < 10; ++i) {
        long_reason += chinese;
    }
    check(chast::parse_display_switch_result(
              payload_of(chast::make_display_switch_result(
                  0, chast::DisplaySwitchCode::Failed, long_reason)),
              result_index, code, reason, &error) &&
              reason.size() <= chast::kMaxDisplaySwitchReason && !reason.empty(),
          "an over-long result reason is truncated on a character boundary");

    std::vector<uint8_t> unknown_code = payload_of(chast::make_display_switch_result(
        0, chast::DisplaySwitchCode::Ok, std::string()));
    unknown_code[2] = 9;
    check(!chast::parse_display_switch_result(unknown_code, result_index, code, reason, &error),
          "DisplaySwitchResult with an unknown code is rejected");

    const uint16_t overlong_size =
        static_cast<uint16_t>(chast::kMaxDisplaySwitchReason + 1);
    std::vector<uint8_t> overlong(5, 0);
    overlong[3] = static_cast<uint8_t>(overlong_size & 0xFFu);
    overlong[4] = static_cast<uint8_t>(overlong_size >> 8);
    overlong.resize(5 + overlong_size, 'a');
    check(!chast::parse_display_switch_result(
              payload_of(chast::make_control(chast::ControlType::DisplaySwitchResult, overlong)),
              result_index, code, reason, &error),
          "DisplaySwitchResult with an over-long reason is rejected");

    std::vector<uint8_t> malformed(6, 0);
    malformed[3] = 1;
    malformed[5] = 0xFF;
    check(!chast::parse_display_switch_result(
              payload_of(chast::make_control(chast::ControlType::DisplaySwitchResult, malformed)),
              result_index, code, reason, &error),
          "DisplaySwitchResult with a malformed reason is rejected");

    std::vector<uint8_t> trailing_result = payload_of(chast::make_display_switch_result(
        0, chast::DisplaySwitchCode::Ok, std::string()));
    trailing_result.push_back(0);
    check(!chast::parse_display_switch_result(trailing_result, result_index, code, reason,
                                              &error),
          "DisplaySwitchResult with trailing bytes is rejected");
}

void test_display_modes() {
    std::string error;
    uint16_t display = chast::kDisplayAuto;
    check(chast::parse_list_display_modes(payload_of(chast::make_list_display_modes(2)), display,
                                          &error) &&
              display == 2,
          "ListDisplayModes round-trips the display index");
    std::vector<uint8_t> short_list = payload_of(chast::make_list_display_modes(1));
    short_list.pop_back();
    check(!chast::parse_list_display_modes(short_list, display, &error),
          "short ListDisplayModes is rejected");
    std::vector<uint8_t> trailing_list = payload_of(chast::make_list_display_modes(1));
    trailing_list.push_back(0);
    check(!chast::parse_list_display_modes(trailing_list, display, &error),
          "ListDisplayModes with trailing bytes is rejected");

    chast::DisplayModeList sent;
    sent.display = 4;
    sent.code = chast::DisplayModeCode::Ok;
    sent.modes = {
        chast::DisplayModeInfo{2560, 1440, 165, false},
        chast::DisplayModeInfo{1920, 1080, 60, true},
        chast::DisplayModeInfo{1280, 720, 60, false},
    };
    chast::DisplayModeList got;
    check(chast::parse_display_modes(payload_of(chast::make_display_modes(sent)), got, &error) &&
              got.display == 4 && got.code == chast::DisplayModeCode::Ok && got.reason.empty() &&
              got.modes.size() == 3 && got.modes[0].width == 2560 &&
              got.modes[0].refresh_hz == 165 && got.modes[1].current && !got.modes[2].current,
          "DisplayModes round-trips a list and the current marker");

    chast::DisplayModeList refused;
    refused.display = 1;
    refused.code = chast::DisplayModeCode::NotFound;
    refused.reason = "该输出不支持此模式";
    check(chast::parse_display_modes(payload_of(chast::make_display_modes(refused)), got,
                                     &error) &&
              got.code == chast::DisplayModeCode::NotFound && got.modes.empty() &&
              got.reason == refused.reason,
          "DisplayModes round-trips a refusal with a UTF-8 reason");
    refused.modes = {chast::DisplayModeInfo{1920, 1080, 60, true}};
    check(chast::parse_display_modes(payload_of(chast::make_display_modes(refused)), got,
                                     &error) &&
              got.modes.empty(),
          "the DisplayModes maker drops modes from a refusal");

    std::string long_reason(250, 'a');
    for (int i = 0; i < 10; ++i) {
        long_reason += "显示器";
    }
    refused.reason = long_reason;
    check(chast::parse_display_modes(payload_of(chast::make_display_modes(refused)), got,
                                     &error) &&
              got.reason.size() <= chast::kMaxDisplayModeReason && !got.reason.empty(),
          "an over-long DisplayModes reason is truncated on a character boundary");

    // A maker that is handed more modes than the wire allows stops at the cap.
    chast::DisplayModeList many;
    many.display = 0;
    for (uint16_t i = 0; i < chast::kMaxDisplayModes + 3; ++i) {
        many.modes.push_back(chast::DisplayModeInfo{
            static_cast<uint16_t>(320 + i), 240, 60, false});
    }
    check(chast::parse_display_modes(payload_of(chast::make_display_modes(many)), got, &error) &&
              got.modes.size() == chast::kMaxDisplayModes,
          "DisplayModes caps the list at kMaxDisplayModes");

    std::vector<uint8_t> unknown_code = payload_of(chast::make_display_modes(sent));
    unknown_code[2] = 9;
    check(!chast::parse_display_modes(unknown_code, got, &error),
          "DisplayModes with an unknown code is rejected");
    std::vector<uint8_t> bad_flags = payload_of(chast::make_display_modes(sent));
    bad_flags[13] |= 0x02;  // first entry's flags byte
    check(!chast::parse_display_modes(bad_flags, got, &error),
          "DisplayModes with unknown entry flags is rejected");
    std::vector<uint8_t> empty_mode = payload_of(chast::make_display_modes(sent));
    empty_mode[7] = 0;
    empty_mode[8] = 0;
    check(!chast::parse_display_modes(empty_mode, got, &error),
          "DisplayModes with a zero-sized mode is rejected");
    chast::DisplayModeList two_current = sent;
    two_current.modes[0].current = true;
    check(!chast::parse_display_modes(payload_of(chast::make_display_modes(two_current)), got,
                                      &error),
          "DisplayModes with two current modes is rejected");

    const uint16_t overlong_size = static_cast<uint16_t>(chast::kMaxDisplayModeReason + 1);
    std::vector<uint8_t> overlong(5, 0);
    overlong[3] = static_cast<uint8_t>(overlong_size & 0xFFu);
    overlong[4] = static_cast<uint8_t>(overlong_size >> 8);
    overlong.resize(5 + overlong_size, 'a');
    overlong.push_back(0);
    overlong.push_back(0);
    check(!chast::parse_display_modes(
              payload_of(chast::make_control(chast::ControlType::DisplayModes, overlong)), got,
              &error),
          "DisplayModes with an over-long reason is rejected");
    std::vector<uint8_t> malformed_reason(7, 0);
    malformed_reason[3] = 1;
    malformed_reason[5] = 0xFF;
    check(!chast::parse_display_modes(
              payload_of(chast::make_control(chast::ControlType::DisplayModes,
                                             malformed_reason)),
              got, &error),
          "DisplayModes with a malformed reason is rejected");
    std::vector<uint8_t> too_many(7, 0);
    too_many[5] = static_cast<uint8_t>((chast::kMaxDisplayModes + 1) & 0xFFu);
    too_many[6] = static_cast<uint8_t>((chast::kMaxDisplayModes + 1) >> 8);
    check(!chast::parse_display_modes(
              payload_of(chast::make_control(chast::ControlType::DisplayModes, too_many)), got,
              &error),
          "DisplayModes over the mode cap is rejected");
    std::vector<uint8_t> refused_with_modes = payload_of(chast::make_display_modes(sent));
    refused_with_modes[2] = 1;
    check(!chast::parse_display_modes(refused_with_modes, got, &error),
          "a refused DisplayModes that lists modes is rejected");
    std::vector<uint8_t> truncated_entry = payload_of(chast::make_display_modes(sent));
    truncated_entry.pop_back();
    check(!chast::parse_display_modes(truncated_entry, got, &error),
          "a truncated DisplayModes entry is rejected");
    std::vector<uint8_t> trailing_modes = payload_of(chast::make_display_modes(sent));
    trailing_modes.push_back(0);
    check(!chast::parse_display_modes(trailing_modes, got, &error),
          "DisplayModes with trailing bytes is rejected");

    uint16_t set_display = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t refresh = 0;
    check(chast::parse_set_display_mode(
              payload_of(chast::make_set_display_mode(1, 1920, 1080, 60)), set_display, width,
              height, refresh, &error) &&
              set_display == 1 && width == 1920 && height == 1080 && refresh == 60,
          "SetDisplayMode round-trips a mode");
    std::vector<uint8_t> short_set = payload_of(chast::make_set_display_mode(1, 1920, 1080, 60));
    short_set.pop_back();
    check(!chast::parse_set_display_mode(short_set, set_display, width, height, refresh, &error),
          "short SetDisplayMode is rejected");
    std::vector<uint8_t> empty_set = payload_of(chast::make_set_display_mode(1, 0, 1080, 60));
    check(!chast::parse_set_display_mode(empty_set, set_display, width, height, refresh, &error),
          "SetDisplayMode with an empty picture is rejected");
    std::vector<uint8_t> trailing_set =
        payload_of(chast::make_set_display_mode(1, 1920, 1080, 60));
    trailing_set.push_back(0);
    check(!chast::parse_set_display_mode(trailing_set, set_display, width, height, refresh,
                                         &error),
          "SetDisplayMode with trailing bytes is rejected");

    chast::DisplayModeCode code = chast::DisplayModeCode::Busy;
    std::string reason;
    check(chast::parse_display_mode_result(
              payload_of(chast::make_display_mode_result(
                  3, 2560, 1440, 144, chast::DisplayModeCode::Ok, std::string())),
              set_display, width, height, refresh, code, reason, &error) &&
              set_display == 3 && width == 2560 && height == 1440 && refresh == 144 &&
              code == chast::DisplayModeCode::Ok && reason.empty(),
          "DisplayModeResult round-trips an accepted mode");
    check(chast::parse_display_mode_result(
              payload_of(chast::make_display_mode_result(
                  0, 1920, 1080, 60, chast::DisplayModeCode::Failed, "the driver refused it")),
              set_display, width, height, refresh, code, reason, &error) &&
              code == chast::DisplayModeCode::Failed && reason == "the driver refused it",
          "DisplayModeResult round-trips a failure reason");
    long_reason = std::string(250, 'a') + "分辨率切换失败";
    check(chast::parse_display_mode_result(
              payload_of(chast::make_display_mode_result(
                  0, 1920, 1080, 60, chast::DisplayModeCode::Failed, long_reason)),
              set_display, width, height, refresh, code, reason, &error) &&
              reason.size() <= chast::kMaxDisplayModeReason && !reason.empty(),
          "an over-long DisplayModeResult reason is truncated on a character boundary");
    std::vector<uint8_t> unknown_result = payload_of(chast::make_display_mode_result(
        0, 1920, 1080, 60, chast::DisplayModeCode::Ok, std::string()));
    unknown_result[8] = 9;
    check(!chast::parse_display_mode_result(unknown_result, set_display, width, height, refresh,
                                            code, reason, &error),
          "DisplayModeResult with an unknown code is rejected");
    std::vector<uint8_t> trailing_result = payload_of(chast::make_display_mode_result(
        0, 1920, 1080, 60, chast::DisplayModeCode::Ok, std::string()));
    trailing_result.push_back(0);
    check(!chast::parse_display_mode_result(trailing_result, set_display, width, height, refresh,
                                            code, reason, &error),
          "DisplayModeResult with trailing bytes is rejected");
}

void test_frame_header() {
    chast::FrameHeader sent;
    sent.seq = 42;
    sent.pts_ms = 1234;
    sent.payload_size = 1000;
    sent.payload_crc32 = 0xA5A5F00D;
    sent.width = 1920;
    sent.height = 1080;
    sent.codec = chast::VideoCodec::Hevc;
    sent.keyframe = true;
    sent.idr = true;
    sent.capture_ms = 0x0000ABCDEF123456ull;

    uint8_t bytes[chast::kFrameHeaderSize]{};
    chast::encode_frame_header(sent, bytes);
    chast::FrameHeader got;
    std::string error;
    const bool decoded =
        chast::decode_frame_header(bytes, sizeof(bytes), got, &error);
    check(decoded, "FrameHeader round-trips");
    if (decoded) {
        check(got.seq == sent.seq && got.pts_ms == sent.pts_ms &&
                  got.payload_size == sent.payload_size &&
                  got.payload_crc32 == sent.payload_crc32,
              "FrameHeader keeps its counters");
        check(got.width == sent.width && got.height == sent.height &&
                  got.codec == sent.codec && got.keyframe && got.idr,
              "FrameHeader keeps the picture description");
        check(got.capture_ms == sent.capture_ms,
              "FrameHeader round-trips the 48-bit capture time");
    }
    check(sizeof(bytes) == 48, "FrameHeader is still 48 bytes");

    sent.capture_ms = 0xFFFFFFFFFFFFull;
    chast::encode_frame_header(sent, bytes);
    check(chast::decode_frame_header(bytes, sizeof(bytes), got, &error) &&
              got.capture_ms == 0xFFFFFFFFFFFFull,
          "FrameHeader carries the largest 48-bit capture time");

    std::vector<uint8_t> old_version(bytes, bytes + sizeof(bytes));
    old_version[4] = static_cast<uint8_t>(chast::kProtocolVersion - 1);
    old_version[5] = 0;
    check(!chast::decode_frame_header(old_version.data(), old_version.size(), got, &error),
          "FrameHeader from another protocol version is rejected");
    check(!chast::decode_frame_header(bytes, chast::kFrameHeaderSize - 1, got, &error),
          "a short FrameHeader is rejected");
}

void test_audio_header() {
    chast::AudioHeader sent;
    sent.seq = 0x1122334455667788ull;
    sent.pts_ms = 987654;
    sent.capture_ms = 0x0000123456789ABCull;
    sent.payload_size = 411;
    sent.payload_crc32 = 0xBEEF1234;
    sent.sample_rate = 48000;
    sent.frame_samples = chast::kAudioFrameSamples;
    sent.channels = 2;
    sent.codec = chast::AudioCodec::AAC;

    uint8_t bytes[chast::kAudioHeaderSize]{};
    chast::encode_audio_header(sent, bytes);
    check(sizeof(bytes) == 48, "AudioHeader is 48 bytes");
    check(std::memcmp(bytes, "CAUD", 4) == 0, "AudioHeader starts with the audio magic");
    chast::AudioHeader got;
    std::string error;
    const bool decoded = chast::decode_audio_header(bytes, sizeof(bytes), got, &error);
    check(decoded, "AudioHeader round-trips");
    if (decoded) {
        check(got.seq == sent.seq && got.pts_ms == sent.pts_ms &&
                  got.capture_ms == sent.capture_ms,
              "AudioHeader keeps its timestamps");
        check(got.payload_size == sent.payload_size &&
                  got.payload_crc32 == sent.payload_crc32,
              "AudioHeader keeps its payload description");
        check(got.sample_rate == sent.sample_rate && got.frame_samples == sent.frame_samples &&
                  got.channels == sent.channels && got.codec == sent.codec,
              "AudioHeader keeps the audio format");
    }

    sent.capture_ms = 0xFFFFFFFFFFFFFFFFull;
    chast::encode_audio_header(sent, bytes);
    check(chast::decode_audio_header(bytes, sizeof(bytes), got, &error) &&
              got.capture_ms == 0xFFFFFFFFFFFFull,
          "AudioHeader masks the capture time to 48 bits");
    sent.capture_ms = 0x0000123456789ABCull;
    chast::encode_audio_header(sent, bytes);

    auto set_u32 = [](std::vector<uint8_t>& header, size_t at, uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            header[at + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    };
    auto rejects_header = [&](std::vector<uint8_t> header, const char* what) {
        chast::AudioHeader dropped;
        check(!chast::decode_audio_header(header.data(), header.size(), dropped, &error), what);
    };
    const std::vector<uint8_t> wire(bytes, bytes + sizeof(bytes));
    std::vector<uint8_t> bad_magic = wire;
    bad_magic[0] = 'X';
    rejects_header(std::move(bad_magic), "an AudioHeader with bad magic is rejected");
    std::vector<uint8_t> old_version = wire;
    old_version[4] = static_cast<uint8_t>(chast::kProtocolVersion - 1);
    old_version[5] = 0;
    rejects_header(std::move(old_version),
                   "an AudioHeader from another protocol version is rejected");
    std::vector<uint8_t> bad_flags = wire;
    bad_flags[6] = 1;
    rejects_header(std::move(bad_flags), "an AudioHeader with unknown flags is rejected");
    std::vector<uint8_t> no_payload = wire;
    set_u32(no_payload, 32, 0);
    rejects_header(std::move(no_payload), "an AudioHeader without a payload is rejected");
    std::vector<uint8_t> huge_payload = wire;
    set_u32(huge_payload, 32, chast::kMaxAudioPayload + 1);
    rejects_header(std::move(huge_payload), "an oversized audio payload is rejected");
    std::vector<uint8_t> bad_rate = wire;
    set_u32(bad_rate, 40, 4000);
    rejects_header(std::move(bad_rate), "an AudioHeader with an unsupported rate is rejected");
    std::vector<uint8_t> bad_frame = wire;
    bad_frame[44] = 0;
    bad_frame[45] = 0;
    rejects_header(std::move(bad_frame), "an AudioHeader with an empty frame is rejected");
    std::vector<uint8_t> bad_channels = wire;
    bad_channels[46] = 3;
    rejects_header(std::move(bad_channels),
                   "an AudioHeader with an unsupported channel count is rejected");
    check(!chast::decode_audio_header(bytes, chast::kAudioHeaderSize - 1, got, &error),
          "a short AudioHeader is rejected");
}

void test_microphone_stream_header() {
    chast::AudioInfo sent;
    sent.enabled = true;
    sent.codec = chast::AudioCodec::Opus;
    sent.sample_rate = 48000;
    sent.channels = 1;
    sent.frame_samples = chast::kOpusFrameSamples;
    sent.extradata.assign(19, 0);
    std::memcpy(sent.extradata.data(), "OpusHead", 8);
    sent.extradata[8] = 1;
    sent.extradata[9] = 1;
    sent.extradata[10] = 0x38;
    sent.extradata[11] = 0x01;
    sent.extradata[14] = 0x80;
    sent.extradata[15] = 0xBB;

    std::string error;
    const std::vector<uint8_t> wire = chast::make_microphone_stream_header(sent);
    check(wire.size() == chast::kMicrophoneStreamHeaderSize + sent.extradata.size(),
          "the microphone stream header is 20 bytes plus its config");
    check(wire.size() >= 4 && std::memcmp(wire.data(), "CMIC", 4) == 0,
          "the microphone stream header starts with the CMIC magic");
    check(chast::microphone_stream_header_length(wire.data(), wire.size()) == wire.size(),
          "the microphone stream length names the full header");
    check(chast::microphone_stream_header_length(wire.data(),
                                                 chast::kMicrophoneStreamHeaderSize) ==
              wire.size(),
          "the microphone stream length is known before the config arrives");
    check(chast::microphone_stream_header_length(
              wire.data(), chast::kMicrophoneStreamHeaderSize - 1) == 0,
          "the microphone stream length waits for the fixed header");
    std::vector<uint8_t> zero_config = wire;
    std::fill(zero_config.begin() + 16, zero_config.begin() + 18, 0);
    check(chast::microphone_stream_header_length(zero_config.data(), zero_config.size()) == 0,
          "the microphone stream length rejects an empty config");
    std::vector<uint8_t> huge_config = wire;
    huge_config[16] = 0xFF;
    huge_config[17] = 0x00;
    check(chast::microphone_stream_header_length(huge_config.data(), huge_config.size()) == 0,
          "the microphone stream length rejects an oversized config");
    std::vector<uint8_t> no_magic = wire;
    no_magic[0] = 'X';
    check(chast::microphone_stream_header_length(no_magic.data(), no_magic.size()) == 0,
          "the microphone stream length rejects another magic");
    check(chast::make_microphone_stream_header(chast::AudioInfo{}).empty(),
          "the encoder refuses a microphone config that is not Opus at 48 kHz");
    chast::AudioInfo wrong_rate = sent;
    wrong_rate.sample_rate = 44100;
    check(chast::make_microphone_stream_header(wrong_rate).empty(),
          "the encoder refuses a microphone config at another sample rate");

    chast::AudioInfo got;
    size_t consumed = 0;
    const bool parsed = chast::decode_microphone_stream_header(
        wire.data(), wire.size(), got, consumed, &error);
    check(parsed, "the microphone stream header round-trips");
    if (parsed) {
        check(consumed == wire.size() && got.codec == chast::AudioCodec::Opus &&
                  got.sample_rate == sent.sample_rate && got.channels == sent.channels &&
                  got.frame_samples == sent.frame_samples &&
                  got.extradata == sent.extradata,
              "the microphone stream header keeps the encoder description");
    }
    std::vector<uint8_t> with_packet = wire;
    with_packet.push_back(0xAA);
    check(chast::decode_microphone_stream_header(with_packet.data(), with_packet.size(), got,
                                                 consumed, &error) &&
              consumed == wire.size(),
          "the microphone stream header stops before the first audio packet");

    auto set_u16 = [](std::vector<uint8_t>& header, size_t at, uint16_t value) {
        header[at] = static_cast<uint8_t>(value);
        header[at + 1] = static_cast<uint8_t>(value >> 8);
    };
    auto set_u32 = [](std::vector<uint8_t>& header, size_t at, uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            header[at + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    };
    auto rejects_header = [&](std::vector<uint8_t> header, const char* what) {
        chast::AudioInfo dropped;
        size_t dropped_consumed = 0;
        check(!chast::decode_microphone_stream_header(header.data(), header.size(), dropped,
                                                      dropped_consumed, &error),
              what);
    };
    rejects_header(std::vector<uint8_t>(wire.begin(), wire.begin() + 19),
                   "a truncated microphone stream header is rejected");
    std::vector<uint8_t> bad_magic = wire;
    bad_magic[0] = 'X';
    rejects_header(std::move(bad_magic), "a microphone stream header with bad magic is rejected");
    std::vector<uint8_t> old_version = wire;
    old_version[4] = chast::kMicrophoneStreamVersion + 1;
    rejects_header(std::move(old_version),
                   "a microphone stream header from another version is rejected");
    std::vector<uint8_t> aac_codec = wire;
    aac_codec[5] = 1;
    rejects_header(std::move(aac_codec), "a microphone stream using AAC is rejected");
    std::vector<uint8_t> unknown_codec = wire;
    unknown_codec[5] = 3;
    rejects_header(std::move(unknown_codec), "a microphone stream with an unknown codec is rejected");
    std::vector<uint8_t> reserved = wire;
    reserved[6] = 1;
    rejects_header(std::move(reserved),
                   "a microphone stream header with a non-zero reserved field is rejected");
    std::vector<uint8_t> tail_reserved = wire;
    tail_reserved[18] = 1;
    rejects_header(std::move(tail_reserved),
                   "a microphone stream header with a non-zero trailing reserved field is rejected");
    std::vector<uint8_t> bad_rate = wire;
    set_u32(bad_rate, 8, 44100);
    rejects_header(std::move(bad_rate), "a microphone stream at another sample rate is rejected");
    std::vector<uint8_t> bad_channels = wire;
    set_u16(bad_channels, 12, 3);
    rejects_header(std::move(bad_channels),
                   "a microphone stream with an unsupported channel count is rejected");
    std::vector<uint8_t> bad_frame = wire;
    set_u16(bad_frame, 14, chast::kAudioFrameSamples);
    rejects_header(std::move(bad_frame),
                   "a microphone stream with an unsupported frame length is rejected");
    std::vector<uint8_t> no_config = wire;
    set_u16(no_config, 16, 0);
    rejects_header(std::move(no_config), "a microphone stream without its config is rejected");
    std::vector<uint8_t> long_config = wire;
    set_u16(long_config, 16, static_cast<uint16_t>(chast::kMaxAudioExtradata + 1));
    rejects_header(std::move(long_config), "an oversized microphone config is rejected");
    rejects_header(std::vector<uint8_t>(wire.begin(), wire.begin() + 25),
                   "a microphone stream whose config is cut short is rejected");
}

void test_audio_parser() {
    chast::AudioHeader sent;
    sent.seq = 9;
    sent.pts_ms = 160;
    sent.capture_ms = 0x1234;
    sent.sample_rate = 48000;
    sent.frame_samples = chast::kAudioFrameSamples;
    sent.channels = 2;
    const std::vector<uint8_t> body = {0xDE, 0xAD, 0xBE, 0xEF};
    sent.payload_size = static_cast<uint32_t>(body.size());
    sent.payload_crc32 = chast::crc32(body.data(), body.size());
    std::vector<uint8_t> packet(chast::kAudioHeaderSize + body.size());
    chast::encode_audio_header(sent, packet.data());
    std::memcpy(packet.data() + chast::kAudioHeaderSize, body.data(), body.size());

    chast::AudioParser parser;
    std::string error;
    chast::AudioHeader got;
    const uint8_t* payload = nullptr;
    bool split_ok = true;
    // Feed all but the last header byte one at a time.
    for (size_t i = 0; i + 1 < chast::kAudioHeaderSize && split_ok; ++i) {
        split_ok = parser.push(packet.data() + i, 1, &error) &&
                   parser.next(got, &payload, &error) ==
                       chast::AudioParser::Status::Incomplete;
    }
    check(split_ok, "a byte-at-a-time audio header waits for the rest");
    check(parser.push(packet.data() + chast::kAudioHeaderSize - 1,
                      packet.size() - (chast::kAudioHeaderSize - 1), &error),
          "the rest of a split audio packet is accepted");
    check(parser.next(got, &payload, &error) == chast::AudioParser::Status::Packet &&
              got.seq == sent.seq && got.payload_size == body.size() &&
              std::memcmp(payload, body.data(), body.size()) == 0,
          "a split audio packet is reassembled intact");
    parser.consume();
    check(parser.buffered() == 0, "consuming a packet empties the audio parser");

    chast::AudioParser pair;
    std::vector<uint8_t> two = packet;
    two.insert(two.end(), packet.begin(), packet.end());
    check(pair.push(two.data(), two.size(), &error), "two audio packets in one chunk are accepted");
    int packets = 0;
    bool payloads_match = true;
    while (pair.next(got, &payload, &error) == chast::AudioParser::Status::Packet) {
        ++packets;
        payloads_match = payloads_match &&
                         std::memcmp(payload, body.data(), body.size()) == 0;
        pair.consume();
    }
    check(packets == 2 && payloads_match, "both audio packets are yielded in order");

    chast::AudioParser corrupt;
    std::vector<uint8_t> damaged = packet;
    damaged[0] = 'X';
    check(corrupt.push(damaged.data(), damaged.size(), &error),
          "a damaged audio packet reaches the parser");
    check(corrupt.next(got, &payload, &error) == chast::AudioParser::Status::Error,
          "a damaged audio packet is reported as an error");

    chast::AudioParser flooded;
    std::vector<uint8_t> junk(4u * 1024u * 1024u + 1, 0);
    check(!flooded.push(junk.data(), junk.size(), &error),
          "an audio stream buffer past the limit is rejected");
}

void test_latency_probe() {
    std::string error;
    uint64_t client_us = 0;
    check(chast::parse_latency_probe(
              payload_of(chast::make_latency_probe(0x1122334455667788ull)), client_us,
              &error) &&
              client_us == 0x1122334455667788ull,
          "LatencyProbe round-trips its client timestamp");

    check(!chast::parse_latency_probe(std::vector<uint8_t>(7), client_us, &error),
          "a short LatencyProbe is rejected");
    std::vector<uint8_t> trailing =
        payload_of(chast::make_latency_probe(1));
    trailing.push_back(0);
    check(!chast::parse_latency_probe(trailing, client_us, &error),
          "LatencyProbe with trailing bytes is rejected");

    uint64_t host_recv_us = 0;
    uint64_t host_send_us = 0;
    const std::vector<uint8_t> reply =
        chast::make_latency_reply(10, 20, 25);
    check(chast::parse_latency_reply(payload_of(reply), client_us, host_recv_us,
                                     host_send_us, &error) &&
              client_us == 10 && host_recv_us == 20 && host_send_us == 25,
          "LatencyReply round-trips all three timestamps");
    check(reply.size() == chast::kControlHeaderSize + 24 &&
              reply[6] == static_cast<uint8_t>(chast::ControlType::LatencyReply),
          "LatencyReply names its control type and size");
    check(!chast::parse_latency_reply(payload_of(chast::make_latency_reply(10, 25, 20)),
                                      client_us, host_recv_us, host_send_us, &error),
          "LatencyReply that sends before receiving is rejected");
}

void test_clipboard() {
    std::string error;
    const std::string text = "hello 剪贴板 😀";
    const std::vector<uint8_t> message = chast::make_clipboard_update(0x1122334455667788ull, text);
    check(message.size() == chast::kControlHeaderSize + 8 + text.size(),
          "ClipboardUpdate carries the sequence and the text");
    check(message.size() > 7 &&
              message[6] == static_cast<uint8_t>(chast::ControlType::ClipboardUpdate),
          "ClipboardUpdate names its control type");
    chast::ClipboardUpdate got;
    check(chast::parse_clipboard_update(payload_of(message), got, &error) &&
              got.seq == 0x1122334455667788ull && got.text == text,
          "ClipboardUpdate round-trips UTF-8 text");

    const std::string largest(chast::kMaxClipboardBytes, 'a');
    check(chast::valid_clipboard_text(largest), "the clipboard length limit is accepted");
    check(chast::parse_clipboard_update(
              payload_of(chast::make_clipboard_update(7, largest)), got, &error) &&
              got.seq == 7 && got.text == largest,
          "a clipboard text at the length limit round-trips");
    check(chast::make_clipboard_update(1, largest + "a").empty(),
          "a text past the length limit is not encoded");

    check(!chast::valid_clipboard_text(""), "an empty clipboard text is rejected");
    check(!chast::valid_clipboard_text(std::string("a\0b", 3)),
          "a clipboard text with NUL is rejected");
    check(!chast::valid_clipboard_text("\x80"), "a lone continuation byte is rejected");
    check(!chast::valid_clipboard_text("\xC0\x80"), "an overlong NUL is rejected");
    check(!chast::valid_clipboard_text("\xE0\x80\x80"), "an overlong three-byte form is rejected");
    check(!chast::valid_clipboard_text("\xED\xA0\x80"), "a UTF-16 surrogate is rejected");
    check(!chast::valid_clipboard_text("\xF4\x90\x80\x80"), "a code point past U+10FFFF is rejected");
    check(!chast::valid_clipboard_text("\xE4\xB8"), "a truncated sequence is rejected");

    check(!chast::parse_clipboard_update(std::vector<uint8_t>(8), got, &error),
          "a ClipboardUpdate without text is rejected");
    std::vector<uint8_t> too_long(8 + chast::kMaxClipboardBytes + 1, 'a');
    check(!chast::parse_clipboard_update(too_long, got, &error),
          "an oversized ClipboardUpdate is rejected");
    std::vector<uint8_t> invalid_utf8{0, 0, 0, 0, 0, 0, 0, 0, 0xED, 0xA0, 0x80};
    check(!chast::parse_clipboard_update(invalid_utf8, got, &error),
          "a ClipboardUpdate with invalid UTF-8 is rejected");
    std::vector<uint8_t> short_message(7, 0);
    check(!chast::parse_clipboard_update(short_message, got, &error),
          "a truncated ClipboardUpdate is rejected");
}

void test_chat() {
    std::string error;
    const std::string text = "请在主机上确认订单 😀\n第二行";
    const std::vector<uint8_t> message = chast::make_chat_message(text);
    check(message.size() == chast::kControlHeaderSize + text.size(),
          "ChatMessage carries the UTF-8 text as its payload");
    check(message.size() > 7 &&
              message[6] == static_cast<uint8_t>(chast::ControlType::ChatMessage),
          "ChatMessage names its control type");
    std::string got;
    check(chast::parse_chat_message(payload_of(message), got, &error) && got == text,
          "ChatMessage round-trips UTF-8 text and newlines");

    const std::string largest(chast::kMaxChatBytes, 'a');
    check(chast::valid_chat_text(largest), "the chat length limit is accepted");
    check(chast::parse_chat_message(payload_of(chast::make_chat_message(largest)), got,
                                    &error) &&
              got == largest,
          "a chat message at the length limit round-trips");
    check(chast::make_chat_message(largest + "a").empty(),
          "a chat message past the length limit is not encoded");
    check(!chast::valid_chat_text(""), "an empty chat message is rejected");
    check(!chast::valid_chat_text(std::string("a\0b", 3)),
          "a chat message with NUL is rejected");
    check(!chast::valid_chat_text("\xED\xA0\x80"), "a chat surrogate is rejected");

    check(!chast::parse_chat_message({}, got, &error),
          "a ChatMessage without text is rejected");
    check(!chast::parse_chat_message(std::vector<uint8_t>(chast::kMaxChatBytes + 1, 'a'),
                                     got, &error),
          "an oversized ChatMessage is rejected");
    check(!chast::parse_chat_message({0xED, 0xA0, 0x80}, got, &error),
          "a ChatMessage with invalid UTF-8 is rejected");
}

void test_cursor_update() {
    std::string error;
    chast::CursorUpdate update;
    update.visible = true;
    update.x = 1280;
    update.y = -40;
    update.hotspot_x = 3;
    update.hotspot_y = 5;
    update.width = 8;
    update.height = 6;
    update.shape_id = 0x1F2E3D4C5B6A7980ull & ((1ull << 53) - 1);
    update.sprite.assign(static_cast<size_t>(update.width) * update.height * 4u, 0x40);
    const std::vector<uint8_t> message = chast::make_cursor_update(update);
    check(message.size() == chast::kControlHeaderSize + 26 + update.sprite.size(),
          "CursorUpdate carries a fixed header and the sprite");
    check(message.size() > 7 &&
              message[6] == static_cast<uint8_t>(chast::ControlType::CursorUpdate),
          "CursorUpdate names its control type");
    chast::CursorUpdate got;
    check(chast::parse_cursor_update(payload_of(message), got, &error) &&
              got.visible && got.x == 1280 && got.y == -40 && got.hotspot_x == 3 &&
              got.hotspot_y == 5 && got.shape_id == update.shape_id &&
              got.width == 8 && got.height == 6 && got.sprite == update.sprite,
          "CursorUpdate round-trips the sprite and the position");

    chast::CursorUpdate moved = update;
    moved.sprite.clear();
    moved.x = 4;
    const std::vector<uint8_t> move_message = chast::make_cursor_update(moved);
    check(move_message.size() == chast::kControlHeaderSize + 26,
          "a position-only CursorUpdate carries no image");
    check(chast::parse_cursor_update(payload_of(move_message), got, &error) && got.sprite.empty() &&
              got.x == 4 && got.width == 8,
          "a position-only CursorUpdate round-trips the shape id and size");

    chast::CursorUpdate hidden;
    check(chast::parse_cursor_update(
              payload_of(chast::make_cursor_update(hidden)), got, &error) &&
              !got.visible && got.shape_id == 0 && got.width == 0,
          "a hidden CursorUpdate round-trips");

    chast::CursorUpdate bad = update;
    bad.sprite.pop_back();
    check(chast::make_cursor_update(bad).empty(),
          "a sprite that does not match its size is not encoded");
    bad = update;
    bad.shape_id = 0;
    check(chast::make_cursor_update(bad).empty(), "a sprite without an id is not encoded");
    bad = update;
    bad.width = chast::kMaxCursorSize + 1;
    check(chast::make_cursor_update(bad).empty(), "an oversized sprite is not encoded");

    std::vector<uint8_t> payload;
    payload.resize(12);
    check(!chast::parse_cursor_update(payload, got, &error),
          "a truncated CursorUpdate is rejected");
    payload = payload_of(chast::make_cursor_update(update));
    std::vector<uint8_t> trailing = payload;
    trailing.push_back(0);
    check(!chast::parse_cursor_update(trailing, got, &error),
          "a CursorUpdate with trailing bytes is rejected");
    payload[1] = 2;
    check(!chast::parse_cursor_update(payload, got, &error),
          "a CursorUpdate with unknown flags is rejected");
    payload[1] = 0;
    payload[10] = 9;  // hotspot_x past an 8-pixel-wide sprite
    check(!chast::parse_cursor_update(payload, got, &error),
          "a hotspot outside the sprite is rejected");
}

void test_stream_info() {
    chast::ServerInfo sent;
    sent.codec = chast::VideoCodec::H264;
    sent.width = 1280;
    sent.height = 720;
    sent.source_width = 2560;
    sent.source_height = 1440;
    sent.framerate = 30;
    sent.bitrate = 4000000;
    sent.origin_x = 1920;
    sent.encoder = "mfxhw64 (VDENC AVC)";
    chast::DisplayDescription display;
    display.name = "\\\\.\\DISPLAY2";
    display.left = 1920;
    display.right = 3200;
    display.bottom = 720;
    sent.displays = {display};
    sent.audio.enabled = true;
    sent.audio.bitrate = 160000;
    sent.audio.extradata = {0x11, 0x90};

    const std::vector<uint8_t> hello_ack = chast::make_hello_ack(sent);
    const std::vector<uint8_t> stream_info = chast::make_stream_info(sent);
    check(payload_of(hello_ack) == payload_of(stream_info),
          "StreamInfo carries the HelloAck payload");
    check(stream_info.size() >= chast::kControlHeaderSize &&
              stream_info[6] == static_cast<uint8_t>(chast::ControlType::StreamInfo),
          "StreamInfo names its control type");

    chast::ServerInfo got;
    std::string error;
    const bool parsed = chast::parse_hello_ack(payload_of(stream_info), got, &error);
    check(parsed, "StreamInfo parses with the HelloAck parser");
    if (parsed) {
        check(got.width == sent.width && got.height == sent.height && got.codec == sent.codec,
              "StreamInfo carries the new stream parameters");
        check(got.source_width == 2560 && got.source_height == 1440,
              "StreamInfo keeps the source size while the encoded picture is scaled");
        check(got.origin_x == sent.origin_x && got.displays.size() == 1 &&
                  got.displays[0].left == 1920,
              "StreamInfo carries the new origin and display list");
        check(got.audio.enabled && got.audio.extradata == sent.audio.extradata,
              "StreamInfo carries the audio description");
    }

    sent.audio.enabled = false;
    sent.audio.extradata.clear();
    chast::ServerInfo muted;
    check(chast::parse_hello_ack(payload_of(chast::make_stream_info(sent)), muted, &error) &&
              !muted.audio.enabled,
          "StreamInfo can turn audio off mid-session");
}

void test_identity() {
    const std::string hex = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
    check(chast::normalize_fingerprint(hex) == hex, "a canonical fingerprint is unchanged");
    check(chast::normalize_fingerprint("01:23:45:67:89:ab:cd:ef"
                                        "01:23:45:67:89:ab:cd:ef"
                                        "01:23:45:67:89:ab:cd:ef"
                                        "01:23:45:67:89:ab:cd:ef") == hex,
          "colons, lower case and spacing normalize to upper hex");
    check(chast::normalize_fingerprint(" 0123 4567 89ab cdef 0123 4567 89ab cdef"
                                       " 0123 4567 89ab cdef 0123 4567 89ab cdef ") == hex,
          "spaces between byte groups are accepted");
    check(chast::normalize_fingerprint(std::string(63, 'A')).empty(),
          "a 63-digit fingerprint is rejected");
    check(chast::normalize_fingerprint(std::string(65, 'A')).empty(),
          "a 65-digit fingerprint is rejected");
    check(chast::normalize_fingerprint(std::string(64, 'G')).empty(),
          "a non-hex fingerprint is rejected");
    check(chast::normalize_fingerprint("").empty(), "an empty fingerprint is rejected");

    check(chast::format_fingerprint(hex) ==
              "01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:"
              "01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF",
          "formatting inserts a colon between bytes");
    check(chast::format_fingerprint("").empty(), "an empty fingerprint formats to nothing");

    check(chast::fingerprint_matches(hex, hex), "an identical fingerprint matches");
    check(chast::fingerprint_matches(chast::format_fingerprint(hex), hex),
          "a formatted pin matches the bare fingerprint");
    std::string other = hex;
    other.back() = '0';
    check(!chast::fingerprint_matches(other, hex), "a 1-bit difference is not a match");
    check(!chast::fingerprint_matches("not-a-fingerprint", hex),
          "an invalid pin never matches");
}

void test_input_events() {
    std::string error;

    chast::InputEvent mouse;
    mouse.kind = chast::InputKind::Mouse;
    mouse.mouse.action = chast::MouseAction::ButtonDown;
    mouse.mouse.buttons = chast::kMouseBack;
    mouse.mouse.x = -32768;
    mouse.mouse.y = 65535;
    mouse.modifiers = chast::kModifierLeftCtrl | chast::kModifierRightShift | chast::kLockCaps |
                      chast::kLockScroll;
    chast::InputEvent got;
    check(chast::parse_input_event(payload_of(chast::make_input_event(mouse)), got, &error),
          "mouse event round-trips");
    check(got.kind == chast::InputKind::Mouse && got.mouse.action == chast::MouseAction::ButtonDown,
          "mouse event keeps kind and action");
    check(got.mouse.buttons == chast::kMouseBack, "mouse event keeps the button");
    check(got.mouse.x == mouse.mouse.x && got.mouse.y == mouse.mouse.y,
          "mouse event keeps signed coordinates");
    check(got.modifiers == mouse.modifiers, "mouse event keeps the modifier and lock state");

    chast::InputEvent key;
    key.kind = chast::InputKind::Key;
    key.key.action = chast::KeyAction::Up;
    key.key.hid_usage = 0xE6;
    key.modifiers = chast::kModifierRightAlt;
    check(chast::parse_input_event(payload_of(chast::make_input_event(key)), got, &error),
          "key event round-trips");
    check(got.kind == chast::InputKind::Key && got.key.action == chast::KeyAction::Up,
          "key event keeps kind and action");
    check(got.key.hid_usage == 0xE6, "key event keeps the HID usage");
    check(got.modifiers == chast::kModifierRightAlt, "key event keeps the modifier bitmap");

    auto rejects = [&](std::vector<uint8_t> payload, const char* what) {
        chast::InputEvent dropped;
        check(!chast::parse_input_event(payload, dropped, &error), what);
    };
    rejects(std::vector<uint8_t>(8), "short InputEvent is rejected");

    const std::vector<uint8_t> body = payload_of(chast::make_input_event(mouse));
    std::vector<uint8_t> unknown_kind = body;
    unknown_kind[0] = 9;
    rejects(unknown_kind, "unknown input kind is rejected");
    std::vector<uint8_t> unknown_action = body;
    unknown_action[1] = 9;
    rejects(unknown_action, "unknown mouse action is rejected");
    std::vector<uint8_t> unknown_buttons = body;
    unknown_buttons[2] = 0x80;
    rejects(unknown_buttons, "unknown mouse buttons are rejected");
    std::vector<uint8_t> no_usage = body;
    no_usage[0] = static_cast<uint8_t>(chast::InputKind::Key);
    rejects(no_usage, "key event without a HID usage is rejected");
    std::vector<uint8_t> unknown_modifiers = body;
    unknown_modifiers[15] = 0x80;
    rejects(unknown_modifiers, "unknown input state bits are rejected");
}

// The injector's whole path except SendInput itself, which is what the dry
// run exists for: a test must not move the real pointer.
void test_injector_dry_run() {
    std::string error;
    struct InjectorFixture {
        std::unique_ptr<chast::InputInjector> injector;
    };

    auto key = [](uint16_t usage, chast::KeyAction action, uint16_t modifiers) {
        chast::InputEvent event;
        event.kind = chast::InputKind::Key;
        event.key.hid_usage = usage;
        event.key.action = action;
        event.modifiers = modifiers;
        return event;
    };

    // A coherent chord needs exactly one injection per event.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        check(fixture.injector != nullptr, "the dry-run injector starts");
        if (fixture.injector) {
            chast::InputEvent move;
            move.kind = chast::InputKind::Mouse;
            move.mouse.action = chast::MouseAction::Move;
            move.mouse.x = 40;
            move.mouse.y = 20;
            move.modifiers = chast::kModifierLeftShift;
            fixture.injector->submit(key(0xE1, chast::KeyAction::Down,
                                         chast::kModifierLeftShift));
            fixture.injector->submit(move);
            fixture.injector->submit(key(0x04, chast::KeyAction::Down,
                                         chast::kModifierLeftShift));
            fixture.injector->submit(key(0x04, chast::KeyAction::Up,
                                         chast::kModifierLeftShift));
            fixture.injector->submit(key(0xE1, chast::KeyAction::Up, 0));

            fixture.injector->stop();
            const chast::InputStats stats = fixture.injector->stats();
            check(stats.applied == 5, "a coherent chord is injected once per event");
            check(stats.rejected == 0 && stats.failed == 0 && stats.dropped == 0,
                  "a coherent chord has no rejects, failures or drops");
        }
    }

    // A Shift Down that never arrived is synthesized from the next bitmap.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            fixture.injector->submit(key(0x04, chast::KeyAction::Down,
                                         chast::kModifierLeftShift));
            fixture.injector->submit(key(0x04, chast::KeyAction::Up,
                                         chast::kModifierLeftShift));
            fixture.injector->stop();  // lifts the synthesized Shift
            const chast::InputStats stats = fixture.injector->stats();
            check(stats.applied == 4, "a missing modifier press is synthesized once");
        }
    }

    // A Shift Up that never arrived is lifted by the next empty bitmap.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            fixture.injector->submit(key(0xE1, chast::KeyAction::Down,
                                         chast::kModifierLeftShift));
            fixture.injector->submit(key(0x04, chast::KeyAction::Down, 0));
            fixture.injector->submit(key(0x04, chast::KeyAction::Up, 0));
            fixture.injector->stop();  // nothing is left held
            const chast::InputStats stats = fixture.injector->stats();
            check(stats.applied == 4, "a missing modifier release is repaired once");
        }
    }

    // AltGr arrives as Windows' synthetic left Ctrl plus Right Alt; the pair is
    // folded into Right Alt, including the Ctrl release at the end.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            const uint16_t altgr = chast::kModifierLeftCtrl | chast::kModifierRightAlt;
            fixture.injector->submit(key(0xE0, chast::KeyAction::Down,
                                         chast::kModifierLeftCtrl));
            fixture.injector->submit(key(0xE6, chast::KeyAction::Down, altgr));
            fixture.injector->submit(key(0xE6, chast::KeyAction::Up,
                                         chast::kModifierLeftCtrl));
            fixture.injector->submit(key(0xE0, chast::KeyAction::Up, 0));
            fixture.injector->stop();
            const chast::InputStats stats = fixture.injector->stats();
            check(stats.applied == 4, "AltGr's synthetic Ctrl is folded away");
            check(stats.rejected == 0 && stats.failed == 0 && stats.dropped == 0,
                  "AltGr has no rejects, failures or drops");
        }
    }

    // Lock keys are states, not held keys: the first event reports Caps on
    // and the injector taps Caps once; later events carrying the same state
    // do nothing, and switching back taps it again.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            fixture.injector->submit(key(0x04, chast::KeyAction::Down, chast::kLockCaps));
            fixture.injector->submit(key(0x04, chast::KeyAction::Up, chast::kLockCaps));
            fixture.injector->submit(key(0x04, chast::KeyAction::Down, 0));
            fixture.injector->submit(key(0x04, chast::KeyAction::Up, 0));
            fixture.injector->stop();
            const chast::InputStats stats = fixture.injector->stats();
            check(stats.lock_toggles == 2, "a lock-state change taps the lock key once");
            check(stats.applied == 8, "each lock tap is a press and a release");
            check(stats.rejected == 0 && stats.failed == 0,
                  "lock synchronization injects cleanly");
        }
    }

    // A host already in the reported lock state is left alone.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            fixture.injector->submit(key(0xE0, chast::KeyAction::Down, 0));
            fixture.injector->stop();
            check(fixture.injector->stats().lock_toggles == 0,
                  "an already matching lock state is left alone");
        }
    }

    // InputRelease lifts what is held, in order, without stopping the thread.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            fixture.injector->submit(key(0xE0, chast::KeyAction::Down,
                                         chast::kModifierLeftCtrl));
            fixture.injector->release_held();
            auto applied = [&] { return fixture.injector->stats().applied; };
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (applied() < 2 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            check(applied() == 2, "a release request lifts the held modifier");
        }
    }

    // Fast toggling of a held modifier stays balanced.
    {
        InjectorFixture fixture{chast::InputInjector::create(true, &error)};
        if (fixture.injector) {
            for (int i = 0; i < 10; ++i) {
                fixture.injector->submit(key(0xE1, chast::KeyAction::Down,
                                             chast::kModifierLeftShift));
                fixture.injector->submit(key(0xE1, chast::KeyAction::Up, 0));
            }
            fixture.injector->stop();
            const chast::InputStats stats = fixture.injector->stats();
            check(stats.applied == 20, "rapid modifier toggling injects every transition once");
        }
    }

    InjectorFixture fixture{chast::InputInjector::create(true, &error)};
    if (!fixture.injector) {
        return;
    }
    chast::InputEvent held;
    held.kind = chast::InputKind::Key;
    held.key.hid_usage = 0xE0;  // Left Ctrl, never released by the peer
    fixture.injector->submit(held);

    chast::InputEvent unknown;
    unknown.kind = chast::InputKind::Key;
    unknown.key.hid_usage = 0x48;  // Pause: no set 1 scan code
    fixture.injector->submit(unknown);

    fixture.injector->stop();
    const chast::InputStats stats = fixture.injector->stats();
    check(stats.applied == 2, "stop() releases the key left down and counts it");
    check(stats.rejected == 1, "an unmappable HID usage is rejected");
}

void test_scancodes() {
    uint16_t scan = 0;
    bool extended = false;

    check(chast::hid_to_scancode(0x04, scan, extended) && scan == 0x1E && !extended,
          "HID A maps to set 1 0x1E");
    check(chast::hid_to_scancode(0x28, scan, extended) && scan == 0x1C && !extended,
          "HID Enter maps to set 1 0x1C");
    check(chast::hid_to_scancode(0x50, scan, extended) && scan == 0x4B && extended,
          "HID Left arrow maps to the extended 0x4B");
    check(chast::hid_to_scancode(0xE5, scan, extended) && scan == 0x36 && !extended,
          "HID Right Shift maps to 0x36");
    check(chast::hid_to_scancode(0xE6, scan, extended) && scan == 0x38 && extended,
          "HID Right Alt maps to the extended 0x38");
    check(chast::hid_to_scancode(0x58, scan, extended) && scan == 0x1C && extended,
          "HID keypad Enter maps to the extended 0x1C");
    check(!chast::hid_to_scancode(0x48, scan, extended), "HID Pause is rejected (needs E1)");
    check(!chast::hid_to_scancode(0x00, scan, extended), "unused HID id is rejected");
}

// Real SendInput, only when asked for on the command line: moves the pointer
// to a known virtual-desktop coordinate, reads it back, and puts it where it
// was. No clicks and no keys, so nothing on the desktop reacts.
bool test_live_pointer() {
    POINT saved{};
    if (!GetCursorPos(&saved)) {
        check(false, "live: cannot read the current pointer position");
        return false;
    }
    const int32_t left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int32_t top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int32_t width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int32_t height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0) {
        check(false, "live: no virtual screen metrics");
        return false;
    }

    struct Restore {
        POINT point;
        ~Restore() {
            SetCursorPos(point.x, point.y);
        }
    } restore{saved};

    std::string error;
    std::unique_ptr<chast::InputInjector> injector =
        chast::InputInjector::create(false, &error);
    if (!injector) {
        check(false, "live: the injector starts");
        return false;
    }

    // Aim at 3/4 across and 1/2 down, then read the result back.
    const int32_t target_x = left + width * 3 / 4;
    const int32_t target_y = top + height / 2;
    chast::InputEvent move;
    move.kind = chast::InputKind::Mouse;
    move.mouse.action = chast::MouseAction::Move;
    move.mouse.x = target_x;
    move.mouse.y = target_y;
    injector->submit(move);

    POINT reached{};
    bool landed = false;
    for (int attempt = 0; attempt < 50 && !landed; ++attempt) {
        Sleep(10);
        if (!GetCursorPos(&reached)) {
            break;
        }
        // Absolute injection quantizes to 1/65535 of the virtual screen, so a
        // pixel or two of error is expected, not a failure.
        landed = std::abs(reached.x - target_x) <= 2 && std::abs(reached.y - target_y) <= 2;
    }
    std::printf("     target=(%d,%d) reached=(%d,%d)\n", target_x, target_y, reached.x,
                reached.y);
    check(landed, "live: an absolute move lands on the requested pixel");

    injector->stop();
    const chast::InputStats stats = injector->stats();
    check(stats.applied == 1 && stats.failed == 0, "live: SendInput accepted the move");
    return landed;
}

void test_file_transfer() {
    std::string error;
    chast::FileOffer offer;
    offer.transfer_id = 7;
    offer.size = 12;
    offer.resume = true;
    offer.name = "report-\xE6\x8A\xA5\xE5\x91\x8A.pdf";
    offer.relative_path = "reports/2026";
    offer.sha256.fill(0xAB);

    chast::FileOffer got_offer;
    check(chast::parse_file_offer(payload_of(chast::make_file_offer(offer)), got_offer,
                                  &error) &&
              got_offer.transfer_id == 7 && got_offer.size == 12 && got_offer.resume &&
              got_offer.name == offer.name && got_offer.sha256 == offer.sha256 &&
              got_offer.relative_path == offer.relative_path,
          "FileOffer round-trips id, size, resume flag, digest, UTF-8 name and directory");

    chast::FileOffer legacy_offer = offer;
    legacy_offer.relative_path.clear();
    std::vector<uint8_t> legacy = payload_of(chast::make_file_offer(legacy_offer));
    legacy.resize(legacy.size() - 2);  // drop the empty v22 directory field
    chast::FileOffer got_legacy;
    check(chast::parse_file_offer(legacy, got_legacy, &error) &&
              got_legacy.name == offer.name && got_legacy.relative_path.empty(),
          "a FileOffer without the directory field still parses at the receive root");

    check(chast::make_offer_relative_path("Trip", "2024\\07") == "Trip/2024/07" &&
              chast::make_offer_relative_path("Trip", "2024/07") == "Trip/2024/07" &&
              chast::make_offer_relative_path("Trip", ".") == "Trip" &&
              chast::make_offer_relative_path("Trip", "") == "Trip" &&
              chast::make_offer_relative_path("Trip", "\\2024") == "Trip/2024",
          "upload directories convert native separators to '/' below the top folder");

    auto rejects_offer = [&](std::vector<uint8_t> payload, const char* what) {
        chast::FileOffer dropped;
        check(!chast::parse_file_offer(payload, dropped, &error), what);
    };
    auto rejects_relative = [&](std::string relative, const char* what) {
        chast::FileOffer bad = offer;
        bad.relative_path = std::move(relative);
        rejects_offer(payload_of(chast::make_file_offer(bad)), what);
    };
    rejects_relative("/absolute", "an absolute FileOffer directory is rejected");
    rejects_relative("C:/Windows", "a drive-letter FileOffer directory is rejected");
    rejects_relative("reports/../secret", "a FileOffer directory with .. is rejected");
    rejects_relative("reports/./2026", "a FileOffer directory with . is rejected");
    rejects_relative("reports//2026", "a FileOffer directory with an empty part is rejected");
    rejects_relative("reports\\2026", "a FileOffer directory with a backslash is rejected");
    // make_file_offer clears an over-long directory, so build the wire form by
    // hand to prove the parser rejects it rather than truncating.
    chast::FileOffer rooted = offer;
    rooted.relative_path.clear();
    std::vector<uint8_t> overlong = payload_of(chast::make_file_offer(rooted));
    const size_t overlong_length = chast::kMaxFileRelativePathBytes + 1;
    overlong.push_back(static_cast<uint8_t>(overlong_length & 0xFF));
    overlong.push_back(static_cast<uint8_t>((overlong_length >> 8) & 0xFF));
    overlong.insert(overlong.end(), overlong_length, 'a');
    rejects_offer(std::move(overlong), "an over-long FileOffer directory is rejected");
    std::string deep;
    for (size_t i = 0; i <= chast::kMaxFileRelativeComponents; ++i) {
        if (!deep.empty()) {
            deep.push_back('/');
        }
        deep.push_back('a');
    }
    rejects_relative(std::move(deep), "a too-deep FileOffer directory is rejected");
    std::vector<uint8_t> unknown_flags = payload_of(chast::make_file_offer(offer));
    unknown_flags[12] = 0x02;  // id(4) + size(8), then flags
    rejects_offer(std::move(unknown_flags), "FileOffer with unknown flags is rejected");

    std::vector<uint8_t> empty_name = payload_of(chast::make_file_offer(offer));
    // id(4) size(8) flags(1) digest(32), then the u16 name length.
    constexpr size_t kNameLengthAt = 45;
    empty_name[kNameLengthAt] = 0;
    empty_name[kNameLengthAt + 1] = 0;
    empty_name.resize(kNameLengthAt + 2);
    rejects_offer(std::move(empty_name), "FileOffer without a name is rejected");

    std::vector<uint8_t> long_name = payload_of(chast::make_file_offer(offer));
    long_name[kNameLengthAt] = static_cast<uint8_t>(chast::kMaxFileNameBytes + 1);
    long_name[kNameLengthAt + 1] =
        static_cast<uint8_t>((chast::kMaxFileNameBytes + 1) >> 8);
    rejects_offer(std::move(long_name), "FileOffer with an oversized name is rejected");

    std::vector<uint8_t> trailing = payload_of(chast::make_file_offer(offer));
    trailing.push_back(0x55);
    rejects_offer(std::move(trailing), "FileOffer with trailing bytes is rejected");

    chast::FileOffer huge;
    huge.transfer_id = 1;
    huge.size = chast::kMaxFileTransferBytes + 1;
    huge.name = "huge.bin";
    std::vector<uint8_t> huge_payload = payload_of(chast::make_file_offer(huge));
    rejects_offer(std::move(huge_payload), "FileOffer past the protocol limit is rejected");

    chast::FileAnswer answer;
    answer.transfer_id = 7;
    answer.code = chast::FileCode::Ok;
    answer.resume_offset = 4096;
    chast::FileAnswer got_answer;
    check(chast::parse_file_answer(payload_of(chast::make_file_answer(answer)), got_answer,
                                   &error) &&
              got_answer.transfer_id == 7 && got_answer.code == chast::FileCode::Ok &&
              got_answer.resume_offset == 4096,
          "FileAnswer round-trips a resume offset");
    answer.code = chast::FileCode::Busy;
    answer.reason = "another transfer is active";
    check(chast::parse_file_answer(payload_of(chast::make_file_answer(answer)), got_answer,
                                   &error) &&
              got_answer.code == chast::FileCode::Busy &&
              got_answer.resume_offset == 0 &&
              got_answer.reason == answer.reason,
          "a refused FileAnswer carries its reason and no offset");

    std::vector<uint8_t> bad_answer_code =
        payload_of(chast::make_file_answer(answer));
    bad_answer_code[4] = 0xFF;  // id(4), then the code
    check(!chast::parse_file_answer(bad_answer_code, got_answer, &error),
          "FileAnswer with an unknown code is rejected");

    chast::FileResult result;
    result.transfer_id = 7;
    result.code = chast::FileCode::DigestMismatch;
    result.received = 12;
    result.reason = "sha-256 mismatch";
    chast::FileResult got_result;
    check(chast::parse_file_result(payload_of(chast::make_file_result(result)), got_result,
                                   &error) &&
              got_result.code == chast::FileCode::DigestMismatch &&
              got_result.received == 12 && got_result.reason == result.reason,
          "FileResult round-trips a digest mismatch");
    result.code = chast::FileCode::Busy;
    std::vector<uint8_t> result_busy = payload_of(chast::make_file_result(result));
    check(!chast::parse_file_result(result_busy, got_result, &error),
          "FileResult refuses a code it cannot report");

    chast::FileCancel cancel;
    cancel.transfer_id = 7;
    cancel.reason = "viewer cancelled";
    chast::FileCancel got_cancel;
    check(chast::parse_file_cancel(payload_of(chast::make_file_cancel(cancel)), got_cancel,
                                   &error) &&
              got_cancel.transfer_id == 7 && got_cancel.code == chast::FileCode::Cancelled &&
              got_cancel.reason == cancel.reason,
          "FileCancel round-trips");
    cancel.code = chast::FileCode::IoError;
    std::vector<uint8_t> cancel_io = payload_of(chast::make_file_cancel(cancel));
    check(!chast::parse_file_cancel(cancel_io, got_cancel, &error),
          "FileCancel with a non-cancel code is rejected");

    chast::FileStreamHeader header;
    header.transfer_id = 7;
    header.offset = 4096;
    uint8_t stream_header[chast::kFileStreamHeaderSize]{};
    chast::encode_file_stream_header(header, stream_header);
    chast::FileStreamHeader got_header;
    check(chast::decode_file_stream_header(stream_header, sizeof(stream_header), got_header,
                                           &error) &&
              got_header.transfer_id == 7 && got_header.offset == 4096,
          "CFIL header round-trips");
    uint8_t bad_magic[chast::kFileStreamHeaderSize]{};
    std::memcpy(bad_magic, stream_header, sizeof(bad_magic));
    bad_magic[0] = 'X';
    check(!chast::decode_file_stream_header(bad_magic, sizeof(bad_magic), got_header, &error),
          "CFIL header with bad magic is rejected");
    uint8_t bad_version[chast::kFileStreamHeaderSize]{};
    std::memcpy(bad_version, stream_header, sizeof(bad_version));
    bad_version[4] = 2;
    check(!chast::decode_file_stream_header(bad_version, sizeof(bad_version), got_header,
                                            &error),
          "CFIL header with a newer version is rejected");
    uint8_t bad_reserved[chast::kFileStreamHeaderSize]{};
    std::memcpy(bad_reserved, stream_header, sizeof(bad_reserved));
    bad_reserved[5] = 1;
    check(!chast::decode_file_stream_header(bad_reserved, sizeof(bad_reserved), got_header,
                                            &error),
          "CFIL header with reserved bytes is rejected");
    check(!chast::decode_file_stream_header(stream_header,
                                            chast::kFileStreamHeaderSize - 1, got_header,
                                            &error),
          "a truncated CFIL header is rejected");
}

void test_clipboard_container() {
    std::string error;
    chast::ClipboardContent content;
    chast::ClipboardFormatData text;
    text.format = chast::ClipboardFormat::Text;
    const std::string hello = "hello \xE4\xB8\x96\xE7\x95\x8C";
    text.data.assign(hello.begin(), hello.end());
    content.formats.push_back(text);

    chast::ClipboardFormatData html;
    html.format = chast::ClipboardFormat::Html;
    const std::string markup =
        "Version:0.9\r\nStartHTML:0000000105\r\nEndHTML:0000000200\r\n"
        "<html><body><b>hello</b></body></html>";
    html.data.assign(markup.begin(), markup.end());
    content.formats.push_back(html);

    chast::ClipboardFormatData rtf;
    rtf.format = chast::ClipboardFormat::Rtf;
    const std::string rtf_text = "{\\rtf1\\ansi hello}";
    rtf.data.assign(rtf_text.begin(), rtf_text.end());
    content.formats.push_back(rtf);

    chast::ClipboardFormatData dib;
    dib.format = chast::ClipboardFormat::Dib;
    dib.data.assign(44, 0);
    // BITMAPINFOHEADER: 40-byte header, 1x1, 32 bpp, one pixel after it.
    dib.data[0] = 40;
    dib.data[4] = 1;
    dib.data[8] = 1;
    dib.data[12] = 1;  // planes
    dib.data[14] = 32; // bit count
    content.formats.push_back(dib);

    const uint64_t hash = chast::clipboard_content_hash(content);
    check(hash != 0, "clipboard content hashes to a non-zero value");
    chast::ClipboardContent reordered = content;
    std::swap(reordered.formats[0], reordered.formats[3]);
    check(chast::clipboard_content_hash(reordered) == hash,
          "the clipboard hash ignores entry order");

    const std::vector<uint8_t> stream = chast::encode_clipboard_stream(content);
    check(!stream.empty() &&
              std::memcmp(stream.data(), chast::kClipboardStreamMagic, 4) == 0 &&
              stream[4] == chast::kClipboardStreamVersion,
          "CCLB stream starts with its magic and version");
    chast::ClipboardContent decoded;
    check(chast::decode_clipboard_stream(stream.data(), stream.size(), decoded, &error),
          "CCLB stream round-trips");
    check(decoded.hash == hash && decoded.formats.size() == 4 &&
              decoded.formats[0].format == chast::ClipboardFormat::Text &&
              decoded.formats[1].format == chast::ClipboardFormat::Html &&
              decoded.formats[2].format == chast::ClipboardFormat::Rtf &&
              decoded.formats[3].format == chast::ClipboardFormat::Dib &&
              decoded.formats[0].data == text.data &&
              decoded.formats[1].data == html.data &&
              decoded.formats[3].data == dib.data,
          "CCLB stream keeps every format and byte");

    auto alters = [&](size_t at, uint8_t value, const char* what) {
        std::vector<uint8_t> broken = stream;
        broken[at] = value;
        chast::ClipboardContent dropped;
        check(!chast::decode_clipboard_stream(broken.data(), broken.size(), dropped, &error),
              what);
    };
    alters(8, static_cast<uint8_t>(stream[8] ^ 0xFF), "a CCLB with a wrong hash is rejected");
    alters(4, 2, "a CCLB with an unknown version is rejected");
    alters(5, 9, "a CCLB with too many formats is rejected");
    chast::ClipboardContent dropped;
    check(!chast::decode_clipboard_stream(stream.data(), stream.size() - 1, dropped, &error),
          "a truncated CCLB is rejected");

    std::vector<uint8_t> trailing = stream;
    trailing.push_back(0);
    check(!chast::decode_clipboard_stream(trailing.data(), trailing.size(), dropped, &error),
          "a CCLB with trailing bytes is rejected");

    chast::ClipboardContent duplicate = content;
    duplicate.formats.insert(duplicate.formats.begin() + 1, text);
    check(chast::encode_clipboard_stream(duplicate).empty(),
          "the encoder refuses duplicate clipboard formats");

    chast::ClipboardContent bad_text;
    chast::ClipboardFormatData nul_text;
    nul_text.format = chast::ClipboardFormat::Text;
    nul_text.data = {'a', 0, 'b'};
    bad_text.formats.push_back(nul_text);
    check(chast::encode_clipboard_stream(bad_text).empty(),
          "the encoder refuses clipboard text with a NUL");

    chast::ClipboardContent bad_dib;
    chast::ClipboardFormatData short_dib;
    short_dib.format = chast::ClipboardFormat::Dib;
    short_dib.data.assign(40, 0);
    short_dib.data[0] = 40;
    bad_dib.formats.push_back(short_dib);
    check(chast::encode_clipboard_stream(bad_dib).empty(),
          "the encoder refuses a DIB with no picture size");
}

}  // namespace

int main(int argc, char** argv) {
    test_hello();
    test_access_denied();
    test_hello_ack();
    test_merged_hello_ack();
    test_permission_update();
    test_sas();
    test_stream_options();
    test_privacy_mode();
    test_display_switch();
    test_display_modes();
    test_file_transfer();
    test_clipboard_container();
    test_frame_header();
    test_audio_header();
    test_microphone_stream_header();
    test_audio_parser();
    test_latency_probe();
    test_clipboard();
    test_chat();
    test_cursor_update();
    test_stream_info();
    test_input_events();
    test_identity();
    test_scancodes();
    test_injector_dry_run();
    bool live = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--live-input") {
            live = true;
        }
    }
    if (live) {
        test_live_pointer();
    }
    if (failures != 0) {
        std::printf("protocol checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("protocol checks passed\n");
    return 0;
}
