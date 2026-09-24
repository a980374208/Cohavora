#pragma once

#include "render/owned_i420_frame.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace livekit::render {

enum class CanvasRenderStage {
    DrawSubmit,
    // GL measures swapBuffers; the DX11 module's Render includes Present.
    // These are CPU call spans, not GPU execution or display completion.
    PresentBlock,
};

class CanvasRenderTimingObserver {
public:
    virtual ~CanvasRenderTimingObserver() = default;
    virtual void OnCanvasStageTiming(
        CanvasRenderStage stage, std::chrono::microseconds duration) = 0;
};

// One batch per actual canvas operation, including repeated draws of a static
// scene. Multiple resources from one session share a sink and count once.
// Different canvases use different batches; no frame-token/global deduplication.
// Only operations containing a valid telemetry binding are attributed. A mixed
// session scene attributes the full operation once to each participating session.
class CanvasRenderTimingBatch final {
public:
    void Add(const RenderFrameMetadata& metadata) {
        if (!metadata.valid()) return;
        auto observer = metadata.observer->CanvasTimingObserver(metadata);
        if (observer && std::find(observers_.begin(), observers_.end(), observer) ==
                observers_.end()) {
            observers_.push_back(std::move(observer));
        }
    }

    void Notify(CanvasRenderStage stage, std::chrono::microseconds duration) const {
        if (duration.count() < 0) return;
        for (const auto& observer : observers_) {
            observer->OnCanvasStageTiming(stage, duration);
        }
    }

private:
    std::vector<std::shared_ptr<CanvasRenderTimingObserver>> observers_;
};

} // namespace livekit::render
