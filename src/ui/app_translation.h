#pragma once

#include <QtCore/QLocale>
#include <QtCore/QStringList>

class QCoreApplication;

namespace MeetingUI::AppTranslation {

// Load Simplified Chinese by default; --language overrides the startup locale.
QLocale startupLocale(const QStringList &arguments);
void install(QCoreApplication &application, const QLocale &locale,
	const QString &translationDirectory = QString());

} // namespace MeetingUI::AppTranslation
