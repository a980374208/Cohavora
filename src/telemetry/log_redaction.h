#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace livekit::secure_log {

inline constexpr std::size_t kMaxInputBytes = 16 * 1024;
inline constexpr std::size_t kMaxOutputBytes = 4 * 1024;

std::string SecretSummary();
std::string OpaqueSummary(std::string_view kind);
std::string EndpointSummary(std::string_view endpoint);
std::string ErrorCodeSummary(std::string_view stage,
                             int code,
                             std::string_view category);
std::string ExceptionSummary(std::string_view stage);
std::string SdpSummary(std::string_view kind, std::string_view sdp);

// Final defensive boundary for trusted templates. Dynamic network payloads and
// exception text must be summarized at their source before reaching this API.
std::string SanitizeForOutput(std::string_view value);

} // namespace livekit::secure_log
