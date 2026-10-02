#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "core/track.h"
#include "render/owned_i420_frame.h"

namespace {

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[OwnedI420FrameTest] FAILED: " << message << std::endl;
        return false;
    }
    return true;
}

} // namespace

int main() {
    // Odd dimensions and padded source strides exercise the exact copy contract
    // used by decoder-owned I420 buffers.
    constexpr int kWidth = 3;
    constexpr int kHeight = 3;
    const std::vector<uint8_t> source_y{
        1, 2, 3, 0xEE, 0xEE,
        4, 5, 6, 0xEE, 0xEE,
        7, 8, 9, 0xEE, 0xEE,
    };
    const std::vector<uint8_t> source_u{
        10, 11, 0xEE,
        12, 13, 0xEE,
    };
    const std::vector<uint8_t> source_v{
        20, 21, 0xEE,
        22, 23, 0xEE,
    };

    const livekit::render::RenderColorSpace color_space{
        livekit::render::RenderColorMatrix::Bt709,
        livekit::render::RenderColorRange::Full,
    };
    auto frame = livekit::render::OwnedI420Frame::CopyFromPlanes(
        kWidth,
        kHeight,
        source_y.data(),
        5,
        source_u.data(),
        3,
        source_v.data(),
        3,
        1234567,
        livekit::VideoRotation::VIDEO_ROTATION_90,
        color_space);

    if (!Expect(frame != nullptr, "valid padded I420 planes should copy") ||
        !Expect(frame->width() == kWidth && frame->height() == kHeight, "luma dimensions must be preserved") ||
        !Expect(frame->chroma_width() == 2 && frame->chroma_height() == 2, "odd dimensions must use ceil chroma dimensions") ||
        !Expect(frame->stride_y() == 3 && frame->stride_u() == 2 && frame->stride_v() == 2, "owned planes must be tightly packed") ||
        !Expect(frame->timestamp_us() == 1234567, "timestamp must be preserved") ||
        !Expect(frame->rotation() == livekit::VideoRotation::VIDEO_ROTATION_90, "rotation must be preserved") ||
        !Expect(frame->color_space().matrix == livekit::render::RenderColorMatrix::Bt709 &&
                    frame->color_space().range == livekit::render::RenderColorRange::Full,
                "colour metadata must be preserved")) {
        return 1;
    }

    const std::vector<uint8_t> expected_y{1, 2, 3, 4, 5, 6, 7, 8, 9};
    const std::vector<uint8_t> expected_u{10, 11, 12, 13};
    const std::vector<uint8_t> expected_v{20, 21, 22, 23};
    if (!Expect(std::vector<uint8_t>(frame->data_y(), frame->data_y() + expected_y.size()) == expected_y,
                "Y plane must omit stride padding") ||
        !Expect(std::vector<uint8_t>(frame->data_u(), frame->data_u() + expected_u.size()) == expected_u,
                "U plane must omit stride padding") ||
        !Expect(std::vector<uint8_t>(frame->data_v(), frame->data_v() + expected_v.size()) == expected_v,
                "V plane must omit stride padding")) {
        return 1;
    }

    // Tight planes use the contiguous path; they must produce the same owned
    // bytes as padded odd-sized planes and remain valid after source mutation.
    auto packed_y = expected_y, packed_u = expected_u, packed_v = expected_v;
    auto packed = livekit::render::OwnedI420Frame::CopyFromPlanes(
        kWidth, kHeight, packed_y.data(), kWidth,
        packed_u.data(), 2, packed_v.data(), 2);
    std::fill(packed_y.begin(), packed_y.end(), 0);
    std::fill(packed_u.begin(), packed_u.end(), 0);
    std::fill(packed_v.begin(), packed_v.end(), 0);
    if (!Expect(packed &&
        std::vector<uint8_t>(packed->data_y(), packed->data_y() + expected_y.size()) == expected_y &&
        std::vector<uint8_t>(packed->data_u(), packed->data_u() + expected_u.size()) == expected_u &&
        std::vector<uint8_t>(packed->data_v(), packed->data_v() + expected_v.size()) == expected_v,
        "tight planes must be fully initialized and independent of source memory")) return 1;

    std::vector<livekit::render::OwnedI420Frame::Ptr> retained;
    for (uint8_t value = 0; value != 32; ++value) {
        const uint8_t y[]{value, value, value, value}, u[]{value}, v[]{value};
        retained.push_back(livekit::render::OwnedI420Frame::CopyFromPlanes(2, 2, y, 2, u, 1, v, 1));
    }
    for (size_t index = 0; index != retained.size(); ++index) {
        const auto& held = retained[index];
        if (!Expect(held && held->data_y()[0] == index && held->data_y()[3] == index &&
            held->data_u()[0] == index && held->data_v()[0] == index,
            "retained frames must remain immutable after storage cache exhaustion")) return 1;
    }
    const auto recycled_pixels = retained.front()->data_y();
    std::weak_ptr<const livekit::render::OwnedI420Frame> released_frame = retained.front();
    retained.front().reset();
    const uint8_t next_y[]{99, 99, 99, 99}, next_uv[]{99};
    auto reused = livekit::render::OwnedI420Frame::CopyFromPlanes(2, 2, next_y, 2, next_uv, 1, next_uv, 1);
    if (!Expect(released_frame.expired() && reused && reused->data_y() == recycled_pixels &&
        reused->data_y()[3] == 99 && retained[1]->data_y()[0] == 1,
        "only released pixel storage may be reused; cached storage must not retain frame objects")) return 1;

    livekit::render::OwnedI420Frame::Ptr after_producer_exit;
    std::thread producer([&] {
        after_producer_exit = livekit::render::OwnedI420Frame::CopyFromPlanes(
            2, 2, next_y, 2, next_uv, 1, next_uv, 1);
    });
    producer.join();
    if (!Expect(after_producer_exit && after_producer_exit->data_y()[3] == 99 &&
        after_producer_exit->data_v()[0] == 99,
        "outstanding frame storage must survive producer thread and cache destruction")) return 1;

    livekit::Track track("TR_I420", "render-test", livekit::TrackKind::Video);
    int first_calls = 0;
    int second_calls = 0;
    auto first = track.subscribeI420VideoFrames([&](livekit::render::OwnedI420Frame::Ptr received) {
        if (received == frame) ++first_calls;
    });
    auto second = track.subscribeI420VideoFrames([&](livekit::render::OwnedI420Frame::Ptr received) {
        if (received == frame) ++second_calls;
    });
    track.notifyI420VideoFrame(frame);
    if (!Expect(first_calls == 1 && second_calls == 1, "all active subscriptions must receive the frame")) {
        return 1;
    }

    first.reset();
    track.notifyI420VideoFrame(frame);
    if (!Expect(first_calls == 1 && second_calls == 2, "reset must cancel only its own subscription")) {
        return 1;
    }

    {
        auto scoped = track.subscribeI420VideoFrames([&](livekit::render::OwnedI420Frame::Ptr) {
            ++first_calls;
        });
        if (!Expect(scoped.active(), "new subscription must report active")) {
            return 1;
        }
    }
    track.notifyI420VideoFrame(frame);
    if (!Expect(first_calls == 1 && second_calls == 3, "destructor must cancel a subscription")) {
        return 1;
    }

    std::cout << "[OwnedI420FrameTest] PASS" << std::endl;
    return 0;
}
