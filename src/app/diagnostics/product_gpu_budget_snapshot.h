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
#include <d3d12.h>
#include <wrl/client.h>
#endif

namespace product_gpu_witness {

class BudgetSampler final {
public:
    BudgetSampler() {
#if defined(_WIN32)
        initialization_ = CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
        if (FAILED(initialization_)) return;
        // A temporary device discovers linked memory nodes before measurement.
        // It submits no work and is released before the first budget snapshot.
        d3d12_ = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto create_device = d3d12_ ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
            GetProcAddress(d3d12_, "D3D12CreateDevice")) : nullptr;
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
            entry.activation = FAILED(described) ? described : adapter.As(&entry.adapter);
            if (create_device && SUCCEEDED(entry.activation)) {
                Microsoft::WRL::ComPtr<ID3D12Device> node_device;
                entry.node_query = create_device(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                    IID_PPV_ARGS(&node_device));
                if (SUCCEEDED(entry.node_query) && node_device) entry.node_count = node_device->GetNodeCount();
            }
            adapters_.push_back(std::move(entry));
        }
#endif
    }

    ~BudgetSampler() {
#if defined(_WIN32)
        if (d3d12_) FreeLibrary(d3d12_);
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
            const bool nodes_known = SUCCEEDED(entry.node_query) && entry.node_count > 0;
            all_valid = all_valid && nodes_known;
            rows << "{\"adapter_ordinal\":" << entry.ordinal
                 << ",\"node_count\":" << entry.node_count
                 << ",\"node_count_hresult\":" << static_cast<unsigned>(entry.node_query)
                 << ",\"nodes\":[";
            for (UINT node = 0; node < entry.node_count; ++node) {
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
        out << "{\"collector\":\"dxgi_in_process_budget\",\"pid\":" << process_id
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
        HRESULT activation = E_FAIL;
        HRESULT node_query = E_NOINTERFACE;
        UINT node_count = 0;
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    };
    HRESULT initialization_ = E_FAIL;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory_;
    HMODULE d3d12_ = nullptr;
    std::vector<Adapter> adapters_;
    unsigned excluded_software_ = 0;
#endif
};

} // namespace product_gpu_witness
