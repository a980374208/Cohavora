#include "wgc_window_capture.h"
#include "d3d11_nv12_frame_buffer.h"
#include "desktop_frame_buffer_pool.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "modules/desktop_capture/shared_desktop_frame.h"
#include "modules/desktop_capture/desktop_capture_types.h"
#include "modules/desktop_capture/win/screen_capture_utils.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#pragma comment(lib, "runtimeobject.lib")

namespace livekit {
namespace {
namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;

bool KeepCaptureRuntimeLoaded() {
    // On Windows 11 26100, apartment teardown can unload GraphicsCapture.dll
    // while its asynchronous cleanup still targets code in that image (observed
    // execute AV in GraphicsCapture.dll_unloaded). Keep one loader reference
    // until process exit; never cache apartment-bound factories or sessions.
    // Deliberately no FreeLibrary, including during static destruction.
    static const HMODULE runtime = LoadLibraryExW(
        L"GraphicsCapture.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return runtime != nullptr;
}

bool CapturePlatformSupported(DesktopSourceKind kind) {
    // GetVersionEx (used by WebRTC's version helper) reports Windows 8 for an
    // unmanifested host such as the runtime probe. Read the actual OS version.
    using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    static const auto get_version = reinterpret_cast<GetVersion>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!get_version || get_version(&version) < 0) return false;
    const bool screen = kind == DesktopSourceKind::Screen;
    const bool newer_major = version.dwMajorVersion > 10;
    const bool win11 = newer_major ||
        (version.dwMajorVersion == 10 && version.dwBuildNumber >= 22000);
    if (!newer_major && (version.dwMajorVersion < 10 || version.dwBuildNumber < 18362 ||
        (screen && version.dwBuildNumber < 19041))) return false;
    return webrtc::HasActiveDisplay() || (!screen && win11);
}

class OwnedWgcWindowCapturer final : public webrtc::DesktopCapturer {
public:
    explicit OwnedWgcWindowCapturer(
        std::function<void(DesktopCaptureProbePhase)> report,
        bool screen = false,
        bool gpu_only = false)
        : report_(std::move(report)), screen_(screen), gpu_only_(gpu_only) {}
    ~OwnedWgcWindowCapturer() override { Close(); }
    bool GetSourceList(SourceList*) override { return false; }
    bool SelectSource(SourceId source) override {
        if (session_) return false;
        if (screen_) return webrtc::GetHmonitorFromDeviceIndex(source, &monitor_) &&
            monitor_ && webrtc::IsMonitorValid(monitor_);
        window_ = reinterpret_cast<HWND>(source);
        return IsWindow(window_) != FALSE;
    }
    void Start(Callback* callback) override { callback_ = callback; }
    void CaptureFrame() override {
        if (!callback_) return;
        try {
            if (!session_) Open();
            auto frame = pool_.TryGetNextFrame();
            if (!frame) {
                // WGC can be idle for a static window. Keep one frame, as
                // the WebRTC capturer does, so idle content is not mistaken
                // for capture failure by the enclosing five-second watchdog.
                std::unique_ptr<webrtc::SharedDesktopFrame> cached;
                if (latest_) {
                    cached = latest_->Share();
                    cached->set_capturer_id(latest_->capturer_id());
                }
                callback_->OnCaptureResult(cached ? Result::SUCCESS : Result::ERROR_TEMPORARY,
                    std::move(cached));
                return;
            }
            // Explicit frame Close on every exit, including D3D exceptions.
            struct FrameCloser {
                capture::Direct3D11CaptureFrame& frame;
                ~FrameCloser() { try { if (frame) frame.Close(); } catch (...) {} }
            } closer{frame};
            const auto size = frame.ContentSize();
            if (size.Width <= 0 || size.Height <= 0 || size.Width > 16384 || size.Height > 16384)
                winrt::throw_hresult(E_INVALIDARG);
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            const int width = std::min(size.Width, static_cast<int>(description.Width));
            const int height = std::min(size.Height, static_cast<int>(description.Height));

            if (gpu_only_) {
                auto gpu_shared = gpu_buffers_.CaptureTexture(
                    device_.get(),
                    context_.get(),
                    adapter_luid_,
                    device_context_mutex_,
                    texture.get(),
                    width,
                    height,
                    false);
                texture = nullptr;
                access = nullptr;
                frame.Close();
                frame = nullptr;
                if (size.Width != pool_width_ || size.Height != pool_height_) {
                    pool_.Recreate(winrt_device_, directx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
                    pool_width_ = size.Width; pool_height_ = size.Height;
                }
                if (!gpu_shared) {
                    std::unique_ptr<webrtc::SharedDesktopFrame> cached;
                    if (latest_) {
                        cached = latest_->Share();
                        cached->set_capturer_id(latest_->capturer_id());
                    }
                    callback_->OnCaptureResult(cached ? Result::SUCCESS : Result::ERROR_TEMPORARY,
                        std::move(cached));
                    return;
                }
                gpu_shared->set_capturer_id(
                    webrtc::DesktopCapturerId::kWgcCapturerWin | kD3d11TextureDesktopFrameCapturerIdBit);
                latest_ = std::move(gpu_shared);
                auto delivered = latest_->Share();
                delivered->set_capturer_id(latest_->capturer_id());
                callback_->OnCaptureResult(Result::SUCCESS, std::move(delivered));
                return;
            }

            if (!staging_ || width != width_ || height != height_) {
                staging_ = nullptr;
                description.Width = width;
                description.Height = height;
                description.MipLevels = description.ArraySize = 1;
                description.SampleDesc = {1, 0};
                description.Usage = D3D11_USAGE_STAGING;
                description.BindFlags = description.MiscFlags = 0;
                description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                winrt::check_hresult(device_->CreateTexture2D(&description, nullptr, staging_.put()));
                width_ = width; height_ = height;
            }
            auto* output = buffers_.Acquire(webrtc::DesktopSize(width, height));
            if (!output) {
                // Both slots are pinned by readers. Drop this fresh frame
                // without waiting for consumers or allocating spill buffers.
                callback_->OnCaptureResult(latest_ ? Result::SUCCESS : Result::ERROR_TEMPORARY,
                    latest_ ? latest_->Share() : nullptr);
                return;
            }
            D3D11_BOX box{0, 0, 0, static_cast<UINT>(width), static_cast<UINT>(height), 1};
            context_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, texture.get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped));
            for (int row = 0; row < height; ++row)
                std::memcpy(output->data() + row * output->stride(),
                    static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch, width * 4);
            context_->Unmap(staging_.get(), 0);
            texture = nullptr;
            access = nullptr;
            frame.Close();
            frame = nullptr;
            if (size.Width != pool_width_ || size.Height != pool_height_) {
                pool_.Recreate(winrt_device_, directx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
                pool_width_ = size.Width; pool_height_ = size.Height;
            }
            output->set_capturer_id(webrtc::DesktopCapturerId::kWgcCapturerWin);
            latest_ = output->Share();
            callback_->OnCaptureResult(Result::SUCCESS, latest_->Share());
        } catch (...) {
            Close();
            callback_->OnCaptureResult(Result::ERROR_PERMANENT, nullptr);
        }
    }
private:
    void Open() {
        if (!KeepCaptureRuntimeLoaded()) winrt::throw_hresult(E_NOINTERFACE);
        // A capture worker's apartment is recreated on each Start. Do not put
        // activation factories in C++/WinRT's process-wide cache across teardown.
        const auto factory = winrt::try_get_activation_factory<capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        if (!factory) winrt::throw_hresult(E_NOINTERFACE);
        winrt::check_hresult(screen_
            ? factory->CreateForMonitor(monitor_, winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item_))
            : factory->CreateForWindow(window_, winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item_)));
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            device_.put(), nullptr, context_.put()));
        QueryD3d11AdapterLuid(device_.get(), &adapter_luid_);
        device_context_mutex_ = std::make_shared<std::mutex>();
        auto dxgi = device_.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        winrt_device_ = inspectable.as<directx::Direct3D11::IDirect3DDevice>();
        const auto size = item_.Size();
        pool_width_ = size.Width; pool_height_ = size.Height;
        const auto pool_factory = winrt::try_get_activation_factory<capture::Direct3D11CaptureFramePool,
            capture::IDirect3D11CaptureFramePoolStatics2>();
        if (!pool_factory) winrt::throw_hresult(E_NOINTERFACE);
        pool_ = pool_factory.CreateFreeThreaded(winrt_device_,
            directx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        session_ = pool_.CreateCaptureSession(item_);
        session_.StartCapture();
    }
    void Report(DesktopCaptureProbePhase phase) noexcept {
        try { if (report_) report_(phase); } catch (...) {}
    }
    void Close() noexcept {
        // Stop the producer before closing its pool. COM Release alone does not
        // express the WinRT session's shutdown contract. No event handler or
        // external consumer retains a capture frame, pool, surface or texture.
        if (session_) {
            try { session_.Close(); Report(DesktopCaptureProbePhase::SessionClosed); }
            catch (...) { Report(DesktopCaptureProbePhase::CloseFailed); }
            session_ = nullptr;
        }
        if (pool_) {
            try { pool_.Close(); Report(DesktopCaptureProbePhase::FramePoolClosed); }
            catch (...) { Report(DesktopCaptureProbePhase::CloseFailed); }
            pool_ = nullptr;
        }
        item_ = nullptr;
        latest_.reset();
        buffers_.Clear();
        gpu_buffers_.Clear();
        staging_ = nullptr;
        if (context_) { context_->ClearState(); context_->Flush(); }
        context_ = nullptr;
        if (winrt_device_) {
            try { winrt_device_.Close(); }
            catch (...) { Report(DesktopCaptureProbePhase::CloseFailed); }
        }
        winrt_device_ = nullptr;
        const bool had_device = static_cast<bool>(device_);
        device_ = nullptr;
        device_context_mutex_.reset();
        if (had_device) Report(DesktopCaptureProbePhase::D3dReleased);
    }
    HWND window_ = nullptr;
    HMONITOR monitor_ = nullptr;
    Callback* callback_ = nullptr;
    std::function<void(DesktopCaptureProbePhase)> report_;
    bool screen_ = false;
    bool gpu_only_ = false;
    capture::GraphicsCaptureItem item_{nullptr};
    capture::Direct3D11CaptureFramePool pool_{nullptr};
    capture::GraphicsCaptureSession session_{nullptr};
    directx::Direct3D11::IDirect3DDevice winrt_device_{nullptr};
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11Texture2D> staging_;
    LUID adapter_luid_{};
    std::shared_ptr<std::mutex> device_context_mutex_;
    std::unique_ptr<webrtc::SharedDesktopFrame> latest_;
    DesktopFrameBufferPool buffers_;
    D3d11DesktopFrameTexturePool gpu_buffers_;
    int width_ = 0, height_ = 0, pool_width_ = 0, pool_height_ = 0;
};

// Project-owned DXGI Output Duplication GPU capturer for full-screen capture.
// Copies the desktop texture on the GPU into a bounded 2-slot GPU texture pool
// and releases the DXGI desktop frame immediately without staging CPU readback.
class OwnedDxgiScreenGpuCapturer final : public webrtc::DesktopCapturer {
public:
    explicit OwnedDxgiScreenGpuCapturer(std::function<void(DesktopCaptureProbePhase)> report)
        : report_(std::move(report)) {}
    ~OwnedDxgiScreenGpuCapturer() override { Close(); }

    bool GetSourceList(SourceList*) override { return false; }

    bool SelectSource(SourceId source) override {
        if (duplication_) return false;
        HMONITOR monitor = nullptr;
        if (!webrtc::GetHmonitorFromDeviceIndex(source, &monitor) || !monitor ||
            !webrtc::IsMonitorValid(monitor)) {
            return false;
        }
        monitor_ = monitor;
        source_id_ = source;
        return OpenDuplication();
    }

    void Start(Callback* callback) override { callback_ = callback; }

    void CaptureFrame() override {
        if (!callback_) return;
        if (!duplication_ && !OpenDuplication()) {
            callback_->OnCaptureResult(Result::ERROR_PERMANENT, nullptr);
            return;
        }
        const auto start_time = std::chrono::steady_clock::now();
        DXGI_OUTDUPL_FRAME_INFO frame_info{};
        Microsoft::WRL::ComPtr<IDXGIResource> resource;
        HRESULT hr = duplication_->AcquireNextFrame(
            latest_ ? 0 : 100, &frame_info, resource.GetAddressOf());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (latest_) {
                auto shared = latest_->Share();
                shared->set_capture_time_ms(0);
                shared->set_capturer_id(latest_->capturer_id());
                callback_->OnCaptureResult(Result::SUCCESS, std::move(shared));
            } else {
                callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
            }
            return;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL ||
            hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            Close();
            callback_->OnCaptureResult(Result::ERROR_PERMANENT, nullptr);
            return;
        }
        if (FAILED(hr) || !resource) {
            std::unique_ptr<webrtc::SharedDesktopFrame> cached;
            if (latest_) {
                cached = latest_->Share();
                cached->set_capturer_id(latest_->capturer_id());
            }
            callback_->OnCaptureResult(
                cached ? Result::SUCCESS : Result::ERROR_TEMPORARY,
                std::move(cached));
            return;
        }

        struct DuplFrameReleaser {
            IDXGIOutputDuplication* dupl;
            ~DuplFrameReleaser() {
                if (dupl) dupl->ReleaseFrame();
            }
        } releaser{duplication_.Get()};

        Microsoft::WRL::ComPtr<ID3D11Texture2D> desktop_tex;
        if (FAILED(resource.As(&desktop_tex)) || !desktop_tex) {
            callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
            return;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desktop_tex->GetDesc(&desc);
        const int width = static_cast<int>(desc.Width);
        const int height = static_cast<int>(desc.Height);
        if (width <= 0 || height <= 0 || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            Close();
            callback_->OnCaptureResult(Result::ERROR_PERMANENT, nullptr);
            return;
        }

        auto gpu_shared = gpu_buffers_.CaptureTexture(
            device_.Get(),
            context_.Get(),
            adapter_luid_,
            device_context_mutex_,
            desktop_tex.Get(),
            width,
            height,
            false);
        if (!gpu_shared) {
            std::unique_ptr<webrtc::SharedDesktopFrame> cached;
            if (latest_) {
                cached = latest_->Share();
                cached->set_capturer_id(latest_->capturer_id());
            }
            callback_->OnCaptureResult(
                cached ? Result::SUCCESS : Result::ERROR_TEMPORARY,
                std::move(cached));
            return;
        }

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        gpu_shared->set_capture_time_ms(elapsed_ms);
        gpu_shared->set_capturer_id(
            webrtc::DesktopCapturerId::kScreenCapturerWinDirectx | kD3d11TextureDesktopFrameCapturerIdBit);
        latest_ = std::move(gpu_shared);
        auto delivered = latest_->Share();
        delivered->set_capturer_id(latest_->capturer_id());
        callback_->OnCaptureResult(Result::SUCCESS, std::move(delivered));
    }

private:
    bool OpenDuplication() {
        Close();
        if (!monitor_) return false;
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                      reinterpret_cast<void**>(factory.GetAddressOf()))) ||
            !factory) {
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter1> matched_adapter;
        Microsoft::WRL::ComPtr<IDXGIOutput1> matched_output;
        for (UINT a = 0;; ++a) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(a, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND || !adapter) {
                break;
            }
            for (UINT o = 0;; ++o) {
                Microsoft::WRL::ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(o, output.GetAddressOf()) == DXGI_ERROR_NOT_FOUND || !output) {
                    break;
                }
                DXGI_OUTPUT_DESC out_desc{};
                if (SUCCEEDED(output->GetDesc(&out_desc)) && out_desc.AttachedToDesktop &&
                    out_desc.Monitor == monitor_) {
                    // Rotation other than identity falls back to WebRTC's rotation-aware DXGI capturer.
                    if (out_desc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
                        out_desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
                        return false;
                    }
                    if (SUCCEEDED(output.As(&matched_output)) && matched_output) {
                        matched_adapter = adapter;
                        break;
                    }
                }
            }
            if (matched_output) break;
        }
        if (!matched_adapter || !matched_output) {
            return false;
        }

        DXGI_ADAPTER_DESC1 adapter_desc{};
        if (FAILED(matched_adapter->GetDesc1(&adapter_desc))) {
            return false;
        }
        adapter_luid_ = adapter_desc.AdapterLuid;

        D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(
                matched_adapter.Get(),
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr,
                0,
                D3D11_SDK_VERSION,
                device_.GetAddressOf(),
                &feature_level,
                context_.GetAddressOf())) ||
            !device_ || !context_) {
            Close();
            return false;
        }

        device_context_mutex_ = std::make_shared<std::mutex>();
        if (FAILED(matched_output->DuplicateOutput(device_.Get(), duplication_.GetAddressOf())) ||
            !duplication_) {
            Close();
            return false;
        }
        return true;
    }

    void Close() noexcept {
        duplication_.Reset();
        latest_.reset();
        gpu_buffers_.Clear();
        if (context_) {
            context_->ClearState();
            context_->Flush();
        }
        context_.Reset();
        const bool had_device = static_cast<bool>(device_);
        device_.Reset();
        device_context_mutex_.reset();
        if (had_device && report_) {
            try { report_(DesktopCaptureProbePhase::D3dReleased); } catch (...) {}
        }
    }

    HMONITOR monitor_ = nullptr;
    SourceId source_id_ = 0;
    Callback* callback_ = nullptr;
    std::function<void(DesktopCaptureProbePhase)> report_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    LUID adapter_luid_{};
    std::shared_ptr<std::mutex> device_context_mutex_;
    std::unique_ptr<webrtc::SharedDesktopFrame> latest_;
    D3d11DesktopFrameTexturePool gpu_buffers_;
};
}
std::unique_ptr<webrtc::DesktopCapturer> CreateOwnedWgcWindowCapturer(
        std::function<void(DesktopCaptureProbePhase)> report) {
    return std::make_unique<OwnedWgcWindowCapturer>(std::move(report), false, false);
}
std::unique_ptr<webrtc::DesktopCapturer> CreateOwnedWgcWindowGpuCapturer(
        std::function<void(DesktopCaptureProbePhase)> report) {
    return std::make_unique<OwnedWgcWindowCapturer>(std::move(report), false, true);
}
bool IsOwnedWgcSupported(DesktopSourceKind kind) {
    try {
        // Preserve the OS/display restrictions of WebRTC's WGC probe. The
        // required interop/free-threaded APIs arrived in Windows 10 1903.
        if (!CapturePlatformSupported(kind)) return false;
        if (!KeepCaptureRuntimeLoaded()) return false;
        // Query actual interfaces and current support, not ApiInformation's
        // metadata reader (which retained a File/Section pair per worker).
        const auto session = winrt::try_get_activation_factory<capture::GraphicsCaptureSession,
            capture::IGraphicsCaptureSessionStatics>();
        const auto item = winrt::try_get_activation_factory<capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        const auto pool = winrt::try_get_activation_factory<capture::Direct3D11CaptureFramePool,
            capture::IDirect3D11CaptureFramePoolStatics2>();
        return session && session.IsSupported() && item && pool;
    } catch (...) { return false; }
}
std::unique_ptr<webrtc::DesktopCapturer> CreateOwnedWgcScreenCapturer(
        std::function<void(DesktopCaptureProbePhase)> report) {
    return std::make_unique<OwnedWgcWindowCapturer>(std::move(report), true, false);
}
std::unique_ptr<webrtc::DesktopCapturer> CreateOwnedWgcScreenGpuCapturer(
        std::function<void(DesktopCaptureProbePhase)> report) {
    return std::make_unique<OwnedWgcWindowCapturer>(std::move(report), true, true);
}
std::unique_ptr<webrtc::DesktopCapturer> CreateOwnedDxgiScreenGpuCapturer(
        std::function<void(DesktopCaptureProbePhase)> report) {
    return std::make_unique<OwnedDxgiScreenGpuCapturer>(std::move(report));
}
}
