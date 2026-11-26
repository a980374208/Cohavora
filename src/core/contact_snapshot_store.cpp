#include "src/core/contact_snapshot_store.h"

#include "src/core/service_account_scope.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>

namespace OpenMeeting {
namespace {

constexpr int kSnapshotSchemaVersion = 1;
constexpr qint64 kMaxSnapshotBytes = 8LL * 1024 * 1024;

QString safeScopeKey(const QString &raw) {
    QString out;
    out.reserve(raw.size());
    for (const QChar ch : raw) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-')) {
            out.append(ch);
        }
        if (out.size() >= 48) break;
    }
    return out.isEmpty() ? QStringLiteral("anonymous") : out;
}

qint64 readSnapshotInt64(const QJsonObject &obj, const QString &key) {
    const QJsonValue v = obj.value(key);
    if (v.isUndefined() || v.isNull()) return 0;
    if (v.isString()) {
        bool ok = false;
        const qint64 n = v.toString().toLongLong(&ok);
        return ok ? n : 0;
    }
    return static_cast<qint64>(v.toDouble(0.0));
}

} // namespace

ContactSnapshotStore::ContactSnapshotStore(QString baseDirectory)
    : _baseDirectory(std::move(baseDirectory)) {
    if (_baseDirectory.trimmed().isEmpty()) {
        const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        _baseDirectory = QDir(appData).filePath(QStringLiteral("contacts_cache"));
    }
}

QString ContactSnapshotStore::resolveBaseDirectory(const QSettings &settings) {
    if (settings.format() == QSettings::IniFormat && !settings.fileName().trimmed().isEmpty()) {
        const QFileInfo fi(settings.fileName());
        return QDir(fi.absolutePath()).filePath(QStringLiteral("contacts_cache"));
    }
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(appData).filePath(QStringLiteral("contacts_cache"));
}

QString ContactSnapshotStore::accountScopeKey(bool loggedIn, const QString &userId, const QString &serviceUrl) {
    return localServiceAccountScopeKey(loggedIn, userId, serviceUrl);
}

QString ContactSnapshotStore::snapshotFilePath(const QString &accountKey) const {
    return QDir(QDir(_baseDirectory).filePath(safeScopeKey(accountKey))).filePath(QStringLiteral("snapshot.json"));
}

ContactCacheSnapshot ContactSnapshotStore::load(const QString &accountKey) const {
    ContactCacheSnapshot snap;
    QFile file(snapshotFilePath(accountKey));
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return snap;
    }
    if (file.size() <= 0 || file.size() > kMaxSnapshotBytes) {
        return snap;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return snap;
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toInt(0) != kSnapshotSchemaVersion) {
        return snap;
    }
    snap.syncSeq = readSnapshotInt64(root, QStringLiteral("syncSeq"));
    snap.treeVersion = readSnapshotInt64(root, QStringLiteral("treeVersion"));
    snap.serverTimeMs = readSnapshotInt64(root, QStringLiteral("serverTimeMs"));
    for (const auto &v : root.value(QStringLiteral("contacts")).toArray()) {
        if (v.isObject()) snap.contacts.push_back(parseContactRelation(v.toObject()));
    }
    for (const auto &v : root.value(QStringLiteral("groups")).toArray()) {
        if (v.isObject()) snap.groups.push_back(parseContactGroup(v.toObject()));
    }
    for (const auto &v : root.value(QStringLiteral("departments")).toArray()) {
        if (v.isObject()) snap.departments.push_back(parseOrgDepartment(v.toObject()));
    }
    return snap;
}

bool ContactSnapshotStore::save(const QString &accountKey, const ContactCacheSnapshot &snapshot) const {
    const QString path = snapshotFilePath(accountKey);
    const QFileInfo fi(path);
    if (!QDir().mkpath(fi.absolutePath())) {
        return false;
    }
    QJsonArray contactsArr;
    for (const auto &c : snapshot.contacts) contactsArr.append(serializeContactRelation(c));
    QJsonArray groupsArr;
    for (const auto &g : snapshot.groups) groupsArr.append(serializeContactGroup(g));
    QJsonArray deptsArr;
    for (const auto &d : snapshot.departments) deptsArr.append(serializeOrgDepartment(d));

    QJsonObject root;
    root.insert(QStringLiteral("version"), kSnapshotSchemaVersion);
    root.insert(QStringLiteral("syncSeq"), static_cast<double>(snapshot.syncSeq));
    root.insert(QStringLiteral("treeVersion"), static_cast<double>(snapshot.treeVersion));
    root.insert(QStringLiteral("serverTimeMs"), static_cast<double>(snapshot.serverTimeMs));
    root.insert(QStringLiteral("contacts"), contactsArr);
    root.insert(QStringLiteral("groups"), groupsArr);
    root.insert(QStringLiteral("departments"), deptsArr);

    QSaveFile saveFile(path);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return saveFile.commit();
}

bool ContactSnapshotStore::clear(const QString &accountKey) const {
    const QString path = snapshotFilePath(accountKey);
    if (!QFile::exists(path)) return true;
    return QFile::remove(path);
}

} // namespace OpenMeeting
