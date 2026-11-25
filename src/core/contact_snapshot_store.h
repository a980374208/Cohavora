#pragma once

#include "src/net/contact_types.h"

#include <QtCore/QSettings>
#include <QtCore/QString>

#include <vector>

namespace OpenMeeting {

struct ContactCacheSnapshot {
    qint64 syncSeq = 0;
    qint64 treeVersion = 0;
    qint64 serverTimeMs = 0;
    std::vector<ContactRelation> contacts;
    std::vector<ContactGroup> groups;
    std::vector<OrgDepartment> departments;
};

class ContactSnapshotStore final {
public:
    explicit ContactSnapshotStore(QString baseDirectory = {});

    [[nodiscard]] static QString resolveBaseDirectory(const QSettings &settings);
    [[nodiscard]] static QString accountScopeKey(bool loggedIn, const QString &userId);

    [[nodiscard]] const QString &baseDirectory() const { return _baseDirectory; }

    [[nodiscard]] ContactCacheSnapshot load(const QString &accountKey) const;
    bool save(const QString &accountKey, const ContactCacheSnapshot &snapshot) const;
    bool clear(const QString &accountKey) const;

private:
    [[nodiscard]] QString snapshotFilePath(const QString &accountKey) const;

    QString _baseDirectory;
};

} // namespace OpenMeeting
