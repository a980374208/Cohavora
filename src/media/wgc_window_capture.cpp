#include "wgc_window_capture.h"
#include "desktop_frame_buffer_pool.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "modules/desktop_capture/shared_desktop_frame.h"
#include "modules/desktop_capture/desktop_capture_types.h"
#include "modules/desktop_capture/win/screen_capture_utils.h"
#include <d3d11.h>
#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <cstring>

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
    explicit OwnedWgcWindowCapturer(std::function<void(DesktopCaptureProbePhase)> report, bool screen = false)
        : report_(std::move(report)), screen_(screen) {}
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
                // WGC can be idle for a static window. Keep one CPU frame, as
                // the WebRTC capturer does, so idle content is not mistaken
                // for capture failure by the enclosing five-second watchdog.
                callback_->OnCaptureResult(latest_ ? Result::SUCCESS : Result::ERROR_TEMPORARY,
                    latest_ ? latest_->Share() : nullptr);
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
        if (had_device) Report(DesktopCaptureProbePhase::D3dReleased);
    }
    HWND window_ = nullptr;
    HMONITOR monitor_ = nullptr;
    Callback* callback_ = nullptr;
    std::function<void(DesktopCaptureProbePhase)> report_;
    bool screen_ = false;
    capture::GraphicsCaptureItem item_{nullptr};
    capture::Direct3D11CaptureFramePool pool_{nullptr};
    capture::GraphicsCaptureSession session_{nullptr};
    directx::Direct3D11::IDirect3DDevice winrt_device_{nullptr};
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11Texture2D> staging_;
    std::unique_ptr<webrtc::SharedDesktopFrame> latest_;
    DesktopFrameBufferPool buffers_;
    int width_ = 0, height_ = 0, pool_width_ = 0, pool_height_ = 0;
};
}
std::unique_ptr<webrtc::DesktopCapturer> CreateOwnedWgcWindowCapturer(
        std::function<void(DesktopCaptureProbePhase)> report) {
    return std::make_unique<OwnedWgcWindowCapturer>(std::move(report));
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
    return std::make_unique<OwnedWgcWindowCapturer>(std::move(report), true);
}
}
