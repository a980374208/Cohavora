#include "src/app/debug_login_options.h"

#include <QtCore/QChar>

namespace MeetingApp {
namespace {

bool IsAccountOption(const QString &argument) {
	return argument == QStringLiteral("-u") ||
		argument == QStringLiteral("--username");
}

bool IsPasswordOption(const QString &argument) {
	return argument == QStringLiteral("-p") ||
		argument == QStringLiteral("--password");
}

} // namespace

void DebugLoginOptions::clearPassword() {
	password.fill(QChar(u'\0'));
	password.clear();
}

DebugLoginOptions ParseDebugLoginOptions(const QStringList &arguments) {
	DebugLoginOptions result;
	bool accountSeen = false;
	bool passwordSeen = false;

	for (int i = 1; i < arguments.size(); ++i) {
		const auto &argument = arguments.at(i);
		if (argument == QStringLiteral("--debug")) {
			result.debugEnabled = true;
			continue;
		}
		if (!IsAccountOption(argument) && !IsPasswordOption(argument)) {
			continue;
		}

		const bool accountOption = IsAccountOption(argument);
		bool &seen = accountOption ? accountSeen : passwordSeen;
		if (seen) {
			result.status = DebugLoginOptionStatus::Invalid;
			result.error = QStringLiteral(
				"Debug credential arguments must not be repeated.");
			result.clearPassword();
			return result;
		}
		seen = true;
		if (++i >= arguments.size()) {
			result.status = DebugLoginOptionStatus::Invalid;
			result.error = QStringLiteral(
				"Debug credential arguments require a following value.");
			result.clearPassword();
			return result;
		}
		if (accountOption) {
			result.account = arguments.at(i);
		} else {
			result.password = arguments.at(i);
		}
	}

	if (!accountSeen && !passwordSeen) {
		return result;
	}
	if (!result.debugEnabled) {
		result.status = DebugLoginOptionStatus::Invalid;
		result.error = QStringLiteral(
			"Debug credential arguments are available only with --debug.");
		result.clearPassword();
		return result;
	}
	if (!accountSeen || !passwordSeen || result.account.trimmed().isEmpty() ||
		result.password.isEmpty()) {
		result.status = DebugLoginOptionStatus::Invalid;
		result.error = QStringLiteral(
			"Debug credential arguments require both non-empty -u and -p values.");
		result.clearPassword();
		return result;
	}

	result.status = DebugLoginOptionStatus::Enabled;
	return result;
}

} // namespace MeetingApp
