#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QString>

namespace MeetingUI::AppBranding {

inline QString name() {
	return QStringLiteral("Cohavora");
}

inline QString displayName() {
	return QCoreApplication::translate("MeetingUI", "Cohavora - Open-source Audio and Video Meeting Client");
}

inline QString tagline() {
	return QCoreApplication::translate("MeetingUI", "Open-source Audio and Video Meeting Client");
}

} // namespace MeetingUI::AppBranding
