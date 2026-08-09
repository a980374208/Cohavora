#include "wasapi_enumerator.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
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

static std::string GetDeviceFriendlyName(IMMDevice* device) {
    if (!device) return "Unknown Audio Device";
    ComPtr<IPropertyStore> store;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &store);
    if (FAILED(hr) || !store) return "Audio Endpoint";

    PROPVARIANT var;
    PropVariantInit(&var);
    hr = store->GetValue(PKEY_Device_FriendlyName, &var);
    if (SUCCEEDED(hr) && var.vt == VT_LPWSTR && var.pwszVal) {
        std::string name = WStringToString(var.pwszVal);
        PropVariantClear(&var);
        return name;
    }
    PropVariantClear(&var);
    return "Audio Endpoint";
}

static void QueryDeviceAudioFormat(IMMDevice* device, uint32_t& rate, uint16_t& channels) {
    rate = 48000;
    channels = 2;
    if (!device) return;

    ComPtr<IAudioClient> client;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (SUCCEEDED(hr) && client) {
        WAVEFORMATEX* pwfx = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&pwfx)) && pwfx) {
            rate = pwfx->nSamplesPerSec;
            channels = pwfx->nChannels;
            CoTaskMemFree(pwfx);
        }
    }
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

static std::vector<WasapiDeviceInfo> EnumerateEndpoints(EDataFlow flow, WasapiCaptureType capture_type) {
    ScopedComInitializer com_init;
    std::vector<WasapiDeviceInfo> devices;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), &enumerator);
    if (FAILED(hr) || !enumerator) {
        spdlog::warn("[WasapiEnumerator] Failed to create MMDeviceEnumerator, hr=0x{:08x}", static_cast<uint32_t>(hr));
        return devices;
    }

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        spdlog::warn("[WasapiEnumerator] Failed to EnumAudioEndpoints, hr=0x{:08x}", static_cast<uint32_t>(hr));
        return devices;
    }

    // 获取当前默认设备的 ID 用于比较
    std::string default_id;
    ComPtr<IMMDevice> default_dev;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eMultimedia, &default_dev)) && default_dev) {
        LPWSTR def_w_id = nullptr;
        if (SUCCEEDED(default_dev->GetId(&def_w_id)) && def_w_id) {
            default_id = WStringToString(def_w_id);
            CoTaskMemFree(def_w_id);
        }
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(collection->Item(i, &dev)) || !dev) continue;

        LPWSTR w_id = nullptr;
        if (FAILED(dev->GetId(&w_id)) || !w_id) continue;

        WasapiDeviceInfo info;
        info.id = WStringToString(w_id);
        CoTaskMemFree(w_id);

        info.name = GetDeviceFriendlyName(dev.Get());
        info.flow = capture_type;
        info.is_default = (!default_id.empty() && info.id == default_id);
        QueryDeviceAudioFormat(dev.Get(), info.default_sample_rate, info.default_channels);

        devices.push_back(info);
    }

    return devices;
}

std::vector<WasapiDeviceInfo> WasapiEnumerator::EnumerateInputDevices() {
    return EnumerateEndpoints(eCapture, WasapiCaptureType::Microphone);
}

std::vector<WasapiDeviceInfo> WasapiEnumerator::EnumerateOutputDevices() {
    return EnumerateEndpoints(eRender, WasapiCaptureType::DesktopLoopback);
}

WasapiDeviceInfo WasapiEnumerator::GetDefaultInputDevice() {
    auto inputs = EnumerateInputDevices();
    for (const auto& dev : inputs) {
        if (dev.is_default) return dev;
    }
    if (!inputs.empty()) return inputs.front();

    WasapiDeviceInfo dummy;
    dummy.id = "";
    dummy.name = "Default Microphone";
    dummy.is_default = true;
    dummy.flow = WasapiCaptureType::Microphone;
    return dummy;
}

WasapiDeviceInfo WasapiEnumerator::GetDefaultOutputDevice() {
    auto outputs = EnumerateOutputDevices();
    for (const auto& dev : outputs) {
        if (dev.is_default) return dev;
    }
    if (!outputs.empty()) return outputs.front();

    WasapiDeviceInfo dummy;
    dummy.id = "";
    dummy.name = "Default Speakers (Loopback)";
    dummy.is_default = true;
    dummy.flow = WasapiCaptureType::DesktopLoopback;
    return dummy;
}

bool WasapiEnumerator::GetDeviceInfo(const std::string& device_id, WasapiCaptureType type, WasapiDeviceInfo& out_info) {
    auto list = (type == WasapiCaptureType::Microphone) ? EnumerateInputDevices() : EnumerateOutputDevices();
    for (const auto& dev : list) {
        if (device_id.empty() && dev.is_default) {
            out_info = dev;
            return true;
        }
        if (dev.id == device_id) {
            out_info = dev;
            return true;
        }
    }
    if (device_id.empty() && !list.empty()) {
        out_info = list.front();
        return true;
    }
    return false;
}

} // namespace livekit
