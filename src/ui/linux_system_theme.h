#pragma once

#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusVariant>
#include <optional>

namespace MeetingUI {

// Owned and used by the Qt thread. Portal replies never change application UI
// directly; AppTheme remains the owner of the effective theme.
class LinuxSystemTheme final : public QObject {
	Q_OBJECT
public:
	explicit LinuxSystemTheme(QObject *parent = nullptr);
	explicit LinuxSystemTheme(QDBusConnection connection, QObject *parent = nullptr);
	std::optional<bool> prefersDark() const { return prefersDark_; }
	void refresh();

signals:
	void changed();

private slots:
	void settingChanged(const QString &nameSpace, const QString &key,
		const QDBusVariant &value, const QDBusMessage &message);

private:
	void queryOwner();
	void setOwner(const QString &owner);
	void readSetting();
	void publish(std::optional<bool> value);
	void retryLater();

	QDBusConnection connection_;
	QTimer retryTimer_;
	QString owner_;
	std::optional<bool> prefersDark_;
	quint64 generation_ = 0;
	quint64 revision_ = 0;
	quint64 readSerial_ = 0;
	quint64 ownerQuerySerial_ = 0;
	bool ownerQueryPending_ = false;
	bool subscribed_ = false;
};

} // namespace MeetingUI
