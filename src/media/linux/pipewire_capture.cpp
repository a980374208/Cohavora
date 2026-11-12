#include "pipewire_capture.h"

#include <pipewire/pipewire.h>
#include <libyuv.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <unistd.h>

namespace livekit::linux_capture {
namespace {
constexpr std::uint64_t kMaximumPixels = 8192ULL * 4320ULL;

// The statically linked WebRTC SDK defines weak global pw_* forwarding stubs.
// They are uninitialized unless its own portal backend was started. Resolve
// every non-inline API on the fixed system DSO handle, never process scope.
class PipeWireApi final {
public:
    decltype(&::pw_init) pw_init = nullptr;
    decltype(&::pw_thread_loop_new) pw_thread_loop_new = nullptr;
    decltype(&::pw_thread_loop_get_loop) pw_thread_loop_get_loop = nullptr;
    decltype(&::pw_context_new) pw_context_new = nullptr;
    decltype(&::pw_context_connect_fd) pw_context_connect_fd = nullptr;
    decltype(&::pw_properties_new) pw_properties_new = nullptr;
    decltype(&::pw_stream_new) pw_stream_new = nullptr;
    decltype(&::pw_stream_add_listener) pw_stream_add_listener = nullptr;
    decltype(&::pw_stream_connect) pw_stream_connect = nullptr;
    decltype(&::pw_thread_loop_start) pw_thread_loop_start = nullptr;
    decltype(&::pw_thread_loop_stop) pw_thread_loop_stop = nullptr;
    decltype(&::pw_stream_disconnect) pw_stream_disconnect = nullptr;
    decltype(&::pw_stream_destroy) pw_stream_destroy = nullptr;
    decltype(&::pw_core_disconnect) pw_core_disconnect = nullptr;
    decltype(&::pw_context_destroy) pw_context_destroy = nullptr;
    decltype(&::pw_thread_loop_destroy) pw_thread_loop_destroy = nullptr;
    decltype(&::pw_stream_update_params) pw_stream_update_params = nullptr;
    decltype(&::pw_stream_dequeue_buffer) pw_stream_dequeue_buffer = nullptr;
    decltype(&::pw_stream_queue_buffer) pw_stream_queue_buffer = nullptr;

    static const PipeWireApi *Get() {
        // Retain the successful DSO handle for the process lifetime. It must
        // outlive all captures, PipeWire loops and independent SDK users.
        static const PipeWireApi api;
        return api.available_ ? &api : nullptr;
    }

private:
    template <typename Function>
    bool Resolve(Function &function, const char *name) {
        function = reinterpret_cast<Function>(::dlsym(library_, name));
        return function != nullptr;
    }

    PipeWireApi() {
        library_ = ::dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library_) return;
        bool complete = true;
        complete = Resolve(pw_init, "pw_init") && complete;
        complete = Resolve(pw_thread_loop_new, "pw_thread_loop_new") && complete;
        complete = Resolve(pw_thread_loop_get_loop, "pw_thread_loop_get_loop") && complete;
        complete = Resolve(pw_context_new, "pw_context_new") && complete;
        complete = Resolve(pw_context_connect_fd, "pw_context_connect_fd") && complete;
        complete = Resolve(pw_properties_new, "pw_properties_new") && complete;
        complete = Resolve(pw_stream_new, "pw_stream_new") && complete;
        complete = Resolve(pw_stream_add_listener, "pw_stream_add_listener") && complete;
        complete = Resolve(pw_stream_connect, "pw_stream_connect") && complete;
        complete = Resolve(pw_thread_loop_start, "pw_thread_loop_start") && complete;
        complete = Resolve(pw_thread_loop_stop, "pw_thread_loop_stop") && complete;
        complete = Resolve(pw_stream_disconnect, "pw_stream_disconnect") && complete;
        complete = Resolve(pw_stream_destroy, "pw_stream_destroy") && complete;
        complete = Resolve(pw_core_disconnect, "pw_core_disconnect") && complete;
        complete = Resolve(pw_context_destroy, "pw_context_destroy") && complete;
        complete = Resolve(pw_thread_loop_destroy, "pw_thread_loop_destroy") && complete;
        complete = Resolve(pw_stream_update_params, "pw_stream_update_params") && complete;
        complete = Resolve(pw_stream_dequeue_buffer, "pw_stream_dequeue_buffer") && complete;
        complete = Resolve(pw_stream_queue_buffer, "pw_stream_queue_buffer") && complete;
        if (!complete) {
            ::dlclose(library_);
            library_ = nullptr;
            return;
        }
        pw_init(nullptr, nullptr);
        available_ = true;
    }

    void *library_ = nullptr;
    bool available_ = false;
};


bool CpuFormat(std::uint32_t format) {
    return format == SPA_VIDEO_FORMAT_BGRA || format == SPA_VIDEO_FORMAT_BGRx ||
        format == SPA_VIDEO_FORMAT_RGBA || format == SPA_VIDEO_FORMAT_RGBx;
}
}

bool SupportedCpuFormat(const spa_video_info_raw &info) {
    return CpuFormat(info.format) && info.size.width >= 2 && info.size.height >= 2 &&
        info.size.width <= 16384 && info.size.height <= 16384 &&
        std::uint64_t(info.size.width) * info.size.height <= kMaximumPixels &&
        !(info.flags & SPA_VIDEO_FLAG_MODIFIER) && info.modifier == 0;
}

std::unique_ptr<CpuFrame> CopyCpuBuffer(const spa_buffer &buffer, const spa_video_info_raw &format) {
    if (!SupportedCpuFormat(format) || buffer.n_datas != 1 || !buffer.datas) return {};
    const auto &data = buffer.datas[0];
    if ((data.type != SPA_DATA_MemPtr && data.type != SPA_DATA_MemFd) ||
        !data.data || !data.chunk || data.chunk->size == 0 ||
        (data.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) return {};
    if (const auto *header = static_cast<const spa_meta_header*>(
            spa_buffer_find_meta_data(&buffer, SPA_META_Header, sizeof(spa_meta_header)));
        header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED)) return {};
    const auto stride = data.chunk->stride;
    const auto width = format.size.width;
    const auto height = format.size.height;
    if (stride <= 0 || std::uint64_t(stride) < std::uint64_t(width) * 4) return {};
    const std::uint64_t extent = std::uint64_t(stride) * (height - 1) + std::uint64_t(width) * 4;
    const std::uint64_t offset = data.chunk->offset;
    if (offset > data.maxsize || extent > data.maxsize - offset || extent > data.chunk->size) return {};
    std::uint32_t x = 0, y = 0, cropped_width = width, cropped_height = height;
    if (const auto *crop = static_cast<const spa_meta_region*>(
            spa_buffer_find_meta_data(&buffer, SPA_META_VideoCrop, sizeof(spa_meta_region)));
        crop && spa_meta_region_is_valid(crop)) {
        if (crop->region.position.x < 0 || crop->region.position.y < 0) return {};
        x = static_cast<std::uint32_t>(crop->region.position.x);
        y = static_cast<std::uint32_t>(crop->region.position.y);
        cropped_width = crop->region.size.width;
        cropped_height = crop->region.size.height;
        if (cropped_width < 2 || cropped_height < 2 || x > width || y > height ||
            cropped_width > width - x || cropped_height > height - y) return {};
    }
    std::uint32_t transform = 0;
    if (const auto *meta = static_cast<const spa_meta_videotransform*>(
            spa_buffer_find_meta_data(&buffer, SPA_META_VideoTransform, sizeof(spa_meta_videotransform)))) {
        if (meta->transform > SPA_META_TRANSFORMATION_Flipped270) return {};
        transform = meta->transform;
    }
    auto frame = std::make_unique<CpuFrame>();
    frame->width = static_cast<int>(cropped_width);
    frame->height = static_cast<int>(cropped_height);
    frame->format = (format.format == SPA_VIDEO_FORMAT_RGBA || format.format == SPA_VIDEO_FORMAT_RGBx)
        ? CpuFrame::Format::Rgba : CpuFrame::Format::Bgra;
    frame->transform = transform;
    frame->pixels.resize(std::size_t(cropped_width) * cropped_height * 4);
    const auto *source = static_cast<const std::uint8_t*>(data.data) + offset +
        std::uint64_t(y) * stride + std::uint64_t(x) * 4;
    const auto row_bytes = std::size_t(cropped_width) * 4;
    for (std::uint32_t row = 0; row != cropped_height; ++row)
        std::memcpy(frame->pixels.data() + row * row_bytes, source + std::uint64_t(row) * stride, row_bytes);
    return frame;
}

VideoFrame ConvertCpuFrame(const CpuFrame &pixels) {
    if (pixels.width < 2 || pixels.height < 2 || pixels.width > 16384 || pixels.height > 16384 ||
        std::uint64_t(pixels.width) * pixels.height > kMaximumPixels ||
        pixels.pixels.size() != std::size_t(pixels.width) * pixels.height * 4 ||
        pixels.transform > SPA_META_TRANSFORMATION_Flipped270)
        throw std::runtime_error("capture_pixels_invalid");
    auto input = VideoFrame::create(pixels.width, pixels.height, VideoBufferType::I420);
    auto planes = input.planeInfos();
    const auto convert = pixels.format == CpuFrame::Format::Bgra ? libyuv::ARGBToI420 : libyuv::ABGRToI420;
    if (convert(pixels.pixels.data(), pixels.width * 4,
            reinterpret_cast<std::uint8_t*>(planes[0].data_ptr), planes[0].stride,
            reinterpret_cast<std::uint8_t*>(planes[1].data_ptr), planes[1].stride,
            reinterpret_cast<std::uint8_t*>(planes[2].data_ptr), planes[2].stride,
            pixels.width, pixels.height) != 0)
        throw std::runtime_error("capture_conversion_failed");

    // SPA describes the transform already applied to the buffer: horizontal
    // reflection first, then counter-clockwise rotation. Undo those operations
    // in reverse order: clockwise rotation first, then horizontal reflection.
    const auto rotation = pixels.transform & 3u;
    if (rotation) {
        const bool swap = rotation == 1 || rotation == 3;
        auto rotated = VideoFrame::create(swap ? input.height() : input.width(),
            swap ? input.width() : input.height(), VideoBufferType::I420);
        const auto source = input.planeInfos();
        const auto target = rotated.planeInfos();
        const auto mode = rotation == 1 ? libyuv::kRotate90 : rotation == 2 ? libyuv::kRotate180 : libyuv::kRotate270;
        if (libyuv::I420Rotate(
                reinterpret_cast<const std::uint8_t*>(source[0].data_ptr), source[0].stride,
                reinterpret_cast<const std::uint8_t*>(source[1].data_ptr), source[1].stride,
                reinterpret_cast<const std::uint8_t*>(source[2].data_ptr), source[2].stride,
                reinterpret_cast<std::uint8_t*>(target[0].data_ptr), target[0].stride,
                reinterpret_cast<std::uint8_t*>(target[1].data_ptr), target[1].stride,
                reinterpret_cast<std::uint8_t*>(target[2].data_ptr), target[2].stride,
                input.width(), input.height(), mode) != 0)
            throw std::runtime_error("capture_transform_failed");
        input = std::move(rotated);
    }
    if (pixels.transform & 4u) {
        auto mirrored = VideoFrame::create(input.width(), input.height(), VideoBufferType::I420);
        const auto source = input.planeInfos();
        const auto target = mirrored.planeInfos();
        if (libyuv::I420Mirror(
                reinterpret_cast<const std::uint8_t*>(source[0].data_ptr), source[0].stride,
                reinterpret_cast<const std::uint8_t*>(source[1].data_ptr), source[1].stride,
                reinterpret_cast<const std::uint8_t*>(source[2].data_ptr), source[2].stride,
                reinterpret_cast<std::uint8_t*>(target[0].data_ptr), target[0].stride,
                reinterpret_cast<std::uint8_t*>(target[1].data_ptr), target[1].stride,
                reinterpret_cast<std::uint8_t*>(target[2].data_ptr), target[2].stride,
                input.width(), input.height()) != 0)
            throw std::runtime_error("capture_transform_failed");
        input = std::move(mirrored);
    }
    return input;
}

class PipeWireCapture::Impl final {
public:
    ~Impl() { Stop(); }

    bool Start(std::uint32_t node, int fd, int fps) {
        Stop();
        api_ = PipeWireApi::Get();
        if (!api_) return Fail(PipeWireFailure::Connect);
        fps_.store(fps, std::memory_order_relaxed);
        failure_.store(PipeWireFailure::None);
        ready_.store(false);
        loop_ = api_->pw_thread_loop_new("cohavora-screencast", nullptr);
        if (!loop_) return Fail(PipeWireFailure::Connect);
        context_ = api_->pw_context_new(api_->pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (!context_) return Fail(PipeWireFailure::Connect);
        const int owned_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (owned_fd < 0) return Fail(PipeWireFailure::Connect);
        // pw_context_connect_fd takes ownership, including its error path.
        core_ = api_->pw_context_connect_fd(context_, owned_fd, nullptr, 0);
        if (!core_) return Fail(PipeWireFailure::Connect);
        core_events_ = {};
        core_events_.version = PW_VERSION_CORE_EVENTS;
        core_events_.error = [](void *data, std::uint32_t, int, int, const char *) {
            static_cast<Impl*>(data)->failure_.store(PipeWireFailure::Stream);
        };
        pw_core_add_listener(core_, &core_hook_, &core_events_, this);
        stream_ = api_->pw_stream_new(core_, "Cohavora screen video", api_->pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen", nullptr));
        if (!stream_) return Fail(PipeWireFailure::Connect);
        stream_events_ = {};
        stream_events_.version = PW_VERSION_STREAM_EVENTS;
        stream_events_.state_changed = [](void *data, pw_stream_state old_state, pw_stream_state state, const char *) {
            auto *self = static_cast<Impl*>(data);
            if (state == PW_STREAM_STATE_ERROR ||
                (state == PW_STREAM_STATE_UNCONNECTED && old_state != PW_STREAM_STATE_UNCONNECTED))
                self->failure_.store(PipeWireFailure::Stream);
            self->streaming_ = state == PW_STREAM_STATE_STREAMING;
            self->ready_.store(self->streaming_ && self->format_valid_);
        };
        stream_events_.param_changed = [](void *data, std::uint32_t id, const spa_pod *param) {
            static_cast<Impl*>(data)->FormatChanged(id, param);
        };
        stream_events_.process = [](void *data) { static_cast<Impl*>(data)->Process(); };
        api_->pw_stream_add_listener(stream_, &stream_hook_, &stream_events_, this);

        std::uint8_t storage[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
        const spa_rectangle preferred_size = SPA_RECTANGLE(1920, 1080);
        const spa_rectangle minimum_size = SPA_RECTANGLE(2, 2);
        const spa_rectangle maximum_size = SPA_RECTANGLE(16384, 16384);
        // Keep the producer capable of every UI preset. The local copy gate
        // selects 15/20/30 fps without renegotiating the authorized stream.
        const spa_fraction preferred_rate = SPA_FRACTION(30, 1);
        const spa_fraction minimum_rate = SPA_FRACTION(0, 1);
        const spa_fraction maximum_rate = SPA_FRACTION(30, 1);
        const spa_pod *params[] = {
            static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
                    SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx,
                    SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx),
                SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
                    &preferred_size, &minimum_size, &maximum_size),
                SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
                    &preferred_rate, &minimum_rate, &maximum_rate)))
        };
        // Deliberately omit DMA-BUF modifiers: the first product backend is CPU
        // capture only and never performs an implicit graphics readback.
        const int result = api_->pw_stream_connect(stream_, PW_DIRECTION_INPUT, node,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
            params, 1);
        if (result < 0 || api_->pw_thread_loop_start(loop_) < 0) return Fail(PipeWireFailure::Connect);
        loop_started_ = true;
        return true;
    }

    void Stop() {
        ready_.store(false);
        // Callers are on the GLib owner, never on this PipeWire loop. Stopping
        // the loop joins every buffer callback before any object is released.
        if (loop_started_) {
            api_->pw_thread_loop_stop(loop_);
            loop_started_ = false;
        }
        if (stream_) {
            spa_hook_remove(&stream_hook_);
            api_->pw_stream_disconnect(stream_);
            api_->pw_stream_destroy(stream_);
            stream_ = nullptr;
        }
        if (core_) {
            spa_hook_remove(&core_hook_);
            api_->pw_core_disconnect(core_);
            core_ = nullptr;
        }
        if (context_) { api_->pw_context_destroy(context_); context_ = nullptr; }
        if (loop_) { api_->pw_thread_loop_destroy(loop_); loop_ = nullptr; }
        format_valid_ = false;
        streaming_ = false;
        next_copy_ = {};
        std::lock_guard lock(frame_mutex_);
        latest_.reset();
    }

    bool Fail(PipeWireFailure reason) {
        Stop();
        failure_.store(reason);
        return false;
    }

    void FormatChanged(std::uint32_t id, const spa_pod *param) {
        if (id != SPA_PARAM_Format || !param) return;
        format_valid_ = false;
        ready_.store(false);
        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0 || !SupportedCpuFormat(info)) {
            failure_.store(PipeWireFailure::Format);
            ready_.store(false);
            return;
        }
        format_ = info;
        format_valid_ = true;
        ready_.store(streaming_);
        std::uint8_t storage[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
        const spa_pod *params[] = {
            static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
                SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
                SPA_PARAM_BUFFERS_size, SPA_POD_Int(info.size.width * info.size.height * 4),
                SPA_PARAM_BUFFERS_stride, SPA_POD_Int(info.size.width * 4),
                SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
                    (1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)))),
            static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)))),
            static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
                SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_region)))),
            static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoTransform),
                SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_videotransform))))
        };
        if (api_->pw_stream_update_params(stream_, params, 4) < 0)
            failure_.store(PipeWireFailure::Format);
    }

    void Process() noexcept {
        pw_buffer *latest = nullptr;
        while (auto *buffer = api_->pw_stream_dequeue_buffer(stream_)) {
            if (latest) api_->pw_stream_queue_buffer(stream_, latest);
            latest = buffer;
        }
        if (!latest) return;
        try { CopyBuffer(latest->buffer); } catch (...) { failure_.store(PipeWireFailure::Stream); }
        api_->pw_stream_queue_buffer(stream_, latest);
    }

    void CopyBuffer(const spa_buffer *buffer) {
        if (!format_valid_ || !buffer || buffer->n_datas != 1) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < next_copy_) return;
        auto frame = CopyCpuBuffer(*buffer, format_);
        if (!frame) return;
        {
            std::lock_guard lock(frame_mutex_);
            latest_ = std::move(frame);
        }
        next_copy_ = now + std::chrono::nanoseconds(1000000000 / std::clamp(fps_.load(), 1, 30));
    }

    const PipeWireApi *api_ = nullptr;
    pw_thread_loop *loop_ = nullptr;
    pw_context *context_ = nullptr;
    pw_core *core_ = nullptr;
    pw_stream *stream_ = nullptr;
    spa_hook core_hook_{};
    spa_hook stream_hook_{};
    pw_core_events core_events_{};
    pw_stream_events stream_events_{};
    bool loop_started_ = false;
    bool streaming_ = false;
    bool format_valid_ = false;
    spa_video_info_raw format_{};
    std::atomic<bool> ready_{false};
    std::atomic<PipeWireFailure> failure_{PipeWireFailure::None};
    std::atomic<int> fps_{20};
    std::chrono::steady_clock::time_point next_copy_{};
    std::mutex frame_mutex_;
    std::unique_ptr<CpuFrame> latest_;
};

PipeWireCapture::PipeWireCapture() : impl_(std::make_unique<Impl>()) {}
PipeWireCapture::~PipeWireCapture() = default;
bool PipeWireCapture::Start(std::uint32_t node, int fd, int fps) { return impl_->Start(node, fd, fps); }
void PipeWireCapture::Stop() { impl_->Stop(); }
void PipeWireCapture::SetFrameRate(int fps) { impl_->fps_.store(fps); }
bool PipeWireCapture::ready() const { return impl_->ready_.load(); }
PipeWireFailure PipeWireCapture::failure() const { return impl_->failure_.load(); }
std::unique_ptr<CpuFrame> PipeWireCapture::TakeFrame() {
    std::lock_guard lock(impl_->frame_mutex_);
    return std::move(impl_->latest_);
}

} // namespace livekit::linux_capture
