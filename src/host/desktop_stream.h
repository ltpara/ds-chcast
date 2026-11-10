#pragma once

#include <functional>
#include <map>
#include <mutex>
#include "chast/capture.h"
#include "chast/encoder.h"
#include "video_qos.h"
#include "stream_recorder.h"

namespace chast {

// Display-owned GPU pipelines. A subscription exposes the existing capture /
// encoder interfaces to the session control loop, but only the service touches
// the GPU. Subscribers receive ordered compressed frames, never encode copies
// of the same image. Each display owns a service thread; capture, encoding,
// reconfiguration and teardown all run there, independent of viewer sends.
class DesktopStreamPool {
    struct CodecGroup;
    struct Source;
    class Capture;
    class Encoder;
public:
    using CaptureFactory = std::function<std::unique_ptr<IDxgiCapturer>(std::string*)>;
    using EncoderFactory = std::function<std::unique_ptr<IVideoEncoder>(
        ID3D11Device*, const EncoderConfig&, std::string*)>;
    using FollowDesktop = std::function<bool(std::string*, bool*)>;
    class CodecPeer {
    public:
        ~CodecPeer();
        CodecPeer(const CodecPeer&) = delete;
        CodecPeer& operator=(const CodecPeer&) = delete;
    private:
        friend class DesktopStreamPool;
        friend class Capture;
        friend struct CodecGroup;
        CodecPeer(std::shared_ptr<CodecGroup>, uint64_t, VideoCodecMask, std::optional<VideoCodec>,
                  VideoCodecMask, VideoChroma);
        std::unique_lock<std::mutex> lock_transaction();
        std::shared_ptr<CodecGroup> group_;
        uint64_t id_;
        VideoCodecMask decoders_;
        std::optional<VideoCodec> preference_;
        VideoCodecMask decoders_444_;
        VideoChroma chroma_preference_;
    };
    std::shared_ptr<CodecPeer> connect_codecs(VideoCodecMask, std::optional<VideoCodec> = {},
        VideoCodecMask decoding_444 = 0, VideoChroma preference = VideoChroma::Yuv420);
    // A connected viewer retains its vote when it temporarily unsubscribes all
    // displays. It can change preferences before adding video again.
    static bool vote_peer(CodecPeer&, std::optional<VideoCodec>, std::string* error,
                          VideoChroma preference = VideoChroma::Yuv420);
    class QualityPeer {
    public:
        QualityPeer(std::shared_ptr<VideoQos> policy, uint64_t id);
        ~QualityPeer();
        QualityPeer(const QualityPeer&) = delete;
        QualityPeer& operator=(const QualityPeer&) = delete;
        void options(uint32_t fps, std::optional<VideoQos::Quality> quality,
                     std::optional<float> custom_ratio = std::nullopt);
        void feedback(uint32_t delay_ms, uint64_t pending_ms, bool new_reply);
    private:
        std::shared_ptr<VideoQos> policy_;
        uint64_t id_;
    };
    std::shared_ptr<QualityPeer> connect_quality(uint32_t max_fps, float ratio, bool abr,
                                                 bool adaptive_fps = true);
    std::unique_ptr<IDxgiCapturer> subscribe(const std::wstring& key,
        CaptureFactory capture, EncoderFactory encoder, std::string* error,
        FollowDesktop follow_desktop = {}, VideoCodecMask decoding_codecs = kVideoCodecAll,
        std::shared_ptr<CodecPeer> peer = {}, HostRecordingOptions recording = {});
private:
    std::mutex mutex_;
    std::map<std::wstring, std::weak_ptr<Source>> sources_;
    std::shared_ptr<VideoQos> qos_;
    std::shared_ptr<CodecGroup> codecs_;
    uint64_t next_peer_ = 0;
    static std::unique_ptr<IVideoEncoder> configure_encoder(Capture&, const EncoderConfig&,
        std::optional<VideoCodec>, bool preserve_preference, std::string* error);
public:
    static std::unique_ptr<IVideoEncoder> encoder(IDxgiCapturer& capture,
        const EncoderConfig& config, std::string* error, bool change_codec = false);
    static std::unique_ptr<IVideoEncoder> vote_codec(IDxgiCapturer& capture,
        const EncoderConfig& config, std::optional<VideoCodec> preference, std::string* error);
    static VideoCodec codec(const IDxgiCapturer& capture);
    static VideoChroma chroma(const IDxgiCapturer& capture);
    static UINT configured_bitrate(const IDxgiCapturer& capture);
    // Read a rate only for the format this connection has announced. A source
    // rebuild must be announced with its new picture before its new rate.
    static bool read_rate(const IVideoEncoder& encoder, UINT& bitrate, UINT& fps);
    // A failed Auto encoder is awaiting a compatible software codec. Keep the
    // session alive while votes/decoder capabilities can still change.
    static bool encoder_recovery_pending(const IDxgiCapturer& capture);
    static void request_keyframe(IDxgiCapturer& capture);
    // Resume this subscriber at a fresh IDR after network backpressure.
    static void discard_backlog(IDxgiCapturer& capture);
    static uint64_t capture_time(const IDxgiCapturer& capture);
    // RustDesk static_refresh::max_repeat_frames: how many times an idle
    // monitor may repeat its last picture so rate control can refine it.
    // 0 disables static refinement for that format.
    static unsigned static_refresh_budget(VideoCodec codec, float quality,
                                          EncoderBackend backend,
                                          const std::string& implementation = {});
    // Latest pointer state, independent of this subscriber's video backlog.
    static std::shared_ptr<const CursorInfo> cursor_snapshot(const IDxgiCapturer& capture);
    // Install this subscriber's pointer sink. The display service invokes it
    // on its own thread right after a frame is captured, before encoding it,
    // so pointer delivery does not wait for video or for the session loop.
    static void set_cursor_sink(IDxgiCapturer& capture,
                                std::function<void(const CursorInfo&)> sink);
    // Reconcile a source with a fresh OS enumeration once for all subscribers.
    // A second viewer adopts the new generation without rebuilding it again.
    static Error refresh_display(IDxgiCapturer& capture, const DisplayInfo& display, bool* changed);
    static Error reserve_audio(IDxgiCapturer& capture, UINT bitrate);
};

} // namespace chast
