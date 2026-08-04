#include "local_video_track.h"
#include "webrtc_manager.h"
#include "rtc_video_source.h"

namespace livekit {

LocalVideoTrack::LocalVideoTrack(const std::string& sid, const std::string& name, std::shared_ptr<VideoSource> source)
    : Track(sid, name, TrackKind::Video), source_(source) {}

std::shared_ptr<LocalVideoTrack> LocalVideoTrack::createLocalVideoTrack(const std::string& name,
                                                              const std::shared_ptr<VideoSource>& source) {
    std::string sid = "TR_VID_" + name;
    auto track = std::make_shared<LocalVideoTrack>(sid, name, source);
    if (source) {
        source->addSink([track](const VideoFrame& frame, const VideoCaptureOptions& options) {
            if (!track->muted()) {
                track->notifyVideoFrame(frame, options);
            }
        });

        auto factory = WebRTCManager::Instance().factory();
        if (factory) {
            WebRTCManager::Instance().worker_thread()->BlockingCall([&]() {
                auto rtc_src = RtcVideoSource::Create(source);
                auto rtc_video_track = factory->CreateVideoTrack(rtc_src, name);
                track->set_rtc_track(rtc_video_track);
            });
        }
    }
    return track;
}

} // namespace livekit
