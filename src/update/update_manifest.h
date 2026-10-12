#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

#include <optional>

namespace Cohavora::Update {

inline constexpr qint64 MaxManifestBytes = 96 * 1024;
inline constexpr qint64 MaxInstallerBytes = 2LL * 1024 * 1024 * 1024;

struct Release {
    QString version;
    QString notes;
    QByteArray sha256;
    qint64 size = 0;
    QList<QUrl> urls;
    // Authenticated release metadata only; client enforcement is deferred.
    bool forceUpdate = false;
};

bool IsSecureUrl(const QUrl &url);
// HTTPS, or the designated Alibaba Web server on HTTP port 80 with a
// canonical static object path below /downloads/ and no query string.
bool IsAllowedUpdateUrl(const QUrl &url);
// Validate original HTTP spelling before QUrl normalizes unreserved escapes
// or ports; HTTP permits no percent encoding and only the exact :80 spelling.
bool IsAllowedUpdateUrlText(const QString &text);
bool IsValidVersion(const QString &version);
bool IsNewerVersion(const QString &candidate, const QString &current);

// publicKey is the 32-byte Ed25519 trust anchor. Parse authenticates the exact
// payload bytes before interpreting release fields; every failure is closed.
std::optional<Release> ParseManifest(
    const QByteArray &envelope,
    const QByteArray &publicKey);

} // namespace Cohavora::Update
