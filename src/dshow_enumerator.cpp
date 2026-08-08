#include "dshow_enumerator.h"
#include "media_converters.h"
#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <wrl/client.h>
#include <sstream>
#include <spdlog/spdlog.h>

using Microsoft::WRL::ComPtr;

namespace livekit {

static std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &result[0], size, nullptr, nullptr);
    return result;
}

static void DeleteMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return;
    if (pmt->cbFormat != 0) {
        CoTaskMemFree(pmt->pbFormat);
        pmt->cbFormat = 0;
        pmt->pbFormat = nullptr;
    }
    if (pmt->pUnk != nullptr) {
        pmt->pUnk->Release();
        pmt->pUnk = nullptr;
    }
    CoTaskMemFree(pmt);
}

static std::vector<DShowCapability> QueryFilterCapabilities(IBaseFilter* filter) {
    std::vector<DShowCapability> caps;
    if (!filter) return caps;

    ComPtr<IEnumPins> enum_pins;
    if (FAILED(filter->EnumPins(&enum_pins)) || !enum_pins) return caps;

    ComPtr<IPin> pin;
    while (enum_pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION dir;
        if (SUCCEEDED(pin->QueryDirection(&dir)) && dir == PINDIR_OUTPUT) {
            ComPtr<IAMStreamConfig> stream_config;
            if (SUCCEEDED(pin.As(&stream_config)) && stream_config) {
                int count = 0, size = 0;
                if (SUCCEEDED(stream_config->GetNumberOfCapabilities(&count, &size)) &&
                    size >= static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS))) {
                    std::vector<BYTE> caps_buffer(size);
                    for (int i = 0; i < count; ++i) {
                        AM_MEDIA_TYPE* pmt = nullptr;
                        if (SUCCEEDED(stream_config->GetStreamCaps(i, &pmt, caps_buffer.data())) && pmt) {
                            auto* scc = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(caps_buffer.data());
                            DShowCapability cap;
                            cap.format = MediaConverters::SubtypeToPixelFormat(pmt->subtype);
                            cap.min_frame_interval = scc->MinFrameInterval;
                            cap.max_frame_interval = scc->MaxFrameInterval;

                            if (scc->MinFrameInterval > 0) {
                                cap.max_fps = static_cast<int>(10000000LL / scc->MinFrameInterval);
                            }
                            if (scc->MaxFrameInterval > 0) {
                                cap.min_fps = static_cast<int>(10000000LL / scc->MaxFrameInterval);
                            }

                            if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                                auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(pmt->pbFormat);
                                cap.width = vih->bmiHeader.biWidth;
                                cap.height = std::abs(vih->bmiHeader.biHeight);
                            } else if (pmt->formattype == FORMAT_VideoInfo2 && pmt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
                                auto* vih2 = reinterpret_cast<VIDEOINFOHEADER2*>(pmt->pbFormat);
                                cap.width = vih2->bmiHeader.biWidth;
                                cap.height = std::abs(vih2->bmiHeader.biHeight);
                            } else {
                                cap.width = scc->MaxOutputSize.cx;
                                cap.height = scc->MaxOutputSize.cy;
                            }

                            if (cap.width > 0 && cap.height > 0) {
                                caps.push_back(cap);
                            }

                            DeleteMediaType(pmt);
                        }
                    }
                }
            }
        }
        pin.Reset();
    }
    return caps;
}

struct ScopedComInitializer {
    HRESULT hr;
    ScopedComInitializer() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ScopedComInitializer() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
};

std::vector<DShowDeviceInfo> DShowEnumerator::EnumerateVideoDevices() {
    ScopedComInitializer com_init;
    std::vector<DShowDeviceInfo> devices;

    ComPtr<ICreateDevEnum> dev_enum;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dev_enum));
    if (FAILED(hr) || !dev_enum) {
        spdlog::warn("[DShowEnumerator] Failed to create CLSID_SystemDeviceEnum, hr=0x{:08x}", static_cast<uint32_t>(hr));
        return devices;
    }

    ComPtr<IEnumMoniker> enum_moniker;
    hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enum_moniker, 0);
    if (hr != S_OK || !enum_moniker) {
        spdlog::info("[DShowEnumerator] No DirectShow video input devices found.");
        return devices;
    }

    ComPtr<IMoniker> moniker;
    bool is_first = true;
    while (enum_moniker->Next(1, &moniker, nullptr) == S_OK) {
        ComPtr<IPropertyBag> prop_bag;
        hr = moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&prop_bag));
        if (SUCCEEDED(hr) && prop_bag) {
            DShowDeviceInfo info;
            info.is_default = is_first;
            is_first = false;

            VARIANT var;
            VariantInit(&var);

            // 读取 FriendlyName
            if (SUCCEEDED(prop_bag->Read(L"FriendlyName", &var, nullptr)) && var.vt == VT_BSTR && var.bstrVal) {
                info.name = WStringToString(var.bstrVal);
                VariantClear(&var);
            }

            // 读取 DevicePath
            if (SUCCEEDED(prop_bag->Read(L"DevicePath", &var, nullptr)) && var.vt == VT_BSTR && var.bstrVal) {
                info.path = WStringToString(var.bstrVal);
                VariantClear(&var);
            }

            if (info.path.empty()) {
                // 如果没有 DevicePath，使用 Moniker DisplayName 代替
                LPOLESTR display_name = nullptr;
                if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr, &display_name)) && display_name) {
                    info.path = WStringToString(display_name);
                    CoTaskMemFree(display_name);
                }
            }

            // 查询 Filter 对应的视频格式能力
            ComPtr<IBaseFilter> filter;
            if (SUCCEEDED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter))) && filter) {
                info.capabilities = QueryFilterCapabilities(filter.Get());
            }

            devices.push_back(info);
        }
        moniker.Reset();
    }

    return devices;
}

std::vector<DShowCapability> DShowEnumerator::GetDeviceCapabilities(const std::string& device_path) {
    auto devices = EnumerateVideoDevices();
    for (const auto& dev : devices) {
        if (dev.path == device_path || dev.name == device_path) {
            return dev.capabilities;
        }
    }
    if (!devices.empty()) {
        return devices.front().capabilities;
    }
    return {};
}

DShowDeviceInfo DShowEnumerator::GetDefaultVideoDevice() {
    auto devices = EnumerateVideoDevices();
    for (const auto& dev : devices) {
        if (dev.is_default) return dev;
    }
    if (!devices.empty()) return devices.front();

    DShowDeviceInfo dummy;
    dummy.name = "Default DirectShow Video Device";
    dummy.path = "";
    dummy.is_default = true;
    return dummy;
}

std::string DShowEnumerator::DumpDeviceInfo(const DShowDeviceInfo& info) {
    std::ostringstream oss;
    oss << "Device Name: " << info.name << "\n";
    oss << "Device Path: " << info.path << "\n";
    oss << "Is Default:  " << (info.is_default ? "Yes" : "No") << "\n";
    oss << "Capabilities count: " << info.capabilities.size() << "\n";
    for (size_t i = 0; i < info.capabilities.size(); ++i) {
        const auto& c = info.capabilities[i];
        oss << "  [" << i << "] " << c.width << "x" << c.height
            << " @" << c.max_fps << "fps (Format: "
            << MediaConverters::PixelFormatToString(c.format) << ")\n";
    }
    return oss.str();
}

} // namespace livekit
