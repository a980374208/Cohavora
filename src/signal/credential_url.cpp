#include "credential_url.h"

#include <asio/ip/address.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <utility>

namespace livekit {
namespace {

bool IsAsciiControlOrSpace(unsigned char value) {
    return value <= 0x20 || value == 0x7f;
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool IsValidScheme(std::string_view scheme) {
    if (scheme.empty() || !std::isalpha(static_cast<unsigned char>(scheme.front()))) {
        return false;
    }
    return std::all_of(scheme.begin() + 1, scheme.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '+' || c == '-' || c == '.';
    });
}

bool IsValidDnsHost(std::string_view host) {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') {
        return false;
    }

    size_t label_start = 0;
    while (label_start < host.size()) {
        const size_t label_end = host.find('.', label_start);
        const size_t end = label_end == std::string_view::npos ? host.size() : label_end;
        const auto label = host.substr(label_start, end - label_start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') {
            return false;
        }
        if (!std::all_of(label.begin(), label.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '-';
            })) {
            return false;
        }
        if (label_end == std::string_view::npos) break;
        label_start = label_end + 1;
    }
    return true;
}

bool IsValidHost(const std::string& host, bool bracketed) {
    std::error_code address_error;
    const auto address = asio::ip::make_address(host, address_error);
    if (!address_error) {
        return bracketed ? address.is_v6() : address.is_v4();
    }
    return !bracketed && IsValidDnsHost(host);
}

bool IsAllowedScheme(const std::string& scheme, CredentialUrlKind kind) {
    switch (kind) {
    case CredentialUrlKind::LiveKitBase:
        return scheme == "http" || scheme == "https" ||
               scheme == "ws" || scheme == "wss";
    case CredentialUrlKind::WebSocket:
        return scheme == "ws" || scheme == "wss";
    case CredentialUrlKind::Http:
        return scheme == "http" || scheme == "https";
    }
    return false;
}

bool IsSecureScheme(const std::string& scheme) {
    return scheme == "https" || scheme == "wss";
}

std::string DefaultPort(const std::string& scheme) {
    return IsSecureScheme(scheme) ? "443" : "80";
}

void SetError(std::error_code& error, std::errc value) {
    error = std::make_error_code(value);
}

} // namespace

std::optional<Url> AdmitCredentialUrl(
    const std::string& input,
    CredentialUrlKind kind,
    CredentialUrlPolicy policy,
    std::error_code& error) {
    error.clear();
    if (input.empty() || std::any_of(input.begin(), input.end(), [](unsigned char c) {
            return IsAsciiControlOrSpace(c);
        })) {
        SetError(error, std::errc::invalid_argument);
        return std::nullopt;
    }

    const size_t scheme_end = input.find("://");
    if (scheme_end == std::string::npos) {
        SetError(error, std::errc::invalid_argument);
        return std::nullopt;
    }

    std::string scheme = input.substr(0, scheme_end);
    if (!IsValidScheme(scheme)) {
        SetError(error, std::errc::invalid_argument);
        return std::nullopt;
    }
    scheme = LowerAscii(std::move(scheme));
    if (!IsAllowedScheme(scheme, kind)) {
        SetError(error, std::errc::protocol_not_supported);
        return std::nullopt;
    }

    const size_t authority_start = scheme_end + 3;
    const size_t authority_end = input.find_first_of("/?#", authority_start);
    const std::string authority = input.substr(
        authority_start,
        authority_end == std::string::npos ? std::string::npos : authority_end - authority_start);
    if (authority.empty() || authority.find('@') != std::string::npos) {
        SetError(error, std::errc::invalid_argument);
        return std::nullopt;
    }

    std::string host;
    std::string port;
    bool bracketed = false;
    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) {
            SetError(error, std::errc::invalid_argument);
            return std::nullopt;
        }
        bracketed = true;
        host = authority.substr(1, close - 1);
        const std::string suffix = authority.substr(close + 1);
        if (!suffix.empty()) {
            if (suffix.front() != ':' || suffix.size() == 1) {
                SetError(error, std::errc::invalid_argument);
                return std::nullopt;
            }
            port = suffix.substr(1);
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            if (authority.find(':') != colon || colon == 0 || colon + 1 == authority.size()) {
                SetError(error, std::errc::invalid_argument);
                return std::nullopt;
            }
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        } else {
            host = authority;
        }
    }

    host = LowerAscii(std::move(host));
    if (!IsValidHost(host, bracketed)) {
        SetError(error, std::errc::invalid_argument);
        return std::nullopt;
    }

    if (!port.empty()) {
        if (!std::all_of(port.begin(), port.end(), [](unsigned char c) {
                return std::isdigit(c);
            })) {
            SetError(error, std::errc::invalid_argument);
            return std::nullopt;
        }
        unsigned long parsed_port = 0;
        try {
            parsed_port = std::stoul(port);
        } catch (...) {
            SetError(error, std::errc::invalid_argument);
            return std::nullopt;
        }
        if (parsed_port == 0 || parsed_port > 65535) {
            SetError(error, std::errc::invalid_argument);
            return std::nullopt;
        }
        port = std::to_string(parsed_port);
    } else {
        port = DefaultPort(scheme);
    }

    std::string path = "/";
    std::string query;
    if (authority_end != std::string::npos) {
        if (input[authority_end] == '#') {
            SetError(error, std::errc::invalid_argument);
            return std::nullopt;
        }
        if (input[authority_end] == '?') {
            query = input.substr(authority_end + 1);
        } else {
            const size_t query_start = input.find('?', authority_end);
            const size_t fragment_start = input.find('#', authority_end);
            if (fragment_start != std::string::npos) {
                SetError(error, std::errc::invalid_argument);
                return std::nullopt;
            }
            path = input.substr(
                authority_end,
                query_start == std::string::npos ? std::string::npos : query_start - authority_end);
            if (query_start != std::string::npos) query = input.substr(query_start + 1);
        }
    }
    if (path.empty() || path.front() != '/' || path.find('\\') != std::string::npos ||
        query.find('\\') != std::string::npos || query.find('#') != std::string::npos) {
        SetError(error, std::errc::invalid_argument);
        return std::nullopt;
    }

    const bool secure = IsSecureScheme(scheme);
    if (!secure && (!policy.allow_insecure || policy.require_secure)) {
        SetError(error, std::errc::permission_denied);
        return std::nullopt;
    }

    return Url{std::move(scheme), std::move(host), std::move(port),
               std::move(path), std::move(query), secure};
}

CredentialUrlPolicy PolicyForDerivedCredentialUrl(
    CredentialUrlPolicy current_policy,
    const Url& admitted_hop) {
    current_policy.require_secure =
        current_policy.require_secure || admitted_hop.secure;
    return current_policy;
}

Url ConvertCredentialUrl(const Url& input, CredentialUrlKind kind) {
    Url result = input;
    switch (kind) {
    case CredentialUrlKind::WebSocket:
        result.scheme = result.secure ? "wss" : "ws";
        break;
    case CredentialUrlKind::Http:
        result.scheme = result.secure ? "https" : "http";
        break;
    case CredentialUrlKind::LiveKitBase:
        break;
    }
    return result;
}

std::string FormatUrlAuthority(const Url& url) {
    std::string result = url.host.find(':') == std::string::npos
        ? url.host
        : "[" + url.host + "]";
    if (url.port != DefaultPort(url.scheme)) result += ":" + url.port;
    return result;
}

std::string FormatCredentialUrl(const Url& url) {
    std::string result = url.scheme + "://" + FormatUrlAuthority(url) + url.path;
    if (!url.query.empty()) result += "?" + url.query;
    return result;
}

} // namespace livekit
