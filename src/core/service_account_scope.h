#pragma once

#include "src/net/credential_store.h"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>

namespace OpenMeeting {

inline QString localServiceAccountScopeKey(bool loggedIn, const QString &userId, const QString &serviceUrl) {
    const QString accountId = userId.trimmed();
    if (!loggedIn || accountId.isEmpty()) return QStringLiteral("anonymous");

    QByteArray scope = canonicalServiceUrl(serviceUrl).toUtf8();
    scope.append('\0');
    scope.append(accountId.toUtf8());
    const QByteArray digest = QCryptographicHash::hash(scope, QCryptographicHash::Sha256).toHex();
    // Legacy account-only paths have no trustworthy service binding.
    return QStringLiteral("acct_v2_") + QString::fromLatin1(digest.left(32));
}

} // namespace OpenMeeting
