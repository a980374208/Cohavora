#pragma once

// QueryVideoMemoryInfo reports the CALLING process, not an arbitrary PID.
// This sampler must live in the product process to witness its GPU budgets.
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

class BudgetSampler final {
public:
    BudgetSampler() {
#if defined(_WIN32)
        initialization_ = CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
        if (FAILED(initialization_)) return;
        // DXGI NodeIndex identifies a physical adapter within a linked adapter,
        // not a scheduler engine. Discover that count without creating a D3D
        // device: even a device which submits no application commands can
        // cause driver initialization/paging work in the measured process.
        gdi32_ = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto open_adapter = gdi32_ ? reinterpret_cast<PFND3DKMT_OPENADAPTERFROMLUID>(
            GetProcAddress(gdi32_, "D3DKMTOpenAdapterFromLuid")) : nullptr;
        const auto query_adapter = gdi32_ ? reinterpret_cast<PFND3DKMT_QUERYADAPTERINFO>(
            GetProcAddress(gdi32_, "D3DKMTQueryAdapterInfo")) : nullptr;
        const auto close_adapter = gdi32_ ? reinterpret_cast<PFND3DKMT_CLOSEADAPTER>(
            GetProcAddress(gdi32_, "D3DKMTCloseAdapter")) : nullptr;
        topology_api_available_ = open_adapter && query_adapter && close_adapter;
        for (UINT ordinal = 0; ; ++ordinal) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            const auto hr = factory_->EnumAdapters1(ordinal, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) { initialization_ = hr; break; }
            DXGI_ADAPTER_DESC1 desc{};
            const auto described = adapter->GetDesc1(&desc);
            if (SUCCEEDED(described) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                ++excluded_software_;
                continue;
            }
            Adapter entry;
            entry.ordinal = ordinal;
            entry.described = described;
            if (SUCCEEDED(described)) entry.luid = desc.AdapterLuid;
            entry.activation = FAILED(described) ? described : adapter.As(&entry.adapter);
            if (topology_api_available_ && SUCCEEDED(entry.activation)) {
                D3DKMT_OPENADAPTERFROMLUID opened{};
                opened.AdapterLuid = entry.luid;
                entry.open_status = open_adapter(&opened);
                if (entry.open_status >= 0) {
                    D3DKMT_PHYSICAL_ADAPTER_COUNT count{};
                    D3DKMT_QUERYADAPTERINFO query{};
                    query.hAdapter = opened.hAdapter;
                    query.Type = KMTQAITYPE_PHYSICALADAPTERCOUNT;
                    query.pPrivateDriverData = &count;
                    query.PrivateDriverDataSize = sizeof(count);
                    entry.node_status = query_adapter(&query);
                    if (entry.node_status >= 0) entry.node_count = count.Count;
                    D3DKMT_CLOSEADAPTER closed{};
                    closed.hAdapter = opened.hAdapter;
                    entry.close_status = close_adapter(&closed);
                }
            }
            adapters_.push_back(std::move(entry));
        }
#endif
    }

    ~BudgetSampler() {
#if defined(_WIN32)
        if (gdi32_) FreeLibrary(gdi32_);
#endif
    }
    BudgetSampler(const BudgetSampler&) = delete;
    BudgetSampler& operator=(const BudgetSampler&) = delete;

    std::string SampleJson() const {
        const auto began = std::chrono::steady_clock::now();
        std::ostringstream rows;
        bool all_valid = false;
        bool topology_current = false;
        unsigned process_id = 0;
        unsigned initialization = 0;
        unsigned excluded = 0;
#if defined(_WIN32)
        process_id = GetCurrentProcessId();
        initialization = static_cast<unsigned>(initialization_);
        excluded = excluded_software_;
        topology_current = factory_.Get() != nullptr && factory_->IsCurrent() != FALSE;
        all_valid = SUCCEEDED(initialization_) && !adapters_.empty() && topology_current;
        bool first = true;
        for (const auto& entry : adapters_) {
            if (!first) rows << ',';
            first = false;
            const bool nodes_known = SUCCEEDED(entry.activation) && entry.open_status >= 0
                && entry.node_status >= 0 && entry.close_status >= 0 && entry.node_count > 0;
            all_valid = all_valid && nodes_known;
            rows << "{\"adapter_ordinal\":" << entry.ordinal
                 << ",\"description_hresult\":" << static_cast<std::uint32_t>(entry.described)
                 << ",\"activation_hresult\":" << static_cast<std::uint32_t>(entry.activation)
                 << ",\"adapter_luid_u64\":";
            if (SUCCEEDED(entry.described)) {
                rows << ((std::uint64_t(static_cast<std::uint32_t>(entry.luid.HighPart)) << 32) | entry.luid.LowPart);
            } else rows << "null";
            rows << ",\"node_count\":";
            if (entry.node_status >= 0 && entry.node_count > 0) rows << entry.node_count; else rows << "null";
            rows << ",\"open_adapter_ntstatus\":" << static_cast<std::uint32_t>(entry.open_status)
                 << ",\"node_count_ntstatus\":" << static_cast<std::uint32_t>(entry.node_status)
                 << ",\"close_adapter_ntstatus\":" << static_cast<std::uint32_t>(entry.close_status)
                 << ",\"nodes\":[";
            for (UINT node = 0; nodes_known && node < entry.node_count; ++node) {
                if (node) rows << ',';
                rows << "{\"node_index\":" << node;
                for (const auto group : {DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
                    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                    const auto hr = entry.adapter ? entry.adapter->QueryVideoMemoryInfo(node, group, &info) : entry.activation;
                    const bool valid = SUCCEEDED(hr);
                    all_valid = all_valid && valid;
                    rows << ",\"" << (group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL ? "local" : "nonlocal")
                         << "\":{\"availability\":\"" << (valid ? "VALID" : "UNAVAILABLE")
                         << "\",\"hresult\":" << static_cast<unsigned>(hr);
                    auto field = [&](const char* name, std::uint64_t value) {
                        rows << ",\"" << name << "\":";
                        if (valid) rows << value; else rows << "null";
                    };
                    field("budget_bytes", info.Budget);
                    field("current_usage_bytes", info.CurrentUsage);
                    field("available_for_reservation_bytes", info.AvailableForReservation);
                    field("current_reservation_bytes", info.CurrentReservation);
                    rows << '}';
                }
                rows << '}';
            }
            rows << "]}";
        }
#endif
        std::ostringstream out;
        out << "{\"schema\":2,\"collector\":\"dxgi_in_process_budget\",\"pid\":" << process_id
            << ",\"topology_source\":\"d3dkmt_physical_adapter_count\",\"topology_api_available\":"
#if defined(_WIN32)
            << (topology_api_available_ ? "true" : "false")
#else
            << "false"
#endif
            << ",\"scope\":\"calling_process_all_enumerated_hardware_adapters_all_nodes\""
            << ",\"availability\":\""
            << (all_valid ? "VALID" : "UNAVAILABLE") << "\",\"initialization_hresult\":" << initialization
            << ",\"software_adapters_excluded\":" << excluded << ",\"adapters\":[" << rows.str()
            << "],\"topology_current\":" << (topology_current ? "true" : "false")
            << ",\"elapsed_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - began).count() << '}';
        return out.str();
    }

private:
#if defined(_WIN32)
    struct Adapter {
        UINT ordinal{};
        HRESULT described = E_FAIL;
        LUID luid{};
        HRESULT activation = E_FAIL;
        NTSTATUS open_status = static_cast<NTSTATUS>(0xc0000002u);
        NTSTATUS node_status = static_cast<NTSTATUS>(0xc0000002u);
        NTSTATUS close_status = static_cast<NTSTATUS>(0xc0000002u);
        UINT node_count = 0;
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    };
    HRESULT initialization_ = E_FAIL;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory_;
    HMODULE gdi32_ = nullptr;
    bool topology_api_available_ = false;
    std::vector<Adapter> adapters_;
    unsigned excluded_software_ = 0;
#endif
};

} // namespace product_gpu_witness
