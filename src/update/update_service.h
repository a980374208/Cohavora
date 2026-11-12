#pragma once

#include "src/update/update_manifest.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QUrl>

#include <memory>

class QFile;
class QCryptographicHash;
class QNetworkAccessManager;
class QNetworkReply;
class QTemporaryDir;
class QSaveFile;
class QTimer;

namespace Cohavora::Update {

enum class State { Idle, Checking, UpToDate, Available, Downloading, Ready,
                   Verifying, InstallerStarted, Failed, Exported };
enum class Error { None, NotConfigured, InvalidConfiguration, Network, InvalidManifest,
                   Disk, Integrity, Launch };

struct Configuration {
    QUrl primaryUrl;
    QUrl fallbackUrl;
    QByteArray publicKey;
    int requestTimeoutMs = 10000;
    int downloadTimeoutMs = 15 * 60 * 1000;
};

// All methods and state belong to the QObject's thread. Network callbacks and
// bounded file work are queued on that same thread, independently of Session.
class UpdateService final : public QObject {
    Q_OBJECT

public:
    // An injected manager must belong to this thread; ownership transfers here.
    explicit UpdateService(Configuration configuration, QString currentVersion,
                           QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    ~UpdateService() override;

    static Configuration BuildConfiguration();

    State state() const { return state_; }
    Error error() const { return error_; }
    const Release &release() const { return release_; }
    bool launchPending() const { return launchPending_; }
    bool checksConfigured() const;
    const QString &exportedPackagePath() const { return exportedPackagePath_; }

    void check();
    void download();
    void cancel();

    // Accept an asynchronous re-verification and launch request. Failure is
    // reported through changed()/error(); the installed application is not quit.
    bool launchInstaller();
    // Linux only: create a new private subdirectory in the selected directory,
    // copy from one verified open descriptor into QSaveFile, then commit. Never
    // replaces a selected existing file, executes a package or starts elevation.
    bool exportPackage(const QString &parentDirectory);

signals:
    void changed();
    void downloadProgress(qint64 received, qint64 total);
    void installerStarted();

private:
    enum class Operation { None, Manifest, Installer, Launch, Export };

    bool hasValidConfiguration();
    void setState(State state, Error error = Error::None);
    void stopActiveOperation();
    void discardDownload();
    void beginManifestSource();
    void beginDownloadSource();
    void beginRequest(const QUrl &url);
    void consumeReply(QNetworkReply *reply, quint64 generation);
    void finishReply(QNetworkReply *reply, quint64 generation);
    void retryOrFail(Error error);
    void fail(Error error);
    void verifyInstallerChunk(quint64 generation);
    bool exportDirectoryMatchesPath() const;
    bool isCurrent(QNetworkReply *reply, quint64 generation) const;

    Configuration configuration_;
    QString currentVersion_;
    State state_ = State::Idle;
    Error error_ = Error::None;
    Release release_;
    QNetworkAccessManager *network_ = nullptr;
    QTimer *deadline_ = nullptr;
    QPointer<QNetworkReply> reply_;
    Operation operation_ = Operation::None;
    quint64 generation_ = 0;
    QList<QUrl> sources_;
    qsizetype sourceIndex_ = 0;
    int redirectCount_ = 0;
    QByteArray manifestBytes_;
    qint64 receivedBytes_ = 0;
    bool drainQueued_ = false;
    bool launchPending_ = false;
    void *directoryLock_ = nullptr;
    QString installerPath_;
    std::unique_ptr<QTemporaryDir> downloadDirectory_;
    std::unique_ptr<QFile> file_;
    std::unique_ptr<QCryptographicHash> hash_;
    std::unique_ptr<QSaveFile> exportFile_;
    std::unique_ptr<QTemporaryDir> exportDirectory_;
    int exportParentDescriptor_ = -1;
    int exportDirectoryDescriptor_ = -1;
    QString exportDirectoryPath_;
    QString exportedPackagePath_;
};

} // namespace Cohavora::Update
