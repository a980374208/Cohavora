#pragma once

#include <optional>
#include <string>
#include <system_error>

namespace livekit {

enum class CredentialUrlKind {
    LiveKitBase,
    WebSocket,
    Http,
};

struct CredentialUrlPolicy {
    bool allow_insecure = false;
    bool require_secure = false;
};

struct Url {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    std::string query;
    bool secure = false;
};

std::optional<Url> AdmitCredentialUrl(
    const std::string& url,
    CredentialUrlKind kind,
    CredentialUrlPolicy policy,
    std::error_code& error);

CredentialUrlPolicy PolicyForDerivedCredentialUrl(
    CredentialUrlPolicy current_policy,
    const Url& admitted_hop);
Url ConvertCredentialUrl(const Url& url, CredentialUrlKind kind);
std::string FormatUrlAuthority(const Url& url);
std::string FormatCredentialUrl(const Url& url);

} // namespace livekit
