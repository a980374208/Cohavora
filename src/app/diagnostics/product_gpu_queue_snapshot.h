#pragma once

// WDDM scheduler packet counters for the actual calling process. Memory nodes
// (DXGI/D3D12) and scheduler/engine nodes (D3DKMT) are different enumerations.
#include <chrono>
#include <atomic>
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
#if defined(_WIN32)
constexpr std::uint64_t LuidBits(const LUID& luid) noexcept {
    return (std::uint64_t(static_cast<std::uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
}
#endif
class QueueSampler final {
public:
    QueueSampler() {
#if defined(_WIN32)
        module_.value = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        query_ = module_.value ? reinterpret_cast<Query>(GetProcAddress(module_.value, "D3DKMTQueryStatistics")) : nullptr;
        process_.value = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
        process_error_ = process_.value ? ERROR_SUCCESS : GetLastError();
        sampler_generation_ = next_generation_.fetch_add(1, std::memory_order_relaxed) + 1;
        FILETIME created{}, exited{}, kernel{}, user{};
        identity_query_succeeded_ = process_.value && GetProcessTimes(process_.value, &created, &exited, &kernel, &user);
        identity_error_ = identity_query_succeeded_ ? ERROR_SUCCESS : (process_.value ? GetLastError() : process_error_);
        identity_pid_ = process_.value ? GetProcessId(process_.value) : 0;
        process_start_filetime_ = (std::uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        initialization_ = CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
        if (FAILED(initialization_)) return;
        for (UINT ordinal = 0; ; ++ordinal) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            const auto enumerated = factory_->EnumAdapters1(ordinal, &adapter);
            if (enumerated == DXGI_ERROR_NOT_FOUND) { enumeration_terminal_ = enumerated; enumeration_complete_ = true; break; }
            if (FAILED(enumerated)) { initialization_ = enumerated; enumeration_terminal_ = enumerated; break; }
            DXGI_ADAPTER_DESC1 desc{};
            const auto described = adapter->GetDesc1(&desc);
            if (SUCCEEDED(described) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            Adapter entry;
            entry.ordinal = ordinal;
            entry.described = described;
            if (SUCCEEDED(described)) {
                entry.luid = desc.AdapterLuid; // Literal identity is emitted only in the run-scoped diagnostic sample.
                entry.flags = desc.Flags;
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
        std::ostringstream rows, readability_rows;
        std::vector<const char*> health_errors;
        unsigned total_nodes = 0, readable_nodes = 0;
        bool all_valid = false, topology_current = false;
        unsigned pid = 0, initialization = 0x80004001u, process_error = 0;
        bool query_available = false, process_available = false;
        const auto read_state = [](unsigned total, unsigned readable) {
            return total == 0 ? "NO_NODES" : readable == total ? "ALL_READABLE" :
                readable == 0 ? "NONE_READABLE" : "PARTIALLY_READABLE";
        };
#if defined(_WIN32)
        pid = GetCurrentProcessId();
        initialization = static_cast<unsigned>(initialization_);
        process_error = process_error_;
        query_available = query_ != nullptr;
        process_available = process_.value != nullptr;
        topology_current = factory_.Get() != nullptr && factory_->IsCurrent() != FALSE;
        all_valid = SUCCEEDED(initialization_) && query_available && process_available && topology_current && !adapters_.empty();
        const bool topology_before = topology_current;
        LARGE_INTEGER qpc_frequency{}, sample_begin_qpc{}, sample_end_qpc{};
        bool raw_clock_valid = QueryPerformanceFrequency(&qpc_frequency) && qpc_frequency.QuadPart > 0
            && QueryPerformanceCounter(&sample_begin_qpc);
        FILETIME sample_begin{}; GetSystemTimePreciseAsFileTime(&sample_begin);
        const auto begin_filetime = (std::uint64_t(sample_begin.dwHighDateTime) << 32) | sample_begin.dwLowDateTime;
        bool first = true;
        for (const auto& entry : adapters_) {
            if (!first) { rows << ','; readability_rows << ','; }
            first = false;
            unsigned adapter_readable = 0;
            const bool nodes_known = SUCCEEDED(entry.described) && entry.status >= 0 && entry.nodes > 0;
            all_valid = all_valid && nodes_known;
            rows << "{\"adapter_ordinal\":" << entry.ordinal << ",\"adapter_luid_u64\":";
            if (SUCCEEDED(entry.described)) rows << LuidBits(entry.luid); else rows << "null";
            rows << ",\"description_hresult\":" << static_cast<std::uint32_t>(entry.described)
                 << ",\"adapter_flags\":";
            if (SUCCEEDED(entry.described)) rows << entry.flags; else rows << "null";
            rows << ",\"scheduler_node_count\":" << entry.nodes
                 << ",\"adapter_ntstatus\":" << static_cast<std::uint32_t>(entry.status) << ",\"nodes\":[";
            for (UINT node = 0; node < entry.nodes; ++node) {
                if (node) rows << ',';
                D3DKMT_QUERYSTATISTICS stats{};
                stats.Type = D3DKMT_QUERYSTATISTICS_PROCESS_NODE;
                stats.AdapterLuid = entry.luid;
                stats.hProcess = process_.value;
                stats.QueryProcessNode.NodeId = node;
                LARGE_INTEGER query_begin_qpc{}, query_end_qpc{};
                const bool query_clock_begin = QueryPerformanceCounter(&query_begin_qpc) != FALSE;
                const auto status = query_ && process_.value ? query_(&stats) : static_cast<NTSTATUS>(0xc0000002u);
                const bool query_clock_end = QueryPerformanceCounter(&query_end_qpc) != FALSE;
                raw_clock_valid = raw_clock_valid && query_clock_begin && query_clock_end
                    && sample_begin_qpc.QuadPart <= query_begin_qpc.QuadPart && query_begin_qpc.QuadPart <= query_end_qpc.QuadPart;
                const bool valid = status >= 0;
                ++total_nodes;
                if (valid) { ++readable_nodes; ++adapter_readable; }
                all_valid = all_valid && valid;
                rows << "{\"node_index\":" << node << ",\"availability\":\"" << (valid ? "VALID" : "UNAVAILABLE")
                     << "\",\"ntstatus\":" << static_cast<std::uint32_t>(status) << ",\"qpc_begin\":";
                if (query_clock_begin) rows << query_begin_qpc.QuadPart; else rows << "null";
                rows << ",\"qpc_end\":";
                if (query_clock_end) rows << query_end_qpc.QuadPart; else rows << "null";
                rows << ",\"running_time_raw\":";
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
            readability_rows << "{\"adapter_ordinal\":" << entry.ordinal << ",\"adapter_luid_u64\":";
            if (SUCCEEDED(entry.described)) readability_rows << LuidBits(entry.luid); else readability_rows << "null";
            readability_rows << ",\"total_nodes\":" << entry.nodes << ",\"readable_nodes\":" << adapter_readable
                << ",\"unavailable_nodes\":" << entry.nodes - adapter_readable
                << ",\"state\":\"" << read_state(entry.nodes, adapter_readable) << "\"}";
        }
        const bool topology_after = factory_.Get() != nullptr && factory_->IsCurrent() != FALSE;
        raw_clock_valid = QueryPerformanceCounter(&sample_end_qpc) && raw_clock_valid
            && sample_begin_qpc.QuadPart <= sample_end_qpc.QuadPart;
        FILETIME sample_end{}; GetSystemTimePreciseAsFileTime(&sample_end);
        const auto end_filetime = (std::uint64_t(sample_end.dwHighDateTime) << 32) | sample_end.dwLowDateTime;
        if (FAILED(initialization_)) health_errors.push_back("initialization_failed");
        if (!query_available) health_errors.push_back("query_api_unavailable");
        if (!process_available) health_errors.push_back("process_handle_unavailable");
        if (process_error != ERROR_SUCCESS) health_errors.push_back("process_open_error");
        if (!identity_query_succeeded_ || identity_pid_ != pid || !pid || !process_start_filetime_ || identity_error_)
            health_errors.push_back("process_identity_unavailable");
        if (!sampler_generation_) health_errors.push_back("sampler_generation_invalid");
        if (!enumeration_complete_ || enumeration_terminal_ != DXGI_ERROR_NOT_FOUND)
            health_errors.push_back("hardware_enumeration_incomplete");
        if (!topology_before || !topology_after) health_errors.push_back("topology_not_current");
        if (adapters_.empty()) health_errors.push_back("hardware_adapters_missing");
        bool descriptions_valid = true, adapter_topology_valid = true;
        for (const auto& entry : adapters_) {
            descriptions_valid = descriptions_valid && SUCCEEDED(entry.described);
            adapter_topology_valid = adapter_topology_valid && entry.status >= 0 && entry.nodes > 0;
        }
        if (!descriptions_valid) health_errors.push_back("adapter_description_unavailable");
        if (!adapter_topology_valid) health_errors.push_back("adapter_topology_unavailable");
        if (begin_filetime < process_start_filetime_ || end_filetime < begin_filetime)
            health_errors.push_back("sample_clock_invalid");
        if (!raw_clock_valid) health_errors.push_back("native_query_clock_unavailable");
        all_valid = all_valid && health_errors.empty();
#else
        health_errors.push_back("platform_unsupported");
#endif
        std::ostringstream out;
        out << "{\"schema\":\"wddm_process_packet_statistics_v3\",\"collector\":\"wddm_process_packet_statistics\",\"pid\":" << pid
            << ",\"scope\":\"process_all_enumerated_hardware_adapters_all_scheduler_nodes\""
            << ",\"availability\":\"" << (all_valid ? "VALID" : "UNAVAILABLE")
            << "\",\"initialization_hresult\":" << initialization
            << ",\"query_available\":" << (query_available ? "true" : "false")
            << ",\"process_handle_available\":" << (process_available ? "true" : "false")
            << ",\"process_error\":" << process_error
            << ",\"topology_current\":" << (topology_current ? "true" : "false")
            << ",\"adapters\":[" << rows.str() << "],\"elapsed_us\":"
            << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count();
#if defined(_WIN32)
        out << ",\"ordinal_luid_receipt\":{\"schema\":1,\"sampler_generation\":" << sampler_generation_
            << ",\"process_identity_valid\":" << (identity_query_succeeded_ && identity_pid_ == pid ? "true" : "false")
            << ",\"process_identity_actual_pid\":" << identity_pid_
            << ",\"process_times_query_error\":" << identity_error_
            << ",\"process_start_filetime_100ns\":";
        if (identity_query_succeeded_) out << process_start_filetime_; else out << "null";
        out << ",\"hardware_enumeration_complete\":" << (enumeration_complete_ ? "true" : "false")
            << ",\"enumeration_terminal_hresult\":" << static_cast<std::uint32_t>(enumeration_terminal_)
            << ",\"topology_before\":" << (topology_before ? "true" : "false")
            << ",\"topology_after\":" << (topology_after ? "true" : "false")
            << ",\"sample_begin_filetime_100ns\":" << begin_filetime
            << ",\"sample_end_filetime_100ns\":" << end_filetime
            << ",\"clock_contract\":\"b14_native_process_node_raw_qpc_v1\",\"raw_clock_valid\":" << (raw_clock_valid ? "true" : "false")
            << ",\"qpc_frequency\":";
        if (qpc_frequency.QuadPart > 0) out << qpc_frequency.QuadPart; else out << "null";
        out << ",\"sample_begin_qpc\":";
        if (sample_begin_qpc.QuadPart > 0) out << sample_begin_qpc.QuadPart; else out << "null";
        out << ",\"sample_end_qpc\":";
        if (sample_end_qpc.QuadPart > 0) out << sample_end_qpc.QuadPart; else out << "null";
        out << '}';
#endif
        out << ",\"node_readability\":{\"schema\":\"wddm_process_node_readability_v1\",\"health\":\""
            << (health_errors.empty() ? "HEALTHY" : "ERROR") << "\",\"health_errors\":[";
        for (std::size_t index = 0; index < health_errors.size(); ++index) {
            if (index) out << ',';
            out << '"' << health_errors[index] << '"';
        }
        out << "],\"state\":\"" << read_state(total_nodes, readable_nodes) << "\",\"total_nodes\":" << total_nodes
            << ",\"readable_nodes\":" << readable_nodes << ",\"unavailable_nodes\":" << total_nodes - readable_nodes
            << ",\"adapters\":[" << readability_rows.str() << "]}";
        out << '}';
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
        UINT flags = 0;
        UINT nodes = 0;
        HRESULT described = E_FAIL;
        NTSTATUS status = static_cast<NTSTATUS>(0xc0000002u);
    };
    inline static std::atomic<std::uint64_t> next_generation_{0};
    std::uint64_t sampler_generation_ = 0, process_start_filetime_ = 0;
    bool enumeration_complete_ = false, identity_query_succeeded_ = false;
    DWORD identity_error_ = ERROR_SUCCESS, identity_pid_ = 0;
    HRESULT enumeration_terminal_ = E_PENDING;
    Query query_ = nullptr;
    DWORD process_error_ = ERROR_SUCCESS;
    HRESULT initialization_ = E_FAIL;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory_;
    std::vector<Adapter> adapters_;
#endif
};
} // namespace product_gpu_witness
