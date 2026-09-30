#pragma once
#include <windows.graphics.capture.interop.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <iostream>
#include "modules/desktop_capture/win/wgc_capturer_win.h"

inline int ProbeWgcCapability() {
    namespace capture = winrt::Windows::Graphics::Capture;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    int result = 1;
    try {
        const auto session = winrt::try_get_activation_factory<capture::GraphicsCaptureSession,
            capture::IGraphicsCaptureSessionStatics>();
        const bool native_supported = session && session.IsSupported();
        const auto factory = winrt::try_get_activation_factory<capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        capture::GraphicsCaptureItem item{nullptr};
        const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        const HRESULT hr = factory ? factory->CreateForMonitor(monitor,
            winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item)) : E_NOINTERFACE;
        std::cout << "[WGC_CAPABILITY] sdk_screen=" << webrtc::IsWgcSupported(webrtc::CaptureType::kScreen)
                  << " sdk_window=" << webrtc::IsWgcSupported(webrtc::CaptureType::kWindow)
                  << " native_session=" << native_supported
                  << " create_for_monitor_hr=" << static_cast<long>(hr)
                  << " monitor_item=" << static_cast<bool>(item) << std::endl;
        result = native_supported && SUCCEEDED(hr) && item ? 0 : 1;
    } catch (const winrt::hresult_error& error) {
        std::cout << "[WGC_CAPABILITY] hr=" << static_cast<long>(error.code()) << std::endl;
    }
    winrt::uninit_apartment();
    return result;
}
