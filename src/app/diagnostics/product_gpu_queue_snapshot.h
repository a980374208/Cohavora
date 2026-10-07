#pragma once

// WDDM scheduler packet counters for the actual calling process. Memory nodes
// (DXGI/D3D12) and scheduler/engine nodes (D3DKMT) are different enumerations.
#include <chrono>
#include <cstdint>
#include <sstream>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#include <dxgi1_4.h>
#include <d3dkmthk.h>
#include <wrl/client.h>
#endif

namespace product_gpu_witness {
class QueueSampler final {
public:
    QueueSampler() {
#if defined(_WIN32)
        module_.value = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        query_ = module_.value ? reinterpret_cast<Query>(GetProcAddress(module_.value, "D3DKMTQueryStatistics")) : nullptr;
        process_.value = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
        process_error_ = process_.value ? ERROR_SUCCESS : GetLastError();
        initialization_ = CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
        if (FAILED(initialization_)) return;
        for (UINT ordinal = 0; ; ++ordinal) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            const auto enumerated = factory_->EnumAdapters1(ordinal, &adapter);
            if (enumerated == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(enumerated)) { initialization_ = enumerated; break; }
            DXGI_ADAPTER_DESC1 desc{};
            const auto described = adapter->GetDesc1(&desc);
            if (SUCCEEDED(described) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            Adapter entry;
            entry.ordinal = ordinal;
            entry.described = described;
            if (SUCCEEDED(described)) {
                entry.luid = desc.AdapterLuid; // Used internally; no hardware identifier is emitted.
                if (query_) {
                    D3DKMT_QUERYSTATISTICS stats{};
                    stats.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
                    stats.AdapterLuid = entry.luid;
                    entry.status = query_(&stats);
                    if (entry.status >= 0) entry.nodes = stats.QueryResult.AdapterInformation.NodeCount;
                }
            }
            adapters_.push_back(std::move(entry));
        }
#endif
    }
    QueueSampler(const QueueSampler&) = delete;
    QueueSampler& operator=(const QueueSampler&) = delete;

    std::string SampleJson() const {
        const auto began = std::chrono::steady_clock::now();
        std::ostringstream rows;
        bool all_valid = false, topology_current = false;
        unsigned pid = 0, initialization = 0, process_error = 0;
        bool query_available = false, process_available = false;
#if defined(_WIN32)
        pid = GetCurrentProcessId();
        initialization = static_cast<unsigned>(initialization_);
        process_error = process_error_;
        query_available = query_ != nullptr;
        process_available = process_.value != nullptr;
        topology_current = factory_.Get() != nullptr && factory_->IsCurrent() != FALSE;
        all_valid = SUCCEEDED(initialization_) && query_available && process_available && topology_current && !adapters_.empty();
        bool first = true;
        for (const auto& entry : adapters_) {
            if (!first) rows << ',';
            first = false;
            const bool nodes_known = SUCCEEDED(entry.described) && entry.status >= 0 && entry.nodes > 0;
            all_valid = all_valid && nodes_known;
            rows << "{\"adapter_ordinal\":" << entry.ordinal << ",\"scheduler_node_count\":" << entry.nodes
                 << ",\"adapter_ntstatus\":" << static_cast<std::uint32_t>(entry.status) << ",\"nodes\":[";
            for (UINT node = 0; node < entry.nodes; ++node) {
                if (node) rows << ',';
                D3DKMT_QUERYSTATISTICS stats{};
                stats.Type = D3DKMT_QUERYSTATISTICS_PROCESS_NODE;
                stats.AdapterLuid = entry.luid;
                stats.hProcess = process_.value;
                stats.QueryProcessNode.NodeId = node;
                const auto status = query_ && process_.value ? query_(&stats) : static_cast<NTSTATUS>(0xc0000002u);
                const bool valid = status >= 0;
                all_valid = all_valid && valid;
                rows << "{\"node_index\":" << node << ",\"availability\":\"" << (valid ? "VALID" : "UNAVAILABLE")
                     << "\",\"ntstatus\":" << static_cast<std::uint32_t>(status) << ",\"running_time_raw\":";
                if (valid) rows << stats.QueryResult.ProcessNodeInformation.RunningTime.QuadPart; else rows << "null";
                rows << ",\"queue_packets\":[";
                for (unsigned type = 0; type < D3DKMT_QUERYSTATISTICS_QUEUE_PACKET_TYPE_MAX; ++type) {
                    if (type) rows << ',';
                    const auto& packet = stats.QueryResult.ProcessNodeInformation.PacketStatistics.QueuePacket[type];
                    rows << "{\"type\":" << type << ",\"submitted\":";
                    if (valid) rows << packet.PacketSubmited; else rows << "null";
                    rows << ",\"completed\":";
                    if (valid) rows << packet.PacketCompleted; else rows << "null";
                    // Keep raw counters if ordering is ambiguous. Do not clamp,
                    // infer wrap, or label the arithmetic a hardware queue depth.
                    rows << ",\"submitted_minus_completed\":";
                    if (valid && packet.PacketSubmited >= packet.PacketCompleted)
                        rows << packet.PacketSubmited - packet.PacketCompleted;
                    else rows << "null";
                    rows << '}';
                }
                rows << "],\"dma_packets\":[";
                for (unsigned type = 0; type < D3DKMT_QUERYSTATISTICS_DMA_PACKET_TYPE_MAX; ++type) {
                    if (type) rows << ',';
                    const auto& packet = stats.QueryResult.ProcessNodeInformation.PacketStatistics.DmaPacket[type];
                    rows << "{\"type\":" << type;
                    auto field = [&](const char* name, std::uint32_t value) {
                        rows << ",\"" << name << "\":";
                        if (valid) rows << value; else rows << "null";
                    };
                    field("submitted", packet.PacketSubmited);
                    field("completed", packet.PacketCompleted);
                    field("preempted", packet.PacketPreempted);
                    field("faulted", packet.PacketFaulted);
                    rows << '}';
                }
                rows << "]}";
            }
            rows << "]}";
        }
#endif
        std::ostringstream out;
        out << "{\"collector\":\"wddm_process_packet_statistics\",\"pid\":" << pid
            << ",\"scope\":\"process_all_enumerated_hardware_adapters_all_scheduler_nodes\""
            << ",\"availability\":\"" << (all_valid ? "VALID" : "UNAVAILABLE")
            << "\",\"initialization_hresult\":" << initialization
            << ",\"query_available\":" << (query_available ? "true" : "false")
            << ",\"process_handle_available\":" << (process_available ? "true" : "false")
            << ",\"process_error\":" << process_error
            << ",\"topology_current\":" << (topology_current ? "true" : "false")
            << ",\"adapters\":[" << rows.str() << "],\"elapsed_us\":"
            << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count() << '}';
        return out.str();
    }
private:
#if defined(_WIN32)
    using Query = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYSTATISTICS*);
    struct Module { HMODULE value = nullptr; ~Module() { if (value) FreeLibrary(value); } } module_;
    struct Process { HANDLE value = nullptr; ~Process() { if (value) CloseHandle(value); } } process_;
    struct Adapter {
        UINT ordinal = 0;
        LUID luid{};
        UINT nodes = 0;
        HRESULT described = E_FAIL;
        NTSTATUS status = static_cast<NTSTATUS>(0xc0000002u);
    };
    Query query_ = nullptr;
    DWORD process_error_ = ERROR_SUCCESS;
    HRESULT initialization_ = E_FAIL;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory_;
    std::vector<Adapter> adapters_;
#endif
};
} // namespace product_gpu_witness
