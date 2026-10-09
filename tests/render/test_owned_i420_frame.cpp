#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include "core/track.h"
#include "render/owned_i420_frame.h"

namespace {

thread_local bool count_producer_array_allocations = false;
thread_local size_t producer_array_allocations = 0;

} // namespace

// Observe pixel allocations only inside the dedicated producer test. Pointer
// equality alone cannot distinguish cache reuse from allocator address reuse.
void* operator new[](size_t bytes) {
    auto* allocation = ::operator new(bytes);
    if (count_producer_array_allocations) ++producer_array_allocations;
    return allocation;
}

void operator delete[](void* allocation) noexcept {
    ::operator delete(allocation);
}

void operator delete[](void* allocation, size_t) noexcept {
    ::operator delete(allocation);
}

namespace {

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[OwnedI420FrameTest] FAILED: " << message << std::endl;
        return false;
    }
    return true;
}

bool HasUniformPlanes(const livekit::render::OwnedI420Frame::Ptr& frame, uint8_t value) {
    if (!frame) return false;
    const auto luma_bytes = size_t(frame->width()) * frame->height();
    const auto chroma_bytes = size_t(frame->chroma_width()) * frame->chroma_height();
    const auto matches = [value](uint8_t byte) { return byte == value; };
    return std::all_of(frame->data_y(), frame->data_y() + luma_bytes, matches) &&
        std::all_of(frame->data_u(), frame->data_u() + chroma_bytes, matches) &&
        std::all_of(frame->data_v(), frame->data_v() + chroma_bytes, matches);
}

bool TestMixed4KProducerCache() {
    using livekit::render::OwnedI420Frame;
    std::array<OwnedI420Frame::Ptr, 8> retained;
    OwnedI420Frame::Ptr temporary;
    bool succeeded = false;
    // A new producer starts with an empty thread-local cache. Hold every frame
    // while populating 2 UHD + 6 HD blocks: 33,177,600 bytes (31.64 MiB).
    std::thread producer([&] {
        std::vector<uint8_t> uhd_source(size_t(3840) * 2160 * 3 / 2);
        std::vector<uint8_t> hd_source(size_t(1280) * 720 * 3 / 2);
        const auto copy = [&](int width, int height, uint8_t value) {
            auto& source = width == 3840 ? uhd_source : hd_source;
            std::fill(source.begin(), source.end(), value);
            const auto luma_bytes = size_t(width) * height;
            const auto chroma_bytes = luma_bytes / 4;
            return OwnedI420Frame::CopyFromPlanes(width, height, source.data(), width,
                source.data() + luma_bytes, width / 2,
                source.data() + luma_bytes + chroma_bytes, width / 2);
        };
        count_producer_array_allocations = true;
        for (size_t index = 0; index != retained.size(); ++index) {
            const bool uhd = index < 2;
            retained[index] = copy(uhd ? 3840 : 1280, uhd ? 2160 : 720, uint8_t(40 + index));
            if (!Expect(HasUniformPlanes(retained[index], uint8_t(40 + index)),
                    "mixed UHD/HD frames must own every copied plane")) return;
        }
        if (!Expect(retained[0]->stride_y() == 3840 && retained[0]->stride_u() == 1920 &&
                retained[0]->data_u() == retained[0]->data_y() + size_t(3840) * 2160 &&
                retained[0]->data_v() == retained[0]->data_u() + size_t(1920) * 1080,
                "UHD I420 storage must be tightly packed")) return;

        const auto released_hd_pixels = retained[2]->data_y();
        retained[2].reset();
        const auto before_temporary = producer_array_allocations;
        // The released HD slot is available, but replacing it with another UHD
        // block would exceed the byte budget. Retained pixels must stay intact.
        temporary = copy(3840, 2160, 70);
        if (!Expect(temporary && producer_array_allocations > before_temporary &&
                temporary->data_y() != retained[0]->data_y() &&
                temporary->data_y() != retained[1]->data_y(),
                "an over-budget UHD frame must receive independent temporary storage")) return;
        const auto before_hd_reuse = producer_array_allocations;
        retained[2] = copy(1280, 720, 82);
        if (!Expect(retained[2] && retained[2]->data_y() == released_hd_pixels &&
                producer_array_allocations == before_hd_reuse,
                "the mixed-size cache must retain and reuse released HD storage")) return;

        const auto released_uhd_pixels = retained[0]->data_y();
        std::weak_ptr<const OwnedI420Frame> released_uhd_frame = retained[0];
        retained[0].reset();
        const auto before_uhd_reuse = producer_array_allocations;
        retained[0] = copy(3840, 2160, 80);
        if (!Expect(released_uhd_frame.expired() && retained[0] &&
                retained[0]->data_y() == released_uhd_pixels &&
                producer_array_allocations == before_uhd_reuse,
                "released UHD pixels must be reused without retaining the old frame or allocating again")) return;
        std::fill(uhd_source.begin(), uhd_source.end(), 0);
        std::fill(hd_source.begin(), hd_source.end(), 0);
        for (size_t index = 0; index != retained.size(); ++index) {
            const auto expected = uint8_t(index == 0 ? 80 : index == 2 ? 82 : 40 + index);
            if (!Expect(HasUniformPlanes(retained[index], expected),
                    "UHD reuse and budget fallback must not overwrite any retained mixed-size frame")) return;
        }
        if (!Expect(HasUniformPlanes(temporary, 70),
                "over-budget pixels must remain immutable while cached storage is reused")) return;
        count_producer_array_allocations = false;
        succeeded = true;
    });
    producer.join();
    if (!succeeded) return false;
    for (size_t index = 0; index != retained.size(); ++index) {
        const auto expected = uint8_t(index == 0 ? 80 : index == 2 ? 82 : 40 + index);
        if (!Expect(HasUniformPlanes(retained[index], expected),
                "retained UHD/HD pixels must survive producer thread and cache destruction")) return false;
    }
    return Expect(HasUniformPlanes(temporary, 70),
        "over-budget UHD pixels must survive producer thread destruction");
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

    if (!TestMixed4KProducerCache()) return 1;

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
