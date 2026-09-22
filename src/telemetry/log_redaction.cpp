#include "log_redaction.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <sstream>
#include <vector>

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

std::string SafeSdpAtom(std::string_view value) {
    if (value.empty() || value.size() > 64) return "unknown";
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (!std::isalnum(byte) && ch != '_' && ch != '-' && ch != '.' &&
            ch != '+' && ch != '/' && ch != '|') {
            return "unknown";
        }
        result.push_back(LowerAscii(ch));
    }
    return result;
}

std::string JoinSdpAtoms(const std::vector<std::string>& values) {
    if (values.empty()) return "none";
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result.push_back('|');
        result += value;
    }
    return result;
}

struct SdpMediaDetail {
    std::string media = "unknown";
    std::string port = "unknown";
    std::string protocol = "unknown";
    std::string mid = "none";
    std::string direction;
    std::string setup = "none";
    std::vector<std::string> formats;
    std::vector<std::string> codecs;
    std::vector<std::string> msids;
    std::size_t format_count = 0;
    std::size_t codec_count = 0;
    std::size_t msid_count = 0;
    std::size_t ssrc_lines = 0;
    std::size_t rid_lines = 0;
    bool rtcp_mux = false;
    bool simulcast = false;
};

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

std::vector<std::string> SdpNegotiationDetails(std::string_view kind,
                                               std::string_view sdp) {
    constexpr std::size_t kMaxLoggedSections = 16;
    constexpr std::size_t kMaxLoggedFormats = 16;
    constexpr std::size_t kMaxLoggedCodecs = 16;
    constexpr std::size_t kMaxLoggedMsids = 4;

    const auto safe_kind = SafeIdentifier(kind);
    if (sdp.size() > kMaxInputBytes) {
        return {"sdp-negotiation{kind=" + safe_kind +
                ",bytes=" + std::to_string(sdp.size()) +
                ",status=omitted,reason=oversized}"};
    }

    std::vector<SdpMediaDetail> sections;
    std::vector<std::string> bundle_mids;
    std::string session_direction;
    SdpMediaDetail* current = nullptr;
    bool inside_media_section = false;
    std::size_t media_section_count = 0;

    std::size_t position = 0;
    while (position < sdp.size()) {
        const auto line_end = sdp.find('\n', position);
        auto line = sdp.substr(
            position,
            line_end == std::string_view::npos ? sdp.size() - position
                                               : line_end - position);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (line.rfind("m=", 0) == 0) {
            ++media_section_count;
            inside_media_section = true;
            current = nullptr;
            if (sections.size() < kMaxLoggedSections) {
                sections.emplace_back();
                current = &sections.back();
                std::istringstream fields{std::string(line.substr(2))};
                std::string media;
                std::string port;
                std::string protocol;
                fields >> media >> port >> protocol;
                current->media = SafeSdpAtom(media);
                current->port = SafeSdpAtom(port);
                current->protocol = SafeSdpAtom(protocol);

                std::string format;
                while (fields >> format) {
                    ++current->format_count;
                    if (current->formats.size() < kMaxLoggedFormats) {
                        current->formats.push_back(SafeSdpAtom(format));
                    }
                }
            }
        } else if (line.rfind("a=group:BUNDLE", 0) == 0 && !inside_media_section) {
            std::istringstream mids{std::string(line.substr(14))};
            std::string mid;
            while (mids >> mid && bundle_mids.size() < kMaxLoggedSections) {
                bundle_mids.push_back(SafeSdpAtom(mid));
            }
        } else if (line == "a=sendrecv" || line == "a=sendonly" ||
                   line == "a=recvonly" || line == "a=inactive") {
            const auto direction = std::string(line.substr(2));
            if (current) current->direction = direction;
            else if (!inside_media_section) session_direction = direction;
        } else if (current && line.rfind("a=mid:", 0) == 0) {
            current->mid = SafeSdpAtom(line.substr(6));
        } else if (current && line.rfind("a=setup:", 0) == 0) {
            current->setup = SafeSdpAtom(line.substr(8));
        } else if (current && line.rfind("a=rtpmap:", 0) == 0) {
            const auto mapping = line.substr(9);
            const auto separator = mapping.find(' ');
            ++current->codec_count;
            if (separator != std::string_view::npos &&
                current->codecs.size() < kMaxLoggedCodecs) {
                current->codecs.push_back(
                    SafeSdpAtom(mapping.substr(0, separator)) + ":" +
                    SafeSdpAtom(mapping.substr(separator + 1)));
            }
        } else if (current && line.rfind("a=msid:", 0) == 0) {
            ++current->msid_count;
            if (current->msids.size() < kMaxLoggedMsids) {
                std::istringstream ids{std::string(line.substr(7))};
                std::string stream_id;
                std::string track_id;
                ids >> stream_id >> track_id;
                current->msids.push_back(
                    SafeSdpAtom(stream_id) + "/" + SafeSdpAtom(track_id));
            }
        } else if (current && line.rfind("a=ssrc:", 0) == 0) {
            ++current->ssrc_lines;
        } else if (current && line.rfind("a=rid:", 0) == 0) {
            ++current->rid_lines;
        } else if (current && line.rfind("a=simulcast:", 0) == 0) {
            current->simulcast = true;
        } else if (current && line == "a=rtcp-mux") {
            current->rtcp_mux = true;
        }

        if (line_end == std::string_view::npos) break;
        position = line_end + 1;
    }

    std::vector<std::string> details;
    details.reserve(sections.size() + 1);
    details.push_back(
        "sdp-negotiation{kind=" + safe_kind +
        ",bytes=" + std::to_string(sdp.size()) +
        ",media_sections=" + std::to_string(media_section_count) +
        ",logged_sections=" + std::to_string(sections.size()) +
        ",bundle=" + JoinSdpAtoms(bundle_mids) +
        ",truncated=" + (media_section_count > sections.size() ? "yes" : "no") + "}");

    for (std::size_t index = 0; index < sections.size(); ++index) {
        const auto& section = sections[index];
        const auto direction = section.direction.empty()
            ? (session_direction.empty() ? "sendrecv" : session_direction)
            : section.direction;
        details.push_back(
            "sdp-mline{kind=" + safe_kind +
            ",index=" + std::to_string(index) +
            ",media=" + section.media +
            ",mid=" + section.mid +
            ",port=" + section.port +
            ",protocol=" + section.protocol +
            ",rejected=" + (section.port == "0" ? "yes" : "no") +
            ",direction=" + direction +
            ",setup=" + section.setup +
            ",formats=" + JoinSdpAtoms(section.formats) +
            ",format_count=" + std::to_string(section.format_count) +
            ",codecs=" + JoinSdpAtoms(section.codecs) +
            ",codec_count=" + std::to_string(section.codec_count) +
            ",msids=" + JoinSdpAtoms(section.msids) +
            ",msid_count=" + std::to_string(section.msid_count) +
            ",ssrc_lines=" + std::to_string(section.ssrc_lines) +
            ",rid_lines=" + std::to_string(section.rid_lines) +
            ",simulcast=" + (section.simulcast ? "yes" : "no") +
            ",rtcp_mux=" + (section.rtcp_mux ? "yes" : "no") + "}");
    }
    return details;
}

std::string SanitizeForOutput(std::string_view value) {
    if (value.size() > kMaxInputBytes) return std::string(kOversizedField);
    if (IsSensitiveText(value)) return std::string(kSensitiveField);
    return NormalizeControls(value);
}

} // namespace livekit::secure_log
