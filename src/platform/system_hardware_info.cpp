#include "system_hardware_info.h"

#include "telemetry/diagnostic_pipeline.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <limits>
#include <mutex>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

namespace livekit::platform {
namespace {

using namespace diagnostic;

Event HardwareEvent(EventKind kind) {
    Event event;
    event.kind = kind;
    event.thread_role = ThreadRole::Media;
    return event;
}

#if defined(_WIN32)
template<std::size_t N>
bool AssignWideModel(HardwareInfoRow& row, const std::array<wchar_t, N>& value) {
    const auto end = std::find(value.begin(), value.end(), L'\0');
    if (end == value.end()) return false;
    std::array<char, N> ascii{};
    for (std::size_t i = 0; i < static_cast<std::size_t>(end - value.begin()); ++i) {
        if (value[i] < 0x20 || value[i] > 0x7e) return false;
        ascii[i] = static_cast<char>(value[i]);
    }
    return row.AssignModel(std::string_view(ascii.data(), end - value.begin()));
}

void ReadCpu(Event& event) {
    auto& row = event.hardware;
    std::array<wchar_t, 256> model{};
    DWORD bytes = static_cast<DWORD>(sizeof(model));
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            L"ProcessorNameString", RRF_RT_REG_SZ | RRF_ZEROONFAILURE,
            nullptr, model.data(), &bytes) == ERROR_SUCCESS) {
        AssignWideModel(row, model);
    }
    const auto processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (processors > 0 && processors <= (std::numeric_limits<std::uint16_t>::max)()) {
        row.logical_processors = static_cast<std::uint16_t>(processors);
        row.known_fields |= HardwareLogicalProcessorsKnown;
    }
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size < 8 || size > 1024 * 1024)
        return;
    std::vector<BYTE> data(size);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(data.data()), &size))
        return;
    std::size_t offset = 0;
    std::uint32_t cores = 0;
    while (offset + 8 <= size) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            data.data() + offset);
        if (info->Size < 8 || info->Size > size - offset) return;
        if (info->Relationship == RelationProcessorCore) ++cores;
        offset += info->Size;
    }
    if (offset == size && cores > 0 && cores <= (std::numeric_limits<std::uint16_t>::max)()) {
        row.physical_cores = static_cast<std::uint16_t>(cores);
        row.known_fields |= HardwarePhysicalCoresKnown;
    }
}

bool ParseDriverVersion(std::wstring_view text, std::uint64_t& version) {
    if (text.empty() || text.size() > 31) return false;
    std::uint64_t result = 0;
    std::size_t at = 0;
    for (unsigned part = 0; part != 4; ++part) {
        unsigned value = 0, digits = 0;
        while (at < text.size() && text[at] >= L'0' && text[at] <= L'9') {
            value = value * 10 + (text[at++] - L'0');
            if (++digits > 5 || value > 65535) return false;
        }
        if (!digits) return false;
        result = (result << 16) | value;
        if (part != 3 && (at == text.size() || text[at++] != L'.')) return false;
    }
    if (at != text.size()) return false;
    version = result;
    return true;
}

bool ReadRegistryDriverVersion(UINT vendor, UINT device, std::uint64_t& version) {
    std::array<wchar_t, 32> prefix{};
    swprintf_s(prefix.data(), prefix.size(), L"PCI\\VEN_%04X&DEV_%04X", vendor, device);
    for (DWORD index = 0; index < 64; ++index) {
        DISPLAY_DEVICEW display{};
        display.cb = sizeof(display);
        if (!EnumDisplayDevicesW(nullptr, index, &display, 0)) break;
        if (_wcsnicmp(display.DeviceID, prefix.data(), wcslen(prefix.data())) != 0) continue;
        constexpr std::wstring_view machinePrefix = L"\\Registry\\Machine\\";
        const auto keyEnd = std::find(std::begin(display.DeviceKey),
            std::end(display.DeviceKey), L'\0');
        if (keyEnd == std::end(display.DeviceKey)) continue;
        const std::wstring_view key(display.DeviceKey, keyEnd - display.DeviceKey);
        if (key.size() <= machinePrefix.size() ||
            _wcsnicmp(key.data(), machinePrefix.data(), machinePrefix.size()) != 0) continue;
        std::array<wchar_t, 64> driver{};
        DWORD bytes = static_cast<DWORD>(sizeof(driver));
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.data() + machinePrefix.size(),
                L"DriverVersion", RRF_RT_REG_SZ | RRF_ZEROONFAILURE,
                nullptr, driver.data(), &bytes) != ERROR_SUCCESS) continue;
        const auto end = std::find(driver.begin(), driver.end(), L'\0');
        if (end != driver.end() && ParseDriverVersion(
                std::wstring_view(driver.data(), end - driver.begin()), version)) return true;
    }
    return false;
}

void LogGpus() {
    auto summary = HardwareEvent(EventKind::HardwareGpuEnumeration);
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        summary.outcome = Outcome::Failure;
        EmitBusinessEvent(summary);
        return;
    }
    bool complete = false;
    for (UINT ordinal = 0; ordinal != 32; ++ordinal) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const auto status = factory->EnumAdapters1(ordinal, &adapter);
        if (status == DXGI_ERROR_NOT_FOUND) { complete = true; break; }
        if (FAILED(status)) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) break;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        auto event = HardwareEvent(EventKind::HardwareGpu);
        auto& row = event.hardware;
        std::array<wchar_t, 128> model{};
        std::copy(std::begin(desc.Description), std::end(desc.Description), model.begin());
        AssignWideModel(row, model);
        row.vendor_id = desc.VendorId;
        row.device_id = desc.DeviceId;
        row.memory_bytes = desc.DedicatedVideoMemory;
        row.auxiliary_memory_bytes = desc.SharedSystemMemory;
        row.known_fields |= HardwareGpuIdentityKnown | HardwareMemoryKnown |
            HardwareAuxiliaryMemoryKnown;
        // Registry version is the installed display package version. DXGI's
        // interface version is used only when that read-only query is absent.
        if (ReadRegistryDriverVersion(desc.VendorId, desc.DeviceId, row.driver_version)) {
            row.known_fields |= HardwareDriverKnown;
        } else {
            LARGE_INTEGER driver{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver))) {
                row.driver_version = static_cast<std::uint64_t>(driver.QuadPart);
                row.known_fields |= HardwareDriverKnown;
                row.flags |= HardwareDriverVersionFromDxgi;
            }
        }
        event.outcome = (row.known_fields & (HardwareModelKnown | HardwareDriverKnown)) ==
            (HardwareModelKnown | HardwareDriverKnown) ? Outcome::Success : Outcome::DegradedSuccess;
        EmitBusinessEvent(event);
        ++summary.device_count;
    }
    if (complete) summary.hardware.flags |= HardwareEnumerationComplete;
    summary.outcome = complete ? Outcome::Success : Outcome::DegradedSuccess;
    EmitBusinessEvent(summary);
}
#endif

void CollectAndLog() {
    auto cpu = HardwareEvent(EventKind::HardwareCpu);
    auto memory = HardwareEvent(EventKind::HardwareMemory);
#if defined(_WIN32)
    ReadCpu(cpu);
    cpu.outcome = (cpu.hardware.known_fields & (HardwareModelKnown |
        HardwareLogicalProcessorsKnown | HardwarePhysicalCoresKnown)) ==
        (HardwareModelKnown | HardwareLogicalProcessorsKnown | HardwarePhysicalCoresKnown)
        ? Outcome::Success : Outcome::DegradedSuccess;
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        memory.hardware.memory_bytes = status.ullTotalPhys;
        memory.hardware.auxiliary_memory_bytes = status.ullAvailPhys;
        memory.hardware.known_fields = HardwareMemoryKnown | HardwareAuxiliaryMemoryKnown;
        memory.outcome = Outcome::Success;
    } else memory.outcome = Outcome::Failure;
#else
    cpu.outcome = Outcome::Failure;
    memory.outcome = Outcome::Failure;
#endif
    EmitBusinessEvent(cpu);
    EmitBusinessEvent(memory);
#if defined(_WIN32)
    LogGpus();
#else
    auto gpu = HardwareEvent(EventKind::HardwareGpuEnumeration);
    gpu.outcome = Outcome::Failure;
    EmitBusinessEvent(gpu);
#endif
}

} // namespace

void LogSystemHardwareInfo() noexcept {
    try {
        static std::once_flag once;
        std::call_once(once, CollectAndLog);
    } catch (...) {
        // Inventory is diagnostic only and cannot fail application startup.
    }
}

} // namespace livekit::platform
