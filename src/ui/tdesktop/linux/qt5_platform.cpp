#include "base/platform/base_platform_file_utilities.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusPendingReply>
#include <QtGui/QDesktopServices>

#include <cstdio>
#include <unistd.h>

namespace base::Platform {

void ShowInFolder(const QString &filepath) {
    auto request = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("ShowItems"));
    request << QStringList{QUrl::fromLocalFile(filepath).toString()}
            << QString();
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(request),
        QCoreApplication::instance());
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
        [filepath](QDBusPendingCallWatcher *finished) {
            const QDBusPendingReply<> reply = *finished;
            if (reply.isError()) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(
                    QFileInfo(filepath).absolutePath()));
            }
            finished->deleteLater();
        });
}

QString CurrentExecutablePath(int argc, char *argv[]) {
    const auto procPath = QFileInfo(QStringLiteral("/proc/self/exe"))
        .canonicalFilePath();
    if (!procPath.isEmpty()) {
        return procPath;
    }
    if (argc <= 0 || !argv || !argv[0]) {
        return {};
    }
    const auto argument = QFile::decodeName(argv[0]);
    const auto resolved = QStandardPaths::findExecutable(argument);
    return resolved.isEmpty() ? QFileInfo(argument).absoluteFilePath() : resolved;
}

QString BundledResourcesPath() {
    return QCoreApplication::applicationDirPath();
}

QString FileNameFromUserString(QString name) {
    return name;
}

bool DeleteDirectory(QString path) {
    const QFileInfo info(path);
    if (info.isSymLink()) {
        return QFile::remove(path);
    }
    return QDir(path).removeRecursively();
}

void RemoveQuarantine(const QString &) {
    // Linux does not use the macOS quarantine extended attribute.
}

bool RenameWithOverwrite(const QString &from, const QString &to) {
    return ::rename(QFile::encodeName(from).constData(),
                    QFile::encodeName(to).constData()) == 0;
}

void FlushFileData(QFile &file) {
    if (file.flush() && file.handle() >= 0) {
        ::fsync(file.handle());
    }
}

} // namespace base::Platform
