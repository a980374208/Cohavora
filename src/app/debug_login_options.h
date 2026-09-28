#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace MeetingApp {

enum class DebugLoginOptionStatus {
	Disabled,
	Enabled,
	Invalid,
};

struct DebugLoginOptions {
	DebugLoginOptionStatus status = DebugLoginOptionStatus::Disabled;
	bool debugEnabled = false;
	QString account;
	QString password;
	QString error;

	void clearPassword();
};

[[nodiscard]] DebugLoginOptions ParseDebugLoginOptions(
	const QStringList &arguments);

// Windows native QSettings uses the registry, independently of APPDATA.
[[nodiscard]] bool ConfigureDebugSettingsRoot(bool debugEnabled, const QString &root);

} // namespace MeetingApp
