#include "diagnostic_spdlog_bridge.h"

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/sink.h>

namespace livekit::diagnostic {
namespace {

class RestrictedSink final : public spdlog::sinks::sink {
public:
    explicit RestrictedSink(std::weak_ptr<DiagnosticPipeline> pipeline)
        : pipeline_(std::move(pipeline)) {}

    void log(const spdlog::details::log_msg&) override {
        if (const auto pipeline = pipeline_.lock()) pipeline->CountSuppressed();
    }
    void flush() override {}
    void set_pattern(const std::string&) override {}
    void set_formatter(std::unique_ptr<spdlog::formatter>) override {}

private:
    std::weak_ptr<DiagnosticPipeline> pipeline_;
};

} // namespace

bool InstallSafeSpdlogAdapter(
    const std::shared_ptr<DiagnosticPipeline>& pipeline) noexcept {
    if (!pipeline) return false;
    try {
        auto sink = std::make_shared<RestrictedSink>(pipeline);
        auto logger = std::make_shared<spdlog::logger>("cohavora_diagnostics", sink);
        logger->set_level(spdlog::level::trace);
        spdlog::set_default_logger(std::move(logger));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace livekit::diagnostic
