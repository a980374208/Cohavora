#pragma once

#include <QtGui/QImage>

#include "src/render/owned_i420_frame.h"

namespace livekit::render {

// The sole Qt CPU rendering backend. It converts an owned I420 frame into an
// owned QImage on the UI/render tick; it never runs on WebRTC's media worker.
class QtCpuVideoRenderer final {
public:
    QImage Convert(const OwnedI420Frame& frame) const;
};

} // namespace livekit::render
