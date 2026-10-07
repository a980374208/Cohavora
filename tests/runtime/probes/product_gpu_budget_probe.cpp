#define NOMINMAX
#include "product_gpu_budget_snapshot.h"
#include "product_gpu_queue_snapshot.h"
#include <filesystem>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>

struct CapabilityDevice {
    HMODULE module = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    HRESULT result = E_NOINTERFACE;
    HRESULT work_result = E_NOINTERFACE;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload, destination;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::uint64_t ticket = 0;
    CapabilityDevice() {
        module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto create = module ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(module, "D3D12CreateDevice")) : nullptr;
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (create && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapters1(0, &adapter)))
            result = create(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(result)) return;
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(work_result = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) return;
        if (FAILED(work_result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return;
        if (FAILED(work_result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) return;
        D3D12_RESOURCE_DESC resource{};
        resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resource.Width = 65536;
        resource.Height = resource.DepthOrArraySize = resource.MipLevels = 1;
        resource.SampleDesc.Count = 1;
        resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        if (FAILED(work_result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return;
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(work_result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&destination)))) return;
        void* data = nullptr;
        D3D12_RANGE no_read{0, 0};
        if (FAILED(work_result = upload->Map(0, &no_read, &data))) return;
        std::memset(data, 0x5a, static_cast<std::size_t>(resource.Width));
        upload->Unmap(0, nullptr);
        for (unsigned copy = 0; copy < 128; ++copy)
            list->CopyBufferRegion(destination.Get(), 0, upload.Get(), 0, resource.Width);
        if (FAILED(work_result = list->Close())) return;
        work_result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    }
    bool PulseAndConfirm() {
        if (FAILED(work_result) || !fence) return false;
        ID3D12CommandList* commands[]{list.Get()};
        queue->ExecuteCommandLists(1, commands);
        if (FAILED(work_result = queue->Signal(fence.Get(), ++ticket))) return false;
        const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        work_result = fence->SetEventOnCompletion(ticket, event);
        const auto waited = SUCCEEDED(work_result) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
        CloseHandle(event);
        return waited == WAIT_OBJECT_0 && fence->GetCompletedValue() == ticket;
    }
    ~CapabilityDevice() {
        fence.Reset(); destination.Reset(); upload.Reset(); list.Reset(); allocator.Reset(); queue.Reset(); device.Reset();
        if (module) FreeLibrary(module);
    }
};

// An independent lifecycle control. Raw PROCESS and PROCESS_ADAPTER fields
// cannot by themselves replace the product's packet/room-release evidence.
std::string ProcessStatisticsJson() {
    struct Module { HMODULE value; ~Module() { if (value) FreeLibrary(value); } } module{
        LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    struct Process { HANDLE value; ~Process() { if (value) CloseHandle(value); } } process{
        OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId())};
    using Query = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYSTATISTICS*);
    const auto query = module.value ? reinterpret_cast<Query>(GetProcAddress(module.value, "D3DKMTQueryStatistics")) : nullptr;
    D3DKMT_QUERYSTATISTICS stats{};
    stats.Type = D3DKMT_QUERYSTATISTICS_PROCESS;
    stats.hProcess = process.value;
    const auto status = query && process.value ? query(&stats) : static_cast<NTSTATUS>(0xc0000002u);
    std::ostringstream out;
    out << "{\"pid\":" << GetCurrentProcessId() << ",\"process_ntstatus\":" << static_cast<std::uint32_t>(status);
    auto field = [&](const char* name, std::uint64_t value) {
        out << ",\"" << name << "\":";
        if (status >= 0) out << value; else out << "null";
    };
    field("node_count_raw", stats.QueryResult.ProcessInformation.NodeCount);
    field("vidpn_source_count_raw", stats.QueryResult.ProcessInformation.VidPnSourceCount);
    field("system_bytes_allocated", stats.QueryResult.ProcessInformation.SystemMemory.BytesAllocated);
    field("system_bytes_reserved", stats.QueryResult.ProcessInformation.SystemMemory.BytesReserved);
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    const auto factory_result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    out << ",\"factory_hresult\":" << static_cast<unsigned>(factory_result) << ",\"adapters\":[";
    bool first = true;
    if (SUCCEEDED(factory_result)) {
        for (UINT ordinal = 0; ; ++ordinal) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            const auto enumerated = factory->EnumAdapters1(ordinal, &adapter);
            if (enumerated == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(enumerated)) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            D3DKMT_QUERYSTATISTICS adapter_stats{};
            adapter_stats.Type = D3DKMT_QUERYSTATISTICS_PROCESS_ADAPTER;
            adapter_stats.AdapterLuid = desc.AdapterLuid;
            adapter_stats.hProcess = process.value;
            const auto adapter_status = query && process.value ? query(&adapter_stats) : static_cast<NTSTATUS>(0xc0000002u);
            if (!first) out << ',';
            first = false;
            out << "{\"adapter_ordinal\":" << ordinal << ",\"ntstatus\":" << static_cast<std::uint32_t>(adapter_status);
            const auto& info = adapter_stats.QueryResult.ProcessAdapterInformation;
            auto adapter_field = [&](const char* name, std::uint64_t value) {
                out << ",\"" << name << "\":";
                if (adapter_status >= 0) out << value; else out << "null";
            };
            adapter_field("node_count_raw", info.NodeCount);
            adapter_field("segments_raw", info.NbSegments);
            adapter_field("vidpn_source_count_raw", info.VidPnSourceCount);
            adapter_field("virtual_memory_usage_raw", info.VirtualMemoryUsage);
            D3DKMT_QUERYSTATISTICS process_by_adapter{};
            process_by_adapter.Type = D3DKMT_QUERYSTATISTICS_PROCESS;
            process_by_adapter.AdapterLuid = desc.AdapterLuid;
            process_by_adapter.hProcess = process.value;
            const auto process_status = query && process.value ? query(&process_by_adapter) : static_cast<NTSTATUS>(0xc0000002u);
            out << ",\"process_with_adapter_ntstatus\":" << static_cast<std::uint32_t>(process_status);
            auto process_field = [&](const char* name, std::uint64_t value) {
                out << ",\"" << name << "\":";
                if (process_status >= 0) out << value; else out << "null";
            };
            const auto& process_info = process_by_adapter.QueryResult.ProcessInformation;
            process_field("process_with_adapter_node_count_raw", process_info.NodeCount);
            process_field("process_with_adapter_vidpn_source_count_raw", process_info.VidPnSourceCount);
            process_field("process_with_adapter_system_bytes_allocated", process_info.SystemMemory.BytesAllocated);
            out << '}';
        }
    }
    out << "]}";
    return out.str();
}

int wmain(int argc, wchar_t** argv) {
    const bool release_control = argc == 3 && std::wstring_view(argv[2]) == L"--release-control";
    if ((argc != 2 && !release_control) || std::filesystem::exists(argv[1])) return 2;
    std::ofstream out(std::filesystem::path(argv[1]), std::ios::binary);
    if (!out) return 2;
    product_gpu_witness::BudgetSampler sampler;
    // Exercise WDDM process statistics while a probe-owned graphics device is
    // alive. This never proves the product's GPU workload or queue bounds.
    product_gpu_witness::QueueSampler queue;
    std::unique_ptr<CapabilityDevice> capability_device;
    if (!release_control) capability_device = std::make_unique<CapabilityDevice>();
    const unsigned samples = release_control ? 9 : 3;
    for (unsigned sequence = 1; sequence <= samples; ++sequence) {
        if (release_control && sequence == 4) capability_device = std::make_unique<CapabilityDevice>();
        if (release_control && sequence == 7) capability_device.reset();
        const bool work_completed = capability_device && capability_device->PulseAndConfirm();
        const auto phase = !release_control || (sequence >= 4 && sequence <= 6) ? "completed_work" :
            (sequence < 4 ? "before_device" : "after_device_release");
        const auto utc = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        out << "{\"sequence\":" << sequence << ",\"utc_ms\":" << utc
            << ",\"phase\":\"" << phase << "\",\"probe_device_alive\":" << (capability_device ? "true" : "false")
            << ",\"probe_device_hresult\":";
        if (capability_device) out << static_cast<unsigned>(capability_device->result); else out << "null";
        out << ",\"probe_work_hresult\":";
        if (capability_device) out << static_cast<unsigned>(capability_device->work_result); else out << "null";
        out
            << ",\"probe_work_completed\":" << (work_completed ? "true" : "false")
            << ",\"gpu_budget\":" << sampler.SampleJson()
            << ",\"gpu_queue\":" << queue.SampleJson()
            << ",\"process_statistics\":" << ProcessStatisticsJson() << "}\n";
        out.flush();
        if (!out) return 1;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}
