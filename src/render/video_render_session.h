#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <QtGui/QImage>

#include "src/core/track.h"
#include "src/render/qt_cpu_video_renderer.h"
#include "src/render/video_render_router.h"
#include "src/render/local_video_render_input.h"

namespace livekit::render {

// UI-owned session that binds Track control-plane events to a single selected
// backend. Frame callbacks capture only State weak_ptr plus generation.
class VideoRenderSession final {
public:
    enum class Backend {
        QtCpu,
        Gpu,
    };

    using FrameReadyCallback = std::function<void(const std::string& identity, const QImage& image)>;
    using GpuFrameReadyCallback = std::function<void(const std::string& render_key, VideoRenderFrame::Ptr frame)>;

    // These counters are intended for UI diagnostics and stress-test assertions.
    // They deliberately describe work that reached the selected backend, rather
    // than the producer-side Router counters alone.
    struct Statistics {
        VideoRenderRouter::Statistics router;
        uint64_t delivered_to_gpu = 0;
        uint64_t delivered_to_qt_cpu = 0;
        uint64_t qt_cpu_conversion_failures = 0;
        uint64_t rejected_track_attachments = 0;
        size_t attached_track_count = 0;
        Backend backend = Backend::QtCpu;
    };

    explicit VideoRenderSession(FrameReadyCallback frame_ready_callback,
                                size_t max_active_tracks = 16);
    ~VideoRenderSession();

    VideoRenderSession(const VideoRenderSession&) = delete;
    VideoRenderSession& operator=(const VideoRenderSession&) = delete;

    // render_key identifies a view independently of the participant (camera
    // and screen share must not overwrite one another).
    void AttachRemoteTrack(const std::shared_ptr<Track>& track, const std::string& identity,
                           const std::string& render_key = {});
    void RemoveTrack(const std::string& track_id);
    void RemoveTracksForIdentity(const std::string& identity);
    void RenderLatestFrames();
    void AttachLocalSource(const std::shared_ptr<VideoSource>& source, const std::string& render_key = "local");
    void DetachLocalSource();
    // UI-thread-only common dispatch, also used by the existing screen mailbox.
    void RenderFrame(const std::string& render_key, VideoRenderFrame::Ptr frame);
    void UseGpuBackend(GpuFrameReadyCallback frame_ready_callback);
    void UseQtCpuBackend();
    void Deactivate();

    Backend backend() const noexcept { return backend_; }
    bool active() const noexcept;
    uint64_t generation() const noexcept;
    Statistics statistics() const noexcept;

private:
    struct State {
        State(uint64_t generation_value, size_t max_active_tracks)
            : generation(generation_value), router(std::make_shared<VideoRenderRouter>(generation_value, max_active_tracks)) {}

        const uint64_t generation;
        std::atomic<bool> active{true};
        std::shared_ptr<VideoRenderRouter> router;
        std::atomic<uint64_t> delivered_to_gpu{0};
        std::atomic<uint64_t> delivered_to_qt_cpu{0};
        std::atomic<uint64_t> qt_cpu_conversion_failures{0};
        std::atomic<uint64_t> rejected_track_attachments{0};
        std::atomic<uint64_t> next_track_binding_generation{1};
    };

    struct TrackBinding {
        std::string identity;
        std::string render_key;
        std::weak_ptr<Track> track;
        uint64_t binding_generation = 0;
        Track::I420VideoFrameSubscription subscription;
    };

    static uint64_t NextGeneration();

    std::shared_ptr<State> state_;
    std::unordered_map<std::string, TrackBinding> tracks_;
    const size_t max_active_tracks_;
    QtCpuVideoRenderer cpu_renderer_;
    FrameReadyCallback frame_ready_callback_;
    GpuFrameReadyCallback gpu_frame_ready_callback_;
    LocalVideoRenderInput local_input_;
    std::string local_render_key_;
    Backend backend_ = Backend::QtCpu;
};

} // namespace livekit::render
