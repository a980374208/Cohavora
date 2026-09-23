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

} // namespace MeetingApp
