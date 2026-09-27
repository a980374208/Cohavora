#pragma once

#include <string>

namespace livekit::telemetry {

// Hashes the executable actually running, including uncommitted internal builds.
// Returns "unknown" when the file cannot be read.
const std::string& CurrentExecutableBuildId();

// CodeView RSDS GUID+Age embedded in the running executable. No PDB path is
// returned because it may contain a developer's user directory.
const std::string& CurrentExecutablePdbIdentity();

} // namespace livekit::telemetry
