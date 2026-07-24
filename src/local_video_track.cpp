#include "local_video_track.h"

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
    }
    return track;
}

} // namespace livekit
