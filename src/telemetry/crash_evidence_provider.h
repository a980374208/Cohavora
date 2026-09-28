#pragma once

#include "stability_ledger.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace livekit::telemetry {

struct CrashEvidenceMetadata {
    std::string process_run_id;
    std::string build_id;
    std::uint32_t code = 0;
    std::uint64_t main_module_rva = 0;
    bool main_module_address = false;
};

struct CrashEvidenceScan {
    StabilityRecoveryEvidence recovery;
    std::vector<CrashEvidenceMetadata> records;
    std::uint64_t rejected_files = 0;
};

// Records fixed-size exception metadata in every configuration. Debug and
// RelWithDebInfo also write local minidumps; Release does not capture memory.
// Never writes through the normal logger or uploads data.
class CrashEvidenceProvider final {
public:
    static std::filesystem::path DefaultRoot();
    static bool CollectionEnabled(const std::filesystem::path& root);
    static bool SetCollectionEnabled(const std::filesystem::path& root,
                                     bool enabled);
    static CrashEvidenceScan Scan(const std::filesystem::path& root,
                                  bool enabled = true);
    static std::unique_ptr<CrashEvidenceProvider> Install(
        const std::filesystem::path& root, std::string process_run_id,
        std::string build_id, bool enabled = true);

    ~CrashEvidenceProvider();
    CrashEvidenceProvider(const CrashEvidenceProvider&) = delete;
    CrashEvidenceProvider& operator=(const CrashEvidenceProvider&) = delete;

    bool installed() const noexcept;
    const std::string& reason() const noexcept { return reason_; }

private:
    struct Impl;
    CrashEvidenceProvider() = default;

    std::unique_ptr<Impl> impl_;
    std::string reason_ = "unsupported_platform";
};

} // namespace livekit::telemetry
