#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>
#include "dshow_types.h"
#include "video_source.h"

namespace livekit {

class DShowSampleGrabberCallback;

class DShowVideoCapture : public std::enable_shared_from_this<DShowVideoCapture> {
public:
    static std::shared_ptr<DShowVideoCapture> Create();

    DShowVideoCapture();
    ~DShowVideoCapture();

    DShowVideoCapture(const DShowVideoCapture&) = delete;
    DShowVideoCapture& operator=(const DShowVideoCapture&) = delete;

    // 初始化视频捕获配置
    bool Init(const DShowCaptureConfig& config, std::shared_ptr<VideoSource> video_source);

    // 启动摄像头捕获
    bool Start();

    // 停止视频捕获
    void Stop();

    // 检查运行状态
    bool IsRunning() const noexcept { return is_running_.load(); }

    // 帧率与抓帧统计
    uint64_t GetCapturedFramesCount() const noexcept { return captured_frames_count_.load(); }
    double GetActualFps() const noexcept;

    // 获取实际协商的分辨率与格式
    int GetNegotiatedWidth() const noexcept { return actual_width_; }
    int GetNegotiatedHeight() const noexcept { return actual_height_; }
    DShowPixelFormat GetNegotiatedFormat() const noexcept { return negotiated_format_; }

    // 供 SampleGrabber 回调交付原始缓冲帧
    void OnRawFrameReceived(double sample_time, const uint8_t* buffer, long buffer_len);

    DShowCaptureConfig GetConfig() const;

private:
    bool BuildFilterGraph();
    void TeardownFilterGraph();

    DShowCaptureConfig config_;
    std::shared_ptr<VideoSource> video_source_;

    std::atomic<bool> is_running_{false};
    std::atomic<uint64_t> captured_frames_count_{0};
    std::chrono::steady_clock::time_point start_time_;

    std::mutex state_mutex_;

    // DirectShow COM 接口
    Microsoft::WRL::ComPtr<ICaptureGraphBuilder2> capture_graph_builder_;
    Microsoft::WRL::ComPtr<IGraphBuilder> graph_builder_;
    Microsoft::WRL::ComPtr<IMediaControl> media_control_;
    Microsoft::WRL::ComPtr<IMediaEventEx> media_event_;
    Microsoft::WRL::ComPtr<IBaseFilter> source_filter_;
    Microsoft::WRL::ComPtr<IBaseFilter> grabber_filter_;
    Microsoft::WRL::ComPtr<IBaseFilter> null_renderer_;

    // 协商后的实际视频参数
    int actual_width_{0};
    int actual_height_{0};
    bool flip_vertically_{false};
    DShowPixelFormat negotiated_format_{DShowPixelFormat::RGB24};

    Microsoft::WRL::ComPtr<DShowSampleGrabberCallback> grabber_callback_;
};

} // namespace livekit
