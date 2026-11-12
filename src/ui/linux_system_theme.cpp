#include "src/ui/linux_system_theme.h"

#include <QtCore/QMetaType>
#include <QtCore/QThread>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusPendingReply>
#include <QtDBus/QDBusServiceWatcher>
#include <utility>

namespace MeetingUI {
namespace {

constexpr auto kService = "org.freedesktop.portal.Desktop";
constexpr auto kPath = "/org/freedesktop/portal/desktop";
constexpr auto kInterface = "org.freedesktop.portal.Settings";
constexpr auto kNamespace = "org.freedesktop.appearance";
constexpr auto kKey = "color-scheme";
constexpr int kCallTimeoutMs = 1500;

QDBusMessage readMessage(const QString &service) {
	auto message = QDBusMessage::createMethodCall(service,
		QString::fromLatin1(kPath), QString::fromLatin1(kInterface), QStringLiteral("Read"));
	message << QString::fromLatin1(kNamespace) << QString::fromLatin1(kKey);
	return message;
}

std::optional<bool> preference(QVariant value) {
	// Older Settings.Read implementations wrap the result in another variant.
	for (int depth = 0; depth != 4 && value.userType() == qMetaTypeId<QDBusVariant>(); ++depth) {
		value = qvariant_cast<QDBusVariant>(value).variant();
	}
	if (value.userType() != QMetaType::UInt) return std::nullopt;
	switch (value.toUInt()) {
	case 1: return true;
	case 2: return false;
	default: return std::nullopt; // 0 means no preference, as do unsupported values.
	}
}

} // namespace

LinuxSystemTheme::LinuxSystemTheme(QObject *parent)
	: LinuxSystemTheme(QDBusConnection::sessionBus(), parent) {
}

LinuxSystemTheme::LinuxSystemTheme(QDBusConnection connection, QObject *parent)
	: QObject(parent), connection_(std::move(connection)) {
	retryTimer_.setSingleShot(true);
	retryTimer_.setInterval(5000);
	connect(&retryTimer_, &QTimer::timeout, this, &LinuxSystemTheme::refresh);
	auto *watcher = new QDBusServiceWatcher(QString::fromLatin1(kService), connection_,
		QDBusServiceWatcher::WatchForOwnerChange, this);
	connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
		[this](const QString &, const QString &, const QString &owner) {
			++ownerQuerySerial_;
			ownerQueryPending_ = false;
			setOwner(owner);
		});
	QTimer::singleShot(0, this, &LinuxSystemTheme::refresh);
}

void LinuxSystemTheme::refresh() {
	Q_ASSERT(QThread::currentThread() == thread());
	if (owner_.isEmpty()) queryOwner();
	else readSetting();
}

void LinuxSystemTheme::queryOwner() {
	if (ownerQueryPending_) return;
	ownerQueryPending_ = true;
	const auto serial = ++ownerQuerySerial_;
	auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
		QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"),
		QStringLiteral("GetNameOwner"));
	message << QString::fromLatin1(kService);
	auto *call = new QDBusPendingCallWatcher(connection_.asyncCall(message, kCallTimeoutMs), this);
	connect(call, &QDBusPendingCallWatcher::finished, this, [this, call, serial] {
		const QDBusPendingReply<QString> reply = *call;
		call->deleteLater();
		if (serial != ownerQuerySerial_) return;
		ownerQueryPending_ = false;
		if (reply.isError() || reply.value().isEmpty()) {
			publish(std::nullopt);
			// Read may activate the portal. Only the unique-owner read below is
			// allowed to publish; an activation reply has no stable generation.
			connection_.asyncCall(readMessage(QString::fromLatin1(kService)), kCallTimeoutMs);
			retryLater();
			return;
		}
		setOwner(reply.value());
	});
}

void LinuxSystemTheme::setOwner(const QString &owner) {
	if (owner_ != owner) {
		if (subscribed_) {
			connection_.disconnect(owner_, QString::fromLatin1(kPath), QString::fromLatin1(kInterface),
				QStringLiteral("SettingChanged"), this,
				SLOT(settingChanged(QString,QString,QDBusVariant,QDBusMessage)));
		}
		owner_ = owner;
		subscribed_ = false;
		++generation_;
		++readSerial_;
		++revision_;
		publish(std::nullopt);
	}
	if (owner_.isEmpty()) {
		retryLater();
		return;
	}
	readSetting();
}

void LinuxSystemTheme::readSetting() {
	if (!subscribed_) {
		subscribed_ = connection_.connect(owner_, QString::fromLatin1(kPath),
			QString::fromLatin1(kInterface), QStringLiteral("SettingChanged"), this,
			SLOT(settingChanged(QString,QString,QDBusVariant,QDBusMessage)));
	}
	const auto generation = generation_;
	const auto revision = revision_;
	const auto serial = ++readSerial_;
	auto *call = new QDBusPendingCallWatcher(connection_.asyncCall(readMessage(owner_), kCallTimeoutMs), this);
	connect(call, &QDBusPendingCallWatcher::finished, this, [this, call, generation, revision, serial] {
		const QDBusPendingReply<QDBusVariant> reply = *call;
		call->deleteLater();
		if (generation != generation_ || revision != revision_ || serial != readSerial_) return;
		if (reply.isError()) {
			publish(std::nullopt);
			retryLater();
			return;
		}
		publish(preference(reply.value().variant()));
		if (subscribed_) retryTimer_.stop();
		else retryLater();
	});
}

void LinuxSystemTheme::settingChanged(const QString &nameSpace, const QString &key,
	const QDBusVariant &value, const QDBusMessage &message) {
	if (message.service() != owner_ || nameSpace != QString::fromLatin1(kNamespace)
		|| key != QString::fromLatin1(kKey)) return;
	++revision_;
	publish(preference(value.variant()));
	if (subscribed_) retryTimer_.stop();
}

void LinuxSystemTheme::publish(std::optional<bool> value) {
	if (prefersDark_ == value) return;
	prefersDark_ = value;
	emit changed();
}

void LinuxSystemTheme::retryLater() {
	if (!retryTimer_.isActive()) retryTimer_.start();
}

} // namespace MeetingUI
