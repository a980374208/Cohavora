#include "log_redaction.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <sstream>

namespace livekit::secure_log {
namespace {

constexpr std::string_view kRedacted = "[redacted]";
constexpr std::string_view kSensitiveField = "[redacted: sensitive log field]";
constexpr std::string_view kOversizedField = "[omitted: oversized log field]";

char LowerAscii(char value) {
    const auto byte = static_cast<unsigned char>(value);
    return static_cast<char>(std::tolower(byte));
}

std::string LowerAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char ch) {
        return LowerAscii(ch);
    });
    return result;
}

bool ContainsControl(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte == 0 || (byte < 0x20 && ch != '\t' && ch != '\r' && ch != '\n') || byte == 0x7f;
    });
}

std::string SafeIdentifier(std::string_view value) {
    if (value.empty() || value.size() > 64) return "unknown";
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (!std::isalnum(byte) && ch != '_' && ch != '-' && ch != '.') {
            return "unknown";
        }
        result.push_back(LowerAscii(ch));
    }
    return result;
}

bool IsSensitiveText(std::string_view value) {
    const auto lower = LowerAscii(value);
    constexpr std::array<std::string_view, 19> markers = {
        "access_token=", "access_token%3d", "refresh_token=", "refresh_token%3d",
        "token=", "token%3d", "password=", "password%3d", "credential=",
        "credential%3d", "secret=", "secret%3d", "authorization:",
        "proxy-authorization:", "cookie:", "set-cookie:", "a=ice-pwd:",
        "a=ice-ufrag:", "candidate:"
    };
    for (const auto marker : markers) {
        if (lower.find(marker) != std::string::npos) return true;
    }

    const auto jwt = lower.find("eyj");
    if (jwt != std::string::npos) {
        const auto first_dot = lower.find('.', jwt + 3);
        const auto second_dot = first_dot == std::string::npos
            ? std::string::npos
            : lower.find('.', first_dot + 1);
        if (second_dot != std::string::npos) return true;
    }
    return false;
}

std::string NormalizeControls(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    bool previous_space = false;
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        const bool replace = ch == '\r' || ch == '\n' || ch == '\t' || byte < 0x20 || byte == 0x7f;
        if (replace) {
            if (!previous_space) result.push_back(' ');
            previous_space = true;
        } else {
            result.push_back(ch);
            previous_space = ch == ' ';
        }
    }
    if (result.size() > kMaxOutputBytes) return std::string(kOversizedField);
    return result;
}

bool ParsePort(std::string_view text, unsigned int* value) {
    if (text.empty() || text.size() > 5) return false;
    unsigned int parsed = 0;
    const auto parsed_result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (parsed_result.ec != std::errc{} || parsed_result.ptr != text.data() + text.size() ||
        parsed == 0 || parsed > std::numeric_limits<unsigned short>::max()) {
        return false;
    }
    *value = parsed;
    return true;
}

std::string RouteName(std::string_view path) {
    if (path.empty() || path == "/") return "root";
    if (path == "/rtc") return "rtc";
    if (path == "/rtc/v1") return "rtc_v1";
    if (path == "/rtc/validate") return "rtc_validate";
    if (path == "/rtc/v1/validate") return "rtc_v1_validate";
    if (path == "/settings/regions") return "settings_regions";
    return "other";
}

} // namespace

std::string SecretSummary() {
    return std::string(kRedacted);
}

std::string OpaqueSummary(std::string_view kind) {
    return "opaque{kind=" + SafeIdentifier(kind) + ",detail=[omitted]}";
}

std::string EndpointSummary(std::string_view endpoint) {
    if (endpoint.empty() || endpoint.size() > kMaxInputBytes || ContainsControl(endpoint)) {
        return "endpoint{invalid}";
    }

    const auto scheme_end = endpoint.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return "endpoint{invalid}";
    }
    const auto scheme = LowerAscii(endpoint.substr(0, scheme_end));
    if (scheme != "ws" && scheme != "wss" && scheme != "http" && scheme != "https") {
        return "endpoint{scheme=unknown,route=other,port=unknown,query=no}";
    }

    const auto authority_start = scheme_end + 3;
    const auto authority_end = endpoint.find_first_of("/?#", authority_start);
    auto authority = endpoint.substr(
        authority_start,
        authority_end == std::string_view::npos ? endpoint.size() - authority_start : authority_end - authority_start);
    if (authority.empty()) return "endpoint{invalid}";
    const auto at = authority.rfind('@');
    if (at != std::string_view::npos) authority.remove_prefix(at + 1);
    if (authority.empty()) return "endpoint{invalid}";

    std::string_view port_text;
    if (authority.front() == '[') {
        const auto closing = authority.find(']');
        if (closing == std::string_view::npos || closing == 1) return "endpoint{invalid}";
        if (closing + 1 < authority.size()) {
            if (authority[closing + 1] != ':') return "endpoint{invalid}";
            port_text = authority.substr(closing + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon || colon == 0) return "endpoint{invalid}";
            port_text = authority.substr(colon + 1);
            authority = authority.substr(0, colon);
        }
        if (authority.empty()) return "endpoint{invalid}";
    }

    unsigned int port = (scheme == "wss" || scheme == "https") ? 443 : 80;
    if (!port_text.empty() && !ParsePort(port_text, &port)) return "endpoint{invalid}";

    std::string_view path = "/";
    bool has_query = false;
    if (authority_end != std::string_view::npos) {
        const auto query_pos = endpoint.find('?', authority_end);
        const auto fragment_pos = endpoint.find('#', authority_end);
        has_query = query_pos != std::string_view::npos &&
            (fragment_pos == std::string_view::npos || query_pos < fragment_pos);
        if (endpoint[authority_end] == '/') {
            const auto path_end = std::min(
                query_pos == std::string_view::npos ? endpoint.size() : query_pos,
                fragment_pos == std::string_view::npos ? endpoint.size() : fragment_pos);
            path = endpoint.substr(authority_end, path_end - authority_end);
        }
    }

    return "endpoint{scheme=" + scheme + ",route=" + RouteName(path) +
        ",port=" + std::to_string(port) + ",query=" + (has_query ? "yes" : "no") + "}";
}

std::string ErrorCodeSummary(std::string_view stage,
                             int code,
                             std::string_view category) {
    return "error{stage=" + SafeIdentifier(stage) + ",category=" + SafeIdentifier(category) +
        ",code=" + std::to_string(code) + ",detail=[omitted]}";
}

std::string ExceptionSummary(std::string_view stage) {
    return "error{stage=" + SafeIdentifier(stage) + ",category=exception,detail=[omitted]}";
}

std::string SdpSummary(std::string_view kind, std::string_view sdp) {
    std::size_t audio = 0;
    std::size_t video = 0;
    std::size_t application = 0;
    std::size_t other = 0;
    if (sdp.size() <= kMaxInputBytes) {
        std::size_t position = 0;
        while (position < sdp.size()) {
            const auto line_end = sdp.find('\n', position);
            auto line = sdp.substr(position, line_end == std::string_view::npos ? sdp.size() - position : line_end - position);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.rfind("m=", 0) == 0) {
                const auto separator = line.find(' ', 2);
                const auto media = LowerAscii(line.substr(2, separator == std::string_view::npos ? line.size() - 2 : separator - 2));
                if (media == "audio") ++audio;
                else if (media == "video") ++video;
                else if (media == "application") ++application;
                else ++other;
            }
            if (line_end == std::string_view::npos) break;
            position = line_end + 1;
        }
    } else {
        other = 1;
    }
    return "sdp{kind=" + SafeIdentifier(kind) + ",bytes=" + std::to_string(sdp.size()) +
        ",audio=" + std::to_string(audio) + ",video=" + std::to_string(video) +
        ",application=" + std::to_string(application) + ",other=" + std::to_string(other) + "}";
}

std::string SanitizeForOutput(std::string_view value) {
    if (value.size() > kMaxInputBytes) return std::string(kOversizedField);
    if (IsSensitiveText(value)) return std::string(kSensitiveField);
    return NormalizeControls(value);
}

} // namespace livekit::secure_log
