#pragma once

#include "../../rtc/video_frame.h"

#include <cstdint>
#include <memory>
#include <vector>

struct spa_buffer;
struct spa_video_info_raw;

namespace livekit::linux_capture {

// Pixels are owned by the application, never by a dequeued PipeWire buffer.
struct CpuFrame {
    enum class Format { Bgra, Rgba };
    int width = 0;
    int height = 0;
    Format format = Format::Bgra;
    std::uint32_t transform = 0;
    std::vector<std::uint8_t> pixels;
};

// Internal buffer boundary shared by the live stream and pixel-contract probe.
// These functions neither own nor retain PipeWire objects or borrowed pixels.
bool SupportedCpuFormat(const spa_video_info_raw&);
std::unique_ptr<CpuFrame> CopyCpuBuffer(const spa_buffer&, const spa_video_info_raw&);
VideoFrame ConvertCpuFrame(const CpuFrame&);

enum class PipeWireFailure { None, Connect, Stream, Format };

class PipeWireCapture final {
public:
    PipeWireCapture();
    ~PipeWireCapture();
    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    // Called on the portal owner thread. The caller retains fd ownership.
    bool Start(std::uint32_t node, int fd, int fps);
    void Stop();
    void SetFrameRate(int fps);
    bool ready() const;
    PipeWireFailure failure() const;
    // A bounded latest-frame mailbox. Conversion happens after this returns.
    std::unique_ptr<CpuFrame> TakeFrame();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace livekit::linux_capture
