#pragma once

#include "src/net/credential_store.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtCore/QUrl>

namespace OpenMeeting {

// Must be initialized once, before SessionManager or the HTTP client is used.
// Before initialization the policy fails closed and rejects HTTP.
void initializeServiceEndpointPolicy(bool debugHttpEnabled);
bool isDebugHttpTransportEnabled();

enum class ServiceEndpointStatus {
    Unconfigured,
    InvalidUrl,
    HttpsAllowed,
    DebugHttpAllowed,
    InsecureTransportBlocked,
};

struct ServiceEndpointPolicy {
    ServiceEndpointStatus status = ServiceEndpointStatus::Unconfigured;
    QString canonicalUrl;

    bool requestAllowed() const {
        return status == ServiceEndpointStatus::HttpsAllowed ||
               status == ServiceEndpointStatus::DebugHttpAllowed;
    }

    bool persistenceAllowed() const {
        return requestAllowed();
    }

    bool isDebugHttp() const {
        return status == ServiceEndpointStatus::DebugHttpAllowed;
    }
};

inline bool hasSafeServicePath(const QUrl &url) {
    auto encodedPath = url.path(QUrl::FullyEncoded);
    static const QRegularExpression encodedSeparator(
        QStringLiteral(R"(%(?:2f|5c))"), QRegularExpression::CaseInsensitiveOption);
    while (true) {
        if (encodedSeparator.match(encodedPath).hasMatch() || encodedPath.contains('\\')) {
            return false;
        }
        for (const auto &segment : encodedPath.split('/')) {
            const auto decoded = QUrl::fromPercentEncoding(segment.toUtf8());
            if (decoded == QStringLiteral(".") || decoded == QStringLiteral("..") ||
                decoded.contains('/') || decoded.contains('\\')) {
                return false;
            }
        }
        const auto decodedPath = QUrl::fromPercentEncoding(encodedPath.toUtf8());
        if (decodedPath == encodedPath) return true;
        if (decodedPath.size() >= encodedPath.size()) return false;
        encodedPath = decodedPath;
    }
}

inline ServiceEndpointPolicy evaluateServiceEndpoint(const QString &input) {
    const auto text = input.trimmed();
    if (text.isEmpty()) return {};

    const auto canonical = canonicalServiceUrl(text);
    if (canonical.isEmpty()) {
        return {ServiceEndpointStatus::InvalidUrl, {}};
    }

    const QUrl url(canonical, QUrl::StrictMode);
    const int port = url.port(-1);
    if (port == 0 || port > 65535 || !hasSafeServicePath(url)) {
        return {ServiceEndpointStatus::InvalidUrl, {}};
    }
    if (url.scheme() == QStringLiteral("https")) {
        return {ServiceEndpointStatus::HttpsAllowed, canonical};
    }
    if (isDebugHttpTransportEnabled()) {
        return {ServiceEndpointStatus::DebugHttpAllowed, canonical};
    }
    return {ServiceEndpointStatus::InsecureTransportBlocked, canonical};
}

inline bool serviceAllowsCredentialPersistence(const QString &input) {
    return evaluateServiceEndpoint(input).persistenceAllowed();
}

inline QString serviceEndpointErrorMessage(ServiceEndpointStatus status) {
    switch (status) {
    case ServiceEndpointStatus::Unconfigured:
        return QString::fromUtf8("尚未配置 HTTPS 服务器地址。");
    case ServiceEndpointStatus::InvalidUrl:
        return QString::fromUtf8("服务器地址格式无效。");
    case ServiceEndpointStatus::InsecureTransportBlocked:
        return QString::fromUtf8("服务器必须使用 HTTPS；HTTP 仅可通过 --debug 启用。");
    default:
        return {};
    }
}

} // namespace OpenMeeting
