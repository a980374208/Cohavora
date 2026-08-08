#include "dshow_capture.h"
#include "dshow_enumerator.h"
#include "media_converters.h"
#include <windows.h>
#include <dvdmedia.h>
#include <wrl/implements.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <iostream>
#include <atomic>

using Microsoft::WRL::ComPtr;

namespace livekit {

#ifndef __ISampleGrabberCB_INTERFACE_DEFINED__
#define __ISampleGrabberCB_INTERFACE_DEFINED__
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample* pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) = 0;
};
#endif

#ifndef __ISampleGrabber_INTERFACE_DEFINED__
#define __ISampleGrabber_INTERFACE_DEFINED__
MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* pCallback, long WhichMethodToCallback) = 0;
};
#endif

static const CLSID CLSID_SampleGrabber_Local =
    { 0xC1F400A0, 0x3F08, 0x11d3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };
static const CLSID CLSID_NullRenderer_Local =
    { 0xC1F400A4, 0x3F08, 0x11d3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };

class DShowSampleGrabberCallback : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    ISampleGrabberCB>
{
public:
    explicit DShowSampleGrabberCallback(std::weak_ptr<DShowVideoCapture> parent)
        : parent_(parent) {}

    STDMETHODIMP SampleCB(double /*SampleTime*/, IMediaSample* /*pSample*/) override {
        return S_OK;
    }

    STDMETHODIMP BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) override {
        if (auto p = parent_.lock()) {
            p->OnRawFrameReceived(SampleTime, pBuffer, BufferLen);
        }
        return S_OK;
    }

private:
    std::weak_ptr<DShowVideoCapture> parent_;
};

static void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat != 0) {
        CoTaskMemFree(mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk != nullptr) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

std::shared_ptr<DShowVideoCapture> DShowVideoCapture::Create() {
    return std::make_shared<DShowVideoCapture>();
}

DShowVideoCapture::DShowVideoCapture() = default;

DShowVideoCapture::~DShowVideoCapture() {
    Stop();
}

double DShowVideoCapture::GetActualFps() const noexcept {
    auto frames = captured_frames_count_.load();
    if (frames == 0) return 0.0;
    auto now = std::chrono::steady_clock::now();
    auto elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time_).count();
    return elapsed_sec > 0.001 ? (frames / elapsed_sec) : 0.0;
}

DShowCaptureConfig DShowVideoCapture::GetConfig() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(state_mutex_));
    return config_;
}

bool DShowVideoCapture::Init(const DShowCaptureConfig& config, std::shared_ptr<VideoSource> video_source) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (is_running_.load()) {
        spdlog::warn("[DShowVideoCapture] Cannot Init while running. Call Stop() first.");
        return false;
    }
    config_ = config;
    video_source_ = video_source;
    actual_width_ = config.width;
    actual_height_ = config.height;
    flip_vertically_ = config.flip_vertically;
    return true;
}

static ComPtr<IPin> GetFilterPin(IBaseFilter* filter, PIN_DIRECTION dir) {
    if (!filter) return nullptr;
    ComPtr<IEnumPins> enum_pins;
    if (FAILED(filter->EnumPins(&enum_pins)) || !enum_pins) return nullptr;
    ComPtr<IPin> pin;
    while (enum_pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION cur_dir;
        if (SUCCEEDED(pin->QueryDirection(&cur_dir)) && cur_dir == dir) {
            return pin;
        }
        pin.Reset();
    }
    return nullptr;
}

bool DShowVideoCapture::BuildFilterGraph() {
    TeardownFilterGraph();

    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&graph_builder_));
    if (FAILED(hr) || !graph_builder_) {
        spdlog::error("[DShowVideoCapture] Failed to create FilterGraph, hr=0x{:08x}", static_cast<uint32_t>(hr));
        return false;
    }

    hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&capture_graph_builder_));
    if (FAILED(hr) || !capture_graph_builder_) {
        spdlog::error("[DShowVideoCapture] Failed to create CaptureGraphBuilder2, hr=0x{:08x}", static_cast<uint32_t>(hr));
        return false;
    }

    hr = capture_graph_builder_->SetFiltergraph(graph_builder_.Get());
    if (FAILED(hr)) return false;

    // 查找目标视频输入设备 Filter
    ComPtr<ICreateDevEnum> dev_enum;
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&dev_enum));
    if (FAILED(hr) || !dev_enum) return false;

    ComPtr<IEnumMoniker> enum_moniker;
    hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enum_moniker, 0);
    if (hr != S_OK || !enum_moniker) {
        spdlog::warn("[DShowVideoCapture] No video capture devices found.");
        return false;
    }

    ComPtr<IMoniker> moniker;
    while (enum_moniker->Next(1, &moniker, nullptr) == S_OK) {
        ComPtr<IPropertyBag> prop_bag;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&prop_bag))) && prop_bag) {
            VARIANT var_name, var_path;
            VariantInit(&var_name);
            VariantInit(&var_path);

            std::string cur_name, cur_path;
            if (SUCCEEDED(prop_bag->Read(L"FriendlyName", &var_name, nullptr)) && var_name.vt == VT_BSTR) {
                int len = WideCharToMultiByte(CP_UTF8, 0, var_name.bstrVal, -1, nullptr, 0, nullptr, nullptr);
                cur_name.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, var_name.bstrVal, -1, &cur_name[0], len, nullptr, nullptr);
                VariantClear(&var_name);
            }
            if (SUCCEEDED(prop_bag->Read(L"DevicePath", &var_path, nullptr)) && var_path.vt == VT_BSTR) {
                int len = WideCharToMultiByte(CP_UTF8, 0, var_path.bstrVal, -1, nullptr, 0, nullptr, nullptr);
                cur_path.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, var_path.bstrVal, -1, &cur_path[0], len, nullptr, nullptr);
                VariantClear(&var_path);
            }

            if (cur_path.empty()) {
                LPOLESTR display_name = nullptr;
                if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr, &display_name)) && display_name) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, display_name, -1, nullptr, 0, nullptr, nullptr);
                    cur_path.resize(len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, display_name, -1, &cur_path[0], len, nullptr, nullptr);
                    CoTaskMemFree(display_name);
                }
            }

            if (config_.device_path.empty() || cur_path == config_.device_path || cur_name == config_.device_path) {
                hr = moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&source_filter_));
                moniker.Reset();
                break;
            }
        }
        moniker.Reset();
    }

    if (!source_filter_) {
        spdlog::error("[DShowVideoCapture] Device not found: {}", config_.device_path);
        return false;
    }

    hr = graph_builder_->AddFilter(source_filter_.Get(), L"Video Capture Source");
    if (FAILED(hr)) return false;

    // 尝试在 Source Filter 的输出 Pin 上设置期望的分辨率与帧率
    ComPtr<IAMStreamConfig> stream_config;
    hr = capture_graph_builder_->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                               source_filter_.Get(), IID_PPV_ARGS(&stream_config));
    if (FAILED(hr) || !stream_config) {
        capture_graph_builder_->FindInterface(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video,
                                              source_filter_.Get(), IID_PPV_ARGS(&stream_config));
    }
    if (!stream_config) {
        auto src_out_pin = GetFilterPin(source_filter_.Get(), PINDIR_OUTPUT);
        if (src_out_pin) {
            src_out_pin.As(&stream_config);
        }
    }

    if (stream_config && config_.width > 0 && config_.height > 0) {
        AM_MEDIA_TYPE* pmt = nullptr;
        if (SUCCEEDED(stream_config->GetFormat(&pmt)) && pmt) {
            if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(pmt->pbFormat);
                vih->bmiHeader.biWidth = config_.width;
                vih->bmiHeader.biHeight = config_.height;
                if (config_.fps > 0) {
                    vih->AvgTimePerFrame = 10000000LL / config_.fps;
                }
                if (config_.preferred_format != DShowPixelFormat::Unknown) {
                    GUID sub = MediaConverters::PixelFormatToSubtype(config_.preferred_format);
                    if (sub != GUID_NULL) {
                        pmt->subtype = sub;
                    }
                }
                stream_config->SetFormat(pmt);
            }
            FreeMediaType(*pmt);
            CoTaskMemFree(pmt);
        }
    }

    // 创建 SampleGrabber Filter
    hr = CoCreateInstance(CLSID_SampleGrabber_Local, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&grabber_filter_));
    if (FAILED(hr) || !grabber_filter_) {
        spdlog::error("[DShowVideoCapture] Failed to create SampleGrabber, hr=0x{:08x}", static_cast<uint32_t>(hr));
        return false;
    }

    hr = graph_builder_->AddFilter(grabber_filter_.Get(), L"Sample Grabber");
    if (FAILED(hr)) return false;

    ComPtr<ISampleGrabber> sample_grabber;
    hr = grabber_filter_.As(&sample_grabber);
    if (FAILED(hr) || !sample_grabber) return false;

    AM_MEDIA_TYPE grabber_mt;
    ZeroMemory(&grabber_mt, sizeof(AM_MEDIA_TYPE));
    grabber_mt.majortype = MEDIATYPE_Video;
    if (config_.preferred_format != DShowPixelFormat::Unknown) {
        grabber_mt.subtype = MediaConverters::PixelFormatToSubtype(config_.preferred_format);
    } else {
        grabber_mt.subtype = GUID_NULL; // 自动接受源 Filter 提供的原生视频子格式 (NV12, YUY2, RGB24 等)
    }
    grabber_mt.formattype = GUID_NULL;

    sample_grabber->SetMediaType(&grabber_mt);
    sample_grabber->SetBufferSamples(FALSE);
    sample_grabber->SetOneShot(FALSE);

    grabber_callback_ = Microsoft::WRL::Make<DShowSampleGrabberCallback>(weak_from_this());
    sample_grabber->SetCallback(grabber_callback_.Get(), 1); // 1 = BufferCB

    // 创建 NullRenderer Filter
    hr = CoCreateInstance(CLSID_NullRenderer_Local, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&null_renderer_));
    if (FAILED(hr) || !null_renderer_) {
        spdlog::error("[DShowVideoCapture] Failed to create NullRenderer, hr=0x{:08x}", static_cast<uint32_t>(hr));
        return false;
    }

    hr = graph_builder_->AddFilter(null_renderer_.Get(), L"Null Renderer");
    if (FAILED(hr)) return false;

    // 智能连接 Filter 管道:
    // 方案 1: 尝试标准 RenderStream (PIN_CATEGORY_CAPTURE)
    hr = capture_graph_builder_->RenderStream(
        &PIN_CATEGORY_CAPTURE,
        &MEDIATYPE_Video,
        source_filter_.Get(),
        grabber_filter_.Get(),
        null_renderer_.Get()
    );

    // 方案 2: 尝试标准 RenderStream (PIN_CATEGORY_PREVIEW)
    if (FAILED(hr)) {
        hr = capture_graph_builder_->RenderStream(
            &PIN_CATEGORY_PREVIEW,
            &MEDIATYPE_Video,
            source_filter_.Get(),
            grabber_filter_.Get(),
            null_renderer_.Get()
        );
    }

    // 方案 3: 尝试无 Category RenderStream
    if (FAILED(hr)) {
        hr = capture_graph_builder_->RenderStream(
            nullptr,
            &MEDIATYPE_Video,
            source_filter_.Get(),
            grabber_filter_.Get(),
            null_renderer_.Get()
        );
    }

    // 方案 4: 直接通过 IGraphBuilder::Connect 进行智能 Pin 直连 (针对 OBS Virtual Camera 等虚拟驱动)
    if (FAILED(hr)) {
        auto src_out = GetFilterPin(source_filter_.Get(), PINDIR_OUTPUT);
        auto grab_in = GetFilterPin(grabber_filter_.Get(), PINDIR_INPUT);
        auto grab_out = GetFilterPin(grabber_filter_.Get(), PINDIR_OUTPUT);
        auto null_in = GetFilterPin(null_renderer_.Get(), PINDIR_INPUT);

        if (src_out && grab_in && grab_out && null_in) {
            hr = graph_builder_->Connect(src_out.Get(), grab_in.Get());
            if (SUCCEEDED(hr)) {
                hr = graph_builder_->Connect(grab_out.Get(), null_in.Get());
            }
        }
    }

    if (FAILED(hr)) {
        spdlog::error("[DShowVideoCapture] RenderStream & Direct Connect failed, hr=0x{:08x}", static_cast<uint32_t>(hr));
        return false;
    }

    // 获取协商后的实际格式与分辨率
    AM_MEDIA_TYPE connected_mt;
    ZeroMemory(&connected_mt, sizeof(AM_MEDIA_TYPE));
    if (SUCCEEDED(sample_grabber->GetConnectedMediaType(&connected_mt))) {
        negotiated_format_ = MediaConverters::SubtypeToPixelFormat(connected_mt.subtype);
        if (connected_mt.formattype == FORMAT_VideoInfo && connected_mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
            auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(connected_mt.pbFormat);
            actual_width_ = vih->bmiHeader.biWidth;
            actual_height_ = std::abs(vih->bmiHeader.biHeight);
            // DirectShow 中 biHeight > 0 表示图像为自下而上存储 (倒置)
            flip_vertically_ = (vih->bmiHeader.biHeight > 0) ^ config_.flip_vertically;
        }
        FreeMediaType(connected_mt);
    }

    graph_builder_.As(&media_control_);
    graph_builder_.As(&media_event_);

    // 关键修复：关闭参考时钟 (SetSyncSource(nullptr))
    // 虚拟摄像头（OBS Virtual Camera）和实时采集源的时间戳基于系统启动时间（如 200,000+ 秒）。
    // 若开启 DirectShow 默认时钟，NullRenderer 会等待时钟赶上时间戳，导致在第 1 帧后永久阻塞！
    ComPtr<IMediaFilter> media_filter;
    if (SUCCEEDED(graph_builder_.As(&media_filter)) && media_filter) {
        media_filter->SetSyncSource(nullptr);
    }

    return true;
}

void DShowVideoCapture::TeardownFilterGraph() {
    if (media_control_) {
        media_control_->Stop();
    }
    media_control_.Reset();
    media_event_.Reset();
    null_renderer_.Reset();
    grabber_filter_.Reset();
    source_filter_.Reset();
    graph_builder_.Reset();
    capture_graph_builder_.Reset();
    grabber_callback_.Reset();
}

bool DShowVideoCapture::Start() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (is_running_.load()) return true;

    HRESULT hr_co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (!BuildFilterGraph()) {
        spdlog::error("[DShowVideoCapture] BuildFilterGraph failed.");
        TeardownFilterGraph();
        if (SUCCEEDED(hr_co)) CoUninitialize();
        return false;
    }

    if (media_control_) {
        // 确保时钟在 Run 之前已彻底禁用
        ComPtr<IMediaFilter> media_filter;
        if (SUCCEEDED(graph_builder_.As(&media_filter)) && media_filter) {
            media_filter->SetSyncSource(nullptr);
        }
        HRESULT hr = media_control_->Run();
        if (FAILED(hr)) {
            spdlog::error("[DShowVideoCapture] MediaControl->Run() failed, hr=0x{:08x}", static_cast<uint32_t>(hr));
            TeardownFilterGraph();
            if (SUCCEEDED(hr_co)) CoUninitialize();
            return false;
        }
    }

    start_time_ = std::chrono::steady_clock::now();
    captured_frames_count_.store(0);
    is_running_.store(true);
    return true;
}

void DShowVideoCapture::Stop() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!is_running_.load()) return;

    TeardownFilterGraph();
    is_running_.store(false);
}

void DShowVideoCapture::OnRawFrameReceived(double sample_time, const uint8_t* buffer, long buffer_len) {
    if (!buffer || buffer_len <= 0 || !video_source_) return;

    captured_frames_count_.fetch_add(1);

    int w = actual_width_;
    int h = actual_height_;
    if (w <= 0 || h <= 0) return;

    // 输出目标 VideoFrame
    VideoFrame frame;
    if (config_.output_format == VideoBufferType::RGBA) {
        frame = VideoFrame::create(w, h, VideoBufferType::RGBA);
        uint8_t* dst_rgba = frame.data();

        switch (negotiated_format_) {
            case DShowPixelFormat::YUY2:
                MediaConverters::ConvertYUY2ToRGBA(buffer, dst_rgba, w, h, flip_vertically_);
                break;
            case DShowPixelFormat::NV12:
                MediaConverters::ConvertNV12ToRGBA(buffer, dst_rgba, w, h, flip_vertically_);
                break;
            case DShowPixelFormat::RGB24:
                MediaConverters::ConvertRGB24ToRGBA(buffer, dst_rgba, w, h, flip_vertically_);
                break;
            case DShowPixelFormat::ARGB32:
                MediaConverters::ConvertARGB32ToRGBA(buffer, dst_rgba, w, h, flip_vertically_);
                break;
            default:
                MediaConverters::ConvertRGB24ToRGBA(buffer, dst_rgba, w, h, flip_vertically_);
                break;
        }
    } else if (config_.output_format == VideoBufferType::NV12) {
        frame = VideoFrame::create(w, h, VideoBufferType::NV12);
        uint8_t* dst_nv12 = frame.data();

        if (negotiated_format_ == DShowPixelFormat::NV12) {
            std::memcpy(dst_nv12, buffer, w * h * 3 / 2);
        } else if (negotiated_format_ == DShowPixelFormat::YUY2) {
            MediaConverters::ConvertYUY2ToNV12(buffer, dst_nv12, w, h);
        } else {
            // RGB24 -> NV12
            std::vector<uint8_t> temp_rgba(w * h * 4);
            MediaConverters::ConvertRGB24ToRGBA(buffer, temp_rgba.data(), w, h, flip_vertically_);
            // Convert RGBA -> NV12 Y plane
            uint8_t* y_p = dst_nv12;
            uint8_t* uv_p = dst_nv12 + (w * h);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const uint8_t* p = temp_rgba.data() + (y * w + x) * 4;
                    y_p[y * w + x] = static_cast<uint8_t>((66 * p[0] + 129 * p[1] + 25 * p[2] + 128) >> 8) + 16;
                    if ((y % 2 == 0) && (x % 2 == 0)) {
                        int u_val = ((-38 * p[0] - 74 * p[1] + 112 * p[2] + 128) >> 8) + 128;
                        int v_val = ((112 * p[0] - 94 * p[1] - 18 * p[2] + 128) >> 8) + 128;
                        uv_p[(y / 2) * w + x] = static_cast<uint8_t>(u_val);
                        uv_p[(y / 2) * w + x + 1] = static_cast<uint8_t>(v_val);
                    }
                }
            }
        }
    }

    VideoCaptureOptions options;
    options.timestamp_us = static_cast<int64_t>(sample_time * 1000000.0);
    if (options.timestamp_us <= 0) {
        options.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    options.rotation = VideoRotation::VIDEO_ROTATION_0;

    video_source_->captureFrame(frame, options);
}

} // namespace livekit
