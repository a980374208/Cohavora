#pragma once

#include "diagnostic_pipeline.h"

#include <memory>

namespace livekit::diagnostic {

bool InstallSafeSpdlogAdapter(
    const std::shared_ptr<DiagnosticPipeline>& pipeline) noexcept;

} // namespace livekit::diagnostic
