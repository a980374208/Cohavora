#pragma once

#include "modules/desktop_capture/desktop_frame.h"
#include "modules/desktop_capture/shared_desktop_frame.h"
#include <array>
#include <memory>

namespace livekit {
// Capture-thread owned. A consumer's shared frame pins its slot: never overwrite
// pixels still in use, and never allocate an unbounded spill queue.
class DesktopFrameBufferPool final {
public:
    webrtc::SharedDesktopFrame* Acquire(webrtc::DesktopSize size) {
        for (auto& slot : slots_) {
            if (slot && slot->IsShared()) continue;
            if (!slot || !slot->size().equals(size))
                slot = webrtc::SharedDesktopFrame::Wrap(
                    std::make_unique<webrtc::BasicDesktopFrame>(size));
            return slot.get();
        }
        return nullptr;
    }
    void Clear() { for (auto& slot : slots_) slot.reset(); }
private:
    std::array<std::unique_ptr<webrtc::SharedDesktopFrame>, 2> slots_;
};
} // namespace livekit
