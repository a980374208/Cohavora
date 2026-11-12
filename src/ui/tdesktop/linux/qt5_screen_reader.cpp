#include "base/platform/linux/base_screen_reader_state_linux.h"
#include "base/platform/base_platform_info.h"

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusPendingReply>
#include <QtDBus/QDBusServiceWatcher>
#include <QtDBus/QDBusVariant>

namespace base::Platform {

struct LinuxScreenReaderState::Private : QObject {
    Q_OBJECT

public:
    explicit Private(Fn<void()> notify) : notify_(std::move(notify)) {
        auto bus = QDBusConnection::sessionBus();
        bus.connect(QStringLiteral("org.a11y.Bus"), QStringLiteral("/org/a11y/bus"),
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"), this,
            SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
        auto *service = new QDBusServiceWatcher(QStringLiteral("org.a11y.Bus"), bus,
            QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(service, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this] { refresh(); });
    }

    void refresh() {
        const auto generation = ++generation_;
        enabled = false;
        notify_();
        auto message = QDBusMessage::createMethodCall(
            QStringLiteral("org.a11y.Bus"), QStringLiteral("/org/a11y/bus"),
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
        message.setAutoStartService(false);
        message << QStringLiteral("org.a11y.Status") << QStringLiteral("ScreenReaderEnabled");
        auto *watcher = new QDBusPendingCallWatcher(
            QDBusConnection::sessionBus().asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation](QDBusPendingCallWatcher *finished) {
                const QDBusPendingReply<QDBusVariant> reply = *finished;
                if (generation == generation_) {
                    enabled = !reply.isError() && reply.value().variant().toBool();
                    notify_();
                }
                finished->deleteLater();
            });
    }

    bool enabled = false;

private Q_SLOTS:
    void propertiesChanged(const QString &interface, const QVariantMap &changes,
                           const QStringList &invalidated) {
        if (interface != QStringLiteral("org.a11y.Status")) {
            return;
        }
        const auto property = QStringLiteral("ScreenReaderEnabled");
        if (changes.contains(property)) {
            ++generation_;
            enabled = changes.value(property).toBool();
            notify_();
        } else if (invalidated.contains(property)) {
            refresh();
        }
    }

private:
    Fn<void()> notify_;
    unsigned generation_ = 0;
};

LinuxScreenReaderState::LinuxScreenReaderState()
: _private(std::make_unique<Private>([this] { updateActive(); }))
, _useX11ScreenReaderDetection(::Platform::IsX11())
, _isActive(false) {
    if (_useX11ScreenReaderDetection) {
        refreshStatusProxy();
    } else {
        QAccessible::installActivationObserver(this);
    }
    updateActive();
}

LinuxScreenReaderState::~LinuxScreenReaderState() {
    if (!_useX11ScreenReaderDetection) {
        QAccessible::removeActivationObserver(this);
    }
}

void LinuxScreenReaderState::refreshStatusProxy() {
    _private->refresh();
}

void LinuxScreenReaderState::updateActive() {
    _isActive = qEnvironmentVariableIsSet("QT_LINUX_ACCESSIBILITY_ALWAYS_ON")
        || (_useX11ScreenReaderDetection ? _private->enabled : QAccessible::isActive());
}

void LinuxScreenReaderState::accessibilityActiveChanged(bool) {
    updateActive();
}

bool LinuxScreenReaderState::active() const {
    return _isActive.current();
}

rpl::producer<bool> LinuxScreenReaderState::activeValue() const {
    return _isActive.value();
}

} // namespace base::Platform

#include "qt5_screen_reader.moc"
