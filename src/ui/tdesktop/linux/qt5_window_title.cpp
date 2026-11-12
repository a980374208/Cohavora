#include "ui/platform/ui_platform_window_title.h"
#include "base/platform/linux/base_linux_xsettings.h"

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusPendingReply>
#include <QtDBus/QDBusVariant>

namespace Ui::Platform {
namespace {

TitleLayout ParseLayout(const QString &value) {
    const auto convert = [](const QString &side) {
        std::vector<TitleControl> result;
        for (const auto &item : side.split(',')) {
            if (item == QStringLiteral("minimize")) result.push_back(TitleControl::Minimize);
            else if (item == QStringLiteral("maximize")) result.push_back(TitleControl::Maximize);
            else if (item == QStringLiteral("close")) result.push_back(TitleControl::Close);
        }
        return result;
    };
    const auto parts = value.split(':');
    return {convert(parts.value(0)), convert(parts.value(1))};
}

class Qt5TitleControlsLayout final : public QObject, public TitleControlsLayout {
    Q_OBJECT

public:
    Qt5TitleControlsLayout()
    : TitleControlsLayout({.right = {TitleControl::Minimize, TitleControl::Maximize,
                                    TitleControl::Close}}) {
        if (const auto settings = base::Platform::XCB::XSettings::Instance()) {
            const auto value = settings->setting("Gtk/DecorationLayout");
            if (value.isValid()) _variable = ParseLayout(value.toString());
            lifetime_ = settings->registerCallbackForProperty("Gtk/DecorationLayout",
                [this](auto *, const QByteArray &, const QVariant &value) {
                    if (value.isValid()) _variable = ParseLayout(value.toString());
                });
        }
        auto bus = QDBusConnection::sessionBus();
        bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.Settings"),
            QStringLiteral("SettingChanged"), this,
            SLOT(settingChanged(QString,QString,QDBusVariant)));
        auto message = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("Read"));
        message << QStringLiteral("org.gnome.desktop.wm.preferences")
                << QStringLiteral("button-layout");
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *finished) {
                const QDBusPendingReply<QDBusVariant> reply = *finished;
                if (!reply.isError() && !changed_) {
                    apply(reply.value());
                }
                finished->deleteLater();
            });
    }

private slots:
    void settingChanged(const QString &group, const QString &key, const QDBusVariant &value) {
        if (group == QStringLiteral("org.gnome.desktop.wm.preferences")
            && key == QStringLiteral("button-layout")) {
            changed_ = true;
            apply(value);
        }
    }

private:
    void apply(const QDBusVariant &value) {
        auto unwrapped = value.variant();
        if (unwrapped.userType() == qMetaTypeId<QDBusVariant>()) {
            unwrapped = qvariant_cast<QDBusVariant>(unwrapped).variant();
        }
        if (unwrapped.type() == QVariant::String) {
            _variable = ParseLayout(unwrapped.toString());
        }
    }

    rpl::lifetime lifetime_;
    bool changed_ = false;
};

} // namespace

std::shared_ptr<TitleControlsLayout> TitleControlsLayout::Create() {
    return std::make_shared<Qt5TitleControlsLayout>();
}

} // namespace Ui::Platform

#include "qt5_window_title.moc"
